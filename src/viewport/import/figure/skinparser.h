// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file skinparser.h
 * @brief Parses a figure's skin binding into per-vertex joint weights for GPU (dual-quaternion)
 *        skinning.
 *
 * The base figure carries a "skin binding" modifier: a list of joints, each naming a bone and the
 * sparse (vertex, weight) pairs it influences. This inverts that into one entry per base vertex —
 * its top-4 influencing bones (as indices) with normalized weights, the form the GPU skinning
 * shader consumes (it blends the joints' dual quaternions by these weights; the figure format
 * authors its weights against dual quaternions, so no re-weighting happens here).
 * Pure std + GLM + nlohmann — no Qt.
 */

#ifndef SKINPARSER_H
#define SKINPARSER_H

#include "figuredata.h"

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace pose {

/// The `skin` object of the first modifier in @p modifierLibrary that carries one (the figure's
/// skin binding), or nullptr when the library has none / isn't an array.
const nlohmann::json* findSkinBinding(const nlohmann::json& modifierLibrary);

/// Parses a SkinBinding modifier's `skin` object into @p vertexCount per-vertex bindings. Each
/// joint's bone (`node` = "#name") is mapped to an index via @p boneNameToIndex; joints whose bone
/// isn't found are skipped. Each vertex keeps its 4 strongest influences, renormalized to sum to 1.
/// @p vertexCount is the binding's own `vertex_count` (falling back to the cage size): weights for
/// vertices at or beyond it are dropped, and if it is SMALLER than the cage the tail cage vertices
/// simply have no entry — subdivision pads them as zero-weight (unskinned) vertices.
std::vector<VertexSkin> parseSkinWeights(const nlohmann::json& skin, int vertexCount,
                                         const std::unordered_map<std::string, int>& boneNameToIndex);

} // namespace pose

#endif // SKINPARSER_H
