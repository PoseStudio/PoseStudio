// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file realphases.h
 * @brief The real-skeleton phases: every user-reported FBIK behaviour, reproduced on a dumped
 *        figure skeleton through the simulator and gated on the measurements that once failed.
 *        realphases.cpp holds them as the RealPhases class — the state every phase shares, the
 *        helpers, and sixteen phase families run in order; `--only <key>` picks phases by the
 *        key each `if (report.wants("key"))` block names.
 */

#ifndef IKHARNESS_REALPHASES_H
#define IKHARNESS_REALPHASES_H

#include "armature.h"
#include "report.h"

#include <vector>

namespace ikharness {

/// Runs every real-figure phase the report wants (see Report::wants) on @p bones.
void realPhases(Report& report, const std::vector<pose::ArmatureBone>& bones);

} // namespace ikharness

#endif // IKHARNESS_REALPHASES_H
