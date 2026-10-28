/**
 * @file scenematerials.h
 * @brief Applies a preset document's materials onto a figure's assembled zone meshes.
 *
 * materialparser turns one `scene.materials` list into per-zone MaterialRefs with UNRESOLVED
 * texture URIs; this layer does the rest — resolving each zone's URIs to on-disk paths through
 * the content roots and writing the result into FigureMesh::material — and it knows the SECOND
 * form a materials preset comes in. Ordinary presets carry `scene.materials`
 * (applySceneMaterials); hierarchical-material presets carry their values as single-key
 * `scene.animations` tracks instead (applyAnimationMaterials), and a stylized character's follower
 * addons only turn their lens/iris shells transparent through that form. Both appliers no-op when
 * their section is absent, so a non-material document passes through harmlessly and the addon
 * loop can hand every preset to both. Cross-file material bases ("/data/…#<SkinShaderBase>") are
 * pre-resolved here for the parser, which only knows same-file "#id" references.
 * Pure std + GLM + nlohmann (+ UriResolver) — no Qt, no Vulkan.
 */

#ifndef SCENEMATERIALS_H
#define SCENEMATERIALS_H

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <unordered_map>

namespace pose {

struct FigureData;
class UriResolver;

/// Applies @p root's `scene.materials` onto @p fig: each zone the scene names gets its resolved
/// colours/maps/opacity, other zones keep what they have. @p referringDir resolves relative
/// texture references (the directory of the document @p root came from). Shared by the main
/// figure path (the preset's own materials) and the addon path (a follower's materials preset).
void applySceneMaterials(FigureData& fig, const nlohmann::json& root, UriResolver& resolver,
                         const std::string& referringDir);

/// Applies a hierarchical-materials preset (asset_info type "preset_hierarchical_material") onto
/// @p fig. Its values ride in single-key `scene.animations` tracks, each url addressing one
/// material property of one zone; only the properties the renderer consumes are read. Zones are
/// matched by name; the node prefix ("<node>#materials/…") is ignored unless @p onlyFigureNode is
/// set, when a track applies only if that prefix names @p fig's own node (FigureData::figureNodeId
/// or figureNodeName) — how a preset listed under ANOTHER follower's slot is applied to this one
/// (see loadFigureFile). No-op without a `scene.animations` array.
void applyAnimationMaterials(FigureData& fig, const nlohmann::json& root, UriResolver& resolver,
                             const std::string& referringDir, bool onlyFigureNode = false);

/// The UV set each material zone of @p root's `scene.materials` names — its `uv_set` URI, spread
/// over the material's `groups` — as zone name → URI; a zone whose material names none is absent.
/// Read BEFORE assembly, unlike the appliers above: the UVs are baked into the render mesh, and a
/// zone's maps are painted on the layout its material names, which need not be the geometry's
/// default (see uvparser.h). The hierarchical-material form carries `?uv_set` tracks too, which
/// are not read: those presets are applied after assembly, and no content seen names a set other
/// than the figure's own through them.
std::unordered_map<std::string, std::string> sceneMaterialUvSets(const nlohmann::json& root);

} // namespace pose

#endif // SCENEMATERIALS_H
