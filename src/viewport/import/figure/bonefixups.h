// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file bonefixups.h
 * @brief Post-parse adjustments to a figure's skeleton: morph-driven joint centres and the legacy
 *        twist-bone bend lock.
 *
 * nodeparser lifts the skeleton exactly as the base file authors it. Two things then have to be
 * corrected before the rig is handed to skinning, and both belong to the figure pipeline rather
 * than the parser: (1) the character's identity morphs also drive each bone's rest centre (a
 * FBM/FHM morph carries hundreds of `center_point` formulas), so the rig must move with the
 * morphed proportions — a base-figure rig around a morphed mesh pivots limbs at the wrong places
 * and the joint overlay floats off the body; and (2) old figure generations that predate the
 * format's per-channel `locked` flag leave a mid-limb twist bone free to BEND, which the geometric
 * fallback here forbids (the runtime's clampBoneEuler then enforces the lock on every posing path).
 * Order matters: apply the centre offsets first, so the lock's length direction reflects the
 * character's proportions. Pure std + GLM — no Qt, no Vulkan.
 */

#ifndef BONEFIXUPS_H
#define BONEFIXUPS_H

#include "figuredata.h"    // FigureBone
#include "morphresolver.h" // JointCenterOffsets

#include <vector>

namespace pose {

/// Shifts each bone's rest origin by its entry in @p offsets (keyed by bone name; bones without an
/// entry are untouched). The offsets are in the same native units as FigureBone::origin — the
/// cm->world scale is applied later, uniformly, to mesh and skeleton together.
void applyJointCenterOffsets(std::vector<FigureBone>& bones, const JointCenterOffsets& offsets);

/// Locks the two bend axes of every "twist" bone that the FORMAT did not already constrain, keeping
/// only the axis most aligned with the bone's length (its twist). A legacy fallback for figure
/// generations without the per-channel `locked` flag: any bone with a format-locked axis is left
/// exactly as authored (see the implementation for why re-deriving would conflict). Bones without
/// a child, or whose child coincides with the joint, are skipped (no length direction).
void lockTwistBoneBendAxes(std::vector<FigureBone>& bones);

} // namespace pose

#endif // BONEFIXUPS_H
