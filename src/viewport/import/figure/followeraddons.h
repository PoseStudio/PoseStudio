// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file followeraddons.h
 * @brief Post-load follower addons: finding a preset's declared followers, fitting them to the
 *        character's morphed shape, and merging them into the parent figure.
 *
 * A character preset can declare follower figures to be loaded after it — a whole figure
 * generation attaches its eyelashes this way, and stylized characters ship replacement anatomy
 * (cartoon eyeballs, brows, lashes) for the standard surfaces their preset hides. Each addon is a
 * self-describing wearable figure that figureimporter loads through its own pipeline; this module
 * owns everything around that: reading the post-load block (findPostLoadAddons), the shape-follow
 * projection that keeps a base-authored follower on the morphed surface (buildAddonProjection +
 * followParentShape), and the merge into the parent's mesh list / skeleton / base-vertex space
 * (mergeAddonFigure). The fit model has three load-bearing rules, each learned from real content
 * and documented at the function that implements it: ordinary followers are projected by the
 * nearest visible parent cage vertex's morph delta; a POPULATED graft marks a vendor-fitted
 * replacement that must NOT be projected (and never re-fit by snapping to parent joints); and
 * projection deltas come only from VISIBLE zones. Pure std + GLM + nlohmann — no Qt, no Vulkan.
 */

#ifndef FOLLOWERADDONS_H
#define FOLLOWERADDONS_H

#include <glm/glm.hpp>

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pose {

struct FigureData;
struct FigureMesh;
struct GeometryData;

/// One follower addon a character preset asks to be loaded after it: the wearable figure file plus
/// any preset documents to apply onto it (usually one materials preset).
struct AddonRef {
    std::string              assetUri;
    std::vector<std::string> presetUris;
};

/// The character-addon declarations in a preset's post-load-script block (scene.extra[] entries of
/// type "scene_post_load_script"). Every entry carrying a PresetFile is collected — the preset key
/// is not fixed ("Mat" usually, but e.g. "Brow Mat" for one vendor's brows) — and URIs are
/// normalized to root-relative form. Malformed entries are skipped, never thrown on.
std::vector<AddonRef> findPostLoadAddons(const nlohmann::json& root);

/// What a follower needs to track the parent's morphed shape: the per-cage-vertex morph delta
/// (morphed − base positions) and which cage vertices may supply one (those of VISIBLE zones).
struct AddonProjection {
    std::vector<glm::vec3> cageDeltas;
    std::vector<uint8_t>   eligibleCageVerts;
    bool                   anyDelta = false; ///< False for an unmorphed base figure — skip the projection.
};

/// Builds the projection inputs from the parent's MORPHED cage @p morphedGeo, its pre-morph
/// @p baseCage positions, and the assembled parent @p meshes (whose materials decide zone
/// visibility: a zone at opacity <= 0.06 — hidden, or a clear shell — contributes no deltas).
AddonProjection buildAddonProjection(const GeometryData& morphedGeo,
                                     const std::vector<glm::vec3>& baseCage,
                                     const std::vector<FigureMesh>& meshes);

/// Shape-follow projection: displaces every render vertex of @p addon by the morph delta of the
/// nearest ELIGIBLE parent base-cage vertex, so a follower authored against the base figure lands
/// on the character's morphed surface. No-op when @p baseCage or the addon is empty.
void followParentShape(FigureData& addon, const std::vector<glm::vec3>& baseCage,
                       const std::vector<glm::vec3>& cageDeltas,
                       const std::vector<uint8_t>& eligibleCageVerts);

/// Merges @p addon into @p parent: meshes appended with their joints remapped into the merged
/// skeleton (shared bones by NAME resolve to the parent's; follower-only bones are appended) and
/// their base-vertex indices offset out of the parent correctives' base-vertex space. The
/// addon's own correctives are dropped (their indices mean nothing in the merged space).
void mergeAddonFigure(FigureData& parent, FigureData&& addon);

} // namespace pose

#endif // FOLLOWERADDONS_H
