/**
 * @file correctivescan.h
 * @brief Discovers a figure's pose correctives (joint-driven corrective morphs) on disk.
 *
 * Correctives aren't listed by the preset — they're morph assets that auto-apply by convention, so
 * they have to be FOUND. This module owns that search: which directories are worth inflating
 * (corrective/flexion packs under the figure's Morphs tree, each dialed character morph's own
 * folder, and — for a point-release figure that ships no packs of its own — a topology-compatible
 * sibling figure's packs), the parallel read/inflate/pre-filter of the candidate files, and the
 * gate resolution that turns a corrective's character/enable drivers into constants. Detection
 * itself (is this modifier a live corrective?) is correctiveparser's job; this file only decides
 * where to look and feeds it. Never keyed on brand or file names — the structure is what differs
 * from one figure generation to the next, and the structure is what's matched.
 * Pure std + GLM + nlohmann (+ UriResolver) — no Qt, no Vulkan.
 */

#ifndef CORRECTIVESCAN_H
#define CORRECTIVESCAN_H

#include "modeldata.h"     // PoseCorrective
#include "morphresolver.h" // DialedMorph

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pose {

class UriResolver;

/// Discovers the figure's pose correctives and returns the LIVE ones (gates resolved, unknown
/// driver joints rejected, duplicates by id dropped). @p baseDir is the base figure's directory
/// (its `Morphs/` tree is scanned), @p boneNames the skeleton's joint names (valid rotation
/// drivers), @p dialed the preset's resolved morphs (the character folders to scan),
/// @p channelValues what the morph walk found each channel to be (ResolvedShape::channelValues:
/// what a corrective's character / enable gates resolve to, before any file's default),
/// @p baseGeometryFile the base `.dsf` path and
/// @p cageVertexCount its cage size (the two inputs of the sibling-base fallback; the cage size is
/// also what every corrective's own vertex count must agree with). Failures in any one file or
/// modifier skip just that file/modifier — a malformed pack never sinks the import. A corrective
/// that is live for this figure and refused all the same — and that no other file supplied — is
/// named in @p skipped (optional), each once, as "corrective '<id>': <why>".
std::vector<PoseCorrective> discoverCorrectives(UriResolver& resolver, const std::string& baseDir,
                                                const std::unordered_set<std::string>& boneNames,
                                                const std::vector<DialedMorph>& dialed,
                                                const std::unordered_map<std::string, float>& channelValues,
                                                const std::string& baseGeometryFile,
                                                std::size_t cageVertexCount,
                                                std::vector<std::string>* skipped = nullptr);

} // namespace pose

#endif // CORRECTIVESCAN_H
