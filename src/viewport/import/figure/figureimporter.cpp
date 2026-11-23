// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file figureimporter.cpp
 * @brief Implementation of FigureImporter — the figure import pipeline, step by step. See
 *        figureimporter.h.
 *
 * This file is the readable spine of the figure importer: loadFigureFile() runs the stages in
 * order and every stage lives in its own module — geometryparser (the cage), morphresolver +
 * morphparser (the character's shape), nodeparser + bonefixups (the skeleton), skinparser (the
 * weights), correctivescan (the pose correctives), uvparser + subdivision (the render mesh),
 * scenematerials (the materials), followeraddons (the followers). Three things stay here, each a
 * function above loadFigureFile: the dialed-morph bake (bakeDialedMorphs — the one place the cage
 * is mutated in place), which UV set each material zone reads (resolveZoneUvSets — it joins the
 * geometry, the scene's materials and the resolver), and the per-addon loop, which recurses into
 * loadFigureFile itself. One UriResolver serves the whole import, addons included (same roots;
 * the document cache is what keeps a shared base file from being inflated once per addon).
 */

#include "figureimporter.h"

#include "bonefixups.h"
#include "correctivescan.h"
#include "figuredocument.h"
#include "figureutils.h"
#include "followeraddons.h"
#include "geometryparser.h"
#include "morphparser.h"
#include "morphresolver.h"
#include "nodeparser.h"
#include "scenematerials.h"
#include "skinparser.h"
#include "subdivision.h"
#include "uriresolver.h"
#include "uvparser.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pose {

namespace {

// Applies the preset's dialed shape morphs to the base vertices — this is what turns the generic
// base figure into the specific character. A character rarely dials the shape morphs directly; it
// dials a *control* whose driver formulas drive the real morphs — often several, a head, a body,
// the feet — so resolveDialedMorphs() walks that graph (for the figure @p scope names: a scene
// lists every figure's dials side by side) and returns each reached morph file + weight. We then
// bake its sparse deltas (position += weight · delta). Controls/scales resolve to no deltas and
// no-op; a morph that fails to load is skipped rather than aborting the whole import, and SAID:
// @p report lists what was applied and what could not be. Returns the resolved shape — the
// reached channels (the corrective scan needs both their folders and their weights), the figure
// scale and the joint-centre offsets the same formulas drive.
ResolvedShape bakeDialedMorphs(const nlohmann::json& presetRoot, UriResolver& resolver,
                               const std::string& presetDir, const FigureScope& scope,
                               GeometryData& geo, ShapeReport& report) {
    ResolvedShape shape = resolveDialedMorphs(presetRoot, resolver, presetDir, scope);
    report.missing = shape.missing;
    // (Every morph document the loop below asks for is in the resolver's cache already: the walk
    // read each reached channel's file to find its formulas — across cores, a wave at a time.)
    for (const DialedMorph& dialed : shape.morphs) {
        std::shared_ptr<const FigureDocument> morphDoc; // keeps a morph file alive through its bake
        const nlohmann::json*                 morphRoot = nullptr;
        std::string                           fragment;
        if (!dialed.url.empty() && dialed.url[0] == '#') {
            morphRoot = &presetRoot; // a morph the preset carries in its own modifier_library
            fragment = dialed.id;
        } else {
            const ResolvedUri ru = resolver.resolve(dialed.url, presetDir);
            if (!ru.resolved()) {
                continue; // the walk already reported its file as missing
            }
            try {
                morphDoc = resolver.loadDocument(ru);
            } catch (const std::exception&) {
                continue;
            }
            morphRoot = &morphDoc->root();
            fragment = ru.fragment;
        }
        std::size_t             authoredFor = 0;
        std::vector<MorphDelta> deltas;
        try {
            deltas = parseMorphDeltas(*morphRoot, fragment, &authoredFor);
        } catch (const std::exception&) {
            // A delta that is no number, a morph block of the wrong type: this one morph is left
            // out — and said — where it used to take the whole import with it.
            report.missing.push_back(dialed.id + ": its deltas could not be read");
            continue;
        }
        if (deltas.empty()) {
            continue; // a control, a scale
        }
        // A morph says how many vertices the geometry it was made for has. One made for another
        // geometry indexes other vertices: added to this cage it moves whichever of ours happen to
        // share the numbers.
        if (authoredFor != 0 && authoredFor != geo.positions.size()) {
            report.missing.push_back(dialed.id + ": made for another geometry (" +
                                     std::to_string(authoredFor) + " vertices, this one has " +
                                     std::to_string(geo.positions.size()) + ")");
            continue;
        }
        for (const auto& [vertexIndex, delta] : deltas) {
            if (vertexIndex < geo.positions.size()) {
                geo.positions[vertexIndex] += dialed.weight * delta;
            }
        }
        report.applied.push_back({dialed.id, dialed.weight, deltas.size()});
    }
    return shape;
}

// The UV set each material zone reads its maps through, and the sets themselves (`perZone` points
// into this object, which is why it is filled in place and never copied).
struct ZoneUvSets {
    UvSet                                                   defaultSet;
    std::unordered_map<std::string, std::unique_ptr<UvSet>> own;     // by resolved key; null = refused
    std::vector<const UvSet*>                               perZone; // one per material zone (>= 1)
    int                                                     zonesOnOwnSet = 0;

    ZoneUvSets() = default;
    ZoneUvSets(const ZoneUvSets&) = delete;
    ZoneUvSets& operator=(const ZoneUvSets&) = delete;
};

// The UV sets. The base geometry names the DEFAULT (resolved relative to the base file, @p baseDir),
// and each scene material may name the set ITS zone's maps are painted on (`uv_set`, resolved
// relative to the preset): a newer generation's character commonly keeps the previous
// generation's skin maps and names that generation's layout for the skin zones while its eyes
// and nails use the new one. The two layouts differ by a few dozen UVs and seam entries —
// read through the default alone, every skin feature landed a little off: a crack along the
// scalp's seam, a patch between the shoulder blades, the throat's notch displaced (found
// 2026-10-26). Each distinct set is parsed once and shared by the zones that name it; a set
// authored for another cage (its vertex_count is not this geometry's) is refused — indexed by
// base vertex, it would scramble the mapping — and the zone keeps the default, as a zone whose
// set cannot be read does. POSESTUDIO_UV_DEFAULT_ONLY is the A/B: the default for every zone.
void resolveZoneUvSets(const GeometryData& geo, const nlohmann::json& root, const FigureScope& scope,
                       UriResolver& resolver, const std::string& baseDir,
                       const std::string& presetDir, ZoneUvSets& sets) {
    std::string defaultUvKey; // the resolved "path#fragment": a material naming it shares the parse
    if (!geo.defaultUvSetUri.empty()) {
        const ResolvedUri uru = resolver.resolve(geo.defaultUvSetUri, baseDir);
        if (uru.resolved()) {
            std::shared_ptr<const FigureDocument> uvDoc = resolver.loadDocument(uru);
            sets.defaultSet = parseUvSet(uvDoc->root(), uru.fragment);
            defaultUvKey = uru.path + '#' + uru.fragment;
        }
    }
    // At least one entry: untagged geometry is one zone to the assembly (subdivision.cpp's
    // zoneOf), and reads the default set through it.
    sets.perZone.assign(std::max<std::size_t>(geo.materialZones.size(), 1), &sets.defaultSet);
    if (std::getenv("POSESTUDIO_UV_DEFAULT_ONLY") == nullptr) {
        const std::unordered_map<std::string, std::string> zoneUris = sceneMaterialUvSets(root, scope);
        for (std::size_t zone = 0; zone < geo.materialZones.size(); ++zone) {
            const auto named = zoneUris.find(geo.materialZones[zone]);
            if (named == zoneUris.end()) {
                continue;
            }
            try {
                const ResolvedUri uru = resolver.resolve(named->second, presetDir);
                if (!uru.resolved()) {
                    continue;
                }
                const std::string key = uru.path + '#' + uru.fragment;
                if (key == defaultUvKey) {
                    continue;
                }
                auto it = sets.own.find(key);
                if (it == sets.own.end()) {
                    auto set = std::make_unique<UvSet>(
                        parseUvSet(resolver.loadDocument(uru)->root(), uru.fragment));
                    const bool fits = !set->empty() &&
                                      (set->vertexCount == 0 ||
                                       set->vertexCount == geo.positions.size());
                    if (!fits) {
                        set.reset(); // remembered as refused, so it is not re-read per zone
                    }
                    it = sets.own.emplace(key, std::move(set)).first;
                }
                if (it->second) {
                    sets.perZone[zone] = it->second.get();
                    ++sets.zonesOnOwnSet;
                }
            } catch (const std::exception&) {
                // An unreadable set: the zone keeps the default, as every zone did before.
            }
        }
    }
}

// The full loader. `includeAddons` is true for the user-facing entry point and false for the
// follower-addon recursion (an addon must not pull in further addons — one level is what the
// format's addon loader does, and it also bounds the recursion). The same flag also means "this is
// the top-level figure": pose correctives are only discovered and carried for it, since a merged
// follower's correctives are dropped anyway (their delta indices mean nothing in the merged
// base-vertex space). @p resolver is shared with the recursion — same roots, one document cache.
FigureData loadFigureFile(const std::string& path, UriResolver& resolver, bool includeAddons) {
    const std::string presetDir = directoryOf(path);

    FigureDocument presetDoc = FigureDocument::loadFromFile(path);
    const nlohmann::json& root = presetDoc.root();

    // Locate the base geometry: the one the scene's figure node names. For a preset that is another
    // file (geometries[0].url = "/data/…/Figure.dsf#geometry"); for a base .dsf it is the file's
    // own geometry_library ("#geometry", or no figure node at all). The figure node is asked
    // FIRST: a user-saved character can carry a geometry_library of its own — a prop that was never
    // saved as an asset is embedded in the scene file — and taken for a base file on the strength
    // of that key, one such character imported as her prop (found 2026-11-10).
    const nlohmann::json* geomEntry = nullptr;
    std::shared_ptr<const FigureDocument> baseDoc; // keeps the referenced base file alive
    std::string baseDir = presetDir;
    std::string baseFile = path; // the base figure .dsf itself (sibling-base corrective matching)

    const std::string geomUri = findGeometryUri(root);
    if (geomUri.empty() || geomUri[0] == '#') {
        if (!root.contains("geometry_library")) {
            throw std::runtime_error("figure import: no geometry reference found in " + path);
        }
        geomEntry = findGeometryEntry(root, referencedId(geomUri));
    } else {
        const ResolvedUri ru = resolver.resolve(geomUri, presetDir);
        if (!ru.resolved()) {
            throw std::runtime_error("figure import: cannot resolve base geometry '" + geomUri + "'");
        }
        baseDoc = resolver.loadDocument(ru);
        baseDir = directoryOf(ru.path);
        baseFile = ru.path;
        geomEntry = findGeometryEntry(baseDoc->root(), ru.fragment);
    }
    if (!geomEntry) {
        throw std::runtime_error("figure import: base geometry library is empty");
    }

    GeometryData geo = parseGeometry(*geomEntry);

    // Pre-morph copy of the base cage: follower addons are authored against THIS shape, so their
    // shape-follow projection (followParentShape) needs base positions + the morph deltas.
    std::vector<glm::vec3> baseCagePositions;
    if (includeAddons) {
        baseCagePositions = geo.positions;
    }

    // WHICH FIGURE of the scene this load is for. A user-saved character is a scene of several —
    // the character, her eyelashes, her hair, a ring — whose dials and materials are listed side
    // by side; everything read out of the scene below is read for this one (see FigureScope).
    const FigureScope scope = figureScopeOf(root);

    // The character's shape: bake the dialed morphs into the cage.
    FigureData out;
    const ResolvedShape shape = bakeDialedMorphs(root, resolver, presetDir, scope, geo, out.shape);
    const std::vector<DialedMorph>& dialedMorphs = shape.morphs;
    const float                     figureScale = shape.figureScale;
    const JointCenterOffsets&       jointCenterOffsets = shape.jointCenterOffsets; // per-bone rest-origin shifts

    // The UV set each material zone reads (the geometry's default, or the one its material names).
    ZoneUvSets uvSets;
    resolveZoneUvSets(geo, root, scope, resolver, baseDir, presetDir, uvSets);

    out.figureScale = figureScale;
    out.zonesOnOwnUvSet = uvSets.zonesOnOwnSet;
    out.graftPopulated = geo.graftPopulated;
    if (const nlohmann::json* figureNode = findFigureNode(root)) {
        out.figureNodeId = stringOf(*figureNode, "id");
        out.figureNodeName = stringOf(*figureNode, "name");
    }

    // Skeleton + skin weights (from the base file's node_library + its SkinBinding modifier), parsed
    // *before* assembly so each de-indexed render vertex can carry its base vertex's joint weights.
    const nlohmann::json& baseRoot = baseDoc ? baseDoc->root() : root;
    if (const auto nodeLib = baseRoot.find("node_library"); nodeLib != baseRoot.end()) {
        out.bones = parseSkeleton(*nodeLib);
    }

    // Shift each joint's rest origin by the morph-driven center_point adjustments, so the skeleton
    // matches the character's morphed proportions (a base-figure rig around a morphed mesh would pivot
    // limbs at the wrong places, and the joint overlay would float off the body).
    applyJointCenterOffsets(out.bones, jointCenterOffsets);
    for (const FigureBone& bone : out.bones) {
        const auto moved = jointCenterOffsets.find(bone.name);
        if (moved != jointCenterOffsets.end() && glm::length(moved->second) > 1e-4f) {
            ++out.shape.jointsMoved;
        }
    }

    // Per BASE-cage vertex; subdivision refines them onto the render mesh's vertices (empty for an
    // unrigged figure).
    std::vector<VertexSkin> vertexSkins;
    if (!out.bones.empty()) {
        // Twist bones sit mid-limb where there's no real joint; keep their axial twist but forbid the
        // mid-bone bend the figure would otherwise allow. Done after the origins (incl. morph-driven
        // offsets) are final, so each bone's length direction reflects the character's proportions.
        lockTwistBoneBendAxes(out.bones);

        std::unordered_map<std::string, int> boneNameToIndex;
        for (int i = 0; i < static_cast<int>(out.bones.size()); ++i) {
            boneNameToIndex.emplace(out.bones[i].name, i);
        }
        if (const auto modLib = baseRoot.find("modifier_library"); modLib != baseRoot.end()) {
            if (const nlohmann::json* skin = findSkinBinding(*modLib)) {
                // The binding's own vertex_count wins over the cage size; if it is smaller, the
                // tail cage vertices get no entry and subdivision pads them as zero-weight
                // (unskinned) vertices rather than failing.
                const int vc = intOf(*skin, "vertex_count", static_cast<int>(geo.positions.size()));
                vertexSkins = parseSkinWeights(*skin, vc, boneNameToIndex);
            }
        }

        // Pose correctives: auto-applied morphs driven by these joints' rotations. Discovered
        // structurally (see correctivescan), so they work across figure generations that name
        // their corrective files differently. Only meaningful once we have a skeleton to drive them,
        // and only for the top-level figure — a follower's are dropped at the merge.
        if (includeAddons) {
            std::unordered_set<std::string> boneNames;
            boneNames.reserve(out.bones.size());
            for (const FigureBone& b : out.bones) {
                boneNames.insert(b.name);
            }
            out.correctives = discoverCorrectives(resolver, baseDir, boneNames, dialedMorphs,
                                                  shape.channelValues, baseFile, geo.positions.size(),
                                                  &out.skipped);
        }
    }

    // Catmull-Clark subdivision: smooth the low-resolution base cage into the render mesh. Positions
    // follow the smooth rules while UVs (per corner, so seams stay put) and skin weights refine
    // linearly, then the result is assembled exactly like the un-subdivided path — so skinning and pose
    // correctives run unchanged, just on more vertices. Because Catmull-Clark is a *linear* operator,
    // each corrective's sparse base-cage deltas are carried onto the subdivided cage by the SAME
    // stencils (carryCorrectivesToSubdivided), so the GPU's live corrective blend needs no change.
    // Level 1 (4x faces) is a good editing-time smoothness; raise for finer at a vertex-count cost.
    constexpr int kSubdivisionLevels = 1;
    SubdivisionResult sub = subdivideFigure(geo, uvSets.perZone, vertexSkins, kSubdivisionLevels);
    out.meshes = std::move(sub.meshes);
    out.baseVertexCount = sub.vertexCount;
    if (!out.correctives.empty()) {
        carryCorrectivesToSubdivided(sub, geo.positions.size(), out.correctives);
    }

    // Materials come from the preset's scene (a base .dsf on its own carries none -> zones keep the
    // default base color) — the ones it lists for THIS figure's geometry. Texture URIs are resolved
    // to on-disk paths inside.
    applySceneMaterials(out, root, resolver, presetDir, &scope, /*followerPreset=*/!includeAddons);

    // Fitted follower addons (anime eyeballs, lashes, …) the preset declares in its post-load block.
    // Characters built this way deliberately hide the standard surfaces the follower replaces (e.g.
    // every stock eye surface dialed to cutout 0), so WITHOUT this step such a figure imports with
    // empty eye sockets. Each addon is a self-describing wearable figure: load it through this same
    // pipeline, apply its materials preset, and merge it into the figure. Failures skip just that
    // addon — a missing follower must not sink the character import.
    if (includeAddons) {
        const AddonProjection projection =
            buildAddonProjection(geo, baseCagePositions, out.meshes);

        // Every follower is loaded and given the presets ITS slot lists first, in slot order; a
        // failure skips that follower alone.
        struct LoadedAddon {
            FigureData               data;
            std::vector<std::string> presetUris; // the slot's own, applied in full below
        };
        std::vector<LoadedAddon> loaded;
        for (const AddonRef& addonRef : findPostLoadAddons(root)) {
            try {
                const ResolvedUri location = resolver.resolve(addonRef.assetUri, presetDir);
                if (!location.resolved()) {
                    continue;
                }
                LoadedAddon entry{loadFigureFile(location.path, resolver, false), addonRef.presetUris};
                for (const std::string& presetUri : addonRef.presetUris) {
                    try {
                        const ResolvedUri presetLoc = resolver.resolve(presetUri, presetDir);
                        const std::shared_ptr<const FigureDocument> addonPresetDoc =
                            resolver.loadDocument(presetLoc);
                        // Relative (non-root-relative) references inside the preset resolve against
                        // the preset document's OWN directory — not the character preset's — or a
                        // relative image_file path would silently miss and drop the map.
                        const std::string addonPresetDir = directoryOf(presetLoc.path);
                        // A materials preset comes in either form; each applier no-ops when its
                        // section is absent (the anime-eye presets use the animation-track form),
                        // so a non-material preset document just passes through harmlessly.
                        applySceneMaterials(entry.data, addonPresetDoc->root(), resolver,
                                            addonPresetDir, nullptr, /*followerPreset=*/true);
                        applyAnimationMaterials(entry.data, addonPresetDoc->root(), resolver,
                                                addonPresetDir);
                    } catch (const std::exception&) {
                        // Unresolvable preset: the follower keeps its own materials.
                    }
                }
                loaded.push_back(std::move(entry));
            } catch (const std::exception&) {
                continue;
            }
        }

        // A hierarchical-materials preset addresses each figure BY NODE NAME, and the format's own
        // loader applies it to the whole hierarchy whichever slot lists it. One generation's
        // "add-on maps" preset carries its eyelash AND its tear follower's materials side by side,
        // and the base figure and several characters list it under the TEAR slot alone — while the
        // eyelash follower's own file holds only a grey placeholder material. Applied to the tear
        // alone, the lashes rendered as opaque grey strips (the known gap until 2026-10-26). So
        // every slot's presets reach every OTHER follower too, restricted to the tracks addressed
        // to that follower's node; the scene-materials form stays with its own slot (it has no
        // node to filter by, and no content needs it across slots). POSESTUDIO_ADDON_PRESETS_OWN_SLOT
        // is the A/B: each follower gets its own slot's presets alone, as before.
        const bool ownSlotOnly = std::getenv("POSESTUDIO_ADDON_PRESETS_OWN_SLOT") != nullptr;
        for (std::size_t i = 0; i < loaded.size() && !ownSlotOnly; ++i) {
            std::unordered_set<std::string> applied(loaded[i].presetUris.begin(),
                                                    loaded[i].presetUris.end());
            for (std::size_t j = 0; j < loaded.size(); ++j) {
                if (j == i) {
                    continue;
                }
                for (const std::string& presetUri : loaded[j].presetUris) {
                    if (!applied.insert(presetUri).second) {
                        continue; // its own slot applied it in full, or another slot already did
                    }
                    try {
                        const ResolvedUri presetLoc = resolver.resolve(presetUri, presetDir);
                        const std::shared_ptr<const FigureDocument> presetDoc =
                            resolver.loadDocument(presetLoc);
                        applyAnimationMaterials(loaded[i].data, presetDoc->root(), resolver,
                                                directoryOf(presetLoc.path),
                                                /*onlyFigureNode=*/true);
                    } catch (const std::exception&) {
                        // Unresolvable preset: this follower keeps what it has.
                    }
                }
            }
        }

        for (LoadedAddon& entry : loaded) {
            // Shape-follow only the followers authored against the BASE figure. A populated
            // graft marks a vendor-fitted in-place replacement (e.g. an anime character's
            // custom eyeballs) — projecting the morph onto those double-shifts them off the
            // face, verified both ways on real content.
            if (projection.anyDelta && !entry.data.graftPopulated) {
                followParentShape(entry.data, baseCagePositions, projection.cageDeltas,
                                  projection.eligibleCageVerts);
            }
            mergeAddonFigure(out, std::move(entry.data));
        }
    }

    return out;
}

} // namespace

FigureData FigureImporter::load(const std::string& path,
                                const std::vector<std::string>& contentRoots) const {
    UriResolver resolver(contentRoots);
    return loadFigureFile(path, resolver, /*includeAddons=*/true);
}

std::string FigureImporter::contentRootOf(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = fs::absolute(fs::path(path), ec).parent_path();
    // The cap bounds the walk on a pathological path (a file filed absurdly deep, a filesystem
    // whose parent_path never converges); real libraries are three to six levels deep.
    for (int level = 0; level < 16 && !dir.empty(); ++level) {
        if (fs::exists(dir / "data", ec)) {
            return dir.string();
        }
        const fs::path parent = dir.parent_path();
        if (parent == dir) {
            break; // the filesystem root
        }
        dir = parent;
    }
    return std::string();
}

} // namespace pose
