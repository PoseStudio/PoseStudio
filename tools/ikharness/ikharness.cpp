// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file ikharness.cpp
 * @brief The full-body-IK harness: unit tests for the IK math and constraints, plus a drag-loop
 *        simulator that runs the REAL engine loop (Armature: beginIkDrag -> dragIkTo per tick ->
 *        the settle -> endIkDrag, through the same cursor filter and tick order as the viewport)
 *        on a figure's dumped skeleton and gates the measured behaviour.
 *
 * Every FBIK policy in scene/ik/ and scene/armatureik.cpp traces to a user-visible failure that
 * was first reproduced here; a change to either is not done until this passes. Build target
 * PoseStudioIkHarness (Qt-free, Vulkan-free); `ctest` runs it. The real-skeleton phases need a
 * dump of an imported figure — run the app once with POSESTUDIO_DUMP_SKELETON=<path> and pass
 * that file as the first argument or in POSESTUDIO_IK_SKELETON; without one they are skipped
 * (the synthetic unit tests always run). The dump is a vendor's rig and stays out of the repo.
 *
 *
 * Usage: ikharness [skeleton.skel] [--only <phase-substring>] [--verbose]
 *        ikharness <skeleton.skel> --custom <bone> <dx> <dy> <dz> <moveTicks> <holdTicks> [pin ...]
 *                  [--pose <bone> <x> <y> <z>]... [--hover <m>] [--scale <s>] [--return <ticks>]
 *                  [--before <bone> <dx> <dy> <dz>]... [--to-rest] [--share <0..1>] [--pin-after]
 *
 * The sources split by role: report.{h,cpp} (gates + printing), simulator.{h,cpp} (the drag-loop
 * simulator over the real Armature), unitphases.{h,cpp} (the skeleton-free unit tests),
 * realphases.{h,cpp} (the gated real-figure phases), and this file (argument parsing, the
 * --custom ad-hoc scenario, main).
 */

#include "armature.h"
#include "realphases.h"
#include "report.h"
#include "simulator.h"
#include "unitphases.h"

#include <glm/glm.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace pose;
using namespace ikharness;

// --custom <bone> <dx> <dy> <dz> <moveTicks> <holdTicks> [pin ...]: one ad-hoc scenario, fully
// reported (verbose), for investigating a behaviour before it becomes a gated phase.
struct CustomSpec {
    bool     set = false;
    Scenario scenario;
};

int main(int argc, char** argv) {
    // Unbuffered, so the harness's per-tick rows and the rig's stderr events interleave in order.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Report report;
    std::string skeletonPath;
    CustomSpec custom;
    int customReturnTicks = 0; // --return <ticks>: bring the cursor back to the start, then hold
    if (const char* env = std::getenv("POSESTUDIO_IK_SKELETON")) {
        skeletonPath = env;
    }
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0 || std::strcmp(argv[i], "-v") == 0) {
            report.verbose = true;
        } else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            report.only = argv[++i];
        } else if (std::strcmp(argv[i], "--custom") == 0 && i + 6 < argc) {
            custom.set = true;
            custom.scenario.grab = argv[i + 1];
            const glm::vec3 offset(static_cast<float>(std::atof(argv[i + 2])),
                                   static_cast<float>(std::atof(argv[i + 3])),
                                   static_cast<float>(std::atof(argv[i + 4])));
            custom.scenario.path = pullPath(offset, std::atoi(argv[i + 5]), std::atoi(argv[i + 6]));
            i += 6;
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                custom.scenario.userPins.emplace_back(argv[++i]);
            }
            report.only = "custom";
            report.verbose = true;
        } else if (std::strcmp(argv[i], "--pose") == 0 && i + 4 < argc) {
            // --pose <bone> <x> <y> <z>: an FK pre-pose (Euler degrees) for --custom; repeatable.
            custom.scenario.prePose.emplace_back(
                argv[i + 1], glm::vec3(static_cast<float>(std::atof(argv[i + 2])),
                                       static_cast<float>(std::atof(argv[i + 3])),
                                       static_cast<float>(std::atof(argv[i + 4]))));
            i += 4;
        } else if (std::strcmp(argv[i], "--before") == 0 && i + 4 < argc) {
            // --before <bone> <dx> <dy> <dz>: a drag made and RELEASED before the custom one
            // (repeatable, in order) — the custom drag starts from the pose it left.
            PreludeDrag pre;
            pre.grab = argv[i + 1];
            pre.offset = glm::vec3(static_cast<float>(std::atof(argv[i + 2])),
                                   static_cast<float>(std::atof(argv[i + 3])),
                                   static_cast<float>(std::atof(argv[i + 4])));
            custom.scenario.before.push_back(pre);
            i += 4;
        } else if (std::strcmp(argv[i], "--share") == 0 && i + 1 < argc) {
            // --share <0..1>: grab the bone's BODY, that share of the way along its rigid limb
            // segment (what a click on the flesh takes hold of), instead of its joint.
            custom.scenario.grabShare = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(argv[i], "--pin-after") == 0) {
            // --pin-after: the pins go on after the --before drags ("sit her down, then pin the hips").
            custom.scenario.pinsAfterBefore = true;
        } else if (std::strcmp(argv[i], "--to-rest") == 0) {
            // --to-rest: the custom drag takes the joint back to where it sat before the --before
            // drags (the given offset only says "move"; its ticks are kept).
            custom.scenario.toRest = true;
        } else if (std::strcmp(argv[i], "--return") == 0 && i + 1 < argc) {
            // --return <ticks>: after the hold, the cursor returns to where the drag began over
            // this many ticks and holds there 60 more — a there-and-back gesture.
            customReturnTicks = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--hover") == 0 && i + 1 < argc) {
            custom.scenario.hover = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            custom.scenario.scale = static_cast<float>(std::atof(argv[++i]));
        } else {
            skeletonPath = argv[i];
        }
    }

    if (custom.set && customReturnTicks > 0 && !custom.scenario.path.empty()) {
        const int last = custom.scenario.path.back().tick;
        custom.scenario.path.push_back({last + customReturnTicks, glm::vec3(0.0f), "return"});
        custom.scenario.path.push_back({last + customReturnTicks + 60, glm::vec3(0.0f), "hold-return"});
    }
    if (report.only.empty() || report.only.find("unit") != std::string::npos) {
        testEulerRoundTrip(report);
        testSkeletonGraph(report);
        testBalance(report);
        testZeroEulerIsBind(report);
        testJointSolver(report);
    }

    std::vector<ArmatureBone> bones;
    if (skeletonPath.empty()) {
        report.skip("[real] figure phases", "no skeleton dump: pass a .skel path or set POSESTUDIO_IK_SKELETON");
    } else if (!Armature::loadDump(skeletonPath, bones) || bones.empty()) {
        report.skip("[real] figure phases", ("could not read " + skeletonPath).c_str());
        ++report.failedPhases;
    } else {
        std::printf("[info] skeleton %s: %zu bones\n", skeletonPath.c_str(), bones.size());
        if (loadBodyMesh(skeletonPath + ".mesh", g_bodyMesh)) {
            std::printf("[info] body mesh %s.mesh: %zu points (the volumes are fitted to it)\n",
                        skeletonPath.c_str(), g_bodyMesh.size());
        } else {
            std::printf("[info] no body mesh sidecar (%s.mesh): skeleton-sized volumes\n", skeletonPath.c_str());
        }
        if (custom.set) {
            const RunResult r = runScenario(bones, custom.scenario, true);
            std::vector<Gate> gates{info("grab start y (m)", r.grabStart.y),
                                    info("grab start z (m)", r.grabStart.z),
                                    info("hip drop at release (m)", r.hipDropAtRelease),
                                    info("hip displacement (m)", r.hipDisp),
                                    info("head displacement (m)", r.headDisp),
                                    info("chest displacement (m)", r.chestDisp),
                                    info("grab rise (m)", r.grabEnd.y - r.grabStart.y),
                                    info("body-volume penetration, drag+settle (mm)", r.volumePenetrationMax * 1000.0),
                                    info("body-volume penetration at the end (mm)", r.volumePenetrationEnd * 1000.0),
                                    info("grab-to-cursor at rest (mm)", r.restingMiss * 1000.0),
                                    info("contact drift, drag (mm)", r.contactDriftMaxDrag * 1000.0),
                                    info("contact drift, after (mm)", r.contactDriftMax * 1000.0),
                                    info("lowest contact vs floor (mm)", (r.contactMinY - r.contactBindMinY) * 1000.0),
                                    info("heel lift max (mm)", r.contactRiseMax * 1000.0),
                                    info("floor penetration (mm)", r.penetrationMax * 1000.0),
                                    info("lowest joint vs its bind height (mm)", r.minJointY * 1000.0),
                                    info("feet min y at the end (m)", r.feetMinY),
                                    info("steps", r.stepsTaken),
                                    info("suspended", r.suspended ? 1.0 : 0.0),
                                    info("user pin max distance (mm)", r.userPinDistMax * 1000.0),
                                    info("pins max / at mouse-up", r.pinsMax * 100 + r.pinsAtRelease),
                                    info("live pins max", r.livePinsMax),
                                    info("live pin slide (mm)", r.livePinSlideMax * 1000.0)};
            if (!r.ok) {
                gates.push_back(gateMin(("error: " + r.error).c_str(), 0.0, 1.0));
            }
            for (const auto& d : r.topEulerDeltas) {
                gates.push_back(info(("largest channel move: " + d.first + " (deg)").c_str(), d.second));
            }
            // Where a few named joints ended up at mouse-up (world height) — the floor checks.
            for (const char* name : {"lHand", "rHand", "head", "lShin", "rShin"}) {
                for (std::size_t i = 0; i < bones.size() && i < r.dragEndPos.size(); ++i) {
                    if (bones[i].name == name) {
                        gates.push_back(info((std::string(name) + " y at mouse-up (m)").c_str(),
                                             r.dragEndPos[i].y));
                    }
                }
            }
            if (r.penetrationNode >= 0) {
                std::printf("         worst floor penetration: %s\n",
                            bones[static_cast<std::size_t>(r.penetrationNode)].name.c_str());
            }
            report.phase("[custom] " + custom.scenario.grab, gates);
        } else {
            realPhases(report, bones);
        }
    }

    std::printf("\n%d phase(s): %d failed, %d skipped\n", report.phases, report.failedPhases,
                report.skippedPhases);
    return report.failedPhases == 0 ? 0 : 1;
}
