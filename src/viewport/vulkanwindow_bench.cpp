/**
 * @file vulkanwindow_bench.cpp
 * @brief The IK loop diagnostics (POSESTUDIO_IK_PERF=1) and the scripted in-app IK benchmark
 *        (POSESTUDIO_IK_BENCH=<bone>[:depth]) — IkDiagnostics' measurements plus the driving
 *        logic on VulkanWindow.
 *
 * One of the translation units of VulkanWindow — see vulkanwindow.h and ikdiagnostics.h.
 * The benchmark drives the REAL drag loop (the 60 Hz timer, solve, render, present) with a
 * scripted cursor and no desktop input, and its per-phase report is the in-app parity reference
 * for tools/ikharness (`[real] bench path`): the path, the phase boundaries, and the report
 * format are calibration data — change them only together with the harness.
 */

#include "vulkanwindow.h"

#include "ikdiagnostics.h"
#include "rendering/vulkanrenderer.h"
#include "scene/scene.h"

#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace pose {

// --- IkDiagnostics ----------------------------------------------------------------------------

std::unique_ptr<IkDiagnostics> IkDiagnostics::fromEnvironment() {
    const QString bench = qEnvironmentVariable("POSESTUDIO_IK_BENCH");
    if (!qEnvironmentVariableIsSet("POSESTUDIO_IK_PERF") && bench.isEmpty()) {
        return nullptr;
    }
    auto diag = std::make_unique<IkDiagnostics>();
    diag->benchSpec = bench;
    diag->clock.start();
    return diag;
}

qint64 IkDiagnostics::tickBegin() {
    const qint64 tickStart = clock.nsecsElapsed();
    if (lastTickNs >= 0) {
        tickInterval.add(static_cast<double>(tickStart - lastTickNs) * 1e-6);
    }
    lastTickNs = tickStart;
    return tickStart;
}

void IkDiagnostics::tickEnd(qint64 tickStart, const glm::vec3* effector, const glm::vec3& target) {
    tickWork.add(static_cast<double>(clock.nsecsElapsed() - tickStart) * 1e-6);
    if (effector) {
        const glm::vec3 eff = *effector;
        lag.add(static_cast<double>(glm::length(eff - target)) * 1000.0);
        if (hasPrevEff) {
            const glm::vec3 step = eff - prevEff;
            const float     pl = glm::length(prevStep);
            if (pl > 1e-6f && glm::length(step) > 1e-6f) {
                // "osc": how far this step travels AGAINST the previous one — the overlap of
                // two consecutive steps pointing opposite ways, summed over the period. A smooth
                // track contributes nothing; a joint trembling about a point contributes its
                // whole per-tick motion.
                const float against = -glm::dot(step, prevStep) / pl;
                if (against > 0.0f) {
                    osc += static_cast<double>(std::min(against, pl));
                }
            }
            prevStep = step;
        }
        prevEff = eff;
        hasPrevEff = true;
    } else {
        hasPrevEff = false;
    }
    ++ticks;
    if (!benchActive && ticks % 60 == 0) {
        report("drag"); // the interactive case: a report per second of dragging
    }
}

qint64 IkDiagnostics::frameBegin() {
    const qint64 f0 = clock.nsecsElapsed();
    if (lastFrameNs >= 0) {
        frameInterval.add(static_cast<double>(f0 - lastFrameNs) * 1e-6);
    }
    lastFrameNs = f0;
    return f0;
}

void IkDiagnostics::frameEnd(qint64 frameStart) {
    draw.add(static_cast<double>(clock.nsecsElapsed() - frameStart) * 1e-6);
}

void IkDiagnostics::dragBegan() {
    // Not reset in report(): the benchmark reports at phase boundaries mid-drag, and dropping
    // the stamps there would cost every phase its first interval sample ("ticks 59").
    lastTickNs = -1;
    lastFrameNs = -1;
}

void IkDiagnostics::report(const char* label) {
    std::fprintf(stderr,
                 "[ikperf] %-8s ticks %4d | tick interval mean %6.2f max %6.2f ms | tick work mean "
                 "%5.2f max %5.2f ms | frame interval mean %6.2f max %6.2f ms (%d) | draw mean "
                 "%5.2f max %5.2f ms | lag mean %6.1f max %6.1f mm | osc %6.1f mm\n",
                 label, tickInterval.n, tickInterval.mean(), tickInterval.max, tickWork.mean(),
                 tickWork.max, frameInterval.mean(), frameInterval.max, frameInterval.n, draw.mean(),
                 draw.max, lag.mean(), lag.max, osc * 1000.0);
    std::fflush(stderr);
    tickInterval.reset();
    tickWork.reset();
    frameInterval.reset();
    draw.reset();
    lag.reset();
    osc = 0.0;
}

// --- The scripted benchmark (VulkanWindow) ------------------------------------------------------

void VulkanWindow::startBench() {
    IkDiagnostics&    d = *m_diag;
    // "<bone>[@share][:depth]": with @share the bench CLICKS the bone's body — it puts a grab point
    // that share of the way along the bone's limb segment, projects it to the screen and runs
    // the real pick at that pixel (picker, grab point, drag plane: the whole press path, with
    // no desktop input) — instead of selecting the joint by name.
    const QString     boneSpec = d.benchSpec.section(QLatin1Char(':'), 0, 0);
    const std::string bone = boneSpec.section(QLatin1Char('@'), 0, 0).toStdString();
    const QString     shareSpec = boneSpec.section(QLatin1Char('@'), 1, 1);
    int               boneIndex = m_renderer ? scene().selectBoneByName(bone) : -2;
    if (boneIndex == -1 && d.benchRetries++ < 120) {
        // No figure yet: a command-line figure imports through the progress dialog (on Windows
        // the first expose — hence the renderer — arrives synchronously inside show(), so the
        // import runs on the interactive path, not the drain), whose processEvents() fires this
        // timer mid-import. Poll until the figure exists.
        QTimer::singleShot(500, this, &VulkanWindow::startBench);
        return;
    }
    if (boneIndex >= 0 && !shareSpec.isEmpty()) {
        scene().setIkGrabOnSegment(shareSpec.toFloat());
        glm::vec3 point(0.0f);
        QPointF   px;
        if (scene().ikGrabPointWorld(point) && projectToScreen(point, px)) {
            const int picked = boneAt(px); // the real pick: selects, and sets the grab point
            glm::vec3 took(0.0f);
            scene().ikGrabPointWorld(took);
            std::fprintf(stderr,
                         "[ikbench] clicked the body of %s at share %.2f, px(%.0f %.0f): picked bone %d (asked %d), "
                         "took hold %.1f mm from the point aimed at\n",
                         bone.c_str(), shareSpec.toFloat(), px.x(), px.y(), picked, boneIndex,
                         glm::length(took - point) * 1000.0f);
            boneIndex = picked;
        }
    }
    const bool began = boneIndex >= 0 && beginIkDrag();
    if (!began) {
        std::fprintf(stderr, "[ikbench] cannot start: bone '%s' index %d, beginDrag %d\n",
                     bone.c_str(), boneIndex, began ? 1 : 0);
        std::fflush(stderr);
        std::_Exit(2); // diagnostic mode: no teardown
        return;
    }
    std::fprintf(stderr, "[ikbench] dragging %s from (%.3f %.3f %.3f)\n", bone.c_str(),
                 m_ik.planePoint.x, m_ik.planePoint.y, m_ik.planePoint.z);
    {
        // Pick diagnostic: a ray through the grabbed joint's screen position must hit its model.
        QPointF px;
        if (projectToScreen(m_ik.planePoint, px)) {
            const Ray ray = cursorRay(px);
            std::fprintf(stderr,
                         "[ikbench] pick through joint at px(%.0f %.0f): model %d; ray o(%.2f %.2f "
                         "%.2f) d(%.2f %.2f %.2f)\n",
                         px.x(), px.y(), scene().pickModel(ray), ray.origin.x, ray.origin.y,
                         ray.origin.z, ray.direction.x, ray.direction.y, ray.direction.z);
        }
    }
    d.benchStart = m_ik.planePoint;
    d.benchDepth = d.benchSpec.section(QLatin1Char(':'), 1, 1) == QLatin1String("depth");
    d.benchDepthOffset = glm::vec3(0.0f);
    d.benchDepthExpected = 0.0f;
    d.benchDepthActual = 0.0f;
    if (d.benchDepth) {
        std::fprintf(stderr, "[ikbench] depth variant: 5 wheel pushes in hold-1, 5 pulls in hold-2\n");
    }
    // The scripted cursor starts ON the grab point (a real drag's first move seeds the filter
    // there too — setIkTarget).
    setIkTarget(d.benchStart);
    d.benchActive = true;
    d.benchTick = 0;
    d.dragBegan();
    d.report("reset");
}

void VulkanWindow::benchAdvance() {
    IkDiagnostics& d = *m_diag;
    // The scripted cursor path, in ticks: a moderate 30cm gesture (0.3 m/s at 60 Hz), a hold,
    // the return, a hold, a fast 36cm flick (1.2 m/s), a hold, then release.
    struct Waypoint {
        int         tick;
        glm::vec3   offset;
        const char* phase; // reported when this waypoint is reached
    };
    static const Waypoint kPath[] = {
        {0, glm::vec3(0.0f), "start"},
        {60, glm::vec3(0.25f, 0.15f, 0.05f), "move-med"},
        {120, glm::vec3(0.25f, 0.15f, 0.05f), "hold-1"},
        {180, glm::vec3(0.0f), "return"},
        {240, glm::vec3(0.0f), "hold-2"},
        {270, glm::vec3(-0.30f, 0.20f, 0.0f), "move-fast"},
        {330, glm::vec3(-0.30f, 0.20f, 0.0f), "hold-3"},
    };
    constexpr int kCount = static_cast<int>(sizeof(kPath) / sizeof(kPath[0]));
    const int     t = ++d.benchTick;
    for (int i = 1; i < kCount; ++i) {
        if (t == kPath[i].tick) {
            d.report(kPath[i].phase);
        }
        if (t <= kPath[i].tick) {
            const float f = static_cast<float>(t - kPath[i - 1].tick) /
                            static_cast<float>(kPath[i].tick - kPath[i - 1].tick);
            m_ik.lastTarget = d.benchStart + glm::mix(kPath[i - 1].offset, kPath[i].offset, f) +
                              d.benchDepthOffset;
            if (d.benchDepth) {
                // A real drag's target always lies ON the drag plane (it is the cursor ray's
                // intersection with it); the scripted path is a world-space offset, so project
                // it onto the plane along the view axis — the path keeps its on-screen shape and
                // depth comes ONLY from the notches, exactly like a mouse-driven drag. (The plain
                // bench keeps its world-space path untouched: its numbers are the calibrated
                // reference the harness mirrors.)
                const glm::mat4 view = m_renderer->camera().view();
                const glm::vec3 axis(view[0][2], view[1][2], view[2][2]);
                m_ik.lastTarget -= axis * glm::dot(m_ik.lastTarget - m_ik.planePoint, axis);
            }
            if (d.benchDepth && t % 10 == 0) {
                // The ":depth" variant: a wheel notch every 10 ticks through the two holds —
                // pushes away from the camera through hold-1, pulls back through hold-2.
                if (t >= 70 && t <= 110) {
                    benchDepthNotch(+1);
                } else if (t >= 190 && t <= 230) {
                    benchDepthNotch(-1);
                }
            }
            return;
        }
    }
    // Past the last waypoint: release, exactly as mouseReleaseEvent's IK branch does.
    beginIkRelease();
}

void VulkanWindow::benchDepthNotch(int notch) {
    IkDiagnostics& d = *m_diag;
    // Exercise the wheel's depth control exactly as wheelEvent does, with the virtual cursor
    // at the current raw target's screen position, and report what the geometry produced:
    // the drag plane's camera distance before/after, the target's displacement ALONG the view
    // axis (must match notch x kIkDepthPerWheelNotch x distance, sign = away from the camera)
    // and ACROSS it (the cursor ray's obliquity — an off-centre pointer's ray is not parallel to
    // the view axis, so keeping the joint under the pointer moves it slightly sideways too).
    QPointF cursor;
    if (!projectToScreen(m_ik.lastTarget, cursor)) {
        std::fprintf(stderr, "[ikbench] depth notch: target behind the camera, skipped\n");
        return;
    }
    // The scripted path moves the raw target in WORLD space, so unlike a real drag (whose
    // target is always derived from the plane) it can sit off the drag plane. Re-derive it onto
    // the plane first (a zero-notch step) and fold that correction into the path offset, so the
    // notch below measures the depth step alone.
    {
        const glm::vec3 offPlane = m_ik.lastTarget;
        if (stepIkDepth(0.0f, cursor)) {
            d.benchDepthOffset += m_ik.lastTarget - offPlane;
        }
    }
    Camera&         camera = m_renderer->camera();
    const glm::mat4 view = camera.view();
    const glm::vec3 away = -glm::vec3(view[0][2], view[1][2], view[2][2]);
    const float     distBefore = glm::dot(m_ik.planePoint - camera.position(), away);
    const float     expected = static_cast<float>(notch) * kIkDepthPerWheelNotch * distBefore;
    const glm::vec3 before = m_ik.lastTarget;
    if (!stepIkDepth(static_cast<float>(notch), cursor)) {
        std::fprintf(stderr, "[ikbench] depth notch %+d: cursor ray missed the plane\n", notch);
        return;
    }
    const float     distAfter = glm::dot(m_ik.planePoint - camera.position(), away);
    const glm::vec3 moved = m_ik.lastTarget - before;
    const float     along = glm::dot(moved, away);
    const float     across = glm::length(moved - away * along);
    d.benchDepthOffset += moved;
    d.benchDepthExpected += expected;
    d.benchDepthActual += along;
    std::fprintf(stderr,
                 "[ikbench] depth notch %+d at tick %d: plane %.3f -> %.3f m from camera; target moved "
                 "%+.1f mm along the view axis (expected %+.1f), %.1f mm across it\n",
                 notch, d.benchTick, distBefore, distAfter, along * 1000.0f, expected * 1000.0f,
                 across * 1000.0f);
    std::fflush(stderr);
}

void VulkanWindow::benchFinish() {
    IkDiagnostics& d = *m_diag;
    d.report("settle");
    if (d.benchDepth) {
        std::fprintf(stderr,
                     "[ikbench] depth total: %+.1f mm along the view axis (expected %+.1f); net "
                     "offset after pushes+pulls (%.1f %.1f %.1f) mm\n",
                     d.benchDepthActual * 1000.0f, d.benchDepthExpected * 1000.0f,
                     d.benchDepthOffset.x * 1000.0f, d.benchDepthOffset.y * 1000.0f,
                     d.benchDepthOffset.z * 1000.0f);
    }
    std::fprintf(stderr, "[ikbench] done\n");
    std::fflush(stderr);
    d.benchActive = false;
    std::_Exit(0); // diagnostic mode: no teardown
}

} // namespace pose
