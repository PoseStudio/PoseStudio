/**
 * @file followeraddons.cpp
 * @brief Implementation of the follower-addon helpers. See followeraddons.h.
 */

#include "followeraddons.h"

#include "figuredata.h"
#include "figureutils.h"
#include "geometryparser.h"
#include "parallelfor.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <unordered_map>

namespace pose {

// The character-addon declarations in a preset's post-load-script block — fitted follower figures
// (e.g. a stylized character's separate anime eyeballs/lashes/brows, which replace standard surfaces
// the preset deliberately hides). Structure, under scene.extra[]:
//   { "type": "scene_post_load_script", "settings": { "PostLoadAddons": { "value": {
//       "<slot>": { "value": { "AssetFile": "….duf", "Presets": { "value": {
//           "<name>": { "value": { "PresetFile": "….duf" } } } } } } } } }
// The preset key is NOT fixed ("Mat" usually, but e.g. "Brow Mat" for one vendor's brows), so every
// entry carrying a PresetFile is collected; the appliers no-op on documents that aren't materials.
std::vector<AddonRef> findPostLoadAddons(const nlohmann::json& root) {
    std::vector<AddonRef> out;
    const auto scene = root.find("scene");
    if (scene == root.end()) {
        return out;
    }
    const auto extra = scene->find("extra");
    if (extra == scene->end() || !extra->is_array()) {
        return out;
    }
    // Addon references are content-root-relative but inconsistently written with/without the leading
    // '/' (both occur within a single shipping preset); normalize so the resolver treats them alike.
    const auto rootRelative = [](std::string uri) {
        if (!uri.empty() && uri[0] != '/') {
            uri.insert(uri.begin(), '/');
        }
        return uri;
    };
    for (const auto& block : *extra) {
        // extra[] and the addon slots may hold any JSON type, and one malformed block must not
        // abort the whole figure: every read below is a tolerant one (childOf only FINDS, stringOf
        // returns empty for anything that is no string).
        if (stringOf(block, "type") != "scene_post_load_script") {
            continue;
        }
        const nlohmann::json* addons =
            childOf(childOf(childOf(&block, "settings"), "PostLoadAddons"), "value");
        if (!addons || !addons->is_object()) {
            continue;
        }
        for (const auto& item : addons->items()) {
            const nlohmann::json* v = childOf(&item.value(), "value");
            if (!v || !v->is_object()) {
                continue;
            }
            AddonRef ref;
            ref.assetUri = rootRelative(stringOf(*v, "AssetFile"));
            if (ref.assetUri.size() <= 1) {
                continue;
            }
            if (const nlohmann::json* presets = childOf(childOf(v, "Presets"), "value");
                presets && presets->is_object()) {
                for (const auto& preset : presets->items()) {
                    const nlohmann::json* file =
                        childOf(childOf(&preset.value(), "value"), "PresetFile");
                    if (file && file->is_string()) {
                        ref.presetUris.push_back(rootRelative(file->get<std::string>()));
                    }
                }
            }
            out.push_back(std::move(ref));
        }
    }
    return out;
}

AddonProjection buildAddonProjection(const GeometryData& morphedGeo,
                                     const std::vector<glm::vec3>& baseCage,
                                     const std::vector<FigureMesh>& meshes) {
    AddonProjection out;
    const std::vector<glm::vec3>& positions = morphedGeo.positions;

    // The morph deltas the followers must track (morphed − base cage). All-zero for an
    // unmorphed base figure, in which case the projection is skipped entirely.
    out.cageDeltas.assign(baseCage.size(), glm::vec3(0.0f));
    for (std::size_t i = 0; i < baseCage.size() && i < positions.size(); ++i) {
        out.cageDeltas[i] = positions[i] - baseCage[i];
        out.anyDelta = out.anyDelta || glm::dot(out.cageDeltas[i], out.cageDeltas[i]) > 1e-8f;
    }

    // Only VISIBLE zones contribute projection deltas. A stylized preset hides the stock
    // surfaces its addons replace (cutout 0) AND its morph typically stashes that hidden
    // geometry (shrinks the stock eyeballs into the head) — followers near the eye region
    // would sample those garbage deltas and land across the face. Clear shells (opacity
    // ~0.05) are excluded with them; the surrounding visible skin supplies sane deltas.
    out.eligibleCageVerts.assign(baseCage.size(), 0);
    std::unordered_map<std::string, float> zoneOpacity;
    for (const FigureMesh& m : meshes) {
        zoneOpacity[m.materialZone] = m.material.opacity;
    }
    for (const GeometryData::Face& f : morphedGeo.faces) {
        float opacity = 1.0f; // zones without a parsed material default to visible
        if (f.material >= 0 && f.material < static_cast<int>(morphedGeo.materialZones.size())) {
            const auto it = zoneOpacity.find(morphedGeo.materialZones[static_cast<std::size_t>(f.material)]);
            if (it != zoneOpacity.end()) {
                opacity = it->second;
            }
        }
        if (opacity <= 0.06f) {
            continue;
        }
        for (int c = 0; c < f.count; ++c) {
            if (f.v[static_cast<std::size_t>(c)] < out.eligibleCageVerts.size()) {
                out.eligibleCageVerts[f.v[static_cast<std::size_t>(c)]] = 1;
            }
        }
    }
    return out;
}

// Auto-follow (shape projection) for follower addons: a follower is authored against the BASE
// figure, but the character's morphs have moved that surface — a skin-tight follower must move
// with it or it hangs where the base surface used to be (fibermesh eyebrows floating in front of
// a morphed forehead). The format's host projects the parent's active morphs onto conformed
// followers at load; this approximates that projection per render vertex with the morph delta of
// the NEAREST PARENT BASE-CAGE VERTEX — the cage is dense (~16k vertices, sub-centimetre spacing
// on the face), so the quantization is invisible for surface-hugging followers. Every vertex is
// projected by ITS OWN nearest cage vertex: there is no notion of a strand here, so the tip of a
// long strand takes the delta of whatever skin lies nearest the tip, not its root's.
void followParentShape(FigureData& addon, const std::vector<glm::vec3>& baseCage,
                       const std::vector<glm::vec3>& cageDeltas,
                       const std::vector<uint8_t>& eligibleCageVerts) {
    if (baseCage.empty() || addon.meshes.empty()) {
        return;
    }
    // Uniform spatial hash over the base cage for nearest-vertex queries. Only ELIGIBLE vertices
    // enter the grid: the caller excludes hidden zones, whose morph deltas are garbage for
    // projection (a stylized preset stashes the stock surfaces it hides — e.g. shrinks the hidden
    // standard eyeballs into the head — and a follower sampling those deltas is thrown off the
    // face; observed, not hypothetical).
    constexpr float kCell = 2.0f; // native cm; a face-region cell holds a handful of cage verts
    glm::vec3 lo = baseCage[0];
    for (const glm::vec3& p : baseCage) {
        lo = glm::min(lo, p);
    }
    // Cell-coordinate hash. Shifted as UNSIGNED: query points (follower vertices) can lie below
    // the cage's min corner, making cell coords negative, and left-shifting a negative signed
    // value is undefined behavior in C++17. Unsigned conversion + shift is fully defined.
    const auto cellKeyOf = [](const glm::ivec3& c) {
        return (static_cast<uint64_t>(c.x) << 42) ^ (static_cast<uint64_t>(c.y) << 21) ^
               static_cast<uint64_t>(c.z);
    };
    const auto cellOf = [&](const glm::vec3& p) {
        return glm::ivec3(glm::floor((p - lo) / kCell));
    };
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    for (uint32_t i = 0; i < baseCage.size(); ++i) {
        if (eligibleCageVerts.empty() || eligibleCageVerts[i]) {
            grid[cellKeyOf(cellOf(baseCage[i]))].push_back(i);
        }
    }
    if (grid.empty()) {
        return;
    }
    const auto nearestDelta = [&](const glm::vec3& p) -> glm::vec3 {
        const glm::ivec3 center = cellOf(p);
        float best = std::numeric_limits<float>::max();
        uint32_t bestIndex = 0;
        bool found = false;
        // Scans one cell, keeping the closest eligible vertex seen so far.
        const auto scanCell = [&](const glm::ivec3& c) {
            const auto it = grid.find(cellKeyOf(c));
            if (it == grid.end()) {
                return;
            }
            for (const uint32_t i : it->second) {
                const glm::vec3 d = baseCage[i] - p;
                const float dist2 = glm::dot(d, d);
                if (dist2 < best) {
                    best = dist2;
                    bestIndex = i;
                    found = true;
                }
            }
        };
        // Expand the search shell-by-shell until a candidate appears. Radius 1 scans the full
        // 3^3 cube (center included); each later radius scans only its new outer shell — the
        // inner cells were covered by earlier radii, so rescanning them is pure waste (a query
        // far from any eligible vertex would otherwise cost O(r^4) cell visits).
        for (int radius = 1; radius <= 64 && !found; ++radius) {
            for (int dz = -radius; dz <= radius; ++dz) {
                for (int dy = -radius; dy <= radius; ++dy) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        if (radius > 1 &&
                            std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) < radius) {
                            continue; // shell only — inner radii already scanned
                        }
                        scanCell(center + glm::ivec3(dx, dy, dz));
                    }
                }
            }
            if (found) {
                // One explicit safety shell at radius+1: the first hit may sit just inside its
                // cell's far boundary, so a vertex one shell further out (but spatially nearer)
                // could still beat it — scan that shell before accepting the result. An
                // APPROXIMATION of the nearest, not a guarantee: a hit in the corner of shell r
                // can lie (r+1)·√3 cells away while shell r+2 begins (r+1) cells away. On a dense
                // cage the first hit comes at radius 1 and the difference is a neighbouring
                // vertex's delta.
                const int r1 = radius + 1;
                for (int dz = -r1; dz <= r1; ++dz) {
                    for (int dy = -r1; dy <= r1; ++dy) {
                        for (int dx = -r1; dx <= r1; ++dx) {
                            if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) <= radius) {
                                continue; // inner cells already scanned
                            }
                            scanCell(center + glm::ivec3(dx, dy, dz));
                        }
                    }
                }
                break;
            }
        }
        return found ? cageDeltas[bestIndex] : glm::vec3(0.0f);
    };
    for (FigureMesh& mesh : addon.meshes) {
        parallelFor(static_cast<int>(mesh.vertices.size()), [&](int vi) {
            Vertex& v = mesh.vertices[static_cast<std::size_t>(vi)];
            v.pos += nearestDelta(v.pos);
        });
    }
}

// Merges a follower figure (an addon: anime eyeballs, lashes, …) into the parent's FigureData.
//
// The geometry is kept EXACTLY where the vendor authored it — no re-fitting. A post-load character
// addon ships pre-fitted to the character its preset dials (the format's addon-loader script just
// loads and parents it; there is no auto-fit step to replicate), so the authored rest positions ARE
// the right ones. Do not "correct" them by snapping to the parent's joints: these rigs deliberately
// place bones away from the base figure's (a flat anime eye pivots from deep inside the head so it
// can slide across the face like a curved screen) — re-fitting by joint deltas shoved the eyes off
// the face. At bind pose the skin matrices are identity, so authored positions render as authored.
//
// The skeleton is merged by NAME: shared bones resolve to the parent's (so the follower tracks the
// figure's pose), and follower-only bones (e.g. extra lens joints) are appended as-is. Pose-time
// caveat: a shared bone uses the PARENT's pivot, not the follower's own — fine at rest, and close
// enough for a first pass at posed attachments.
//
// "Name" is the node's `name` FIELD (FigureBone::nodeName), not its id: one figure generation's
// base file renames thirty ids away from the bones' names, and its followers use the names. Matched
// by id, a whole generation's eyelash follower failed to find twelve of its bones and they were
// APPENDED as follower-only: an invisible second spine (upper abdomen to upper neck, five bones
// with no limits) hanging off the figure's lower abdomen plus six face bones — pickable, riding
// the lower abdomen rigidly while the real spine bent away from them, and splitting the trunk's
// mass share in the balance model. An id match is the fallback, for a bone without a name match.
void mergeAddonFigure(FigureData& parent, FigureData&& addon) {
    if (addon.meshes.empty()) {
        return;
    }
    std::unordered_map<std::string, int> parentIndexByName; // by the node's `name` (see above)
    std::unordered_map<std::string, int> parentIndexById;
    for (int i = 0; i < static_cast<int>(parent.bones.size()); ++i) {
        const FigureBone& bone = parent.bones[i];
        parentIndexByName.emplace(bone.nodeName.empty() ? bone.name : bone.nodeName, i);
        parentIndexById.emplace(bone.name, i);
    }

    const int addonBoneCount = static_cast<int>(addon.bones.size());
    std::vector<int> mergedIndex(addonBoneCount, -1);
    // First pass: bones the parent already has resolve to the parent's own.
    for (int i = 0; i < addonBoneCount; ++i) {
        const FigureBone& bone = addon.bones[i];
        const auto byName = parentIndexByName.find(bone.nodeName.empty() ? bone.name : bone.nodeName);
        if (byName != parentIndexByName.end()) {
            mergedIndex[i] = byName->second;
            continue;
        }
        const auto byId = parentIndexById.find(bone.name);
        if (byId != parentIndexById.end()) {
            mergedIndex[i] = byId->second;
        }
    }
    // Second pass: append the follower-only bones in the addon's order, assigning every one its
    // merged slot before any parent link is resolved. The addon's list is PARENTS FIRST
    // (parseSkeleton sees to it), and that order survives the append — an appended bone's parent
    // is either one of the figure's own bones, all of which lie below the first appended slot, or
    // a follower-only bone appended before it — so the merged list keeps the engine's contract.
    std::vector<int> appended; // merged indices of the appended bones (parents still addon-space)
    for (int i = 0; i < addonBoneCount; ++i) {
        if (mergedIndex[i] >= 0) {
            continue;
        }
        mergedIndex[i] = static_cast<int>(parent.bones.size());
        appended.push_back(mergedIndex[i]);
        parent.bones.push_back(addon.bones[i]);
    }
    // Third pass: with every slot known, remap the appended bones' parent links into the merged
    // skeleton (-1 stays the root; an out-of-range index degrades to the root rather than UB).
    for (const int mergedIdx : appended) {
        const int p = parent.bones[mergedIdx].parent;
        parent.bones[mergedIdx].parent = (p >= 0 && p < addonBoneCount) ? mergedIndex[p] : -1;
    }

    // Keep the follower's render vertices out of the parent correctives' base-vertex space
    // (corrective deltas index base vertices; a colliding index would let a parent corrective
    // deform the follower on the GPU). The offset is the parent's whole CAGE size, not the highest
    // index its meshes happen to reference: a corrective can carry a delta on a cage vertex no
    // face uses (subdivision keeps such a vertex's delta verbatim), and that index sat above the
    // referenced maximum — where the first follower vertex used to land.
    const uint32_t baseVertexOffset = static_cast<uint32_t>(parent.baseVertexCount);

    for (FigureMesh& mesh : addon.meshes) {
        for (Vertex& v : mesh.vertices) {
            for (int s = 0; s < 4; ++s) {
                const int j = static_cast<int>(v.joints[s]);
                if (j < addonBoneCount) {
                    v.joints[s] = static_cast<uint32_t>(mergedIndex[j]); // remap into the merged skeleton
                }
            }
        }
        for (uint32_t& b : mesh.baseVertex) {
            b += baseVertexOffset;
        }
        parent.meshes.push_back(std::move(mesh));
    }
    // The merged space now spans the follower's cage too, so a further addon lands past it.
    parent.baseVertexCount += addon.baseVertexCount;
    parent.skipped.insert(parent.skipped.end(), addon.skipped.begin(), addon.skipped.end());
    // The follower's own correctives (rare, and addressed to ITS base cage) are dropped with `addon`:
    // their delta indices have no meaning in the merged base-vertex space.
}

} // namespace pose
