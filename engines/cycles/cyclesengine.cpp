/**
 * @file cyclesengine.cpp
 * @brief The Cycles render engine behind PoseStudio's render engine interface
 *        (renderengineapi.h): devices, one render at a time on its own thread, the image.
 *
 * A RENDER'S LIFE. start() copies the description's top level (the arrays it points at stay the
 * application's, as the interface says) and hands it to a RENDER THREAD, so the caller — the
 * application's GUI thread — returns at once: creating the device (a CUDA context, an OptiX
 * pipeline) and building the scene take from a moment to, the first time a kernel is loaded,
 * a minute. That thread creates the Cycles session, builds its scene (scenebuilder.h), starts
 * it, waits for it and DESTROYS it before it reports a final state — so "the render has ended"
 * is exactly "nothing of Cycles holds a pointer into the application's memory any more". The
 * image outlives the session: it is copied out of Cycles's buffers as they update (the output
 * driver below) into a frame of this engine's own.
 *
 * Nothing thrown leaves this file: every entry point and the render thread catch and report
 * through Status.
 */

#include "memoryimage.h"
#include "renderengineapi.h"
#include "scenebuilder.h"

#include "posestudiocycles_export.h"

#include "device/device.h"
#include "scene/film.h"
#include "scene/integrator.h"
#include "scene/pass.h"
#include "scene/scene.h"
#include "session/buffers.h"
#include "session/output_driver.h"
#include "session/session.h"

#include "util/log.h"
#include "util/path.h"
#include "util/system.h"
#include "util/version.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace posecycles {

using namespace pose::renderapi;

namespace {

const char* const kPassName = "combined";

void copyText(char* destination, std::size_t size, const std::string& text) {
    if (size == 0) {
        return;
    }
    const std::size_t count = std::min(size - 1, text.size());
    std::memcpy(destination, text.data(), count);
    destination[count] = '\0';
}

DeviceType deviceTypeOf(ccl::DeviceType type) {
    switch (type) {
    case ccl::DEVICE_CUDA: return DeviceType::Cuda;
    case ccl::DEVICE_OPTIX: return DeviceType::Optix;
    case ccl::DEVICE_HIP: return DeviceType::Hip;
    case ccl::DEVICE_ONEAPI: return DeviceType::OneApi;
    case ccl::DEVICE_METAL: return DeviceType::Metal;
    default: return DeviceType::Cpu;
    }
}

/// The image as it stands, in the application's layout (RGBA float, row 0 = top), shared by the
/// output driver (Cycles's threads write it) and the engine (the application reads it).
struct Frame {
    std::mutex         mutex;
    std::vector<float> rgba;
    int                width = 0;
    int                height = 0;
    uint64_t           version = 0;

    void reset(int w, int h) {
        const std::lock_guard<std::mutex> lock(mutex);
        width = w;
        height = h;
        rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0.0f);
        ++version;
    }
};

/// Copies Cycles's render buffer into the Frame whenever Cycles updates it (every second or
/// two while rendering, and once when it is done — then denoised, if denoising is on). A tile
/// is copied to its place: an image larger than one tile fills in tile by tile.
class FrameOutputDriver final : public ccl::OutputDriver {
public:
    FrameOutputDriver(std::shared_ptr<Frame> frame, float exposure) : m_frame(std::move(frame)), m_exposure(exposure) {}

    void write_render_tile(const Tile& tile) override { copy(tile); }
    bool update_render_tile(const Tile& tile) override {
        copy(tile);
        return true;
    }

private:
    void copy(const Tile& tile) {
        const int w = tile.size.x;
        const int h = tile.size.y;
        if (w <= 0 || h <= 0) {
            return;
        }
        m_pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
        if (!tile.get_pass_pixels(kPassName, 4, m_pixels.data())) {
            return;
        }
        // The exposure is applied HERE, to the finished pixels (their alpha is coverage and is
        // left alone): Cycles's own film exposure also multiplies the denoised shadow-catcher
        // pass — a ratio, not a radiance — and a floor that only shows shadows then came out
        // darker than the backdrop it stands for, all over, by the exposure.
        if (m_exposure != 1.0f) {
            for (std::size_t i = 0; i < m_pixels.size(); i += 4) {
                m_pixels[i] *= m_exposure;
                m_pixels[i + 1] *= m_exposure;
                m_pixels[i + 2] *= m_exposure;
            }
        }
        const std::lock_guard<std::mutex> lock(m_frame->mutex);
        if (tile.full_size.x != m_frame->width || tile.full_size.y != m_frame->height) {
            return; // (not this render's size: a stale update)
        }
        // Cycles's rows run bottom-up, and a tile's offset is from the bottom-left.
        for (int y = 0; y < h; ++y) {
            const int row = m_frame->height - 1 - (tile.offset.y + y);
            if (row < 0 || row >= m_frame->height || tile.offset.x < 0 || tile.offset.x + w > m_frame->width) {
                continue;
            }
            std::memcpy(m_frame->rgba.data() + (static_cast<std::size_t>(row) * m_frame->width + tile.offset.x) * 4u,
                        m_pixels.data() + static_cast<std::size_t>(y) * w * 4u, static_cast<std::size_t>(w) * 4u * sizeof(float));
        }
        ++m_frame->version;
    }

    std::shared_ptr<Frame> m_frame;
    std::vector<float>     m_pixels;
    float                  m_exposure = 1.0f;
};

class CyclesEngine final : public Engine {
public:
    CyclesEngine() : m_frame(std::make_shared<Frame>()) {}

    const char* name() const override { return "Cycles"; }
    const char* version() const override { return CYCLES_VERSION_STRING; }

    int32_t deviceCount() override {
        try {
            refreshDevices();
            return static_cast<int32_t>(m_devices.size());
        } catch (...) {
            return 0;
        }
    }

    bool device(int32_t index, DeviceInfo& out) override {
        if (index < 0 || static_cast<std::size_t>(index) >= m_devices.size()) {
            return false;
        }
        const ccl::DeviceInfo& info = m_devices[static_cast<std::size_t>(index)];
        out = DeviceInfo();
        copyText(out.id, sizeof(out.id), info.id);
        copyText(out.name, sizeof(out.name), info.description);
        out.type = deviceTypeOf(info.type);
        return true;
    }

    bool start(const Scene& scene, const Settings& settings) override {
        cancel(); // one render at a time
        try {
            refreshDevices();
            m_scene = scene;
            m_settings = settings;
            m_deviceId = settings.device != nullptr ? settings.device : "";
            m_settings.device = nullptr; // (the copy in m_deviceId is the one that lives)
            m_frame->reset(std::max(settings.width, 0), std::max(settings.height, 0));
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_state = State::Starting;
                m_error.clear();
                m_text = "Starting";
                m_progress = 0.0f;
                m_sample = 0;
                m_seconds = 0.0;
            }
            m_cancel = false;
            m_thread = std::thread([this] { run(); });
            return true;
        } catch (const std::exception& e) {
            fail(e.what());
        } catch (...) {
            fail("Unknown error");
        }
        return false;
    }

    void cancel() override {
        m_cancel = true;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_session != nullptr) {
                m_session->progress.set_cancel("Cancelled");
                m_session->cancel(true);
            }
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    void status(Status& out) override {
        out = Status();
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_session != nullptr) {
            // (Progress's getters take its own lock: safe beside the session thread.)
            ccl::Progress& progress = m_session->progress;
            std::string status;
            std::string substatus;
            progress.get_status(status, substatus);
            m_text = substatus.empty() ? status : status + ": " + substatus;
            m_progress = static_cast<float>(std::clamp(progress.get_progress(), 0.0, 1.0));
            m_sample = progress.get_current_sample();
            double total = 0.0;
            double render = 0.0;
            progress.get_time(total, render);
            m_seconds = total;
        }
        out.state = m_state;
        out.progress = m_state == State::Finished ? 1.0f : m_progress;
        out.sample = m_sample;
        out.samples = m_settings.samples;
        out.seconds = m_seconds;
        copyText(out.text, sizeof(out.text), m_text);
        copyText(out.error, sizeof(out.error), m_error);
        {
            const std::lock_guard<std::mutex> frameLock(m_frame->mutex);
            out.imageVersion = m_frame->version;
        }
    }

    bool copyImage(float* rgba, int32_t width, int32_t height) override {
        if (rgba == nullptr) {
            return false;
        }
        const std::lock_guard<std::mutex> lock(m_frame->mutex);
        if (width != m_frame->width || height != m_frame->height || m_frame->rgba.empty()) {
            return false;
        }
        std::memcpy(rgba, m_frame->rgba.data(), m_frame->rgba.size() * sizeof(float));
        return true;
    }

    void destroy() override {
        cancel();
        delete this;
    }

private:
    ~CyclesEngine() = default;

    void refreshDevices() {
        if (!m_devices.empty()) {
            return;
        }
        // The CPU first, then every GPU device Cycles can open whose driver is recent enough.
        std::vector<ccl::DeviceInfo> gpus;
        for (const ccl::DeviceInfo& info : ccl::Device::available_devices()) {
            if (info.type == ccl::DEVICE_CPU) {
                m_devices.insert(m_devices.begin(), info);
            } else if (info.meets_driver_requirement) {
                gpus.push_back(info);
            }
        }
        m_devices.insert(m_devices.end(), gpus.begin(), gpus.end());
    }

    void fail(const std::string& why) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_state = State::Failed;
        m_error = why;
        m_text = "Failed";
    }

    /// The render thread: one whole render, the session created and destroyed inside it.
    void run() {
        std::string failure;
        bool        cancelled = false;
        try {
            ccl::SessionParams sessionParams;
            sessionParams.background = true; // a final render: full resolution from the first sample
            sessionParams.headless = false;  // … with the image updated while it renders
            sessionParams.samples = std::max(m_settings.samples, 1);
            sessionParams.threads = std::max(m_settings.threads, 0);
            sessionParams.use_auto_tile = true;
            sessionParams.tile_size = 2048;
            sessionParams.temp_dir = ccl::path_user_get("tiles");
            // (A render larger than a tile is written to this folder tile by tile — Cycles does not
            // make it: without it every such render failed with "Error writing tile to file".)
            ccl::path_create_directories(ccl::path_join(sessionParams.temp_dir, "tile"));
            sessionParams.device = m_devices.front(); // the CPU
            for (const ccl::DeviceInfo& info : m_devices) {
                if (!m_deviceId.empty() && info.id == m_deviceId) {
                    sessionParams.device = info;
                    break;
                }
            }
            sessionParams.denoise_device = sessionParams.device;

            ccl::SceneParams sceneParams;
            sceneParams.shadingsystem = ccl::SHADINGSYSTEM_SVM;
            sceneParams.background = true;
            sceneParams.use_texture_cache = false; // (no .tx files are made or looked for)

            auto session = std::make_unique<ccl::Session>(sessionParams, sceneParams);
            if (session->device->have_error()) {
                throw std::runtime_error(session->device->error_message());
            }
            session->set_output_driver(std::make_unique<FrameOutputDriver>(m_frame, std::max(m_settings.exposure, 0.0f)));

            ccl::Scene* scene = session->scene.get();
            std::string error;
            if (!buildScene(scene, m_scene, m_settings, error)) {
                throw std::runtime_error(error);
            }

            // Denoising, and the one pass that is read back: the combined image — denoised once
            // there is a denoised result, the noisy one until then (PathTraceTile's own rule).
            const bool denoise = m_settings.denoise != 0;
            scene->integrator->set_use_denoise(denoise);
            if (denoise) {
                scene->integrator->set_denoiser_type(ccl::DENOISER_OPENIMAGEDENOISE);
                scene->integrator->set_denoise_use_gpu(true);
            }
            ccl::Pass* pass = scene->create_node<ccl::Pass>();
            pass->set_name(ccl::ustring(kPassName));
            pass->set_type(ccl::PASS_COMBINED);
            pass->set_mode(denoise ? ccl::PassMode::DENOISED : ccl::PassMode::NOISY);

            ccl::BufferParams bufferParams;
            bufferParams.width = m_settings.width;
            bufferParams.height = m_settings.height;
            bufferParams.full_width = m_settings.width;
            bufferParams.full_height = m_settings.height;
            bufferParams.window_width = m_settings.width;
            bufferParams.window_height = m_settings.height;

            session->reset(sessionParams, bufferParams);
            if (!m_cancel) {
                {
                    const std::lock_guard<std::mutex> lock(m_mutex);
                    m_session = session.get();
                    m_state = State::Rendering;
                }
                session->start();
                session->wait();
            }

            // The outcome, read before the session goes.
            cancelled = m_cancel || session->progress.get_cancel();
            if (session->progress.get_error()) {
                failure = session->progress.get_error_message();
                if (failure.empty()) {
                    failure = "The render failed.";
                }
            }
            {
                Status last;
                status(last); // (the last progress numbers, kept for after the session)
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_session = nullptr;
            }
            session.reset(); // Cycles lets go of the application's memory here
        } catch (const std::exception& e) {
            failure = e.what();
        } catch (...) {
            failure = "Unknown error";
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_session = nullptr;
        if (!failure.empty()) {
            m_state = State::Failed;
            m_error = failure;
            m_text = "Failed";
        } else if (cancelled) {
            m_state = State::Cancelled;
            m_text = "Cancelled";
        } else {
            m_state = State::Finished;
            m_text = "Finished";
        }
    }

    std::vector<ccl::DeviceInfo> m_devices;
    std::shared_ptr<Frame>       m_frame;

    // The render in flight (the render thread's; m_mutex guards what status() and cancel() share).
    Scene             m_scene;
    Settings          m_settings;
    std::string       m_deviceId;
    std::thread       m_thread;
    std::atomic<bool> m_cancel{false};

    std::mutex    m_mutex;
    ccl::Session* m_session = nullptr; // non-null while the session renders
    State         m_state = State::Idle;
    std::string   m_error;
    std::string   m_text;
    float         m_progress = 0.0f;
    int           m_sample = 0;
    double        m_seconds = 0.0;
};

} // namespace

} // namespace posecycles

extern "C" POSESTUDIOCYCLES_EXPORT pose::renderapi::Engine* posestudio_create_render_engine(int32_t apiVersion,
                                                                                           const char* resourceDir,
                                                                                           const char* userDir) {
    if (apiVersion != POSESTUDIO_RENDER_API_VERSION) {
        return nullptr;
    }
    try {
        // Cycles finds its kernels and shaders under the folder it was installed to, which a
        // library cannot learn from the process (that is PoseStudio's own folder): given here.
        static std::once_flag once;
        const std::string     resources = resourceDir != nullptr ? resourceDir : "";
        const std::string     user = userDir != nullptr ? userDir : "";
        std::call_once(once, [&] {
            ccl::log_init(nullptr);
            ccl::path_init(resources, user);
            ccl::system_max_open_files_ensure();
        });
        return new posecycles::CyclesEngine();
    } catch (...) {
        return nullptr;
    }
}
