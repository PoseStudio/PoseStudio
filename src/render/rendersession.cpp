// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file rendersession.cpp
 * @brief A production render driven from the GUI thread. See rendersession.h.
 */

#include "rendersession.h"

#include "splatcomposite.h"

#include "parallelfor.h"
#include "renderenginelibrary.h"

#include <QFile>
#include <QFileInfo>
#include <QSignalBlocker>

#include <tinyexr.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace pose {

namespace {

constexpr int kPollMs = 100;

/// The process's engine library: loaded on first use, kept until the process ends.
struct EngineSlot {
    std::unique_ptr<RenderEngineLibrary> library;
    QString                              error;
    bool                                 tried = false;
};
EngineSlot& engineSlot() {
    static EngineSlot slot;
    return slot;
}

/// The count of running final renders, announced (RenderSession::activity).
int g_activeRenders = 0;

/// The session whose render the process's SHARED engine is running now (null: it is free). An
/// engine instance renders one thing at a time and its start() ends what it was rendering, so
/// a second session must not start on it meanwhile (RenderSession::start).
const RenderSession* g_sharedEngineUser = nullptr;

} // namespace

QString RenderDevice::label() const {
    switch (type) {
    case renderapi::DeviceType::Cuda: return name + QStringLiteral(" (CUDA)");
    case renderapi::DeviceType::Optix: return name + QStringLiteral(" (OptiX)");
    case renderapi::DeviceType::Hip: return name + QStringLiteral(" (HIP)");
    case renderapi::DeviceType::OneApi: return name + QStringLiteral(" (oneAPI)");
    case renderapi::DeviceType::Metal: return name + QStringLiteral(" (Metal)");
    default: return name;
    }
}

RenderSession::RenderSession(QObject* parent) : QObject(parent) {
    m_timer.setInterval(kPollMs);
    connect(&m_timer, &QTimer::timeout, this, &RenderSession::poll);
}

RenderSession::~RenderSession() {
    // A render still running is ended WITHOUT a word to the session's listeners: they are its
    // owner's slots, and an owner that holds the session by value is half destroyed by now (its
    // later members are gone, its QObject base and so the connections are not). Nobody is left
    // to show a last image either.
    const QSignalBlocker quiet(this);
    if (m_running) {
        m_engine->cancel(); // (returns once the engine no longer reads the scene)
        finishRun();
    }
}

bool RenderSession::engineInstalled() {
    return RenderEngineLibrary::present(RenderEngineLibrary::cyclesDirectory());
}

renderapi::Engine* RenderSession::engine(QString* error) {
    EngineSlot& slot = engineSlot();
    if (!slot.tried) {
        slot.tried = true;
        slot.library = RenderEngineLibrary::load(RenderEngineLibrary::cyclesDirectory(), &slot.error);
    }
    if (!slot.library && error != nullptr) {
        *error = slot.error;
    }
    return slot.library ? slot.library->engine() : nullptr;
}

std::unique_ptr<RenderEngineInstance> RenderSession::createEngine(QString* error) {
    if (engine(error) == nullptr) {
        return nullptr;
    }
    return engineSlot().library->createEngine();
}

int RenderSession::activeRenders() {
    return g_activeRenders;
}

RenderActivity* RenderSession::activity() {
    static RenderActivity notifier;
    return &notifier;
}

QString RenderSession::engineName() {
    renderapi::Engine* e = engine();
    return e != nullptr ? QStringLiteral("%1 %2").arg(QString::fromUtf8(e->name()), QString::fromUtf8(e->version())) : QString();
}

QVector<RenderDevice> RenderSession::devices() {
    QVector<RenderDevice> out;
    renderapi::Engine*    e = engine();
    if (e == nullptr) {
        return out;
    }
    const int count = e->deviceCount();
    for (int i = 0; i < count; ++i) {
        renderapi::DeviceInfo info;
        if (e->device(i, info)) {
            out.push_back({QString::fromUtf8(info.id), QString::fromUtf8(info.name), info.type});
        }
    }
    return out;
}

QString RenderSession::defaultDeviceId() {
    const QVector<RenderDevice> all = devices();
    // A hardware ray-tracing device first (OptiX, Metal), then any other GPU, then the CPU.
    for (const renderapi::DeviceType wanted :
         {renderapi::DeviceType::Optix, renderapi::DeviceType::Metal, renderapi::DeviceType::Cuda, renderapi::DeviceType::Hip,
          renderapi::DeviceType::OneApi}) {
        for (const RenderDevice& device : all) {
            if (device.type == wanted) {
                return device.id;
            }
        }
    }
    return QString();
}

bool RenderSession::start(std::shared_ptr<RenderSceneHolder> scene, const RenderOptions& options, QString* error) {
    stop(); // (this session's own render, if one is running)
    renderapi::Engine* e = engine(error);
    if (e == nullptr) {
        return false;
    }
    if (!scene) {
        if (error != nullptr) {
            *error = tr("There is no scene to render.");
        }
        return false;
    }
    // WHICH ENGINE. The shared one while it is free. While ANOTHER session's render is on it — the
    // Render window's, when a material is saved and its icon rendered meanwhile — an instance of
    // this session's own: starting on the shared engine would end that render without a word to
    // its session, which would go on reading this render's status and pixels as its own. A second
    // instance costs what a render costs anyway (every final render builds its own session inside
    // the engine); the two share the device for as long as both run.
    const bool sharedEngineBusy = g_sharedEngineUser != nullptr && g_sharedEngineUser != this;
    if (sharedEngineBusy) {
        if (!m_ownEngine) {
            m_ownEngine = createEngine();
        }
        if (!m_ownEngine) {
            if (error != nullptr) {
                *error = tr("The render engine is busy with another render and could not start a second one beside it.");
            }
            return false;
        }
        e = m_ownEngine->get();
    }
    const RenderSceneHolder::View view = scene->view();
    m_scene = std::move(scene);
    m_options = options;
    m_options.width = std::clamp(options.width, 16, 16384);
    m_options.height = std::clamp(options.height, 16, 16384);
    m_options.samples = std::clamp(options.samples, 1, 1 << 20);
    m_tonemap = view.tonemap;
    m_deviceId = options.deviceId.toUtf8();

    renderapi::Settings settings;
    settings.width = m_options.width;
    settings.height = m_options.height;
    settings.samples = m_options.samples;
    settings.device = m_deviceId.isEmpty() ? nullptr : m_deviceId.constData();
    settings.denoise = m_options.denoise ? 1 : 0;
    settings.exposure = view.exposure;
    settings.transparentBackground = m_options.transparent ? 1 : 0;

    // GAUSSIAN SPLATS: the engine cannot draw them, so the scene is rendered over nothing — its
    // floor's shadows kept as coverage — and composited into the capture's own layers, which the
    // viewport draws now at this render's size (splatcomposite.h).
    m_composite.reset();
    if (std::shared_ptr<RenderSplatLayers> layers = m_scene->splatLayers(m_options.width, m_options.height)) {
        if (layers->width == m_options.width && layers->height == m_options.height) {
            SplatComposite::View compositeView;
            compositeView.tonemap = m_tonemap;
            compositeView.exposure = view.exposure;
            compositeView.transparent = m_options.transparent;
            const renderapi::Environment& environment = m_scene->scene().environment;
            for (int c = 0; c < 3; ++c) {
                compositeView.backdropColor[c] = environment.backdropColor[c];
            }
            m_composite = std::make_unique<SplatComposite>(std::move(*layers), compositeView);
            settings.transparentBackground = 1;
            settings.shadowAlpha = 1;
        }
    }

    m_linear.assign(static_cast<std::size_t>(m_options.width) * static_cast<std::size_t>(m_options.height) * 4u, 0.0f);
    m_image = QImage();
    m_shownVersion = 0;
    if (!e->start(m_scene->scene(), settings)) {
        e->status(m_status);
        if (error != nullptr) {
            *error = QString::fromUtf8(m_status.error);
        }
        m_scene.reset();
        return false;
    }
    m_engine = e;
    if (!sharedEngineBusy) {
        g_sharedEngineUser = this;
    }
    m_running = true;
    ++g_activeRenders;
    emit activity()->changed(g_activeRenders);
    e->status(m_status);
    m_timer.start();
    emit statusChanged();
    return true;
}

void RenderSession::stop() {
    if (!m_running) {
        return;
    }
    m_engine->cancel(); // (returns once the engine no longer reads the scene)
    m_engine->status(m_status);
    refreshImage();
    finishRun();
}

void RenderSession::finishRun() {
    m_timer.stop();
    m_running = false;
    m_engine = nullptr; // (the engine's next render is not this session's to read)
    if (g_sharedEngineUser == this) {
        g_sharedEngineUser = nullptr;
    }
    g_activeRenders = std::max(g_activeRenders - 1, 0);
    emit activity()->changed(g_activeRenders);
    m_scene.reset(); // the engine is done with it
    emit statusChanged();
    emit ended();
}

void RenderSession::poll() {
    renderapi::Engine* e = m_engine;
    if (e == nullptr || !m_running) {
        return;
    }
    e->status(m_status);
    if (m_status.imageVersion != m_shownVersion) {
        refreshImage();
    }
    emit statusChanged();
    if (m_status.state == renderapi::State::Finished || m_status.state == renderapi::State::Cancelled ||
        m_status.state == renderapi::State::Failed) {
        finishRun();
    }
}

void RenderSession::refreshImage() {
    renderapi::Engine* e = m_engine;
    if (e == nullptr || m_linear.empty()) {
        return;
    }
    const int w = m_options.width;
    const int h = m_options.height;
    if (!e->copyImage(m_linear.data(), w, h)) {
        return;
    }
    m_shownVersion = m_status.imageVersion;
    if (m_image.width() != w || m_image.height() != h) {
        m_image = QImage(w, h, QImage::Format_RGBA8888);
    }
    if (m_composite) {
        // The scene laid into its Gaussian splats: the display picture, and the same picture in
        // linear light in place of the engine's (what an OpenEXR file is saved from).
        m_composite->compose(m_linear.data(), m_image.bits(), static_cast<std::size_t>(m_image.bytesPerLine()), m_linear.data());
        emit imageChanged();
        return;
    }
    // The viewport's last step, per channel, as 8 bits: splatcomposite.h's displayEncode — ONE
    // definition of it, so a render with a capture composited in and one without agree to the level.
    const bool         tonemap = m_tonemap;
    const bool         keepAlpha = m_options.transparent;
    uchar*             bits = m_image.bits();
    const qsizetype    stride = m_image.bytesPerLine();
    const float*       linear = m_linear.data();
    parallelFor(h, [&](int y) {
        const float* in = linear + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4u;
        uchar*       out = bits + static_cast<qsizetype>(y) * stride;
        for (int x = 0; x < w; ++x, in += 4, out += 4) {
            // (A transparent render's color is premultiplied by its coverage: unpremultiply for
            // the 8-bit picture, whose formats store straight alpha.)
            const float alpha = keepAlpha ? std::clamp(in[3], 0.0f, 1.0f) : 1.0f;
            const float inv = (keepAlpha && alpha > 1.0e-4f) ? 1.0f / alpha : 1.0f;
            out[0] = displayToByte(displayEncode(in[0] * inv, tonemap));
            out[1] = displayToByte(displayEncode(in[1] * inv, tonemap));
            out[2] = displayToByte(displayEncode(in[2] * inv, tonemap));
            out[3] = displayToByte(alpha);
        }
    });
    emit imageChanged();
}

bool RenderSession::save(const QString& path, QString* error) const {
    const auto fail = [&](const QString& why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };
    if (m_image.isNull() || m_linear.empty()) {
        return fail(tr("There is no image to save yet."));
    }
    if (QFileInfo(path).suffix().compare(QStringLiteral("exr"), Qt::CaseInsensitive) == 0) {
        // The engine's linear pixels, 32-bit float. Encoded in memory and written through QFile,
        // so a path with characters outside the system code page is written like any other.
        unsigned char* buffer = nullptr;
        const char*    exrError = nullptr;
        const int      bytes = SaveEXRToMemory(m_linear.data(), m_options.width, m_options.height, 4, /*fp16=*/0, &buffer, &exrError);
        if (bytes <= 0 || buffer == nullptr) {
            const QString why = exrError != nullptr ? QString::fromUtf8(exrError) : tr("The image could not be encoded.");
            if (exrError != nullptr) {
                FreeEXRErrorMessage(exrError);
            }
            return fail(why);
        }
        QFile file(path);
        const bool written = file.open(QIODevice::WriteOnly) && file.write(reinterpret_cast<const char*>(buffer), bytes) == bytes;
        std::free(buffer);
        return written ? true : fail(file.errorString());
    }
    // The 8-bit picture has STRAIGHT alpha. A format with alpha (PNG) keeps it; one without
    // (JPEG) is written by Qt with the alpha dropped — the straight colors as they are, which
    // for a transparent render is the scene's color where it covers and black only where
    // nothing does (an edge pixel is NOT darkened by its coverage).
    if (!m_image.save(path)) {
        return fail(tr("The image could not be written to %1.").arg(path));
    }
    return true;
}

} // namespace pose
