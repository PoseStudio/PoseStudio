// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file unitphases.h
 * @brief The harness's unit phases: the IK math primitives and the solver, tested without a
 *        figure (Euler round-trips for all six rotation orders incl. gimbal lock, graph
 *        re-rooting, the support polygon, the joint-space solver on a synthetic rig).
 */

#ifndef IKHARNESS_UNITPHASES_H
#define IKHARNESS_UNITPHASES_H

#include "report.h"

namespace ikharness {

void testEulerRoundTrip(Report& report);
void testSkeletonGraph(Report& report);
void testBalance(Report& report);
/// A pose brought back to exactly zero is the bind, bit for bit (Armature::recomposePoseLocal).
void testZeroEulerIsBind(Report& report);
/// The whole-body joint-space solver (jointsolver.h) on a synthetic branched chain with
/// random oriented frames and every rotation order: FK parity with the engine's composition,
/// the analytic Jacobian against finite differences, convergence onto a reachable target
/// with a pin held, and PATH INDEPENDENCE (the target returned, the pose returns).
void testJointSolver(Report& report);

} // namespace ikharness

#endif // IKHARNESS_UNITPHASES_H
