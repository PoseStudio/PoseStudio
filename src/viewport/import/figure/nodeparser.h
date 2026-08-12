/**
 * @file nodeparser.h
 * @brief Parses a figure's node_library into a flat skeleton (bones + parent links + rest pose).
 *
 * The base figure file describes its skeleton as a node_library: a figure root plus one node per
 * bone, each with a parent reference, a center_point (the joint's rest origin), and an orientation.
 * This lifts those into a flat FigureBone list with parents resolved to indices — the form the
 * skinning runtime needs. Pure std + GLM + nlohmann — no Qt.
 */

#ifndef NODEPARSER_H
#define NODEPARSER_H

#include "figuredata.h"

#include <nlohmann/json_fwd.hpp>

#include <vector>

namespace pose {

/// Parses @p nodeLibrary into an ordered bone list (only `figure`/`bone` nodes; cameras/lights are
/// skipped). Each bone's `parent` is resolved to an index into the returned list (-1 for the root).
std::vector<FigureBone> parseSkeleton(const nlohmann::json& nodeLibrary);

} // namespace pose

#endif // NODEPARSER_H
