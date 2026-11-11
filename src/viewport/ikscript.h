/**
 * @file ikscript.h
 * @brief IkScript — the state of the scripted in-app posing test (POSESTUDIO_IK_SCRIPT=<file>),
 *        kept out of VulkanWindow's header.
 *
 * Developer tooling, like IkDiagnostics: VulkanWindow owns one through a unique_ptr that is null
 * unless the environment asks for it. The script drives the REAL viewport — the picker at a
 * pixel, the press path, the camera-parallel drag plane, the 60 Hz tick, the solve, the release
 * settle, render and present — with NO desktop input (nothing is injected into the OS: the
 * window's own gesture functions are called with scripted cursor positions), reports what a
 * user would see as numbers, and saves rendered frames as PNGs read back from the GPU
 * (VulkanRenderer::requestCapture), so a covered window captures as well as a visible one.
 * The runner lives in vulkanwindow_script.cpp; tools/ikscripts/README.md is the command reference.
 */

#ifndef IKSCRIPT_H
#define IKSCRIPT_H

#include <QPointF>
#include <QString>
#include <QStringList>

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace pose {

struct IkScript {
    /// Null unless POSESTUDIO_IK_SCRIPT names a readable file.
    static std::unique_ptr<IkScript> fromEnvironment();

    QStringList lines;      ///< The script, one command a line (# comments and blanks dropped).
    int         next = 0;   ///< The next line to run.
    QString     outDir;     ///< Where shots and the report go (POSESTUDIO_IK_SCRIPT_OUT, else the script's folder).
    int         retries = 0;
    bool        materialLinked = true; ///< The Materials tab's "Edit matching zones together" for `material` (matlink on|off).
    int         waitTicks = 0;         ///< The ticks a `wait` has left (scriptWaitTick: each one also advances a fall in flight).

    // --- A drag in flight ------------------------------------------------------------------
    bool      dragging = false;   ///< Between `press` and `release`.
    QPointF   cursorPx;           ///< The scripted cursor now (window pixels).
    glm::vec3 cursorWorld{0.0f};  ///< The world-space cursor now (`dragw` moves it directly; `drag` through the drag plane).
    // The move in progress (a `drag`/`dragw` line): from -> to over moveTicks, then holdTicks.
    bool      moving = false;
    bool      worldMove = false;
    QPointF   fromPx, toPx;
    glm::vec3 fromWorld{0.0f}, toWorld{0.0f};
    int       moveTicks = 0;
    int       holdTicks = 0;
    int       tick = 0;
    // What the drag did, for `report` and `expect`.
    float     jumpMax = 0.0f;                ///< Worst single-tick move of any joint (m).
    int       jumpBone = -1;                 ///< ... which joint, and at which tick of the drag.
    int       jumpTick = 0;
    int       dragTicks = 0;                 ///< Ticks since the press.
    std::vector<glm::vec3> lastPositions;    ///< Joint positions at the previous tick.
    std::vector<glm::vec3> pressPositions;   ///< Joint positions at the press.

    // --- Waiting -------------------------------------------------------------------------------
    bool    awaitingSettle = false; ///< `release`: until the settle finishes.
    bool    awaitingShot = false;   ///< `shot`: until the captured frame arrives.
    QString shotPath;               ///< ... and where the shot goes.

    // --- Expectations (`expect`): a script is a TEST, its exit code the number that failed ------
    int expectations = 0;
    int failures = 0;
};

} // namespace pose

#endif // IKSCRIPT_H
