// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file unitphases.cpp
 * @brief The unit phases (see unitphases.h): synthetic inputs, exact expectations.
 */

#include "unitphases.h"

#include "armature.h"
#include "balancecontroller.h"
#include "ikmath.h"
#include "jointsolver.h"
#include "report.h"
#include "skeletongraph.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using namespace pose;

namespace ikharness {

void testEulerRoundTrip(Report& report) {
    const char* orders[] = {"XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX"};
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> angle(-170.0f, 170.0f);
    double worst = 0.0;
    double worstGimbal = 0.0;
    for (const char* order : orders) {
        for (int trial = 0; trial < 200; ++trial) {
            const glm::vec3 e(angle(rng), angle(rng), angle(rng));
            const glm::mat4 m = eulerMatrix(e, order);
            const glm::vec3 back = eulerFromMatrix(glm::mat3(m), order);
            const glm::mat4 m2 = eulerMatrix(back, order);
            double err = 0.0;
            for (int c = 0; c < 3; ++c) {
                for (int r = 0; r < 3; ++r) {
                    err = std::max(err, static_cast<double>(std::abs(m[c][r] - m2[c][r])));
                }
            }
            worst = std::max(worst, err);
        }
        // Gimbal lock: the middle-applied angle at ±90 must still reproduce the matrix.
        for (const float mid : {90.0f, -90.0f}) {
            glm::vec3 e(angle(rng), angle(rng), angle(rng));
            e[order[1] == 'X' ? 0 : order[1] == 'Y' ? 1 : 2] = mid;
            const glm::mat4 m = eulerMatrix(e, order);
            const glm::vec3 back = eulerFromMatrix(glm::mat3(m), order);
            const glm::mat4 m2 = eulerMatrix(back, order);
            double err = 0.0;
            for (int c = 0; c < 3; ++c) {
                for (int r = 0; r < 3; ++r) {
                    err = std::max(err, static_cast<double>(std::abs(m[c][r] - m2[c][r])));
                }
            }
            worstGimbal = std::max(worstGimbal, err);
        }
    }
    report.phase("[unit] euler round-trip (6 orders)",
                 {gateMax("worst matrix error", worst, 1e-4),
                  gateMax("worst gimbal-lock error", worstGimbal, 1e-3)});
}

void testSkeletonGraph(Report& report) {
    // 0 -> 1 -> 2 -> 3, 1 -> 4 -> 5 (a Y-shaped tree).
    const std::vector<int> parents{-1, 0, 1, 2, 1, 4};
    SkeletonGraph graph;
    graph.build(parents);
    const bool rootedOk = graph.root() == 0 && graph.parentOf(3) == 2;
    graph.setRoot(5);
    const bool rerooted = graph.parentOf(4) == 5 && graph.parentOf(1) == 4 &&
                          graph.parentOf(0) == 1 && graph.parentOf(2) == 1 &&
                          graph.traversalOrder().front() == 5;
    const std::vector<char> active = graph.markActivePaths({3});
    const bool marks = active[3] && active[2] && active[1] && active[4] && active[5] && !active[0];
    report.phase("[unit] skeleton graph re-rooting",
                 {gateMin("initial rooting", rootedOk ? 1.0 : 0.0, 1.0),
                  gateMin("re-rooted at a leaf", rerooted ? 1.0 : 0.0, 1.0),
                  gateMin("active path marking", marks ? 1.0 : 0.0, 1.0)});
}

void testBalance(Report& report) {
    const std::vector<glm::vec2> pts{{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0.5f, 0.5f}};
    const std::vector<glm::vec2> hull = BalanceController::supportPolygon(pts);
    const bool inside = BalanceController::insidePolygon(hull, glm::vec2(0.5f, 0.5f));
    const bool outside = !BalanceController::insidePolygon(hull, glm::vec2(1.5f, 0.5f));
    const glm::vec2 fixed = BalanceController::closestBalancedPoint(hull, glm::vec2(1.5f, 0.5f), 0.05f);
    report.phase("[unit] support polygon",
                 {gateMax("hull vertex count error", std::abs(static_cast<double>(hull.size()) - 4.0), 0.0),
                  gateMin("inside test", inside ? 1.0 : 0.0, 1.0),
                  gateMin("outside test", outside ? 1.0 : 0.0, 1.0),
                  gateMax("balanced point x error", std::abs(fixed.x - 0.95), 1e-3)});
}

void testZeroEulerIsBind(Report& report) {
    // A three-bone chain with oriented frames: pose a joint, bring it back to exactly zero, and
    // every world matrix must equal the freshly built armature's to the last bit (the frame
    // product OR * I * OR^-1 is the identity only to a rounding, and a solve that starts a few
    // 1e-7 off ends elsewhere: the determinism phase's cause).
    std::vector<ArmatureBone> bones(3);
    bones[0].name = "root";
    bones[1].name = "mid";
    bones[1].parent = 0;
    bones[1].localBindTranslation = glm::vec3(0.1f, 0.4f, 0.02f);
    bones[1].orientation = glm::vec3(23.0f, -71.0f, 8.5f);
    bones[1].rotationOrder = "YZX";
    bones[2].name = "tip";
    bones[2].parent = 1;
    bones[2].localBindTranslation = glm::vec3(-0.03f, 0.35f, 0.01f);
    bones[2].orientation = glm::vec3(-40.0f, 12.0f, 33.0f);
    bones[2].rotationOrder = "ZYX";
    Armature fresh;
    fresh.build(bones);
    Armature posed;
    posed.build(bones);
    posed.setBoneRotation("mid", glm::vec3(31.0f, -17.0f, 42.0f));
    posed.setBoneRotation("tip", glm::vec3(-12.0f, 55.0f, 9.0f));
    posed.setBoneRotation("mid", glm::vec3(0.0f));
    posed.setBoneRotation("tip", glm::vec3(0.0f));
    double worst = 0.0;
    for (std::size_t b = 0; b < bones.size(); ++b) {
        const glm::mat4 a = fresh.poseGlobal(b);
        const glm::mat4 z = posed.poseGlobal(b);
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                worst = std::max(worst, static_cast<double>(std::abs(a[col][row] - z[col][row])));
            }
        }
    }
    report.phase("[unit] a zero Euler composes to the bind exactly",
                 {gateMax("worst world-matrix difference after a pose and back (it read 1e-7 as a frame product)", worst, 0.0)});
}

void testJointSolver(Report& report) {
    // Two limbs off a three-bone trunk, random oriented frames, every rotation order.
    const char* orders[] = {"XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX"};
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    struct Spec { int parent; glm::vec3 offset; };
    const std::vector<Spec> specs = {
        {-1, {0.0f, 1.0f, 0.0f}},                                     // 0 root
        {0, {0.0f, 0.15f, 0.0f}},  {1, {0.0f, 0.15f, 0.01f}},          // 1-2 trunk
        {2, {0.18f, 0.10f, 0.0f}}, {3, {0.28f, 0.0f, 0.0f}}, {4, {0.26f, 0.0f, 0.02f}}, // 3-5 arm
        {0, {0.09f, -0.05f, 0.0f}}, {6, {0.0f, -0.42f, 0.02f}}, {7, {0.0f, -0.40f, -0.02f}}, // 6-8 leg
    };
    std::vector<JointSolverBone> bones(specs.size());
    for (std::size_t i = 0; i < specs.size(); ++i) {
        bones[i].parent = specs[i].parent;
        bones[i].bindOffset = glm::dvec3(specs[i].offset);
        const glm::vec3 axis = glm::normalize(glm::vec3(unit(rng), unit(rng), unit(rng)) + glm::vec3(0.01f));
        bones[i].orient = glm::dmat3(glm::mat3(glm::rotate(glm::mat4(1.0f), unit(rng) * 1.2f, axis)));
        bones[i].rotationOrder = orders[i % 6];
        bones[i].minDeg = glm::dvec3(-120.0);
        bones[i].maxDeg = glm::dvec3(120.0);
        bones[i].limited = glm::bvec3(true, true, true);
    }
    JointSolver solver;
    solver.setSkeleton(bones);

    JointSolver::Pose pose;
    pose.eulerDeg.assign(specs.size(), glm::dvec3(0.0));
    pose.translation.assign(specs.size(), glm::dvec3(0.0));
    for (std::size_t i = 0; i < specs.size(); ++i) {
        pose.eulerDeg[i] = glm::dvec3(unit(rng), unit(rng), unit(rng)) * 40.0;
    }
    pose.translation[0] = glm::dvec3(0.02, -0.03, 0.01);

    // 1. FK parity with the engine's float composition (Armature::recomposePoseLocal).
    JointSolver::Frames frames;
    solver.forwardKinematics(pose, frames);
    double fkError = 0.0;
    {
        std::vector<glm::mat4> global(specs.size());
        for (std::size_t i = 0; i < specs.size(); ++i) {
            const glm::mat4 orient = glm::mat4(glm::mat3(bones[i].orient));
            glm::mat4 local = glm::translate(glm::mat4(1.0f), specs[i].offset) * orient *
                              eulerMatrix(glm::vec3(pose.eulerDeg[i]), bones[i].rotationOrder) *
                              glm::inverse(orient);
            local[3] += glm::vec4(glm::vec3(pose.translation[i]), 0.0f);
            global[i] = specs[i].parent >= 0 ? global[static_cast<std::size_t>(specs[i].parent)] * local : local;
            fkError = std::max(fkError, glm::length(glm::dvec3(glm::vec3(global[i][3])) - frames.pos[i]));
        }
    }

    // 2. The analytic Jacobian against central differences (plain rows: no Huber).
    JointSolver::Problem problem;
    for (int b = 0; b < static_cast<int>(specs.size()); ++b) {
        for (int a = 0; a < 3; ++a) {
            JointSolverDof dof;
            dof.bone = b;
            dof.axis = a;
            dof.stiffness = 0.3;
            dof.reference = pose.eulerDeg[static_cast<std::size_t>(b)][a];
            problem.dofs.push_back(dof);
        }
    }
    for (int a = 0; a < 3; ++a) {
        JointSolverDof dof;
        dof.bone = 0;
        dof.axis = 3 + a;
        dof.stiffness = 20.0;
        dof.reference = pose.translation[0][a];
        problem.dofs.push_back(dof);
    }
    const glm::dvec3 handStart = frames.pos[5];
    const glm::dvec3 footStart = frames.pos[8];
    JointPositionTask hand;
    hand.bone = 5;
    hand.offset = glm::dvec3(0.05, 0.01, 0.0);
    hand.target = handStart + glm::dvec3(0.03, 0.02, -0.01);
    hand.weight = 3.0;
    problem.positions.push_back(hand);
    JointOrientationTask sole;
    sole.bone = 8;
    sole.target = frames.rot[8] * glm::dmat3(glm::mat3(glm::rotate(glm::mat4(1.0f), 1.0e-4f, glm::vec3(0, 0, 1)))); // small: the rotation log's own curvature is not the Jacobian's business
    sole.perAxis = true; // a per-axis hold in a tilted frame
    sole.frame = glm::dmat3(glm::mat3(glm::rotate(glm::mat4(1.0f), 0.4f, glm::normalize(glm::vec3(1.0f, 2.0f, 0.5f)))));
    sole.axisWeights = glm::dvec3(0.5, 3.0, 2.0);
    problem.orientations.push_back(sole);
    JointPlaneTask floor;
    floor.bone = 8;
    floor.planePoint = footStart + glm::dvec3(0.0, 0.01, 0.0); // 1cm above the foot: violated
    floor.weight = 5.0;
    problem.planes.push_back(floor);
    // A plane RIDING the trunk (bone 2), violated by the forearm joint (bone 4): the body-volume
    // row, whose Jacobian must account for the plane's own motion.
    JointPlaneTask chest;
    chest.bone = 4;
    chest.refBone = 2;
    chest.normal = glm::normalize(glm::dvec3(0.3, 0.2, 1.0));
    chest.planePoint = glm::transpose(frames.rot[2]) * (frames.pos[4] - frames.pos[2]) + chest.normal * 0.02;
    chest.weight = 6.0;
    problem.planes.push_back(chest);
    // ... and a joint-space coupling of two channels (the spine's "bends as one" rows).
    if (problem.dofs.size() >= 2) {
        JointCouplingTask coupled;
        coupled.dofA = 0;
        coupled.dofB = 1;
        coupled.scaleA = 1.3;
        coupled.scaleB = 0.7;
        coupled.refA = 0.05;
        coupled.refB = -0.02;
        coupled.weight = 60.0;
        problem.couplings.push_back(coupled);
    }
    const std::vector<float> masses(specs.size(), 1.0f);
    const std::vector<glm::vec2> hull{{-2.0f, -2.0f}, {-1.9f, -2.0f}, {-1.9f, -1.9f}, {-2.0f, -1.9f}}; // far off: active
    problem.masses = &masses;
    problem.supportHull = &hull;
    problem.balanceWeight = 4.0;
    const double jacobianError = solver.jacobianError(pose, problem);
    // ... and the per-axis hold at a LARGE error: a foot in a kneel stands pitched 80 degrees from
    // the orientation it is held to (its pitch is nearly free, its heading hard). The hold's rows
    // were once the rotation vector split into components, exact only for a small error, and this
    // check was run at 0.0001 radians because "the rotation log's curvature is not the Jacobian's
    // business" — it was: that mismatch is what made every kneel crawl and jump.
    JointSolver::Problem pitched = problem;
    {
        const glm::dmat3 about0(glm::mat3(glm::rotate(glm::mat4(1.0f), 1.4f, glm::vec3(sole.frame[0]))));
        const glm::dmat3 about2(glm::mat3(glm::rotate(glm::mat4(1.0f), 0.15f, glm::vec3(sole.frame[2]))));
        pitched.orientations[0].target = about2 * about0 * frames.rot[8];
    }
    const double jacobianErrorPitched = solver.jacobianError(pose, pitched);

    // 3. A reach with the foot pinned, then the target brought BACK: the pose returns.
    JointSolver::Problem drag;
    drag.dofs = problem.dofs;
    JointPositionTask pin;
    pin.bone = 8;
    pin.target = footStart;
    pin.weight = 1.0e7;
    drag.positions.push_back(pin);
    JointPositionTask goal;
    goal.bone = 5;
    goal.target = handStart;
    goal.weight = 1.0e4;
    goal.huber = 0.05;
    drag.positions.push_back(goal);
    const JointSolver::Pose start = pose;
    JointSolver::Settings settings;
    settings.maxIterations = 30;
    const glm::dvec3 reach(0.10, 0.12, 0.08);
    double missAtReach = 0.0, pinAtReach = 0.0, worstStill = 0.0;
    for (int tick = 1; tick <= 40; ++tick) { // out over 20 ticks, back over 20
        const double f = tick <= 20 ? tick / 20.0 : (40 - tick) / 20.0;
        drag.positions[1].target = handStart + reach * f;
        solver.solve(pose, drag, settings);
        if (tick == 20) {
            solver.forwardKinematics(pose, frames);
            missAtReach = glm::length(frames.pos[5] - drag.positions[1].target);
            pinAtReach = glm::length(frames.pos[8] - footStart);
            // A still target: further ticks must not move the pose (nothing to tremble).
            const JointSolver::Pose held = pose;
            for (int k = 0; k < 5; ++k) {
                solver.solve(pose, drag, settings);
            }
            for (std::size_t i = 0; i < specs.size(); ++i) {
                for (int a = 0; a < 3; ++a) {
                    worstStill = std::max(worstStill, std::abs(pose.eulerDeg[i][a] - held.eulerDeg[i][a]));
                }
            }
        }
    }
    double returnError = 0.0;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        for (int a = 0; a < 3; ++a) {
            returnError = std::max(returnError, std::abs(pose.eulerDeg[i][a] - start.eulerDeg[i][a]));
        }
    }
    report.phase("[unit] joint-space solver",
                 {gateMax("FK vs the engine's composition (m)", fkError, 1e-5),
                  gateMax("analytic Jacobian vs finite differences", jacobianError, 1e-4),
                  gateMax("... with the per-axis hold 80 degrees off its target", jacobianErrorPitched, 1e-4),
                  gateMax("hand miss at the reach (mm)", missAtReach * 1000.0, 0.05),
                  gateMax("pinned foot slip at the reach (mm)", pinAtReach * 1000.0, 0.01),
                  gateMax("pose change over 5 still ticks (deg)", worstStill, 1e-3),
                  gateMax("pose error after the target returned (deg)", returnError, 0.02)});
}

} // namespace ikharness
