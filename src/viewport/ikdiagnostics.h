// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file ikdiagnostics.h
 * @brief IK drag-loop diagnostics (POSESTUDIO_IK_PERF=1) and the state of the scripted IK
 *        benchmark (POSESTUDIO_IK_BENCH=<bone>[:depth]).
 *
 * This exists so the 60 Hz drag tick and the frame loop test ONE pointer — VulkanWindow::m_diag,
 * null unless one of those environment variables is set — instead of a scatter of perf/bench
 * members and flags on the window. The measurements, the benchmark's bookkeeping, and the
 * report live here; the DRIVING logic (startBench / benchAdvance / benchDepthNotch /
 * benchFinish, in vulkanwindow_bench.cpp) stays on VulkanWindow because it moves the drag
 * target and reads the camera.
 *
 * The benchmark is the in-app parity reference for the IK harness (tools/ikharness): the report
 * format, the phases at which it prints, and what each statistic counts must not change.
 */

#ifndef IKDIAGNOSTICS_H
#define IKDIAGNOSTICS_H

#include <QElapsedTimer>
#include <QString>

#include <glm/glm.hpp>

#include <algorithm>
#include <memory>

namespace pose {

/**
 * @struct IkDiagnostics
 * @brief Per-drag timing/lag statistics plus the scripted benchmark's cursor-path state.
 *
 * Measures, per report period: the drag timer's tick interval and per-tick CPU work, the frame
 * interval and per-frame draw CPU time while a drag/settle is live, the grabbed joint's lag
 * behind the (virtual) cursor, and its oscillation. The interactive mode prints a report per
 * second of dragging; the benchmark prints one per scripted phase.
 */
struct IkDiagnostics {
    /// Running mean/max of one measurement.
    struct PerfStat {
        double sum = 0.0;
        double max = 0.0;
        int    n = 0;
        void   add(double v) {
            sum += v;
            max = std::max(max, v);
            ++n;
        }
        double mean() const { return n > 0 ? sum / n : 0.0; }
        void   reset() {
            sum = 0.0;
            max = 0.0;
            n = 0;
        }
    };

    /// Null unless POSESTUDIO_IK_PERF or POSESTUDIO_IK_BENCH is set — the hot paths test this
    /// pointer and nothing else.
    static std::unique_ptr<IkDiagnostics> fromEnvironment();

    /// True when POSESTUDIO_IK_BENCH names a bone: the scripted drag runs instead of user input.
    bool benchRequested() const { return !benchSpec.isEmpty(); }

    /// Called at the top of every drag-timer tick: records the interval since the previous tick
    /// and returns this tick's start stamp (ns) for tickEnd.
    qint64 tickBegin();
    /// Called at the end of every tick: records the tick's CPU work and, when @p effector is
    /// non-null (a drag with a target is live), the grabbed joint's lag behind @p target and its
    /// oscillation. Prints the interactive "drag" report every 60 ticks (never while the
    /// benchmark drives — it reports per phase).
    void tickEnd(qint64 tickStart, const glm::vec3* effector, const glm::vec3& target);
    /// Frame hooks (only while a drag or settle is live): the interval since the previous
    /// measured frame, and the draw's CPU time.
    qint64 frameBegin();
    void   frameEnd(qint64 frameStart);
    /// A drag began (interactive or scripted): forget the previous tick/frame stamps, or the
    /// first interval of this drag would measure the idle gap since the last one as a bogus max.
    void dragBegan();
    /// Prints the report line for @p label and resets every statistic.
    void report(const char* label);

    // --- Measurements -----------------------------------------------------------------------
    QElapsedTimer clock;
    qint64        lastTickNs = -1;
    qint64        lastFrameNs = -1;
    PerfStat      tickInterval;
    PerfStat      tickWork;
    PerfStat      frameInterval;
    PerfStat      draw;
    PerfStat      lag;
    // Oscillation of the grabbed joint: the accumulated overlap of consecutive per-tick steps
    // pointing against each other (a smooth track reverses never; trembling reverses every tick).
    double    osc = 0.0;
    glm::vec3 prevEff{0.0f};
    glm::vec3 prevStep{0.0f};
    bool      hasPrevEff = false;
    int       ticks = 0;

    // --- The scripted benchmark ------------------------------------------------------------
    QString   benchSpec;      // "<bone>" or "<bone>:depth" (POSESTUDIO_IK_BENCH)
    bool      benchActive = false;
    int       benchTick = 0;
    int       benchRetries = 0;
    glm::vec3 benchStart{0.0f};
    // Bench ":depth" variant: wheel notches applied through stepIkDepth during the holds (five
    // pushes in hold-1, five pulls in hold-2). The displacement they produce is folded into the
    // scripted path (benchDepthOffset) so the path keeps its own shape on top of the depth.
    bool      benchDepth = false;
    glm::vec3 benchDepthOffset{0.0f};
    float     benchDepthExpected = 0.0f; // Σ expected along-view displacement (m)
    float     benchDepthActual = 0.0f;   // Σ measured along-view displacement (m)
};

} // namespace pose

#endif // IKDIAGNOSTICS_H
