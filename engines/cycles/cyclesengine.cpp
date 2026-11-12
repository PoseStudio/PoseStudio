/**
 * @file cyclesengine.cpp
 * @brief The Cycles render engine behind PoseStudio's render engine interface
 *        (renderengineapi.h): devices, one render at a time on its own thread, the image.
 *
 * A FINAL RENDER'S LIFE. start() copies the description's top level (the arrays it points at stay
 * the application's, as the interface says) and hands it to a RENDER THREAD, so the caller — the
 * application's GUI thread — returns at once: creating the device (a CUDA context, an OptiX
 * pipeline) and building the scene take from a moment to, the first time a kernel is loaded,
 * a minute. That thread creates the Cycles session, builds its scene (scenebuilder.h), starts
 * it, waits for it and DESTROYS it before it reports a final state — so "the render has ended"
 * is exactly "nothing of Cycles holds a pointer into the application's memory any more". The
 * image outlives the session: it is copied out of Cycles's buffers as they update (the output
 * driver below) into a frame of this engine's own.
 *
 * AN INTERACTIVE RENDER'S (Settings::interactive: the viewport's) is the same thread kept ALIVE
 * around a Cycles session in its interactive mode — coarse first, at a fraction of the size
 * while things change, refined to the sample count while they do not, then waiting. The thread
 * sleeps until update() leaves it something to take in — a camera, or a whole new description —
 * which it applies under the scene's mutex and RESETS the session for: the device, its kernels
 * and every image both descriptions share are paid for once. Its picture reaches the frame
 * through a DISPLAY driver (Cycles's own path for a viewport: the pixels as half floats at
 * whatever size the session renders at just now), enlarged to the image's size.
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
#include "session/display_driver.h"
#include "session/output_driver.h"
#include "session/session.h"

#include "util/half.h"
#include "util/log.h"
#include "util/path.h"
#include "util/system.h"
#include "util/tbb.h"
#include "util/thread.h"
#include "util/version.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
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
/// drivers (Cycles's threads write it) and the engine (the application reads it).
struct Frame {
    std::mutex         mutex;
    std::vector<float> rgba;
    int                width = 0;
    int                height = 0;
    uint64_t           version = 0;
    bool               hasPixels = false; // false from a (re)size until the first pixels of that size
    // An interactive render's camera updates: the number of the one the session now renders
    // (set as it is taken in), and of the last one whose picture has reached this frame.
    uint64_t cameraRendering = 0;
    uint64_t cameraShown = 0;

    /// A new render's (or a resized one's) image: its size, and no pixels yet — what was last
    /// copied out stays the application's until there are.
    void reset(int w, int h) {
        const std::lock_guard<std::mutex> lock(mutex);
        width = w;
        height = h;
        rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0.0f);
        hasPixels = false;
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
        m_frame->hasPixels = true;
        ++m_frame->version;
    }

    std::shared_ptr<Frame> m_frame;
    std::vector<float>     m_pixels;
    float                  m_exposure = 1.0f;
};

/// An interactive render's picture: Cycles hands a display driver its pixels as half floats at
/// the size it renders at JUST NOW — a fraction of the image's while the view changes, the
/// image's own once it settles. They are enlarged (nearest: each coarse pixel a block, as every
/// path tracer's viewport shows them) into the Frame, rows turned top-down, the exposure applied
/// as the output driver applies it. Nothing is drawn here: the application shows the Frame.
class FrameDisplayDriver final : public ccl::DisplayDriver {
public:
    FrameDisplayDriver(std::shared_ptr<Frame> frame, std::shared_ptr<std::atomic<float>> exposure)
        : m_frame(std::move(frame)), m_exposure(std::move(exposure)) {}

    void next_tile_begin() override {}

    bool update_begin(const Params& params, const int width, const int height) override {
        if (width <= 0 || height <= 0) {
            return false;
        }
        m_fullWidth = params.size.x;
        m_fullHeight = params.size.y;
        m_width = width;
        m_height = height;
        m_half.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
        return true;
    }

    ccl::half4* map_texture_buffer() override { return m_half.data(); }
    void        unmap_texture_buffer() override {}

    void update_end() override {
        const std::lock_guard<std::mutex> lock(m_frame->mutex);
        const int fullWidth = m_frame->width;
        const int fullHeight = m_frame->height;
        if (m_fullWidth != fullWidth || m_fullHeight != fullHeight || fullWidth <= 0 || fullHeight <= 0) {
            return; // (not this image's size: an update from before a resize)
        }
        const float       exposure = m_exposure->load();
        const ccl::half4* source = m_half.data();
        float*            target = m_frame->rgba.data();
        const int         width = m_width;
        const int         height = m_height;
        m_columns.resize(static_cast<std::size_t>(fullWidth));
        for (int x = 0; x < fullWidth; ++x) {
            m_columns[static_cast<std::size_t>(x)] = std::min(static_cast<int>((static_cast<int64_t>(x) * width) / fullWidth), width - 1);
        }
        const int* columns = m_columns.data();
        ccl::parallel_for(0, fullHeight, [=](const int row) {
            // (Cycles's row 0 is the BOTTOM row; the frame's is the top.)
            const int         fromBottom = fullHeight - 1 - row;
            const int         sourceRow = std::min(static_cast<int>((static_cast<int64_t>(fromBottom) * height) / fullHeight), height - 1);
            const ccl::half4* in = source + static_cast<std::size_t>(sourceRow) * static_cast<std::size_t>(width);
            float*            out = target + static_cast<std::size_t>(row) * static_cast<std::size_t>(fullWidth) * 4u;
            for (int x = 0; x < fullWidth; ++x, out += 4) {
                const ccl::half4& texel = in[columns[x]];
                out[0] = ccl::half_to_float_image(texel.x) * exposure;
                out[1] = ccl::half_to_float_image(texel.y) * exposure;
                out[2] = ccl::half_to_float_image(texel.z) * exposure;
                out[3] = ccl::half_to_float_image(texel.w);
            }
        });
        m_frame->hasPixels = true;
        m_frame->cameraShown = m_frame->cameraRendering;
        ++m_frame->version;
    }

    void zero() override {} // (the last picture stays up until the next one is there)
    void draw(const Params& /*params*/) override {}

private:
    std::shared_ptr<Frame>              m_frame;
    std::shared_ptr<std::atomic<float>> m_exposure;
    std::vector<ccl::half4>             m_half;
    std::vector<int>                    m_columns;
    int                                 m_fullWidth = 0;
    int                                 m_fullHeight = 0;
    int                                 m_width = 0;
    int                                 m_height = 0;
};

ccl::BufferParams bufferParamsFor(int width, int height) {
    ccl::BufferParams params;
    params.width = width;
    params.height = height;
    params.full_width = width;
    params.full_height = height;
    params.window_width = width;
    params.window_height = height;
    return params;
}

class CyclesEngine final : public Engine {
public:
    CyclesEngine() : m_frame(std::make_shared<Frame>()), m_exposure(std::make_shared<std::atomic<float>>(1.0f)) {}

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
            m_interactive = settings.interactive != 0;
            m_exposure->store(std::max(settings.exposure, 0.0f));
            m_frame->reset(std::max(settings.width, 0), std::max(settings.height, 0));
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_state = State::Starting;
                m_error.clear();
                m_text = "Starting";
                m_progress = 0.0f;
                m_sample = 0;
                m_seconds = 0.0;
                m_pending = Pending();
                m_sceneRequested = 0;
                m_sceneApplied = 0;
                m_cameraRequested = 0;
                if (m_interactive) {
                    // The first description is the first update: taken in by the thread as every
                    // later one is.
                    m_pending.scene = true;
                    m_pending.description = m_scene;
                    m_pending.settings = m_settings;
                    m_pending.sceneId = ++m_sceneRequested;
                }
            }
            {
                const std::lock_guard<std::mutex> lock(m_frame->mutex);
                m_frame->cameraRendering = 0;
                m_frame->cameraShown = 0;
            }
            m_cancel = false;
            m_thread = std::thread([this] {
                if (m_interactive) {
                    runInteractive();
                } else {
                    run();
                }
            });
            return true;
        } catch (const std::exception& e) {
            fail(e.what());
        } catch (...) {
            fail("Unknown error");
        }
        return false;
    }

    uint64_t update(const Scene& scene, const Settings& settings, uint32_t what) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_interactive || !m_thread.joinable() || m_cancel ||
            (m_state != State::Starting && m_state != State::Rendering)) {
            return 0;
        }
        if ((what & kUpdateScene) != 0) {
            m_pending.scene = true;
            m_pending.camera = false;   // (the description brings its camera …
            m_pending.lighting = false; //  … and its lighting)
            m_pending.description = scene;
            m_pending.settings = settings;
            m_pending.settings.device = nullptr;
            m_pending.sceneId = ++m_sceneRequested;
            m_pending.cameraId = ++m_cameraRequested;
        } else if ((what & kUpdateLighting) != 0) {
            // (Beside a scene description not yet taken in, if there is one: applied after it.)
            m_pending.lighting = true;
            m_pending.lightingDescription = scene;
            m_pending.settings.exposure = settings.exposure;
            m_pending.lightingId = ++m_sceneRequested;
            m_pending.cameraId = ++m_cameraRequested;
        } else if ((what & kUpdateCamera) != 0) {
            if (m_pending.scene) {
                // A description not yet taken in: its camera and size are replaced in place.
                m_pending.description.camera = scene.camera;
            } else {
                m_pending.camera = true;
                m_pending.description.camera = scene.camera;
            }
            m_pending.settings.width = settings.width;
            m_pending.settings.height = settings.height;
            m_pending.cameraId = ++m_cameraRequested;
        }
        m_wake.notify_all();
        return m_sceneRequested;
    }

    void setPaused(int32_t paused) override {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_interactive && m_session != nullptr) {
            m_session->set_pause(paused != 0);
        }
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
        m_wake.notify_all();
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
            if (m_interactive && progress.get_error() && m_state != State::Failed) {
                // (An interactive session never ends on its own: its failure is read here.)
                m_state = State::Failed;
                m_error = progress.get_error_message();
                if (m_error.empty()) {
                    m_error = "The render failed.";
                }
            }
        }
        out.state = m_state;
        out.progress = m_state == State::Finished ? 1.0f : m_progress;
        out.sample = m_sample;
        out.samples = m_settings.samples;
        out.seconds = m_seconds;
        out.sceneVersion = m_sceneApplied;
        copyText(out.text, sizeof(out.text), m_text);
        copyText(out.error, sizeof(out.error), m_error);
        {
            const std::lock_guard<std::mutex> frameLock(m_frame->mutex);
            out.imageVersion = m_frame->version;
            out.imageWidth = m_frame->hasPixels ? m_frame->width : 0;
            out.imageHeight = m_frame->hasPixels ? m_frame->height : 0;
            out.updatesShown = m_frame->cameraShown;
        }
    }

    bool copyImage(float* rgba, int32_t width, int32_t height) override {
        if (rgba == nullptr) {
            return false;
        }
        const std::lock_guard<std::mutex> lock(m_frame->mutex);
        if (width != m_frame->width || height != m_frame->height || m_frame->rgba.empty() || !m_frame->hasPixels) {
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

    /// What update() has left for the interactive thread to take in.
    struct Pending {
        bool     scene = false;    // a whole description (and its camera)
        bool     camera = false;   // the camera and the size alone
        bool     lighting = false; // the lights, the environment and the floor alone
        Scene    description;      // (the top level is a copy; what it points at is the application's)
        Scene    lightingDescription;
        Settings settings;
        uint64_t sceneId = 0;
        uint64_t lightingId = 0;
        uint64_t cameraId = 0;
    };

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

    /// The session parameters both kinds of render share: the samples, the threads, the device.
    ccl::SessionParams sessionParams(const Settings& settings) const {
        ccl::SessionParams params;
        params.samples = std::max(settings.samples, 1);
        params.threads = std::max(settings.threads, 0);
        params.device = m_devices.front(); // the CPU
        for (const ccl::DeviceInfo& info : m_devices) {
            if (!m_deviceId.empty() && info.id == m_deviceId) {
                params.device = info;
                break;
            }
        }
        params.denoise_device = params.device;
        return params;
    }

    /// The render thread of a FINAL render: one whole render, the session created and destroyed
    /// inside it.
    void run() {
        std::string failure;
        bool        cancelled = false;
        try {
            ccl::SessionParams sessionParams = this->sessionParams(m_settings);
            sessionParams.background = true; // a final render: full resolution from the first sample
            sessionParams.headless = false;  // … with the image updated while it renders
            sessionParams.use_auto_tile = true;
            sessionParams.tile_size = 2048;
            sessionParams.temp_dir = ccl::path_user_get("tiles");
            // (A render larger than a tile is written to this folder tile by tile — Cycles does not
            // make it: without it every such render failed with "Error writing tile to file".)
            ccl::path_create_directories(ccl::path_join(sessionParams.temp_dir, "tile"));

            ccl::SceneParams sceneParams;
            sceneParams.shadingsystem = ccl::SHADINGSYSTEM_SVM;
            sceneParams.background = true;
            sceneParams.use_texture_cache = false; // (no .tx files are made or looked for)

            auto session = std::make_unique<ccl::Session>(sessionParams, sceneParams);
            if (session->device->have_error()) {
                throw std::runtime_error(session->device->error_message());
            }
            session->set_output_driver(std::make_unique<FrameOutputDriver>(m_frame, std::max(m_settings.exposure, 0.0f)));

            ccl::Scene*  scene = session->scene.get();
            SceneBuilder builder(scene);
            std::string  error;
            if (!builder.build(m_scene, m_settings, error)) {
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

            session->reset(sessionParams, bufferParamsFor(m_settings.width, m_settings.height));
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
        finish(failure, cancelled);
    }

    /// The render thread of an INTERACTIVE render: the session kept alive, each update taken in
    /// under the scene's mutex and the session reset for it (see the file comment).
    void runInteractive() {
        std::string failure;
        try {
            ccl::SessionParams sessionParams = this->sessionParams(m_settings);
            sessionParams.background = false; // interactive: coarse first, and alive when done
            sessionParams.headless = false;
            sessionParams.use_auto_tile = false; // (a viewport is one tile)

            ccl::SceneParams sceneParams;
            sceneParams.shadingsystem = ccl::SHADINGSYSTEM_SVM;
            sceneParams.background = false;
            sceneParams.use_texture_cache = false;

            auto session = std::make_unique<ccl::Session>(sessionParams, sceneParams);
            if (session->device->have_error()) {
                throw std::runtime_error(session->device->error_message());
            }
            session->set_display_driver(std::make_unique<FrameDisplayDriver>(m_frame, m_exposure));

            ccl::Scene*  scene = session->scene.get();
            SceneBuilder builder(scene);
            bool         started = false;
            int          width = m_settings.width;
            int          height = m_settings.height;

            for (;;) {
                Pending work;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_wake.wait(lock, [this] {
                        return m_cancel.load() || m_pending.scene || m_pending.camera || m_pending.lighting;
                    });
                    if (m_cancel) {
                        break;
                    }
                    work = m_pending;
                    m_pending.scene = false;
                    m_pending.camera = false;
                    m_pending.lighting = false;
                }
                const int newWidth = std::max(work.settings.width, 1);
                const int newHeight = std::max(work.settings.height, 1);
                Settings  sized = work.settings;
                sized.width = newWidth;
                sized.height = newHeight;
                {
                    // The scene is changed between two of the session's own updates of it, never
                    // during one (Blender's viewport synchronizes the same way).
                    const ccl::thread_scoped_lock sceneLock(scene->mutex);
                    if (work.scene) {
                        std::string error;
                        if (!builder.build(work.description, sized, error)) {
                            throw std::runtime_error(error);
                        }
                        // A viewport's denoising: from the first sample, the fast prefilter — the
                        // picture is looked at while it converges.
                        const bool denoise = sized.denoise != 0;
                        scene->integrator->set_use_denoise(denoise);
                        if (denoise) {
                            scene->integrator->set_denoiser_type(ccl::DENOISER_OPENIMAGEDENOISE);
                            scene->integrator->set_denoise_use_gpu(true);
                            scene->integrator->set_denoise_start_sample(1);
                            scene->integrator->set_denoiser_prefilter(ccl::DENOISER_PREFILTER_FAST);
                            scene->integrator->set_denoiser_quality(ccl::DENOISER_QUALITY_BALANCED);
                        }
                        if (!started) {
                            ccl::Pass* pass = scene->create_node<ccl::Pass>();
                            pass->set_name(ccl::ustring(kPassName));
                            pass->set_type(ccl::PASS_COMBINED);
                            pass->set_mode(ccl::PassMode::NOISY);
                            scene->film->set_display_pass(ccl::PASS_COMBINED);
                        }
                        sessionParams.samples = std::max(sized.samples, 1);
                    } else if (work.camera) {
                        builder.camera(work.description.camera, sized);
                    }
                    if (work.lighting) {
                        builder.lighting(work.lightingDescription, sized);
                    }
                    if (work.scene || work.lighting) {
                        m_exposure->store(std::max(sized.exposure, 0.0f));
                    }
                    if (newWidth != width || newHeight != height || !started) {
                        width = newWidth;
                        height = newHeight;
                        m_frame->reset(width, height);
                    }
                    session->reset(sessionParams, bufferParamsFor(width, height));
                }
                {
                    // (reset() has waited for the sample in flight: what reaches the frame from
                    // here on is this camera's.)
                    const std::lock_guard<std::mutex> frameLock(m_frame->mutex);
                    m_frame->cameraRendering = work.cameraId;
                }
                {
                    const std::lock_guard<std::mutex> lock(m_mutex);
                    if (work.scene) {
                        m_sceneApplied = work.sceneId;
                        m_settings.samples = sessionParams.samples;
                    }
                    if (work.lighting) {
                        m_sceneApplied = std::max(m_sceneApplied, work.lightingId);
                    }
                    if (!started) {
                        m_session = session.get();
                        m_state = State::Rendering;
                    }
                }
                if (!started) {
                    session->start();
                    started = true;
                }
            }

            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_session = nullptr;
            }
            session.reset(); // Cycles lets go of the application's memory here
        } catch (const std::exception& e) {
            failure = e.what();
        } catch (...) {
            failure = "Unknown error";
        }
        finish(failure, /*cancelled=*/true);
    }

    /// A render thread's last word: the final state.
    void finish(const std::string& failure, bool cancelled) {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_session = nullptr;
        if (!failure.empty()) {
            m_state = State::Failed;
            m_error = failure;
            m_text = "Failed";
        } else if (m_state == State::Failed) {
            // (an interactive session's failure, already read by status(): kept)
        } else if (cancelled) {
            m_state = State::Cancelled;
            m_text = "Cancelled";
        } else {
            m_state = State::Finished;
            m_text = "Finished";
        }
    }

    std::vector<ccl::DeviceInfo>        m_devices;
    std::shared_ptr<Frame>              m_frame;
    std::shared_ptr<std::atomic<float>> m_exposure; // an interactive render's (its updates may change it)

    // The render in flight (the render thread's; m_mutex guards what status(), update() and
    // cancel() share).
    Scene             m_scene;
    Settings          m_settings;
    std::string       m_deviceId;
    bool              m_interactive = false;
    std::thread       m_thread;
    std::atomic<bool> m_cancel{false};

    std::mutex              m_mutex;
    std::condition_variable m_wake;              // an interactive render's thread sleeps on it
    Pending                 m_pending;
    uint64_t                m_sceneRequested = 0;
    uint64_t                m_sceneApplied = 0;
    uint64_t                m_cameraRequested = 0;
    ccl::Session*           m_session = nullptr; // non-null while the session renders
    State                   m_state = State::Idle;
    std::string             m_error;
    std::string             m_text;
    float                   m_progress = 0.0f;
    int                     m_sample = 0;
    double                  m_seconds = 0.0;
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
