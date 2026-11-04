/**
 * @file vulkanwindow_vr.cpp
 * @brief VulkanWindow's VR Mode: the OpenXR session's lifecycle, the thread that waits on the
 *        headset's frame pacing, and the GUI-thread stereo frame.
 *
 * One of the translation units of VulkanWindow — see vulkanwindow.h for the map. The headset
 * itself was found at startup (OpenXrSystem, from ViewportWidget) and the Vulkan device created
 * to its requirements (initializeVulkan); this file starts a session on demand (setVrMode),
 * runs it, and tears it down.
 *
 * HOW A FRAME RUNS. OpenXR paces the application: xrWaitFrame blocks until the runtime wants
 * the next frame — up to a whole frame period, at 72-120 Hz. Blocking the GUI thread for that
 * would make every mouse event and the 60 Hz drag timer wait on the headset, so that one call
 * runs on a dedicated thread (startVrWaiter), which the spec designs for exactly this: the
 * thread waits, hands the predicted display time over (VrSync + a queued call), and then waits
 * on a condition variable until the GUI thread has finished the frame before waiting again.
 * The GUI thread (onVrFrameReady) does the rest in order: beginFrame -> locate the two eyes ->
 * acquire both swapchain images -> VulkanRenderer::drawVrFrame (shadow once, each eye's scene,
 * composited into the headset's images) -> release -> endFrame -> requestUpdate() for the
 * desktop MIRROR (the renderer composites the left eye into the window while VR is on). Scene
 * mutations (posing, imports, lighting) all happen on the GUI thread too, so they serialize
 * with the frame without locks.
 *
 * PLACEMENT. The runtime's STAGE space is floor-level and in metres, like the scene, so a figure
 * stands at true scale on the user's real floor. The first tracked frame (and View → Recenter
 * VR View) places the user ~2 m in front of the framed figure, facing it (vrRecenterTransform);
 * where they then walk is their own business.
 */

#include "vulkanwindow.h"

#include "openxrinput.h"
#include "openxrsession.h"
#include "openxrsystem.h"
#include "rendering/vulkancommon.h"
#include "rendering/vulkancontext.h"
#include "rendering/vulkanrenderer.h"
#include "scene/camera.h"
#include "scene/scene.h"
#include "vrmath.h"
#include "vrsettings.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QTimer>
#include <QVulkanInstance>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pose {

namespace {
/// How far in front of the figure the user is placed by a recentre (metres).
constexpr float kStandDistance = 2.2f;
/// The world height of an EYE-LEVEL (LOCAL) reference space's origin, when the runtime offers
/// no floor-level (STAGE) space: a typical standing eye height, so the floor still lands near
/// the user's feet.
constexpr float kEyeLevelFloorOffset = 1.65f;
/// The session poll cadence: OpenXR's state changes arrive as events, polled.
constexpr int kVrPollMs = 10;

// --- The controllers (applyVrInput) --------------------------------------------------------
/// Walking speed at full stick (metres per second) — a stroll around a figure.
constexpr float kMoveSpeed = 1.5f;
/// Stick deflection below which a stick reads as at rest (resting sticks drift a little).
constexpr float kStickDeadzone = 0.2f;
/// Turning speed at full deflection of the right stick (degrees per second) — SMOOTH turning,
/// like the walk: the user found a stepped (snap) turn distracting, and a modest rate keeps it
/// comfortable.
constexpr float kTurnSpeedDegrees = 90.0f;
/// The trigger as a button, with hysteresis (an analog trigger hovering at the threshold must
/// not click twice).
constexpr float kTriggerOn = 0.6f;
constexpr float kTriggerOff = 0.4f;
/// The pointer ray is picked with the SAME code as a mouse click, through a virtual camera that
/// looks down the ray: a square viewport of this many pixels over this field of view — the
/// picker's pixel tolerances (14 px on a joint, 32 px on a bone body) become angular ones, about
/// 1.6° and 3.6° here, a few centimetres at arm's length.
constexpr float kPickViewportPx = 512.0f;
constexpr float kPickFovDegrees = 60.0f;
/// How far a pointer ray draws when it hits nothing (it stops on the floor before that).
constexpr float kPointerLength = 3.0f;
/// While a joint is dragged on the ray, the right stick's Y pushes it away / pulls it in — the
/// mouse wheel's depth control — at this many metres per second at full deflection.
constexpr float kDragDepthSpeed = 0.8f;
/// A HAND GRAB: a trigger press whose ray hits nothing takes the joint within this reach of the
/// controller itself (metres) — reaching out and taking hold, rather than pointing.
constexpr float kHandGrabRadius = 0.15f;
/// The small controller glyph at each ray's origin: a stub back along the ray and an up tick.
constexpr float kGlyphStubLength = 0.06f;
constexpr float kGlyphTickLength = 0.03f;
/// The ray's colours (linear): at rest, over a joint, and carrying one.
const glm::vec3 kPointerIdle(0.55f, 0.55f, 0.6f);
const glm::vec3 kPointerHover(0.1046f, 0.2423f, 0.6038f); // the selection accent (postprocess.h)
const glm::vec3 kPointerDrag(0.95f, 0.45f, 0.08f);

/// A camera looking down a pointer ray, so the mouse's screen-space pick runs on it unchanged
/// (Camera::setExplicitView): the ray is the viewport's centre pixel.
Camera pointerPickCamera(const glm::vec3& origin, const glm::vec3& direction) {
    const glm::vec3 up = std::abs(direction.y) > 0.95f ? glm::vec3(0.0f, 0.0f, -1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    glm::mat4 projection = glm::perspective(glm::radians(kPickFovDegrees), 1.0f, 0.02f, 100.0f);
    projection[1][1] *= -1.0f; // Vulkan's clip-space Y, as Camera does
    Camera camera;
    camera.setExplicitView(glm::lookAt(origin, origin + direction, up), projection);
    return camera;
}
} // namespace

/// The hand-off between the wait thread and the GUI thread. Shared (shared_ptr) by the thread,
/// the window's VrState and the frame handler's scope guard, so whichever goes last keeps it.
struct VrSync {
    std::mutex              mutex;
    std::condition_variable cv;
    bool    stop = false;           // the thread must exit
    bool    frameConsumed = true;   // the GUI thread finished the handed-over frame
    bool    framePending = false;   // a frame time is waiting for the GUI thread
    bool    waiterFailed = false;   // xrWaitFrame failed: the session is gone
    int64_t time = 0;               // the pending frame's predicted display time
    bool    shouldRender = false;   // the runtime wants content for it
    int64_t period = 0;             // the headset's display period (nanoseconds)
};

/// The frame-time readout (POSESTUDIO_VR_PERF=1): per second, how many frames the headset asked
/// for, how many were rendered, how many came LATE (a display-time gap of more than 1.5 periods —
/// a frame the runtime had to reproject), and the GUI thread's work per frame.
struct VrPerf {
    bool          on = false;
    QElapsedTimer clock;
    qint64        windowStart = 0;
    int           frames = 0;
    int           rendered = 0;
    int           late = 0;
    double        workMs = 0.0;
    double        workMaxMs = 0.0;
    int64_t       lastDisplayTime = 0;
    int64_t       period = 0;
};

struct VulkanWindow::VrState {
    std::unique_ptr<OpenXrSession> session;
    std::shared_ptr<VrSync>        sync;
    std::thread                    waiter;
    QTimer*                        pollTimer = nullptr;
    glm::mat4                      worldFromStage{1.0f};
    bool                           needsRecenter = true;
    bool                           announcedTracking = false;
    // The controllers (applyVrInput): the previous frame's time (for the walk's and the turn's
    // dt), each trigger's held state and aim orientation (the roll between frames twists a held
    // joint), and the hand carrying an IK drag with where the grabbed point rides in the HAND's
    // frame — straight down the ray at the grab distance for a ray grab, the joint's offset from
    // the hand for a hand grab — so the point follows the hand's every move and turn.
    int64_t   lastFrameTime = 0;
    bool      selectHeld[2] = {false, false};
    bool      prevAimValid[2] = {false, false};
    glm::quat prevAim[2] = {glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)};
    int       dragHand = -1;
    glm::vec3 grabOffset{0.0f};
    VrPerf    perf;
};

void VulkanWindow::VrStateDeleter::operator()(VrState* state) const {
    delete state;
}

void VulkanWindow::setOpenXrSystem(OpenXrSystem* system) {
    m_xr = system;
}

glm::vec3 VulkanWindow::vrStandPoint() const {
    glm::vec3 mn;
    glm::vec3 mx;
    if (m_renderer && scene().framingBounds(mn, mx)) {
        const glm::vec3 center = 0.5f * (mn + mx);
        // A figure faces +Z: stand on that side, looking back at it (-Z). Half the depth of the
        // bounds keeps the distance to the figure's front, not its centre.
        return glm::vec3(center.x, 0.0f, mx.z + kStandDistance);
    }
    return glm::vec3(0.0f, 0.0f, kStandDistance);
}

bool VulkanWindow::setVrMode(bool on) {
    if (!on) {
        stopVr(QString());
        return false;
    }
    if (m_vr) {
        return true;
    }
    if (!m_renderer || m_deviceFailed) {
        emit vrError(tr("The 3D viewport is not available, so there is nothing to show in the headset."));
        return false;
    }
    if (!m_xr) {
        emit vrError(VrSettings::status());
        return false;
    }

    OpenXrSession::VulkanBinding binding;
    binding.instance = m_instance->vkInstance();
    binding.physicalDevice = m_context->physicalDevice();
    binding.device = m_context->device();
    binding.queueFamilyIndex = m_context->graphicsFamily();
    binding.queueIndex = 0; // VulkanContext takes queue 0 of the family (vkGetDeviceQueue(…, 0))

    std::string reason;
    // The depth layer needs the resolve shader's multisampled read: only with MSAA.
    const bool wantDepth = m_context->sampleCount() != VK_SAMPLE_COUNT_1_BIT;
    auto session = OpenXrSession::create(*m_xr, binding, static_cast<float>(VrSettings::renderScale()), wantDepth,
                                         reason);
    if (!session) {
        qWarning() << "[vr] session creation failed:" << QString::fromStdString(reason);
        emit vrError(QString::fromStdString(reason));
        return false;
    }

    std::unique_ptr<VrState, VrStateDeleter> vr(new VrState());
    vr->sync = std::make_shared<VrSync>();
    // Until the first tracked frame recentres properly: the stage origin in front of the figure.
    vr->worldFromStage = vrRecenterTransform(glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), vrStandPoint(),
                                             session->floorLevel() ? 0.0f : kEyeLevelFloorOffset);
    vr->needsRecenter = true;
    vr->perf.on = qEnvironmentVariableIsSet("POSESTUDIO_VR_PERF");
    if (vr->perf.on) {
        vr->perf.clock.start();
    }
    try {
        m_renderer->beginVr(session->renderPass(), session->eyeExtent(), !session->formatIsSrgb(),
                            session->hasDepth() ? session->depthRenderPass() : VK_NULL_HANDLE,
                            VrSettings::reducedEffects());
    } catch (const VulkanError& e) {
        qWarning() << "[vr] the eye targets could not be created:" << e.what();
        emit vrError(tr("The headset's render targets could not be created: %1").arg(e.what()));
        return false;
    }
    vr->session = std::move(session);
    vr->pollTimer = new QTimer(this);
    vr->pollTimer->setInterval(kVrPollMs);
    connect(vr->pollTimer, &QTimer::timeout, this, &VulkanWindow::onVrPoll);
    vr->pollTimer->start();
    m_vr = std::move(vr);
    ++m_vrGeneration;
    qInfo() << "[vr] VR Mode started";
    emit vrModeChanged(true);
    requestUpdate();
    return true;
}

void VulkanWindow::recenterVr() {
    if (m_vr) {
        m_vr->needsRecenter = true;
    }
}

void VulkanWindow::stopVr(const QString& error) {
    if (!m_vr) {
        return;
    }
    endVrDrag(); // a joint carried on a controller ray lands as on a mouse release
    if (m_renderer) {
        scene().setVrPointerLines({}); // the rays go with the controllers
    }
    stopVrWaiter();
    if (m_vr->pollTimer) {
        m_vr->pollTimer->stop();
        m_vr->pollTimer->deleteLater();
        m_vr->pollTimer = nullptr;
    }
    m_vr->session->end(); // a no-op unless running
    if (m_renderer) {
        m_renderer->endVr(); // waits for the device; the eye targets go before the session's images
    }
    m_vr.reset(); // the session: swapchains, spaces
    ++m_vrGeneration;
    qInfo() << "[vr] VR Mode ended" << (error.isEmpty() ? QString() : QStringLiteral("(%1)").arg(error));
    requestUpdate(); // the desktop view renders its own camera again
    emit vrModeChanged(false);
    if (!error.isEmpty()) {
        emit vrError(error);
    }
}

void VulkanWindow::startVrWaiter() {
    if (!m_vr || m_vr->waiter.joinable()) {
        return;
    }
    std::shared_ptr<VrSync> sync = m_vr->sync;
    {
        std::lock_guard<std::mutex> lock(sync->mutex);
        sync->stop = false;
        sync->frameConsumed = true;
        sync->framePending = false;
        sync->waiterFailed = false;
    }
    OpenXrSession* session = m_vr->session.get(); // outlives the thread: stopVrWaiter joins before the session goes
    const uint64_t generation = m_vrGeneration;
    m_vr->waiter = std::thread([this, sync, session, generation]() {
        const auto wakeGui = [this, generation]() {
            // A queued call: it runs on the GUI thread, and a window being destroyed drops it.
            QMetaObject::invokeMethod(
                this,
                [this, generation]() {
                    if (m_vrGeneration == generation) {
                        onVrFrameReady();
                    }
                },
                Qt::QueuedConnection);
        };
        while (true) {
            {
                std::unique_lock<std::mutex> lock(sync->mutex);
                sync->cv.wait(lock, [&]() { return sync->stop || sync->frameConsumed; });
                if (sync->stop) {
                    return;
                }
                sync->frameConsumed = false;
            }
            int64_t time = 0;
            bool shouldRender = false;
            int64_t period = 0;
            if (!session->waitFrame(time, shouldRender, period)) {
                {
                    std::lock_guard<std::mutex> lock(sync->mutex);
                    sync->waiterFailed = true;
                }
                wakeGui();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(sync->mutex);
                if (sync->stop) {
                    return;
                }
                sync->time = time;
                sync->shouldRender = shouldRender;
                sync->period = period;
                sync->framePending = true;
            }
            wakeGui();
        }
    });
}

void VulkanWindow::stopVrWaiter() {
    if (!m_vr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_vr->sync->mutex);
        m_vr->sync->stop = true;
    }
    m_vr->sync->cv.notify_all();
    if (m_vr->waiter.joinable()) {
        m_vr->waiter.join(); // at most one frame period: xrWaitFrame returns at the next frame boundary
    }
}

void VulkanWindow::onVrPoll() {
    if (!m_vr) {
        return;
    }
    switch (m_vr->session->pollEvents()) {
    case OpenXrSession::Event::Ready:
        if (!m_vr->session->running()) {
            if (!m_vr->session->begin()) {
                stopVr(tr("The headset session could not be started."));
                return;
            }
            startVrWaiter();
        }
        break;
    case OpenXrSession::Event::Stopping:
        // The runtime ends the session (the user exited it from the headset, or the link
        // dropped): stop waiting on frames FIRST, then end — a Gone usually follows.
        stopVrWaiter();
        m_vr->session->end();
        break;
    case OpenXrSession::Event::Gone:
        stopVr(tr("The headset session ended."));
        break;
    case OpenXrSession::Event::None:
        break;
    }
}

void VulkanWindow::onVrFrameReady() {
    if (!m_vr || !m_renderer) {
        return;
    }
    VrState& vr = *m_vr;
    std::shared_ptr<VrSync> sync = vr.sync;
    int64_t time = 0;
    bool shouldRender = false;
    bool failed = false;
    int64_t period = 0;
    {
        std::lock_guard<std::mutex> lock(sync->mutex);
        failed = sync->waiterFailed;
        if (!failed) {
            if (!sync->framePending) {
                return; // a stale wake-up (the frame was already taken)
            }
            sync->framePending = false;
            time = sync->time;
            shouldRender = sync->shouldRender;
            period = sync->period;
        }
    }
    const qint64 workStart = vr.perf.on ? vr.perf.clock.nsecsElapsed() : 0;
    // Whatever happens below — a lost session, a device loss — the wait thread may wait for the
    // next frame once this scope ends (it holds its own reference to the sync block).
    struct Consumed {
        std::shared_ptr<VrSync> sync;
        ~Consumed() {
            {
                std::lock_guard<std::mutex> lock(sync->mutex);
                sync->frameConsumed = true;
            }
            sync->cv.notify_all();
        }
    } consumed{sync};

    if (failed) {
        stopVr(tr("The headset stopped delivering frames."));
        return;
    }
    if (!vr.session->running()) {
        return; // a Stopping arrived between the wait and this call
    }
    if (!vr.session->beginFrame()) {
        stopVr(tr("The headset refused a frame."));
        return;
    }

    bool rendered = false;
    try {
        VrEyeView views[2];
        // The play space first — a recentre, then the controllers' walk / turn — so the eyes are
        // located, and the frame drawn, from where the user now stands.
        if (shouldRender && vr.needsRecenter) {
            glm::vec3 head;
            glm::quat facing;
            if (vr.session->headPose(time, head, facing)) {
                vr.worldFromStage = vrRecenterTransform(head, facing, vrStandPoint(),
                                                        vr.session->floorLevel() ? 0.0f : kEyeLevelFloorOffset);
                vr.needsRecenter = false;
            }
        }
        if (shouldRender) {
            const float dt = vr.lastFrameTime > 0
                                 ? std::clamp(static_cast<float>(time - vr.lastFrameTime) * 1.0e-9f, 0.0f, 0.1f)
                                 : 0.0f;
            vr.lastFrameTime = time;
            VrInput input;
            if (vr.session->syncInput(time, input)) {
                applyVrInput(vr, input, dt, time);
            }
        }
        if (shouldRender && vr.session->locateViews(time, vr.worldFromStage, views)) {
            if (!vr.announcedTracking) {
                vr.announcedTracking = true;
                qInfo() << "[vr] tracking; first stereo frame";
            }
            VkFramebuffer framebuffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
            VkFramebuffer depthFramebuffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
            bool acquired[2] = {false, false};
            bool ok = true;
            for (int eye = 0; eye < 2 && ok; ++eye) {
                uint32_t imageIndex = 0;
                acquired[eye] = vr.session->acquireEye(eye, imageIndex);
                ok = acquired[eye];
                if (ok) {
                    framebuffers[eye] = vr.session->framebuffer(eye, imageIndex);
                    depthFramebuffers[eye] = vr.session->depthFramebuffer(eye); // null without a depth layer
                }
            }
            if (ok) {
                m_renderer->drawVrFrame(views, framebuffers, depthFramebuffers);
                rendered = true;
            }
            for (int eye = 0; eye < 2; ++eye) {
                if (acquired[eye]) {
                    vr.session->releaseEye(eye); // after the submit that renders it
                }
            }
        }
    } catch (const VulkanError& e) {
        vr.session->endFrame(time, false);
        failDevice("VR rendering", e); // releases everything, VR Mode included; `vr` is gone now
        return;
    }
    vr.session->endFrame(time, rendered);
    if (rendered) {
        requestUpdate(); // the desktop mirror follows the headset
    }

    if (vr.perf.on) {
        VrPerf& perf = vr.perf;
        const double workMs = static_cast<double>(perf.clock.nsecsElapsed() - workStart) * 1.0e-6;
        ++perf.frames;
        perf.rendered += rendered ? 1 : 0;
        perf.workMs += workMs;
        perf.workMaxMs = std::max(perf.workMaxMs, workMs);
        perf.period = period;
        if (perf.lastDisplayTime > 0 && period > 0 && time - perf.lastDisplayTime > period + period / 2) {
            ++perf.late;
        }
        perf.lastDisplayTime = time;
        const qint64 now = perf.clock.nsecsElapsed();
        if (now - perf.windowStart >= 1'000'000'000) {
            qInfo().noquote() << QStringLiteral("[vr perf] %1 frames/s, %2 rendered, %3 late; display period %4 ms "
                                                "(%5 Hz); GUI work per frame mean %6 ms, max %7 ms")
                                     .arg(perf.frames)
                                     .arg(perf.rendered)
                                     .arg(perf.late)
                                     .arg(perf.period * 1.0e-6, 0, 'f', 2)
                                     .arg(perf.period > 0 ? 1.0e9 / static_cast<double>(perf.period) : 0.0, 0, 'f', 1)
                                     .arg(perf.frames > 0 ? perf.workMs / perf.frames : 0.0, 0, 'f', 2)
                                     .arg(perf.workMaxMs, 0, 'f', 2);
            perf.windowStart = now;
            perf.frames = perf.rendered = perf.late = 0;
            perf.workMs = perf.workMaxMs = 0.0;
        }
    }
}

void VulkanWindow::endVrDrag() {
    if (!m_vr || m_vr->dragHand < 0) {
        return;
    }
    m_vr->dragHand = -1;
    if (!m_ik.dragging) {
        return;
    }
    // The mouse release's two outcomes (mouseReleaseEvent): a press that never moved the pose
    // was a CLICK — the selection stays, no settle, no undo entry; a drag hands off to the
    // animated settle, which commits the undo entry when it lands.
    if (!m_ik.poseChanged) {
        abortIkDrag();
    } else {
        beginIkRelease();
    }
    requestUpdate();
}

void VulkanWindow::applyVrInput(VrState& vr, const VrInput& input, float dt, int64_t time) {
    // --- The buttons: the desktop actions a posing session in the headset needs. -------------
    // (Each is refused or no-ops where its desktop twin would be: undo/redo close open edits
    // and are left alone while a drag owns the pose; ground and pin do their own guarding.)
    if (input.recenterPressed) {
        vr.needsRecenter = true;
    }
    if (!dragInFlight()) {
        if (input.undoPressed) {
            undo();
        }
        if (input.redoPressed) {
            redo();
        }
        if (input.groundPressed) {
            groundFigure();
        }
    }
    if (input.pinPressed) {
        togglePinSelectedJoint(); // P: one undoable pose edit; refused while a drag owns the pose
    }

    // --- Locomotion: the left stick walks where the HEAD faces (forward/back, strafe), the
    // right stick turns about the head — both as moves of the play space, so the room's
    // tracking keeps working underneath. -----------------------------------------------------
    glm::vec3  head(0.0f);
    glm::quat  facing(1.0f, 0.0f, 0.0f, 0.0f);
    const bool headTracked = vr.session->headPose(time, head, facing);
    const float moveAmount = glm::length(input.move);
    if (headTracked && moveAmount > kStickDeadzone && dt > 0.0f) {
        glm::vec3 forward = facing * glm::vec3(0.0f, 0.0f, -1.0f);
        forward.y = 0.0f;
        forward = glm::length(forward) > 1.0e-4f ? glm::normalize(forward) : glm::vec3(0.0f, 0.0f, -1.0f);
        const glm::vec3 right(-forward.z, 0.0f, forward.x);
        // Past the deadzone the speed ramps with the deflection: a nudge is a creep.
        const float     speed = std::min((moveAmount - kStickDeadzone) / (1.0f - kStickDeadzone), 1.0f);
        const glm::vec2 dir = input.move / moveAmount;
        const glm::vec3 step = (forward * dir.y + right * dir.x) * (speed * kMoveSpeed * dt); // stage space
        vr.worldFromStage = vr.worldFromStage * glm::translate(glm::mat4(1.0f), step);
    }
    const float turn = input.turn.x;
    if (headTracked && std::abs(turn) > kStickDeadzone && dt > 0.0f) {
        // Smooth turning, the rate following the deflection past the deadzone. A push to the
        // right turns the user right: their forward swings clockwise seen from above, a NEGATIVE
        // rotation about +Y. The pivot is the head's floor point, so the user turns on the spot
        // rather than orbiting the play space's origin.
        const float     rate = std::min((std::abs(turn) - kStickDeadzone) / (1.0f - kStickDeadzone), 1.0f);
        const float     angle = -glm::radians(kTurnSpeedDegrees) * rate * dt * (turn > 0.0f ? 1.0f : -1.0f);
        const glm::mat4 toPivot = glm::translate(glm::mat4(1.0f), glm::vec3(head.x, 0.0f, head.z));
        const glm::mat4 fromPivot = glm::translate(glm::mat4(1.0f), glm::vec3(-head.x, 0.0f, -head.z));
        vr.worldFromStage = vr.worldFromStage * toPivot *
                            glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 1.0f, 0.0f)) * fromPivot;
    }

    // --- The pointers: each hand's aim ray, in the world. --------------------------------------
    std::vector<PointerLine> lines;
    for (int h = 0; h < 2; ++h) {
        const VrHandInput& hand = input.hand[h];
        const bool pressed = hand.select >= kTriggerOn || (vr.selectHeld[h] && hand.select > kTriggerOff);
        const bool pressEdge = pressed && !vr.selectHeld[h];
        const bool releaseEdge = !pressed && vr.selectHeld[h];
        vr.selectHeld[h] = pressed;

        if (!hand.aimValid) {
            vr.prevAimValid[h] = false;
            if (releaseEdge && vr.dragHand == h) {
                endVrDrag(); // a release with the controller untracked still lets go
            }
            continue;
        }
        const glm::vec3 origin = glm::vec3(vr.worldFromStage * glm::vec4(hand.aimPosition, 1.0f));
        const glm::quat handRotation = glm::quat_cast(glm::mat3(vr.worldFromStage)) * hand.aimOrientation; // world
        glm::vec3 direction = handRotation * glm::vec3(0.0f, 0.0f, -1.0f);
        direction = glm::length(direction) > 1.0e-6f ? glm::normalize(direction) : glm::vec3(0.0f, 0.0f, -1.0f);
        const Camera pickCamera = pointerPickCamera(origin, direction);
        const float  centre = 0.5f * kPickViewportPx;
        // The controller's roll since the previous frame, about its own pointing axis (stage
        // space, both poses): the swing-twist split of the frame-to-frame rotation.
        float rollDegrees = 0.0f;
        if (vr.prevAimValid[h]) {
            const glm::quat delta = hand.aimOrientation * glm::inverse(vr.prevAim[h]);
            const glm::vec3 axis = hand.aimOrientation * glm::vec3(0.0f, 0.0f, -1.0f);
            const float     along = glm::dot(glm::vec3(delta.x, delta.y, delta.z), axis);
            glm::quat       twist = glm::quat(delta.w, axis * along);
            if (glm::length(twist) > 1.0e-6f) {
                twist = glm::normalize(twist);
                if (twist.w < 0.0f) {
                    twist = -twist; // the same rotation, the short way round
                }
                const float angle = 2.0f * std::atan2(glm::length(glm::vec3(twist.x, twist.y, twist.z)), twist.w);
                rollDegrees = glm::degrees(angle) * (along >= 0.0f ? 1.0f : -1.0f);
            }
        }
        vr.prevAim[h] = hand.aimOrientation;
        vr.prevAimValid[h] = true;

        // Hover: the joint the ray is on, with no selection made — through the CHEAP 2D pick
        // (the joints and bone bodies projected, Scene::setPick2D): the exact skin cast the
        // trigger's press uses re-skins the whole figure on the CPU, too much for two hands at
        // the headset's rate, and a hover cue needs only the nearby joint. Skipped mid-drag.
        int       hoverModel = -1;
        int       hoverBone = -1;
        glm::vec3 hoverPoint(0.0f);
        bool      hovering = false;
        if (!m_ik.dragging && !m_ik.settling) {
            scene().setPick2D(true);
            hovering = scene().pickBoneAt(centre, centre, kPickViewportPx, kPickViewportPx, pickCamera, hoverModel,
                                          hoverBone, hoverPoint);
            scene().setPick2D(false);
        }

        // The trigger goes down: the left mouse button's press. A joint under the ray is selected
        // and its full-body-IK drag begins (the grabbed point then rides the ray at the distance
        // it was grabbed at); empty space click-selects the model under the ray or clears the
        // selection. A gesture already in flight (the other hand's, the mouse's, a settle) is
        // left alone.
        if (pressEdge && !dragInFlight()) {
            closeSettlingEdits(); // a settle still landing, a fall, an open dial edit: closed, as for a mouse press
            // The joint under the ray — or, with nothing on the ray, the joint within reach of the
            // hand itself (a HAND GRAB: taking hold rather than pointing).
            int  bone = scene().selectBoneAt(centre, centre, kPickViewportPx, kPickViewportPx, pickCamera);
            bool byHand = false;
            if (bone < 0) {
                bone = scene().selectNearestBone(origin, kHandGrabRadius);
                byHand = bone >= 0;
            }
            if (bone >= 0 && beginIkDrag(false)) {
                vr.dragHand = h;
                // Where the grabbed point rides, in the hand's frame: down the ray at the grab
                // distance (a ray grab), or wherever the joint is relative to the hand (a hand grab).
                vr.grabOffset = byHand ? glm::inverse(handRotation) * (m_ik.planePoint - origin)
                                       : glm::vec3(0.0f, 0.0f, -std::max(0.05f, glm::dot(m_ik.planePoint - origin, direction)));
                m_preEditPose = scene().capturePose(); // the undo snapshot (committed when the settle lands)
                vr.session->hapticPulse(h, 0.03f, 0.6f); // the grab, felt
            } else {
                Ray ray;
                ray.origin = origin;
                ray.direction = direction;
                const int picked = scene().pickModel(ray);
                if (picked != scene().selectedModelIndex()) {
                    scene().setSelectedModel(picked);
                }
            }
            requestUpdate(); // the selection (outline / highlight) changed
        }
        // Held: the grabbed point goes where the hand now carries it (the 60 Hz tick solves
        // toward it, as for the mouse); the right stick's Y pushes / pulls it along the ray; and
        // the controller's ROLL turns the joint's TWIST dial by the same angle (the Transform
        // tab's Twist, the B/S/T mode's T) — a wrist turn twists the limb.
        if (m_ik.dragging && vr.dragHand == h) {
            if (std::abs(input.turn.y) > kStickDeadzone && dt > 0.0f) {
                vr.grabOffset += glm::vec3(0.0f, 0.0f, -input.turn.y * kDragDepthSpeed * dt);
                if (glm::length(vr.grabOffset) < 0.05f) {
                    vr.grabOffset = glm::vec3(0.0f, 0.0f, -0.05f); // never pulled into the hand
                }
            }
            setIkTarget(origin + handRotation * vr.grabOffset);
            if (std::abs(rollDegrees) > 0.05f) {
                if (const Armature* armature = scene().figureArmature()) {
                    const JointDial twistDial = armature->jointDial(armature->highlightBone(), JointDialKind::Twist);
                    if (twistDial.valid() && std::abs(twistDial.fullDeg) > 1.0e-3f) {
                        const float units = rollDegrees * 100.0f / std::abs(twistDial.fullDeg);
                        scene().setSelectedJointDial(static_cast<int>(JointDialKind::Twist),
                                                     armature->jointDialValue(twistDial) + units);
                        m_ik.poseChanged = true; // a twist is an edit: the release settles + commits
                    }
                }
            }
        }
        if (releaseEdge && vr.dragHand == h) {
            endVrDrag();
        }

        // The controller itself: a stub back along the ray and an up tick, so the hand reads as
        // a hand and the ray's origin is never in doubt.
        {
            PointerLine stub;
            stub.from = origin - direction * kGlyphStubLength;
            stub.to = origin;
            stub.color = kPointerIdle * 0.7f;
            lines.push_back(stub);
            PointerLine tick;
            tick.from = origin;
            tick.to = origin + (handRotation * glm::vec3(0.0f, 1.0f, 0.0f)) * kGlyphTickLength;
            tick.color = kPointerIdle * 0.7f;
            lines.push_back(tick);
        }

        // The ray as drawn: to the carried point, to the hovered joint, or to the floor / 3 m out.
        PointerLine line;
        line.from = origin;
        if (vr.dragHand == h) {
            line.to = origin + handRotation * vr.grabOffset;
            line.color = kPointerDrag;
            line.marker = true;
        } else if (hovering) {
            line.to = hoverPoint;
            line.color = kPointerHover;
            line.marker = true;
        } else {
            float reach = kPointerLength;
            const bool floorHit = direction.y < -1.0e-4f && -origin.y / direction.y < kPointerLength;
            if (floorHit) {
                reach = -origin.y / direction.y;
            }
            line.to = origin + direction * reach;
            line.color = kPointerIdle;
            line.marker = floorHit;
        }
        lines.push_back(line);
    }
    scene().setVrPointerLines(std::move(lines));
}

} // namespace pose
