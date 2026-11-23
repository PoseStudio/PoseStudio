// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file scenematerials.cpp
 * @brief Implementation of applySceneMaterials / applyAnimationMaterials. See scenematerials.h.
 */

#include "scenematerials.h"

#include "figuredata.h"
#include "figuredocument.h"
#include "figureutils.h"
#include "materialparser.h"
#include "uriresolver.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pose {

namespace {

// HOW A MATERIAL'S MAPS MEET THE ZONE'S EARLIER ONES. A zone can be given a material more than
// once — a follower's own file, then the materials preset its slot lists — and a later
// application replaces every VALUE of an earlier one. Its maps follow one of two rules, by slot,
// and the two tables below ARE the rules (a map member added to FigureMaterial must be listed in
// one of them, or under the normal / bump / detail cases in applySceneMaterials: a member in
// none would be left holding a URI where a path belongs):
using MapMember = std::string FigureMaterial::*;

//  - KEPT where the later material names no map for the slot: a preset that re-colours a zone
//    without naming its diffuse map leaves the map the zone had.
const MapMember kMapsKeptWhenAbsent[] = {
    &FigureMaterial::diffuseMapPath,      &FigureMaterial::opacityMapPath,
    &FigureMaterial::roughnessMapPath,    &FigureMaterial::specMaskMapPath,
    &FigureMaterial::translucencyMapPath, &FigureMaterial::translucencyWeightMapPath,
    &FigureMaterial::emissionMapPath,     &FigureMaterial::diffuseRoughnessMapPath,
};

//  - REPLACED — the later material's alone, so cleared where it names none. (Which slot follows
//    which rule is how each was first written, kept as found: making them one rule would change
//    what a twice-applied zone ends up with, which is a question of behaviour, not of structure.)
const MapMember kMapsReplaced[] = {
    &FigureMaterial::coatWeightMapPath,        &FigureMaterial::coatColorMapPath,
    &FigureMaterial::coatRoughnessMapPath,     &FigureMaterial::coatReflectivityMapPath,
    &FigureMaterial::coatBumpMapPath,          &FigureMaterial::coatAnisotropyMapPath,
    &FigureMaterial::coatAnisotropyRotationMapPath,
    &FigureMaterial::specularColorMapPath,     &FigureMaterial::anisotropyMapPath,
    &FigureMaterial::anisotropyRotationMapPath, &FigureMaterial::lobe2RoughnessMapPath,
    &FigureMaterial::lobeRatioMapPath,         &FigureMaterial::displacementMapPath,
    &FigureMaterial::transmissionColorMapPath, &FigureMaterial::transmissionWeightMapPath,
    &FigureMaterial::sheenWeightMapPath,       &FigureMaterial::sheenColorMapPath,
    &FigureMaterial::sheenRoughnessMapPath,    &FigureMaterial::thinFilmThicknessMapPath,
};

} // namespace

std::unordered_map<std::string, std::string> sceneMaterialUvSets(const nlohmann::json& root,
                                                                 const FigureScope& scope) {
    std::unordered_map<std::string, std::string> out;
    const auto scene = root.find("scene");
    if (scene == root.end()) {
        return out;
    }
    const auto materials = scene->find("materials");
    if (materials == scene->end() || !materials->is_array()) {
        return out;
    }
    for (const auto& mat : *materials) {
        if (!mat.is_object() || !scope.owns(stringOf(mat, "geometry"))) {
            continue; // another figure's material (its zone may share this figure's zone's name)
        }
        const auto uvSet = mat.find("uv_set");
        if (uvSet == mat.end() || !uvSet->is_string()) {
            continue;
        }
        const std::string uri = uvSet->get<std::string>();
        const auto groups = mat.find("groups");
        if (uri.empty() || groups == mat.end() || !groups->is_array()) {
            continue;
        }
        for (const auto& zone : *groups) {
            if (zone.is_string()) {
                out[zone.get<std::string>()] = uri; // the LAST material naming a zone wins, as the appliers' do
            }
        }
    }
    return out;
}

void applySceneMaterials(FigureData& fig, const nlohmann::json& root, UriResolver& resolver,
                         const std::string& referringDir, const FigureScope* scope,
                         bool followerPreset) {
    if (!root.contains("scene") || !root["scene"].contains("materials")) {
        return;
    }
    static const nlohmann::json kNoLibrary = nlohmann::json::array();
    const nlohmann::json& imageLibrary =
        root.contains("image_library") ? root["image_library"] : kNoLibrary;

    // parseMaterials merges each scene material with the base it extends by url, but only within
    // the same file ("#id"). Newer skin materials extend a CROSS-FILE base
    // ("/data/.../#<SkinShaderBase>") whose channel defaults would otherwise be lost (the old
    // "roughness falls back to default" limitation). Pre-resolve those here: load the referenced
    // document (cached by the resolver), hand its base material to parseMaterials as an EXTRA
    // base, and record for the scene material the key that base is filed under — without copying
    // the scene materials or the material library (both were once deep-copied and mutated; the
    // overrides map replaces that). The key is the base's FILE and id together: filed by the id
    // alone, a cross-file base was shadowed by an entry of the same id in the preset's own
    // material_library, and two files' bases of one id were taken for each other.
    const nlohmann::json& sceneMaterials = root["scene"]["materials"];
    const nlohmann::json* libraryPtr = &kNoLibrary;
    if (const auto lib = root.find("material_library"); lib != root.end() && lib->is_array()) {
        libraryPtr = &(*lib);
    }
    MaterialBaseOverrides overrides;
    overrides.baseIdsAsWritten = followerPreset; // see scenematerials.h
    std::vector<std::shared_ptr<const FigureDocument>> baseDocs; // keep the referenced documents alive
    std::unordered_set<std::string> importedBases; // base keys already looked up (found or not)
    if (sceneMaterials.is_array()) {
        for (const auto& mat : sceneMaterials) {
            if (scope && mat.is_object() && !scope->owns(stringOf(mat, "geometry"))) {
                overrides.skip.insert(&mat); // another figure's material
                continue;
            }
            const std::string url = stringOf(mat, "url");
            if (url.size() < 2 || url[0] == '#') {
                continue; // same-file (or no) base — parseMaterials handles it directly
            }
            try {
                const ResolvedUri ru = resolver.resolve(url, referringDir);
                if (!ru.resolved() || ru.fragment.empty()) {
                    continue;
                }
                // A base is looked up once; its key counts as visited even if that load fails, so
                // it isn't retried per material.
                const std::string key = ru.path + '#' + ru.fragment;
                if (importedBases.insert(key).second) {
                    const std::shared_ptr<const FigureDocument> baseDoc = resolver.loadDocument(ru);
                    baseDocs.push_back(baseDoc);
                    if (const auto ml = baseDoc->root().find("material_library");
                        ml != baseDoc->root().end() && ml->is_array()) {
                        for (const auto& baseMat : *ml) {
                            if (stringOf(baseMat, "id") == ru.fragment) {
                                overrides.extraBases.emplace(key, &baseMat);
                                break;
                            }
                        }
                    }
                }
                overrides.crossFileBase.emplace(&mat, key);
            } catch (const std::exception&) {
                // Unresolvable base: the scene material still parses from its own overrides alone.
            }
        }
    }

    const std::unordered_map<std::string, FigureMaterial> parsed =
        parseMaterials(sceneMaterials, *libraryPtr, imageLibrary, &overrides, &fig.skipped);
    for (FigureMesh& mesh : fig.meshes) {
        const auto it = parsed.find(mesh.materialZone);
        if (it == parsed.end()) {
            continue;
        }
        // Every VALUE of the parsed material replaces the zone's; its maps arrive as the URIs the
        // file wrote and are resolved here, each meeting the zone's earlier map by its slot's
        // rule (see kMapsKeptWhenAbsent / kMapsReplaced).
        const FigureMaterial& earlier = mesh.material;
        FigureMaterial        material = it->second;
        const auto resolveInPlace = [&](MapMember map) {
            std::string& uri = material.*map;
            if (!uri.empty()) {
                uri = resolver.resolve(uri, referringDir).path; // (empty again where nothing on disk matches)
            }
        };
        for (const MapMember map : kMapsReplaced) {
            resolveInPlace(map);
        }
        for (const MapMember map : kMapsKeptWhenAbsent) {
            if ((material.*map).empty()) {
                material.*map = earlier.*map;
            } else {
                resolveInPlace(map);
            }
        }
        // The normal and the bump map are kept likewise, each WITH its strength: a strength
        // dialed on a channel that carries no map says nothing about the zone's earlier map.
        if (material.normalMapPath.empty()) {
            material.normalMapPath = earlier.normalMapPath;
            material.normalStrength = earlier.normalStrength;
        } else {
            resolveInPlace(&FigureMaterial::normalMapPath);
        }
        if (material.bumpMapPath.empty()) {
            material.bumpMapPath = earlier.bumpMapPath;
            material.bumpStrength = earlier.bumpStrength;
        } else {
            resolveInPlace(&FigureMaterial::bumpMapPath);
        }
        // The detail layer is kept or replaced WHOLE, by whether this material has its normal map
        // (the parser reads the layer's values only then).
        if (material.detailNormalMapPath.empty()) {
            material.detailNormalMapPath = earlier.detailNormalMapPath;
            material.detailWeight = earlier.detailWeight;
            material.detailTiles = earlier.detailTiles;
            material.detailTilesV = earlier.detailTilesV;
            material.detailOffset = earlier.detailOffset;
            material.detailRoughness = earlier.detailRoughness;
            material.detailWeightMapPath = earlier.detailWeightMapPath;
            material.detailRoughnessMapPath = earlier.detailRoughnessMapPath;
        } else {
            resolveInPlace(&FigureMaterial::detailNormalMapPath);
            resolveInPlace(&FigureMaterial::detailWeightMapPath);
            resolveInPlace(&FigureMaterial::detailRoughnessMapPath);
        }
        mesh.material = std::move(material);
    }
}

// Unlike a scene-materials document, the hierarchical preset form carries its values as
// single-key animation tracks — each entry's url addresses one material property:
//   "<node>#materials/<zone>:?diffuse/image_file"
//   "<node>#materials/<zone>:?extra/studio_material_channels/channels/<Channel>/value"
// with keys[0][1] holding the value. Only the properties the renderer consumes are read; the rest
// (tiling, gloss layering, …) are skipped. Zones are matched by name; the node prefix is ignored
// for a preset applied to the follower whose own slot lists it (within one figure-plus-addons
// import, zone names don't collide in practice), and is REQUIRED to name the figure's node when
// the preset comes from another slot (onlyFigureNode) — a hierarchical preset carries several
// followers' materials side by side, each under its own node.
void applyAnimationMaterials(FigureData& fig, const nlohmann::json& root, UriResolver& resolver,
                             const std::string& referringDir, bool onlyFigureNode) {
    const nlohmann::json* animations = childOf(childOf(&root, "scene"), "animations");
    if (!animations || !animations->is_array()) {
        return;
    }

    struct ZoneOverride {
        glm::vec3   baseColor{0.0f};
        std::string diffuseUri, normalUri, bumpUri, opacityUri;
        float       roughness = 0.0f;
        float       cutout = 1.0f;
        float       refraction = 0.0f;
        bool        hasBaseColor = false, hasRoughness = false, hasCutout = false,
                    hasRefraction = false;
    };
    std::unordered_map<std::string, ZoneOverride> zones;

    for (const auto& entry : *animations) {
        const std::string url = stringOf(entry, "url");
        const auto keys = entry.find("keys");
        if (keys == entry.end() || !keys->is_array() || keys->empty() || !(*keys)[0].is_array() ||
            (*keys)[0].size() < 2) {
            continue;
        }
        const nlohmann::json& value = (*keys)[0][1];

        // "<node>#materials/<zone>:?<property>" -> zone + property (both URL-encoded).
        const std::size_t hash = url.find('#');
        if (hash == std::string::npos) {
            continue;
        }
        if (onlyFigureNode) {
            // The node prefix is the node's id or name, URL-encoded ("Some%20Figure%20Tear"); a
            // selection form ("name://@selection/<node>:") is reduced to the node. A track with
            // no prefix at all is left to the zone match, as before.
            std::string node = urlDecode(url.substr(0, hash));
            if (node.rfind("name://", 0) == 0) {
                node = node.substr(node.find_last_of('/') + 1);
                if (!node.empty() && node.back() == ':') {
                    node.pop_back();
                }
            }
            if (!node.empty() && node != fig.figureNodeId && node != fig.figureNodeName) {
                continue;
            }
        }
        std::string rest = url.substr(hash + 1);
        constexpr const char* kMaterialsPrefix = "materials/";
        if (rest.rfind(kMaterialsPrefix, 0) != 0) {
            continue;
        }
        rest = rest.substr(std::string(kMaterialsPrefix).size());
        const std::size_t sep = rest.find(":?");
        if (sep == std::string::npos) {
            continue;
        }
        const std::string zone = urlDecode(rest.substr(0, sep));
        const std::string prop = urlDecode(rest.substr(sep + 2));
        ZoneOverride& zo = zones[zone];

        constexpr const char* kChannelsPrefix = "extra/studio_material_channels/channels/";
        if (prop == "diffuse/value" && value.is_array() && value.size() >= 3 && value[0].is_number() &&
            value[1].is_number() && value[2].is_number()) {
            zo.baseColor = glm::vec3(value[0].get<float>(), value[1].get<float>(),
                                     value[2].get<float>());
            zo.hasBaseColor = true;
        } else if (prop == "diffuse/image_file" && value.is_string()) {
            zo.diffuseUri = value.get<std::string>();
        } else if (prop == "transparency/value" && value.is_number()) {
            zo.cutout = value.get<float>(); // legacy core opacity property (see parseMaterials)
            zo.hasCutout = true;
        } else if (prop == "transparency/image_file" && value.is_string()) {
            zo.opacityUri = value.get<std::string>();
        } else if (prop.rfind(kChannelsPrefix, 0) == 0) {
            const std::string channel = prop.substr(std::string(kChannelsPrefix).size());
            if (channel == "Cutout Opacity/value" && value.is_number()) {
                zo.cutout = value.get<float>();
                zo.hasCutout = true;
            } else if (channel == "Cutout Opacity/image_file" && value.is_string()) {
                zo.opacityUri = value.get<std::string>();
            } else if (channel == "Refraction Weight/value" && value.is_number()) {
                zo.refraction = value.get<float>();
                zo.hasRefraction = true;
            } else if (channel == "Glossy Roughness/value" && value.is_number()) {
                zo.roughness = value.get<float>();
                zo.hasRoughness = true;
            } else if (channel == "Normal Map/image_file" && value.is_string()) {
                zo.normalUri = value.get<std::string>();
            } else if (channel == "Bump Strength/image_file" && value.is_string()) {
                zo.bumpUri = value.get<std::string>();
            }
        }
    }

    for (FigureMesh& mesh : fig.meshes) {
        const auto it = zones.find(mesh.materialZone);
        if (it == zones.end()) {
            continue;
        }
        const ZoneOverride& zo = it->second;
        if (zo.hasBaseColor) {
            mesh.material.baseColor = zo.baseColor;
        }
        if (zo.hasRoughness) {
            mesh.material.roughness = zo.roughness;
        }
        // Same transparency rule as parseMaterials: cutout is the opacity, and a strong refraction
        // weight marks a clear shell (here: the anime eye's lens over the iris card).
        if (zo.hasCutout || zo.hasRefraction) {
            mesh.material.opacity = zo.hasCutout ? zo.cutout : 1.0f;
            if (zo.refraction > 0.5f) {
                mesh.material.opacity = std::min(mesh.material.opacity, 0.05f);
            }
        }
        if (!zo.diffuseUri.empty()) {
            mesh.material.diffuseMapPath = resolver.resolve(zo.diffuseUri, referringDir).path;
        }
        if (!zo.normalUri.empty()) {
            mesh.material.normalMapPath = resolver.resolve(zo.normalUri, referringDir).path;
        }
        if (!zo.bumpUri.empty()) {
            mesh.material.bumpMapPath = resolver.resolve(zo.bumpUri, referringDir).path;
        }
        if (!zo.opacityUri.empty()) {
            mesh.material.opacityMapPath = resolver.resolve(zo.opacityUri, referringDir).path;
        }
    }
}

} // namespace pose
