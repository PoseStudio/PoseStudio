/**
 * @file skinparser.h
 * @brief Parses a figure's skin binding into per-vertex joint weights for linear-blend skinning.
 *
 * The base figure carries a "skin binding" modifier: a list of joints, each naming a bone and the
 * sparse (vertex, weight) pairs it influences. This inverts that into one entry per base vertex —
 * its top-4 influencing bones (as indices) with normalized weights, the form a GPU skinning shader
 * consumes. Pure std + GLM + nlohmann — no Qt.
 */

#ifndef SKINPARSER_H
#define SKINPARSER_H

#include "figuredata.h"

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace pose {

/// Parses a SkinBinding modifier's `skin` object into @p vertexCount per-vertex bindings. Each
/// joint's bone (`node` = "#name") is mapped to an index via @p boneNameToIndex; joints whose bone
/// isn't found are skipped. Each vertex keeps its 4 strongest influences, renormalized to sum to 1.
std::vector<VertexSkin> parseSkinWeights(const nlohmann::json& skin, int vertexCount,
                                         const std::unordered_map<std::string, int>& boneNameToIndex);

} // namespace pose

#endif // SKINPARSER_H
