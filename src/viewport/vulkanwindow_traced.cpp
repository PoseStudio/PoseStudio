/**
 * @file vulkanwindow_traced.cpp
 * @brief THE RENDERED VIEWPORT MODE (the shade-mode table's `pathTraced` row): the scene path
 *        traced by the render engine, in the viewport, kept in step with what the user does.
 *        (A TU of VulkanWindow — see vulkanwindow.h for the class and its other TUs.)
 *
 * The engine (render/renderengineapi.h) renders INTERACTIVELY on an instance of its own — coarse
 * at first, refined for as long as nothing changes, alive when its samples are done — and this
 * file tells it what changed, in the cheapest of three ways:
 *
 *   THE CAMERA   (an orbit, a dolly, a resize): the camera alone. The picture follows the view
 *                as a path tracer's does — coarse while it moves, sharp when it rests.
 *   THE LIGHTING (a light object moved or set, an Environment dial, another panorama): the
 *                lights, the environment and the floor's shadows — the geometry stays on the
 *                device. A light dragged in the viewport is path traced as it goes.
 *   THE SCENE    (a pose, a material, a model added or removed): everything, captured as a final
 *                render captures it (captureRenderScene). WHILE A POSE IS BEING CHANGED the
 *                viewport shows the rasterized PBR shading instead — a drag runs at sixty poses
 *                a second, and each would be a scene to rebuild — and the path-traced picture
 *                comes back a moment after the pose rests.
 *
 * WHICH of the three is read off SIGNATURES of the live scene, compared with what the engine was
 * last sent — once per rendered frame (every change requests one) and on a timer that also polls
 * the engine: no gesture has to remember this mode. An update is not sent before the picture of
 * the last one has been shown (Status::updatesShown), so every step of a navigation is seen.
 *
 * THE DESCRIPTIONS' MEMORY: the engine borrows what a description points at until it has taken
 * a newer one in (Engine::update). Each description sent is HELD here with the version it
 * becomes, and let go once the engine reports a later version of its kind.
 *
 * A final render (Render → Render Image) has the device to itself: this mode's render is paused
 * while one runs (RenderSession::activity) and the viewport shows the PBR shading meanwhile.
 */

#include "vulkanwindow.h"

#include "constants.h"
#include "preferencesmanager.h"
#include "renderengineapi.h"
#include "renderenginelibrary.h"
#include "renderexport.h"
#include "renderscene.h"
#include "rendersession.h"
#include "scene/camera.h"
#include "scene/lightingsettings.h"
#include "scene/model.h"
#include "scene/scene.h"
#include "scene/scenelight.h"
#include "scene/shademode.h"
#include "rendering/vulkanrenderer.h"

#include <QElapsedTimer>
#include <QSize>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace pose {

namespace {

constexpr int kPollMs = 33;        // the engine is asked for its picture thirty times a second
constexpr int kSceneSettleMs = 180; // a changed scene is sent once it has stood still this long
constexpr int kDefaultSamples = 128; // (POSESTUDIO_TRACED_SAMPLES overrides)

/// POSESTUDIO_TRACED_TRACE=1 prints, for every description sent to the engine, how long its first
/// picture took to arrive and how long the finished one did — the mode's latency, measured.
const bool kTracedTrace = qEnvironmentVariableIsSet("POSESTUDIO_TRACED_TRACE");

/// What of a model a path-traced picture depends on.
struct ModelSignature {
    const Model*  model = nullptr;
    std::uint64_t pose = 0;      // Armature::skinVersion
    std::uint64_t materials = 0; // Model::materialRevision
    glm::mat4     transform{1.0f};
    bool operator==(const ModelSignature& o) const {
        return model == o.model && pose == o.pose && materials == o.materials && transform == o.transform;
    }
};
struct SceneSignature {
    std::vector<ModelSignature> models;
    std::uint64_t               revision = 0;      // Scene::modelsRevision: which models those addresses are
    float                       subsurface = 0.0f; // (the Subsurface dial is a material's, to a renderer)
    bool operator==(const SceneSignature& o) const {
        return models == o.models && revision == o.revision && subsurface == o.subsurface;
    }
};
struct LightingSignature {
    LightingSettings        dials;
    std::vector<SceneLight> lights;
    QString                 environment;
    float                   environmentScale = 1.0f;
    bool operator==(const LightingSignature& o) const {
        return dials == o.dials && lights == o.lights && environment == o.environment && environmentScale == o.environmentScale;
    }
};
struct CameraSignature {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    int       width = 0;
    int       height = 0;
    bool operator==(const CameraSignature& o) const {
        return view == o.view && projection == o.projection && width == o.width && height == o.height;
    }
};

/// A description sent to the engine, held until the engine has let go of it.
struct HeldScene {
    std::uint64_t                      version = 0;
    std::shared_ptr<RenderSceneHolder> scene;
};

} // namespace

struct VulkanWindow::TracedState {
    std::unique_ptr<RenderEngineInstance> engine;
    bool                                  started = false; // the engine's interactive render runs
    bool                                  paused = false;  // … but a final render has the device
    QTimer                                poll;

    // What the engine was last sent.
    SceneSignature    sentScene;
    LightingSignature sentLighting;
    CameraSignature   sentCamera;
    std::uint64_t     updatesSent = 0;      // update() calls so far (Status::updatesShown counts the same)
    std::uint64_t     sceneVersion = 0;     // the version the last SCENE description becomes …
    std::uint64_t     sceneUpdate = 0;      // … and the update count it was sent at
    int               scenesSent = 0;       // scene descriptions sent so far (the first included)
    int               lightingsSent = 0;    // … and lighting-only ones
    std::deque<HeldScene> scenes;           // descriptions the engine may still read: the scenes,
    std::deque<HeldScene> lightings;        // … and the lighting-only ones

    // A changed scene waiting to stand still.
    SceneSignature pendingScene;
    bool           scenePending = false;
    QElapsedTimer  sceneChangedAt;

    // The picture.
    std::uint64_t      imageVersion = 0;
    std::vector<float> pixels;
    int                shownWidth = 0; // the size of the picture the renderer was last handed
    int                shownHeight = 0;
    renderapi::Status  status;
    QByteArray         deviceId;
    QString            lastStatusText;

    // The trace's stopwatch (kTracedTrace): the last description sent, and when.
    QElapsedTimer traceClock;
    const char*   traceKind = nullptr;
    std::uint64_t traceUpdate = 0;      // the update count it was sent at
    std::uint64_t traceImageAtSend = 0; // the engine's image version then
    bool          traceShown = false;   // its first picture has been reported

    void mark(const char* kind) {
        if (kTracedTrace) {
            traceKind = kind;
            traceUpdate = updatesSent;
            traceImageAtSend = status.imageVersion;
            traceShown = false;
            traceClock.restart();
        }
    }
};

void VulkanWindow::TracedStateDeleter::operator()(TracedState* state) const {
    delete state;
}

namespace {

renderapi::Settings tracedSettings(const QByteArray& deviceId, const QSize& pixels, float exposure) {
    renderapi::Settings settings;
    settings.width = std::max(pixels.width(), 16);
    settings.height = std::max(pixels.height(), 16);
    bool      ok = false;
    const int samples = qEnvironmentVariableIntValue("POSESTUDIO_TRACED_SAMPLES", &ok);
    settings.samples = ok && samples > 0 ? samples : kDefaultSamples;
    settings.device = deviceId.isEmpty() ? nullptr : deviceId.constData();
    settings.denoise = qEnvironmentVariableIsSet("POSESTUDIO_TRACED_NO_DENOISE") ? 0 : 1;
    settings.exposure = exposure;
    settings.interactive = 1;
    return settings;
}

} // namespace

bool VulkanWindow::tracedModeWanted() const {
    return shadeModeAt(m_deferred.shadeMode).pathTraced;
}

bool VulkanWindow::tracedShown() const {
    return m_renderer && m_renderer->tracedShown();
}

void VulkanWindow::syncTracedMode() {
    const bool wanted = tracedModeWanted() && m_renderer != nullptr;
    if (wanted && !m_traced) {
        startTraced();
    } else if (!wanted && m_traced) {
        stopTraced(QString());
    }
}

void VulkanWindow::startTraced() {
    QString                               error;
    std::unique_ptr<RenderEngineInstance> engine = RenderSession::createEngine(&error);
    if (!engine) {
        // No engine in this copy, or one that will not load: say so, and stay in the PBR mode.
        const QString why = RenderSession::engineInstalled()
                                ? error
                                : tr("This copy of PoseStudio has no render engine, so the Rendered mode is not available.");
        setShadeMode(kDefaultShadeMode);
        emit tracedModeFailed(why);
        return;
    }
    m_traced.reset(new TracedState);
    TracedState& t = *m_traced;
    t.engine = std::move(engine);

    // The device the Render window was last told to use; the fastest found until it has been.
    QString device = PreferencesManager::instance().getValue(Constants::PREF_RENDER_DEVICE).toString();
    if (const QString forced = qEnvironmentVariable("POSESTUDIO_TRACED_DEVICE"); !forced.isEmpty()) {
        device.clear();
        for (const RenderDevice& candidate : RenderSession::devices()) {
            if (candidate.label().contains(forced, Qt::CaseInsensitive)) {
                device = candidate.id;
                break;
            }
        }
        if (forced.compare(QLatin1String("cpu"), Qt::CaseInsensitive) == 0) {
            device = QStringLiteral("CPU");
        }
    } else {
        bool known = false;
        for (const RenderDevice& candidate : RenderSession::devices()) {
            known = known || candidate.id == device;
        }
        if (!known) {
            device = RenderSession::defaultDeviceId();
        }
    }
    t.deviceId = device.toUtf8();

    t.poll.setInterval(kPollMs);
    connect(&t.poll, &QTimer::timeout, this, &VulkanWindow::tracedTick);
    t.poll.start();
    // A final render has the device to itself (see the file comment).
    connect(RenderSession::activity(), &RenderActivity::changed, this, &VulkanWindow::onRenderActivity, Qt::UniqueConnection);
    t.paused = RenderSession::activeRenders() > 0;
    requestUpdate();
}

void VulkanWindow::stopTraced(const QString& error) {
    if (!m_traced) {
        return;
    }
    TracedState& t = *m_traced;
    t.poll.stop();
    if (t.engine && t.engine->get() != nullptr) {
        t.engine->get()->cancel(); // (returns once the engine reads nothing of ours)
    }
    m_traced.reset();
    if (m_renderer) {
        m_renderer->setTracedShown(false);
        m_renderer->clearTracedImage();
        requestUpdate();
    }
    emit tracedStatusChanged(QString());
    if (!error.isEmpty()) {
        setShadeMode(kDefaultShadeMode);
        emit tracedModeFailed(error);
    }
}

void VulkanWindow::onRenderActivity(int running) {
    if (!m_traced) {
        return;
    }
    TracedState& t = *m_traced;
    const bool   pause = running > 0;
    if (pause != t.paused) {
        t.paused = pause;
        if (t.started) {
            t.engine->get()->setPaused(pause ? 1 : 0);
        }
        requestUpdate();
    }
}

void VulkanWindow::tracedTick() {
    if (!m_traced || !m_renderer) {
        return;
    }
    TracedState&       t = *m_traced;
    renderapi::Engine* engine = t.engine->get();
    const QSize        pixels = pixelExtent();
    if (pixels.width() < 16 || pixels.height() < 16) {
        return; // (minimised)
    }

    // --- What the scene is now ---
    Scene&         live = scene();
    SceneSignature sceneNow;
    for (std::size_t i = 0; i < live.modelCount(); ++i) {
        const Model* model = live.modelAt(i);
        if (model == nullptr || model->splats() != nullptr) {
            continue; // (a splat scene is not in a path-traced picture)
        }
        sceneNow.models.push_back({model, model->armature().skinVersion(), model->materialRevision(), model->transform()});
    }
    sceneNow.revision = live.modelsRevision();
    sceneNow.subsurface = live.lightingSettings().subsurface;
    LightingSignature lightingNow;
    lightingNow.dials = live.lightingSettings();
    lightingNow.lights = live.lights();
    lightingNow.environment = m_appliedEnvironmentPath;
    lightingNow.environmentScale = m_appliedEnvironmentScale;
    CameraSignature cameraNow;
    cameraNow.view = m_renderer->camera().view();
    cameraNow.projection = m_renderer->camera().projection();
    cameraNow.width = pixels.width();
    cameraNow.height = pixels.height();

    // A pose in the making (a drag, its settle, a fall, a dial being turned), a material being
    // scrubbed, VR Mode, a final render running: the rasterized shading shows, nothing is sent.
    const bool busy = m_ik.dragging || m_ik.settling || (m_fall.timer != nullptr && m_fall.timer->isActive()) ||
                      m_transformEdit || m_materialEdit || vrModeActive() || t.paused;

    const auto show = [&](bool shown) {
        if (m_renderer->tracedShown() != shown) {
            m_renderer->setTracedShown(shown);
            requestUpdate();
        }
    };
    const auto announce = [&](const QString& text) {
        if (text != t.lastStatusText) {
            t.lastStatusText = text;
            emit tracedStatusChanged(text);
        }
    };
    const float exposure = live.lightingSettings().exposure;

    // --- The engine's first description ---
    if (!t.started) {
        show(false);
        if (busy) {
            return;
        }
        std::shared_ptr<RenderSceneHolder> captured = captureRenderScene();
        if (!captured) {
            return;
        }
        const renderapi::Settings settings = tracedSettings(t.deviceId, pixels, exposure);
        if (!engine->start(captured->scene(), settings)) {
            renderapi::Status status;
            engine->status(status);
            stopTraced(QString::fromUtf8(status.error));
            return;
        }
        t.started = true;
        t.updatesSent = 0;
        t.scenesSent = 1;
        t.sceneVersion = 1;
        t.sceneUpdate = 0;
        t.scenes.push_back({1, std::move(captured)});
        t.sentScene = sceneNow;
        t.sentLighting = lightingNow;
        t.sentCamera = cameraNow;
        t.scenePending = false;
        t.mark("the first scene");
        announce(tr("Rendered: starting…"));
        return;
    }

    // --- The engine's state; the descriptions it has let go of ---
    engine->status(t.status);
    if (t.status.state == renderapi::State::Failed) {
        stopTraced(QString::fromUtf8(t.status.error));
        return;
    }
    const auto release = [&](std::deque<HeldScene>& held, std::uint64_t alsoBelow) {
        // The newest description of this kind the engine has taken in is the one it reads; every
        // older one — and, for the lighting ones, every one older than the scene now in — is free.
        std::uint64_t current = alsoBelow;
        for (const HeldScene& h : held) {
            if (h.version <= t.status.sceneVersion) {
                current = std::max(current, h.version);
            }
        }
        while (!held.empty() && held.front().version < current) {
            held.pop_front();
        }
    };
    release(t.scenes, 0);
    std::uint64_t sceneIn = 0;
    for (const HeldScene& h : t.scenes) {
        if (h.version <= t.status.sceneVersion) {
            sceneIn = std::max(sceneIn, h.version);
        }
    }
    release(t.lightings, sceneIn);

    // --- The scene changed: the PBR shading shows until the new picture is in ---
    if (!(sceneNow == t.sentScene)) {
        show(false);
        if (busy) {
            t.scenePending = false;
            announce(tr("Rendered: waiting for the pose"));
            return;
        }
        if (!t.scenePending || !(sceneNow == t.pendingScene)) {
            t.pendingScene = sceneNow;
            t.scenePending = true;
            t.sceneChangedAt.restart();
        }
        if (t.sceneChangedAt.elapsed() < kSceneSettleMs) {
            return; // (the poll timer comes back)
        }
        std::shared_ptr<RenderSceneHolder> captured = captureRenderScene();
        if (!captured) {
            return;
        }
        const std::uint64_t version =
            engine->update(captured->scene(), tracedSettings(t.deviceId, pixels, exposure), renderapi::kUpdateScene);
        if (version == 0) {
            stopTraced(tr("The render engine stopped."));
            return;
        }
        ++t.updatesSent;
        ++t.scenesSent;
        t.sceneVersion = version;
        t.sceneUpdate = t.updatesSent;
        t.scenes.push_back({version, std::move(captured)});
        t.sentScene = sceneNow;
        t.sentLighting = lightingNow;
        t.sentCamera = cameraNow;
        t.scenePending = false;
        t.mark("a scene update");
        announce(tr("Rendered: updating…"));
        return;
    }
    t.scenePending = false;
    if (busy) {
        show(false);
        announce(t.paused ? tr("Rendered: paused while an image renders") : tr("Rendered: waiting for the pose"));
        return;
    }

    // --- The lighting or the camera changed: sent once the last update's picture has been shown ---
    const bool caughtUp = t.status.updatesShown >= t.updatesSent;
    if (caughtUp && !(lightingNow == t.sentLighting)) {
        if (std::shared_ptr<RenderSceneHolder> captured = captureRenderLightingOnly()) {
            const std::uint64_t version =
                engine->update(captured->scene(), tracedSettings(t.deviceId, pixels, exposure), renderapi::kUpdateLighting);
            if (version != 0) {
                ++t.updatesSent;
                ++t.lightingsSent;
                t.lightings.push_back({version, std::move(captured)});
                t.sentLighting = lightingNow;
                t.mark("a lighting update");
            }
        }
    } else if (caughtUp && !(cameraNow == t.sentCamera)) {
        renderapi::Scene cameraOnly;
        cameraOnly.camera = renderCamera(m_renderer->camera());
        if (engine->update(cameraOnly, tracedSettings(t.deviceId, pixels, exposure), renderapi::kUpdateCamera) != 0) {
            ++t.updatesSent;
            t.sentCamera = cameraNow;
            t.mark("a camera update");
        }
    }

    // --- The picture: shown once it is a picture of the scene as it stands ---
    const bool sceneShown = t.status.sceneVersion >= t.sceneVersion && t.status.updatesShown >= t.sceneUpdate &&
                            t.status.imageWidth > 0 && t.status.imageHeight > 0;
    if (sceneShown && t.status.imageVersion != t.imageVersion) {
        const std::size_t count = static_cast<std::size_t>(t.status.imageWidth) * static_cast<std::size_t>(t.status.imageHeight) * 4u;
        t.pixels.resize(count);
        if (engine->copyImage(t.pixels.data(), t.status.imageWidth, t.status.imageHeight)) {
            t.imageVersion = t.status.imageVersion;
            t.shownWidth = t.status.imageWidth;
            t.shownHeight = t.status.imageHeight;
            m_renderer->setTracedImage(t.pixels.data(), static_cast<uint32_t>(t.status.imageWidth),
                                       static_cast<uint32_t>(t.status.imageHeight));
            requestUpdate();
        }
    }
    // (A picture of ANOTHER SIZE than the viewport — the window is being resized, the engine has
    // not delivered the new size yet — is not shown: drawn over the new viewport it would be the
    // old picture stretched. The rasterized shading stands in until the sizes agree.)
    const bool sized = t.shownWidth == pixels.width() && t.shownHeight == pixels.height();
    show(sceneShown && t.imageVersion != 0 && sized);
    if (t.traceKind != nullptr) {
        if (!t.traceShown && sceneShown && t.status.updatesShown >= t.traceUpdate && t.status.imageVersion != t.traceImageAtSend) {
            t.traceShown = true;
            std::fprintf(stderr, "[traced] %s: its first picture after %lld ms (sample %d of %d, %dx%d)\n", t.traceKind,
                         static_cast<long long>(t.traceClock.elapsed()), t.status.sample, t.status.samples, t.status.imageWidth,
                         t.status.imageHeight);
            std::fflush(stderr);
        }
        if (t.traceShown && t.status.samples > 0 && t.status.sample >= t.status.samples && t.status.updatesShown >= t.updatesSent) {
            std::fprintf(stderr, "[traced] %s: finished after %lld ms\n", t.traceKind, static_cast<long long>(t.traceClock.elapsed()));
            std::fflush(stderr);
            t.traceKind = nullptr;
        }
    }
    if (!sceneShown) {
        announce(tr("Rendered: updating…"));
    } else if (t.status.samples > 0 && t.status.sample < t.status.samples) {
        announce(tr("Rendered: %1 / %2 samples").arg(t.status.sample).arg(t.status.samples));
    } else {
        announce(tr("Rendered"));
    }
}

bool VulkanWindow::scriptTracedMetric(const QString& name, double& value) const {
    const QString n = name.toLower();
    if (n == QLatin1String("traced.on")) {
        value = m_traced ? 1.0 : 0.0;
    } else if (n == QLatin1String("traced.shown")) {
        value = tracedShown() ? 1.0 : 0.0;
    } else if (n == QLatin1String("traced.sample")) {
        value = m_traced ? m_traced->status.sample : 0.0;
    } else if (n == QLatin1String("traced.samples")) {
        value = m_traced ? m_traced->status.samples : 0.0;
    } else if (n == QLatin1String("traced.done")) {
        // The picture on screen is the finished one: every sample of the scene, the lighting
        // and the camera as they stand.
        value = (m_traced && tracedShown() && m_traced->status.samples > 0 && m_traced->status.sample >= m_traced->status.samples &&
                 m_traced->status.updatesShown >= m_traced->updatesSent && m_traced->imageVersion == m_traced->status.imageVersion)
                    ? 1.0
                    : 0.0;
    } else if (n == QLatin1String("traced.updates")) {
        value = m_traced ? static_cast<double>(m_traced->updatesSent) : 0.0;
    } else if (n == QLatin1String("traced.scenes")) {
        value = m_traced ? static_cast<double>(m_traced->scenesSent) : 0.0;
    } else if (n == QLatin1String("traced.lightings")) {
        value = m_traced ? static_cast<double>(m_traced->lightingsSent) : 0.0;
    } else if (n == QLatin1String("traced.width")) {
        value = m_traced ? m_traced->shownWidth : 0.0;
    } else if (n == QLatin1String("traced.height")) {
        value = m_traced ? m_traced->shownHeight : 0.0;
    } else if (n == QLatin1String("traced.held")) {
        value = m_traced ? static_cast<double>(m_traced->scenes.size() + m_traced->lightings.size()) : 0.0;
    } else {
        return false;
    }
    return true;
}

} // namespace pose
