/**
 * @file vulkanwindow_ik.cpp
 * @brief VulkanWindow's full-body-IK drag loop and the animated ground fall — the interaction
 *        state machines that run on timers.
 *
 * One of the seven translation units of VulkanWindow — see vulkanwindow.h for the map. The
 * IK drag is the viewport's posing gesture and its MOTION is engineered, not just its end pose
 * (CLAUDE.md, the FBIK section): the mouse only records a raw target; the 60 Hz timer is the
 * SOLE solve driver, low-pass filtering that target (IkCursorFilter, shared with the harness so
 * the app and the tests run the identical loop) and issuing it once per tick; the release hands
 * off to an ANIMATED settle on the same timer; and everything that must not overlap a settle or
 * a fall lands it first (finishIkSettle / finishGroundFall). The Model/Armature side of the loop
 * lives in scene/meshik.cpp — this file only drives it at the viewport's tick order.
 */

#include "vulkanwindow.h"

#include "ikdiagnostics.h"
#include "ikscript.h"
#include "rendering/vulkanrenderer.h"
#include "scene/scene.h"

#include <QTimer>

#include <algorithm>
#include <cmath>

namespace pose {

void VulkanWindow::onIkTick() {
    if (!m_renderer) {
        m_ikTimer->stop();
        m_ik.settling = false;
        return;
    }
    const qint64 tickStart = m_diag ? m_diag->tickBegin() : 0;
    if (m_diag && m_diag->benchActive && m_ik.dragging) {
        benchAdvance(); // the scripted cursor: moves m_ik.lastTarget along the bench path
    }
    if (m_script && m_ik.dragging) {
        scriptDragTick(); // the scripted test's cursor (vulkanwindow_script.cpp)
    }
    if (m_ik.settling) {
        // Animated release settle: one capped round per tick until the feet land (the landing
        // tick's frame is requested by finishIkSettle itself).
        if (scene().settleBoneIkTick()) {
            requestUpdate();
        } else {
            finishIkSettle();
            if (m_diag && m_diag->benchActive) {
                benchFinish();
            }
            if (m_script) {
                scriptSettled();
            }
        }
    } else if (m_ik.dragging && m_ik.hasTarget) {
        // Redraw only when the solve actually moved the pose: under a still cursor it converges
        // to the same pose every tick, and re-rendering an unchanged frame at 60 Hz would burn
        // GPU/battery for nothing (rendering is event-driven everywhere else too).
        if (issueIkTarget()) {
            m_ik.poseChanged = true; // a real edit: the release may settle + commit undo
            requestUpdate();
        }
    }
    if (m_diag) {
        glm::vec3  effector;
        const bool live = m_ik.dragging && m_ik.hasTarget && scene().ikGrabPointWorld(effector);
        m_diag->tickEnd(tickStart, live ? &effector : nullptr, m_ik.lastTarget);
    }
}

bool VulkanWindow::issueIkTarget() {
    // The raw cursor target goes through the adaptive low-pass (IkCursorFilter, shared with
    // the IK harness so the app and the tests run the identical loop) before the solve.
    return scene().dragBoneIkTo(m_ik.filter.update(m_ik.lastTarget));
}

bool VulkanWindow::beginJointGesture(const QPointF& localPos, bool scoped) {
    if (boneAt(localPos) < 0) {
        return false;
    }
    // IK: drag the grabbed point through a camera-parallel plane anchored at it — the whole body
    // following (a plain press), or with Ctrl the grabbed CHAIN alone (a limb up to the body, the
    // head and neck to the chest, the spine over a still pelvis, the hips with only their legs),
    // nothing else moving. (Until 2026-09-26 Ctrl+drag was the single-joint FK rotate; turning
    // one joint is the Transform dials' job, and the B / S / T mouse mode's.)
    beginIkDrag(scoped);
    if (dragInFlight()) {
        m_preEditPose = scene().capturePose(); // snapshot for undo (committed on release)
    }
    return true;
}

bool VulkanWindow::beginGroupGesture(const QPointF& localPos) {
    if (!m_renderer) {
        return false;
    }
    bool      wasMember = false;
    const int bone = scene().shiftPressBoneAt(static_cast<float>(localPos.x()), static_cast<float>(localPos.y()),
                                              static_cast<float>(width()), static_cast<float>(height()),
                                              m_renderer->camera(), wasMember);
    if (bone < 0) {
        return false;
    }
    // THE GROUP: the pressed joint is the primary, the rest ride with it (Armature::beginIkDrag).
    // Released without a move it is a Shift+CLICK: a new joint stays added, a member is removed
    // (mouseReleaseEvent, through m_ik.groupClickBone).
    m_ik.groupClickBone = wasMember ? bone : -1;
    beginIkDrag(false);
    if (dragInFlight()) {
        m_preEditPose = scene().capturePose();
    } else if (wasMember) {
        scene().removeGroupBone(bone); // (no drag could begin: the click's removal, now)
        m_ik.groupClickBone = -1;
    }
    return true;
}

bool VulkanWindow::beginIkDrag(bool scoped) {
    // (The plane passes through the point of the bone the user took hold of — the joint itself
    // for a click on the joint — and the targets are where that point should go.)
    // (Ctrl asks for IkScope::Part, which the armature resolves by what was grabbed: the body
    // itself -> the whole figure moved as she is posed, off the floor's contacts; a limb, the head
    // -> its chain alone; a finger or a toe -> its digit.)
    m_ik.dragging = scene().beginBoneIkDrag(scoped ? IkScope::Part : IkScope::Body) &&
                    scene().ikGrabPointWorld(m_ik.planePoint);
    if (m_ik.dragging) {
        m_ik.hasTarget = false;
        m_ik.poseChanged = false;
        if (m_diag) {
            m_diag->dragBegan();
        }
        m_ikTimer->start();
    }
    return m_ik.dragging;
}

void VulkanWindow::beginIkRelease() {
    m_ik.hasTarget = false;
    m_ik.dragging = false;
    m_ik.settling = true; // the timer keeps ticking through the animated settle
}

void VulkanWindow::abortIkDrag() {
    if (!m_ik.dragging) {
        return;
    }
    scene().endBoneIkDrag();
    m_ik.dragging = false;
    m_ik.hasTarget = false;
    m_ikTimer->stop();
    if (m_ik.poseChanged) {
        // The drag DID move the pose and no settle will run: the edit stays, so it must be
        // undoable (a stale drag used to lose its entry).
        m_ik.poseChanged = false;
        scene().finalizePose();
        commitPoseUndo();
    }
}

void VulkanWindow::finishIkSettle() {
    if (!m_ik.settling) {
        return;
    }
    m_ik.settling = false;
    m_ikTimer->stop();
    if (m_renderer) {
        scene().endBoneIkDrag();
        scene().finalizePose(); // the settled-pose hook, deferred through drag AND settle
        commitPoseUndo();
        requestUpdate(); // the landed pose must show — several callers reach here on paths with
                         // no request of their own
    }
}

bool VulkanWindow::dragPlaneHit(const QPointF& localPos, glm::vec3& outWorld) const {
    if (!m_renderer) {
        return false; // (a drag implies a renderer; the guard keeps the invariant explicit)
    }
    const glm::mat4 view = m_renderer->camera().view();
    const glm::vec3 planeNormal(view[0][2], view[1][2], view[2][2]); // toward the camera
    const Ray       ray = cursorRay(localPos);
    const float     denom = glm::dot(ray.direction, planeNormal);
    if (std::abs(denom) <= 1e-4f) {
        return false; // the ray runs along the plane
    }
    const float t = glm::dot(m_ik.planePoint - ray.origin, planeNormal) / denom;
    if (t <= 0.0f) {
        return false; // the plane is behind the camera
    }
    outWorld = ray.origin + ray.direction * t;
    return true;
}

void VulkanWindow::setIkTarget(const glm::vec3& target) {
    m_ik.lastTarget = target;
    if (!m_ik.hasTarget) {
        m_ik.filter.seed(m_ik.planePoint); // seed the filter at the grab point
        m_ik.hasTarget = true;
    }
}

bool VulkanWindow::stepIkDepth(float notches, const QPointF& cursorPos) {
    if (!m_renderer) {
        return false;
    }
    Camera&         camera = m_renderer->camera();
    const glm::mat4 view = camera.view();
    const glm::vec3 towardCamera(view[0][2], view[1][2], view[2][2]);
    const float     distance = glm::dot(m_ik.planePoint - camera.position(), -towardCamera);
    // The plane may never come closer than 10cm to the camera: pulled through (or behind) it,
    // the cursor ray could no longer hit it and the drag would go dead. The step is scaled by
    // the CURRENT distance (floored at the same 10cm so a plane already at the limit can still
    // be pushed away), and a pull is clamped so the plane stops exactly at the limit.
    constexpr float kMinPlaneDistance = 0.10f;
    float push = notches * kIkDepthPerWheelNotch * std::max(distance, kMinPlaneDistance);
    push = std::max(push, kMinPlaneDistance - distance);
    m_ik.planePoint -= towardCamera * push;
    glm::vec3 target;
    if (!dragPlaneHit(cursorPos, target)) {
        return false;
    }
    setIkTarget(target); // a wheel before any move seeds the filter at the grab point, like a move
    return true;
}

// --- Ground fall -------------------------------------------------------------------------------

void VulkanWindow::groundFigure() {
    if (!m_renderer || m_ik.dragging || m_fall.timer->isActive()) {
        return; // no renderer yet, a drag owns the pose, or a fall is already in flight
    }
    finishIkSettle(); // ground the LANDED pose, not a transient mid-settle frame
    float lowestY = 0.0f;
    if (!scene().figureGroundGap(lowestY) || std::abs(lowestY) < 1e-4f) {
        return; // nothing to ground, or already resting on the floor
    }
    const int figure = scene().activeFigureIndex(); // the figure figureGroundGap measured
    // The drop goes into her POSE (the root's pose translation: Armature::shiftPoseY), so grounding
    // is a pose edit: one undo step, committed when she has landed (and her landing's bounce is
    // over) — or at once, below.
    m_preEditPose = scene().capturePose();
    if (lowestY < 0.0f) {
        // Sunk into the floor: nothing falls upward — lift it out in one step.
        scene().translateModelY(figure, -lowestY);
        scene().finalizePose();
        commitPoseUndo();
        requestUpdate();
        return;
    }
    // Hovering: FALL. The timer applies the free-fall curve's increments (onFallTick) to THIS
    // figure — undo can switch the active figure mid-fall, and the drop must still land on
    // the one that started falling.
    m_fall.figure = figure;
    m_fall.height = lowestY;
    m_fall.dropped = 0.0f;
    m_fall.clock.start();
    m_fall.timer->start();
}

void VulkanWindow::onFallTick() {
    if (!m_renderer) {
        m_fall.timer->stop();
        return;
    }
    const float t = static_cast<float>(m_fall.clock.nsecsElapsed()) * 1e-9f;
    if (m_fall.bouncing) {
        // The landing bounce: the pose for this moment, until the bounce is over — then the
        // pose she landed in, exactly.
        if (!scene().landingBounceTick(m_fall.figure, t - m_fall.landedAt)) {
            scene().endLandingBounce(m_fall.figure);
            m_fall.timer->stop();
            m_fall.bouncing = false;
            m_fall.figure = -1;
            commitGroundDrop();
        }
        requestUpdate();
        return;
    }
    if (m_fall.height <= 0.0f) {
        m_fall.timer->stop();
        return;
    }
    constexpr float kGravity = 9.81f; // m/s², world units are metres
    const float dropped = std::min(m_fall.height, 0.5f * kGravity * t * t);
    const float dy = dropped - m_fall.dropped;
    if (dy > 0.0f) {
        scene().translateModelY(m_fall.figure, -dy);
        m_fall.dropped = dropped;
        requestUpdate();
    }
    if (dropped >= m_fall.height) {
        // Landed — at sqrt(2 g h), which is what the legs now absorb if she came down on her
        // feet (the bounce runs on from here, on this timer and clock); else the fall is over.
        const float height = m_fall.height;
        m_fall.height = 0.0f;
        m_fall.dropped = 0.0f;
        if (scene().beginLandingBounce(m_fall.figure, std::sqrt(2.0f * kGravity * height))) {
            m_fall.bouncing = true;
            m_fall.landedAt = std::sqrt(2.0f * height / kGravity);
        } else {
            m_fall.timer->stop();
            m_fall.figure = -1;
            commitGroundDrop();
        }
    }
}

void VulkanWindow::commitGroundDrop() {
    // The drop is in her pose: the settled-pose hook, and one undo entry (against the snapshot
    // groundFigure took; a no-op when nothing moved).
    scene().finalizePose();
    commitPoseUndo();
}

void VulkanWindow::finishGroundFall() {
    if (!m_fall.timer->isActive()) { // (the timer is made in the constructor: never null)
        return;
    }
    m_fall.timer->stop();
    if (m_renderer && m_fall.bouncing) {
        scene().endLandingBounce(m_fall.figure); // (the pose she landed in: the bounce changes nothing)
        requestUpdate();
    } else if (m_renderer && m_fall.height > m_fall.dropped) {
        // (Completed at once, the fall has no landing to absorb: no bounce.)
        scene().translateModelY(m_fall.figure, -(m_fall.height - m_fall.dropped));
        requestUpdate();
    }
    m_fall.height = 0.0f;
    m_fall.dropped = 0.0f;
    m_fall.bouncing = false;
    m_fall.figure = -1;
    if (m_renderer) {
        commitGroundDrop();
    }
}

} // namespace pose
