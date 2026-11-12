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
#include <vector>

namespace pose {

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
        if (!mat.is_object() || !scope.owns(mat.value("geometry", std::string()))) {
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
    // base, and record the scene material's url as the same-file "#fragment" form parseMaterials
    // handles — without copying the scene materials or the material library (both were once
    // deep-copied and mutated; the overrides map replaces that).
    const nlohmann::json& sceneMaterials = root["scene"]["materials"];
    const nlohmann::json* libraryPtr = &kNoLibrary;
    if (const auto lib = root.find("material_library"); lib != root.end() && lib->is_array()) {
        libraryPtr = &(*lib);
    }
    MaterialBaseOverrides overrides;
    overrides.baseIdsAsWritten = followerPreset; // see scenematerials.h
    std::vector<std::shared_ptr<const FigureDocument>> baseDocs; // keep the referenced documents alive
    std::unordered_set<std::string> importedBases; // fragments already looked up (found or not)
    if (sceneMaterials.is_array()) {
        for (const auto& mat : sceneMaterials) {
            if (scope && mat.is_object() && !scope->owns(mat.value("geometry", std::string()))) {
                overrides.skip.insert(&mat); // another figure's material
                continue;
            }
            const std::string url = mat.value("url", std::string());
            if (url.size() < 2 || url[0] == '#') {
                continue; // same-file (or no) base — parseMaterials handles it directly
            }
            try {
                const ResolvedUri ru = resolver.resolve(url, referringDir);
                if (!ru.resolved() || ru.fragment.empty()) {
                    continue;
                }
                // The FIRST document naming a fragment supplies its base; the fragment counts as
                // visited even if that load fails, so it isn't retried per material.
                if (importedBases.insert(ru.fragment).second) {
                    const std::shared_ptr<const FigureDocument> baseDoc = resolver.loadDocument(ru);
                    baseDocs.push_back(baseDoc);
                    if (const auto ml = baseDoc->root().find("material_library");
                        ml != baseDoc->root().end() && ml->is_array()) {
                        for (const auto& baseMat : *ml) {
                            if (baseMat.value("id", std::string()) == ru.fragment) {
                                overrides.extraBases.emplace(ru.fragment, &baseMat);
                                break;
                            }
                        }
                    }
                }
                overrides.urlRewrites.emplace(&mat, "#" + ru.fragment);
            } catch (const std::exception&) {
                // Unresolvable base: the scene material still parses from its own overrides alone.
            }
        }
    }

    const std::unordered_map<std::string, MaterialRefs> refs =
        parseMaterials(sceneMaterials, *libraryPtr, imageLibrary, &overrides);
    for (FigureMesh& mesh : fig.meshes) {
        const auto it = refs.find(mesh.materialZone);
        if (it == refs.end()) {
            continue;
        }
        const MaterialRefs& ref = it->second;
        mesh.material.baseColor = ref.baseColor;
        mesh.material.roughness = ref.roughness;
        mesh.material.specularWeight = ref.specularWeight;
        mesh.material.specularWeightWithMap = ref.specularWeightWithMap;
        mesh.material.metallic = ref.metallic;
        mesh.material.lobe1Roughness = ref.lobe1Roughness;
        mesh.material.lobe2Roughness = ref.lobe2Roughness;
        mesh.material.lobeRatio = ref.lobeRatio;
        mesh.material.topCoatWeight = ref.topCoatWeight;
        mesh.material.topCoatEnabled = ref.topCoatEnabled;
        mesh.material.topCoatRoughness = ref.topCoatRoughness;
        mesh.material.translucencyWeight = ref.translucencyWeight;
        mesh.material.opacity = ref.opacity;
        // The canonical material's further channels (materialparser.h).
        mesh.material.emissionColor = ref.emissionColor;
        mesh.material.emission = ref.emission;
        if (!ref.emissionImageUri.empty()) {
            mesh.material.emissionMapPath = resolver.resolve(ref.emissionImageUri, referringDir).path;
        }
        mesh.material.ior = ref.ior;
        mesh.material.transmission = ref.transmission;
        mesh.material.transmissionColor = ref.transmissionColor;
        mesh.material.transmissionDepth = ref.transmissionDepth;
        mesh.material.transmissionRoughness = ref.transmissionRoughness;
        mesh.material.thinWalled = ref.thinWalled;
        mesh.material.subsurfaceColor = ref.subsurfaceColor;
        mesh.material.subsurfaceRadiusColor = ref.subsurfaceRadiusColor;
        mesh.material.subsurfaceRadius = ref.subsurfaceRadius;
        mesh.material.subsurfaceAnisotropy = ref.subsurfaceAnisotropy;
        mesh.material.anisotropy = ref.anisotropy;
        mesh.material.anisotropyRotation = ref.anisotropyRotation;
        mesh.material.diffuseRoughness = ref.diffuseRoughness;
        if (!ref.diffuseRoughnessImageUri.empty()) {
            mesh.material.diffuseRoughnessMapPath = resolver.resolve(ref.diffuseRoughnessImageUri, referringDir).path;
        }
        mesh.material.coatColor = ref.coatColor;
        mesh.material.coatIor = ref.coatIor;
        const auto mapPath = [&](const std::string& uri) { return uri.empty() ? std::string() : resolver.resolve(uri, referringDir).path; };
        mesh.material.coatWeightMapPath = mapPath(ref.coatWeightImageUri);
        mesh.material.coatColorMapPath = mapPath(ref.coatColorImageUri);
        mesh.material.coatRoughnessMapPath = mapPath(ref.coatRoughnessImageUri);
        mesh.material.coatReflectivityMapPath = mapPath(ref.coatReflectivityImageUri);
        mesh.material.coatBumpMapPath = mapPath(ref.coatBumpImageUri);
        mesh.material.coatBumpStrength = ref.coatBumpStrength;
        mesh.material.specularColorMapPath = mapPath(ref.specularColorImageUri);
        mesh.material.displacementMapPath = mapPath(ref.displacementImageUri);
        mesh.material.transmissionColorMapPath = mapPath(ref.transmissionColorImageUri);
        mesh.material.transmissionWeightMapPath = mapPath(ref.transmissionWeightImageUri);
        mesh.material.coatAnisotropy = ref.coatAnisotropy;
        mesh.material.coatAnisotropyRotation = ref.coatAnisotropyRotation;
        mesh.material.coatAnisotropyMapPath = mapPath(ref.coatAnisotropyImageUri);
        mesh.material.coatAnisotropyRotationMapPath = mapPath(ref.coatAnisotropyRotationImageUri);
        mesh.material.sheen = ref.sheen;
        mesh.material.sheenColor = ref.sheenColor;
        mesh.material.sheenRoughness = ref.sheenRoughness;
        mesh.material.sheenWeightMapPath = mapPath(ref.sheenWeightImageUri);
        mesh.material.sheenColorMapPath = mapPath(ref.sheenColorImageUri);
        mesh.material.sheenRoughnessMapPath = mapPath(ref.sheenRoughnessImageUri);
        mesh.material.thinFilm = ref.thinFilm;
        mesh.material.thinFilmThickness = ref.thinFilmThickness;
        mesh.material.thinFilmIor = ref.thinFilmIor;
        mesh.material.thinFilmThicknessMapPath = mapPath(ref.thinFilmThicknessImageUri);
        mesh.material.displacementStrength = ref.displacementStrength;
        mesh.material.displacementMidLevel = ref.displacementMidLevel;
        mesh.material.displacementSubdiv = ref.displacementSubdiv;
        mesh.material.anisotropyMapPath = mapPath(ref.anisotropyImageUri);
        mesh.material.anisotropyRotationMapPath = mapPath(ref.anisotropyRotationImageUri);
        mesh.material.lobe2RoughnessMapPath = mapPath(ref.lobe2RoughnessImageUri);
        mesh.material.lobeRatioMapPath = mapPath(ref.lobeRatioImageUri);
        mesh.material.uvTiling = ref.uvTiling;
        mesh.material.uvOffset = ref.uvOffset;
        if (!ref.roughnessImageUri.empty()) {
            mesh.material.roughnessMapPath = resolver.resolve(ref.roughnessImageUri, referringDir).path;
        }
        if (!ref.specWeightImageUri.empty()) {
            mesh.material.specMaskMapPath = resolver.resolve(ref.specWeightImageUri, referringDir).path;
        }
        if (!ref.translucencyImageUri.empty()) {
            mesh.material.translucencyMapPath =
                resolver.resolve(ref.translucencyImageUri, referringDir).path;
        }
        if (!ref.translucencyWeightImageUri.empty()) {
            mesh.material.translucencyWeightMapPath = resolver.resolve(ref.translucencyWeightImageUri, referringDir).path;
        }
        if (!ref.detailNormalImageUri.empty()) {
            mesh.material.detailNormalMapPath =
                resolver.resolve(ref.detailNormalImageUri, referringDir).path;
            mesh.material.detailWeight = ref.detailWeight;
            mesh.material.detailTiles = ref.detailTiles;
            mesh.material.detailTilesV = ref.detailTilesV;
            mesh.material.detailOffset = ref.detailOffset;
            mesh.material.detailRoughness = ref.detailRoughness;
            mesh.material.detailWeightMapPath = mapPath(ref.detailWeightImageUri);
            mesh.material.detailRoughnessMapPath = mapPath(ref.detailRoughnessImageUri);
        }
        if (!ref.diffuseImageUri.empty()) {
            mesh.material.diffuseMapPath = resolver.resolve(ref.diffuseImageUri, referringDir).path;
        }
        if (!ref.normalImageUri.empty()) {
            mesh.material.normalMapPath = resolver.resolve(ref.normalImageUri, referringDir).path;
            mesh.material.normalStrength = ref.normalStrength;
        }
        if (!ref.bumpImageUri.empty()) {
            mesh.material.bumpMapPath = resolver.resolve(ref.bumpImageUri, referringDir).path;
            mesh.material.bumpStrength = ref.bumpStrength;
        }
        if (!ref.opacityImageUri.empty()) {
            mesh.material.opacityMapPath = resolver.resolve(ref.opacityImageUri, referringDir).path;
        }
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
        const std::string url = entry.value("url", std::string());
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
            // The node prefix is the node's id or name, URL-encoded ("Female%208_1%20Tear"); a
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
        if (prop == "diffuse/value" && value.is_array() && value.size() >= 3) {
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
