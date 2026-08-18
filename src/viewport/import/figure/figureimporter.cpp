/**
 * @file figureimporter.cpp
 * @brief Implementation of FigureImporter. See figureimporter.h.
 */

#include "figureimporter.h"

#include "correctiveparser.h"
#include "figuredocument.h"
#include "geometryparser.h"
#include "materialparser.h"
#include "morphparser.h"
#include "morphresolver.h"
#include "nodeparser.h"
#include "skinparser.h"
#include "parallelfor.h"
#include "subdivision.h"
#include "uriresolver.h"
#include "uvparser.h"

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace pose {

namespace {

std::string directoryOf(const std::string& path) {
    return std::filesystem::path(path).parent_path().string();
}

// Finds the geometry_library entry named @p fragment (or the first one) in @p doc.
const nlohmann::json* findGeometry(const nlohmann::json& doc, const std::string& fragment) {
    const auto it = doc.find("geometry_library");
    if (it == doc.end() || !it->is_array() || it->empty()) {
        return nullptr;
    }
    if (!fragment.empty()) {
        for (const auto& g : *it) {
            if (g.value("id", std::string()) == fragment) {
                return &g;
            }
        }
    }
    return &(*it)[0];
}

// The channel id (fragment after '#', before '?') of a driver url, ignoring a leading "Node:" alias.
std::string channelId(const std::string& url) {
    const std::size_t slash = url.find('/');
    const std::size_t colon = url.find(':');
    std::string rest = (colon != std::string::npos && (slash == std::string::npos || colon < slash))
                           ? url.substr(colon + 1)
                           : url;
    const std::size_t hash = rest.find('#');
    std::string frag = (hash == std::string::npos) ? rest : rest.substr(hash + 1);
    const std::size_t q = frag.find('?');
    return (q == std::string::npos) ? frag : frag.substr(0, q);
}

// Strips a leading "Node:" alias so the remaining path is root-relative for the resolver.
std::string stripAlias(const std::string& url) {
    const std::size_t slash = url.find('/');
    const std::size_t colon = url.find(':');
    return (colon != std::string::npos && (slash == std::string::npos || colon < slash))
               ? url.substr(colon + 1)
               : url;
}

// Discovers the figure's pose correctives (joint-driven corrective morphs) and returns the live ones.
//
// Correctives aren't listed by the preset — they're morph assets that auto-apply by convention. We
// find them structurally (a morph with a joint-rotation-driven formula), never by brand/file name
// (naming differs across figure generations). To keep the scan bounded we only inflate files in
// directories whose name marks them as corrective/flexion packs, plus the directories of the dialed
// character morphs (so a loaded character's own correctives come along). Each candidate modifier is
// confirmed by parseCorrective, which also folds its character/enable gates to constants and drops
// any that a gate switched off (e.g. a different character's correctives). @p dialed supplies both the
// character-morph directories to scan and the dialed weights that resolve those gates.
std::vector<PoseCorrective> discoverCorrectives(UriResolver& resolver, const std::string& baseDir,
                                                const std::unordered_set<std::string>& boneNames,
                                                const std::vector<DialedMorph>& dialed) {
    namespace fs = std::filesystem;

    // Gate resolver: a value-channel driver resolves to its dialed weight if the preset dials it,
    // else to the channel's own default (loaded once and cached). This makes base correctives (gated
    // by a default-on toggle) fire and a non-loaded character's correctives (gated by that character's
    // shape morph, default 0) drop — uniformly, without any figure-specific knowledge.
    std::unordered_map<std::string, float> dialedWeights;
    for (const DialedMorph& d : dialed) {
        dialedWeights[channelId(d.url)] = d.weight;
    }
    std::unordered_map<std::string, float> gateCache;

    CorrectiveContext ctx;
    ctx.boneNames = &boneNames;
    ctx.resolveValue = [&](const std::string& url) -> float {
        const std::string id = channelId(url);
        if (const auto it = dialedWeights.find(id); it != dialedWeights.end()) {
            return it->second;
        }
        if (const auto it = gateCache.find(id); it != gateCache.end()) {
            return it->second;
        }
        float def = 0.0f;
        try {
            const std::shared_ptr<const FigureDocument> doc = resolver.loadDocument(stripAlias(url), baseDir);
            if (const auto ml = doc->root().find("modifier_library");
                ml != doc->root().end() && ml->is_array()) {
                for (const auto& m : *ml) {
                    if (m.value("id", std::string()) == id) {
                        if (const auto ch = m.find("channel"); ch != m.end()) {
                            def = ch->value("value", 0.0f);
                        }
                        break;
                    }
                }
            }
        } catch (const std::exception&) {
            def = 0.0f;
        }
        gateCache[id] = def;
        return def;
    };

    // Candidate directories: corrective/flexion packs under <baseDir>/Morphs, plus each dialed morph's
    // own directory.
    std::vector<std::string> dirs;
    std::error_code ec;
    const fs::path morphsRoot = fs::path(baseDir) / "Morphs";
    if (fs::exists(morphsRoot, ec)) {
        for (fs::recursive_directory_iterator it(morphsRoot, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (!it->is_directory(ec)) {
                continue;
            }
            std::string name = it->path().filename().string();
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name.find("corrective") != std::string::npos || name.find("flexion") != std::string::npos) {
                dirs.push_back(it->path().string());
            }
        }
    }
    for (const DialedMorph& d : dialed) {
        const ResolvedUri ru = resolver.resolve(d.url, baseDir);
        if (ru.resolved()) {
            dirs.push_back(fs::path(ru.path).parent_path().string());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());

    // Gather the candidate files first, so the expensive part — file read + gzip inflate + JSON
    // parse, across hundreds of morph files — can run on all cores. The modifier walk stays serial:
    // parseCorrective itself is cheap, and its gate resolution goes through the resolver/caches
    // above, which aren't thread-safe.
    std::vector<std::string> files;
    for (const std::string& dir : dirs) {
        for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (!it->is_regular_file(ec) || it->path().extension() != ".dsf") {
                continue;
            }
            files.push_back(it->path().string());
        }
    }

    std::vector<PoseCorrective> correctives;
    std::unordered_set<std::string> seenIds;
    // Bounded chunks keep peak memory at ~a couple of parsed documents per core rather than the
    // whole candidate set at once (a corrective pack's parsed JSON is megabytes each).
    const unsigned hw = std::thread::hardware_concurrency();
    const std::size_t chunk = std::max<std::size_t>(1, static_cast<std::size_t>(hw > 1 ? hw : 1) * 2);
    std::vector<std::unique_ptr<FigureDocument>> docs;
    for (std::size_t base = 0; base < files.size(); base += chunk) {
        const std::size_t n = std::min(chunk, files.size() - base);
        docs.clear();
        docs.resize(n);
        parallelFor(static_cast<int>(n), [&](int i) {
            try {
                const std::string& file = files[base + static_cast<std::size_t>(i)];
                // Pre-filter before the (expensive) DOM parse: a corrective's driver formula always
                // references a joint rotation channel as the literal text "?rotation/" (the format
                // writes these urls unencoded — every parser here splits on the raw '?'). The
                // candidate dirs include each dialed morph's own folder, which can hold hundreds of
                // plain shape morphs; inflating is cheap, but DOM-parsing them all dominated the
                // corrective scan, and none can ever pass parseCorrective.
                std::ifstream in(file, std::ios::binary | std::ios::ate);
                if (!in) {
                    return;
                }
                const std::streamoff size = in.tellg();
                if (size <= 0) {
                    return;
                }
                std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
                in.seekg(0, std::ios::beg);
                if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
                    return;
                }
                if (isGzip(bytes)) {
                    bytes = gunzip(bytes.data(), bytes.size());
                }
                const std::string_view text(reinterpret_cast<const char*>(bytes.data()),
                                            bytes.size());
                if (text.find("?rotation/") == std::string_view::npos) {
                    return; // no joint-rotation driver anywhere — cannot be a corrective
                }
                docs[static_cast<std::size_t>(i)] = std::make_unique<FigureDocument>(
                    FigureDocument::loadFromBytes(std::move(bytes), file));
            } catch (const std::exception&) {
                // unreadable/!gzip/!json — left null, skipped below
            }
        });
        for (const std::unique_ptr<FigureDocument>& doc : docs) {
            if (!doc) {
                continue;
            }
            const auto ml = doc->root().find("modifier_library");
            if (ml == doc->root().end() || !ml->is_array()) {
                continue;
            }
            for (const auto& mod : *ml) {
                PoseCorrective pc;
                if (parseCorrective(mod, ctx, pc) && seenIds.insert(pc.id).second) {
                    correctives.push_back(std::move(pc));
                }
            }
        }
    }
    return correctives;
}

// Scans a preset's scene.nodes for the figure instance and returns its geometry URI ("…dsf#geometry").
std::string findGeometryUri(const nlohmann::json& root) {
    const auto scene = root.find("scene");
    if (scene == root.end() || !scene->contains("nodes")) {
        return {};
    }
    for (const auto& node : (*scene)["nodes"]) {
        const auto geos = node.find("geometries");
        if (geos != node.end() && geos->is_array() && !geos->empty()) {
            const std::string uri = (*geos)[0].value("url", std::string());
            if (!uri.empty()) {
                return uri;
            }
        }
    }
    return {};
}

// Builds a bone's rest-orientation matrix from its Euler orientation (degrees), composed X·Y·Z — the
// same frame the poser rotates in (mesh.cpp's eulerMatrix(orientation, "XYZ")). Since the bind
// transform is translation-only, this matrix's columns are the bone's three world-space rotation-
// channel axes (the axes its pose Euler angles rotate about).
glm::mat3 orientationAxes(const glm::vec3& degrees) {
    const glm::mat4 m =
        glm::rotate(glm::mat4(1.0f), glm::radians(degrees.x), glm::vec3(1.0f, 0.0f, 0.0f)) *
        glm::rotate(glm::mat4(1.0f), glm::radians(degrees.y), glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::rotate(glm::mat4(1.0f), glm::radians(degrees.z), glm::vec3(0.0f, 0.0f, 1.0f));
    return glm::mat3(m);
}

// A "twist" bone (lThighTwist, rForearmTwist, …) sits partway along a limb — mid-femur, mid-forearm —
// where a human has no joint; it exists only to spread the limb's axial twist across the skin. It
// should rotate about its own length (the twist) but must never bend, or the limb folds mid-bone where
// nothing can. Figures leave such a bone's bend axes unconstrained, so we constrain them here: find the
// rotation-channel axis most aligned with the bone's length (its twist axis) and lock the other two to
// zero. Detection is by the "twist" name convention (stable across figure generations) plus a child
// bone to define the length direction; a bone missing either is left untouched. The runtime's existing
// clampBoneEuler() then enforces the lock across every posing path (gizmo, drag, loaded pose).
void lockTwistBoneBendAxes(std::vector<FigureBone>& bones) {
    std::vector<int> firstChild(bones.size(), -1);
    for (int i = 0; i < static_cast<int>(bones.size()); ++i) {
        const int p = bones[static_cast<std::size_t>(i)].parent;
        if (p >= 0 && p < static_cast<int>(bones.size()) && firstChild[static_cast<std::size_t>(p)] < 0) {
            firstChild[static_cast<std::size_t>(p)] = i;
        }
    }
    for (std::size_t i = 0; i < bones.size(); ++i) {
        FigureBone& bone = bones[i];
        std::string lowered = bone.name;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered.find("twist") == std::string::npos) {
            continue; // not a twist bone
        }
        // If the figure already pins an axis via the format's `locked` channel flag (read by
        // nodeparser), trust that authored data over this geometric fallback — re-deriving the
        // twist axis here could disagree with it and kill the legitimate twist channel. This
        // heuristic only remains for old figure generations that don't write `locked` at all.
        bool formatLocked = false;
        for (int a = 0; a < 3; ++a) {
            if (bone.rotationLimited[a] && bone.rotationMin[a] == bone.rotationMax[a]) {
                formatLocked = true;
                break;
            }
        }
        if (formatLocked) {
            continue;
        }
        const int child = firstChild[i];
        if (child < 0) {
            continue; // no child to define the bone's length direction
        }
        const glm::vec3 along = bones[static_cast<std::size_t>(child)].origin - bone.origin;
        if (glm::dot(along, along) < 1e-12f) {
            continue; // degenerate: child coincides with this joint
        }
        const glm::vec3 dir = glm::normalize(along);
        const glm::mat3 axes = orientationAxes(bone.orientation);
        int   twistAxis = 0;
        float bestAlignment = -1.0f;
        for (int a = 0; a < 3; ++a) {
            const float alignment = std::abs(glm::dot(dir, glm::normalize(axes[a])));
            if (alignment > bestAlignment) {
                bestAlignment = alignment;
                twistAxis = a;
            }
        }
        for (int a = 0; a < 3; ++a) {
            if (a == twistAxis) {
                continue; // keep the twist axis as the figure defined it
            }
            bone.rotationMin[a] = 0.0f;
            bone.rotationMax[a] = 0.0f;
            bone.rotationLimited[a] = true; // clamped to [0,0] => no bend
        }
    }
}

// Nested-object navigation for the addon settings tree (returns nullptr anywhere along a miss).
const nlohmann::json* childOf(const nlohmann::json* node, const char* key) {
    if (!node) {
        return nullptr;
    }
    const auto it = node->find(key);
    return it == node->end() ? nullptr : &(*it);
}

// One follower addon a character preset asks to be loaded after it: the wearable figure file plus
// any preset documents to apply onto it (usually one materials preset).
struct AddonRef {
    std::string              assetUri;
    std::vector<std::string> presetUris;
};

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
        if (block.value("type", std::string()) != "scene_post_load_script") {
            continue;
        }
        const nlohmann::json* addons =
            childOf(childOf(childOf(&block, "settings"), "PostLoadAddons"), "value");
        if (!addons || !addons->is_object()) {
            continue;
        }
        for (const auto& item : addons->items()) {
            const nlohmann::json* v = childOf(&item.value(), "value");
            if (!v) {
                continue;
            }
            AddonRef ref;
            ref.assetUri = rootRelative(v->value("AssetFile", std::string()));
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

// Applies a document's scene materials onto the assembled zone meshes: each zone the scene names
// gets its resolved colors/maps/opacity, other zones keep what they have. Shared by the main figure
// path (the preset's own materials) and the addon path (a follower's separate materials preset).
void applySceneMaterials(FigureData& fig, const nlohmann::json& root, UriResolver& resolver,
                         const std::string& referringDir) {
    if (!root.contains("scene") || !root["scene"].contains("materials")) {
        return;
    }
    static const nlohmann::json kNoLibrary = nlohmann::json::array();
    const nlohmann::json& imageLibrary =
        root.contains("image_library") ? root["image_library"] : kNoLibrary;

    // parseMaterials merges each scene material with the base it extends by url, but only within
    // the same file ("#id"). Newer skin materials extend a CROSS-FILE base
    // ("/data/.../#PBRSkin") whose channel defaults would otherwise be lost (the old
    // "roughness falls back to default" limitation). Pre-resolve those here: load the referenced
    // document (cached by the resolver), append its base material to an augmented library, and
    // rewrite the scene material's url to the same-file "#fragment" form parseMaterials handles.
    nlohmann::json sceneMaterials = root["scene"]["materials"]; // copy — urls are rewritten below
    nlohmann::json library =
        root.contains("material_library") ? root["material_library"] : nlohmann::json::array();
    if (!library.is_array()) {
        library = nlohmann::json::array();
    }
    std::unordered_set<std::string> importedBases; // fragments already appended
    for (auto& mat : sceneMaterials) {
        const std::string url = mat.value("url", std::string());
        if (url.size() < 2 || url[0] == '#') {
            continue; // same-file (or no) base — parseMaterials handles it directly
        }
        try {
            const ResolvedUri ru = resolver.resolve(url, referringDir);
            if (!ru.resolved() || ru.fragment.empty()) {
                continue;
            }
            if (importedBases.insert(ru.fragment).second) {
                const std::shared_ptr<const FigureDocument> baseDoc =
                    resolver.loadDocument(url, referringDir);
                if (const auto ml = baseDoc->root().find("material_library");
                    ml != baseDoc->root().end() && ml->is_array()) {
                    for (const auto& baseMat : *ml) {
                        if (baseMat.value("id", std::string()) == ru.fragment) {
                            library.push_back(baseMat);
                            break;
                        }
                    }
                }
            }
            mat["url"] = "#" + ru.fragment;
        } catch (const std::exception&) {
            // Unresolvable base: the scene material still parses from its own overrides alone.
        }
    }

    const std::unordered_map<std::string, MaterialRefs> refs =
        parseMaterials(sceneMaterials, library, imageLibrary);
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
        mesh.material.topCoatRoughness = ref.topCoatRoughness;
        mesh.material.translucencyWeight = ref.translucencyWeight;
        mesh.material.opacity = ref.opacity;
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
        if (!ref.detailNormalImageUri.empty()) {
            mesh.material.detailNormalMapPath =
                resolver.resolve(ref.detailNormalImageUri, referringDir).path;
            mesh.material.detailWeight = ref.detailWeight;
            mesh.material.detailTiles = ref.detailTiles;
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

// Applies a hierarchical-materials preset (asset_info type "preset_hierarchical_material") onto the
// assembled zone meshes. Unlike a scene-materials document, this preset form carries its values as
// single-key animation tracks — each entry's url addresses one material property:
//   "<node>#materials/<zone>:?diffuse/image_file"
//   "<node>#materials/<zone>:?extra/studio_material_channels/channels/<Channel>/value"
// with keys[0][1] holding the value. Only the properties the renderer consumes are read; the rest
// (tiling, gloss layering, …) are skipped. Zones are matched by name alone (the node prefix is
// ignored) — within one figure-plus-addons import, zone names don't collide in practice.
void applyAnimationMaterials(FigureData& fig, const nlohmann::json& root, UriResolver& resolver,
                             const std::string& referringDir) {
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
        const std::string zone = UriResolver::urlDecode(rest.substr(0, sep));
        const std::string prop = UriResolver::urlDecode(rest.substr(sep + 2));
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

// Auto-follow (shape projection) for follower addons: a follower is authored against the BASE
// figure, but the character's morphs have moved that surface — a skin-tight follower must move
// with it or it hangs where the base surface used to be (fibermesh eyebrows floating in front of
// a morphed forehead). The format's host projects the parent's active morphs onto conformed
// followers at load; this approximates that projection per render vertex with the morph delta of
// the NEAREST PARENT BASE-CAGE VERTEX — the cage is dense (~16k vertices, sub-centimetre spacing
// on the face), so the quantization is invisible for surface-hugging followers, and a follower's
// off-surface parts (strand tips) inherit their root's delta, preserving strand shape.
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
    const auto cellKey = [&](const glm::vec3& p) {
        const glm::ivec3 c = glm::ivec3(glm::floor((p - lo) / kCell));
        return (static_cast<int64_t>(c.x) << 42) ^ (static_cast<int64_t>(c.y) << 21) ^
               static_cast<int64_t>(c.z);
    };
    std::unordered_map<int64_t, std::vector<uint32_t>> grid;
    for (uint32_t i = 0; i < baseCage.size(); ++i) {
        if (eligibleCageVerts.empty() || eligibleCageVerts[i]) {
            grid[cellKey(baseCage[i])].push_back(i);
        }
    }
    if (grid.empty()) {
        return;
    }
    const auto nearestDelta = [&](const glm::vec3& p) -> glm::vec3 {
        const glm::ivec3 center = glm::ivec3(glm::floor((p - lo) / kCell));
        float best = std::numeric_limits<float>::max();
        uint32_t bestIndex = 0;
        bool found = false;
        // Expand the search ring until a candidate appears, then one ring further so a neighbour
        // just across a cell boundary can't beat the first hit unseen.
        for (int radius = 1; radius <= 64 && !found; ++radius) {
            for (int dz = -radius; dz <= radius; ++dz) {
                for (int dy = -radius; dy <= radius; ++dy) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const glm::ivec3 c = center + glm::ivec3(dx, dy, dz);
                        const auto it = grid.find((static_cast<int64_t>(c.x) << 42) ^
                                                  (static_cast<int64_t>(c.y) << 21) ^
                                                  static_cast<int64_t>(c.z));
                        if (it == grid.end()) {
                            continue;
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
                    }
                }
            }
            if (found) {
                // One safety ring: rescan at radius+1 happens naturally by letting the loop run
                // once more with `found` already set — break out after that extra pass instead.
                for (int dz = -(radius + 1); dz <= radius + 1; ++dz) {
                    for (int dy = -(radius + 1); dy <= radius + 1; ++dy) {
                        for (int dx = -(radius + 1); dx <= radius + 1; ++dx) {
                            if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) <= radius) {
                                continue; // inner cells already scanned
                            }
                            const glm::ivec3 c = center + glm::ivec3(dx, dy, dz);
                            const auto it = grid.find((static_cast<int64_t>(c.x) << 42) ^
                                                      (static_cast<int64_t>(c.y) << 21) ^
                                                      static_cast<int64_t>(c.z));
                            if (it == grid.end()) {
                                continue;
                            }
                            for (const uint32_t i : it->second) {
                                const glm::vec3 d = baseCage[i] - p;
                                const float dist2 = glm::dot(d, d);
                                if (dist2 < best) {
                                    best = dist2;
                                    bestIndex = i;
                                }
                            }
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
void mergeAddonFigure(FigureData& parent, FigureData&& addon) {
    if (addon.meshes.empty()) {
        return;
    }
    std::unordered_map<std::string, int> parentIndexByName;
    for (int i = 0; i < static_cast<int>(parent.bones.size()); ++i) {
        parentIndexByName.emplace(parent.bones[i].name, i);
    }

    const int addonBoneCount = static_cast<int>(addon.bones.size());
    std::vector<int> mergedIndex(addonBoneCount, -1);
    // First pass: bones the parent already has resolve to the parent's own.
    for (int i = 0; i < addonBoneCount; ++i) {
        const auto it = parentIndexByName.find(addon.bones[i].name);
        if (it != parentIndexByName.end()) {
            mergedIndex[i] = it->second;
        }
    }
    // Second pass: append the follower-only bones (parseSkeleton emits parents before children, so a
    // parent link to another appended bone is already resolved when its child asks for it).
    for (int i = 0; i < addonBoneCount; ++i) {
        if (mergedIndex[i] >= 0) {
            continue;
        }
        FigureBone bone = addon.bones[i];
        bone.parent = (bone.parent >= 0) ? mergedIndex[bone.parent] : -1;
        mergedIndex[i] = static_cast<int>(parent.bones.size());
        parent.bones.push_back(std::move(bone));
    }

    // Keep the follower's render vertices out of the parent correctives' base-vertex space (corrective
    // deltas index base vertices; a colliding index would re-morph the follower's mesh on pose-settle).
    uint32_t baseVertexOffset = 0;
    for (const FigureMesh& mesh : parent.meshes) {
        for (const uint32_t b : mesh.baseVertex) {
            baseVertexOffset = std::max(baseVertexOffset, b + 1);
        }
    }

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
    // The follower's own correctives (rare, and addressed to ITS base cage) are dropped with `addon`:
    // their delta indices have no meaning in the merged base-vertex space.
}

// The full loader. `includeAddons` is true for the user-facing entry point and false for the
// follower-addon recursion (an addon must not pull in further addons — one level is what the
// format's addon loader does, and it also bounds the recursion).
FigureData loadFigureFile(const std::string& path, const std::vector<std::string>& contentRoots,
                          bool includeAddons) {
    UriResolver resolver(contentRoots);
    const std::string presetDir = directoryOf(path);

    FigureDocument presetDoc = FigureDocument::loadFromFile(path);
    const nlohmann::json& root = presetDoc.root();

    // Locate the base geometry. Either this file *is* a base .dsf (has geometry_library), or it's a
    // preset that references one via a scene node's geometries[0].url.
    const nlohmann::json* geomEntry = nullptr;
    std::shared_ptr<const FigureDocument> baseDoc; // keeps the referenced base file alive
    std::string baseDir = presetDir;

    if (root.contains("geometry_library")) {
        geomEntry = findGeometry(root, std::string());
    } else {
        const std::string geomUri = findGeometryUri(root);
        if (geomUri.empty()) {
            throw std::runtime_error("figure import: no geometry reference found in " + path);
        }
        const ResolvedUri ru = resolver.resolve(geomUri, presetDir);
        if (!ru.resolved()) {
            throw std::runtime_error("figure import: cannot resolve base geometry '" + geomUri + "'");
        }
        baseDoc = resolver.loadDocument(geomUri, presetDir);
        baseDir = directoryOf(ru.path);
        geomEntry = findGeometry(baseDoc->root(), ru.fragment);
    }
    if (!geomEntry) {
        throw std::runtime_error("figure import: base geometry library is empty");
    }

    GeometryData geo = parseGeometry(*geomEntry);

    // A POPULATED graft declaration (it names parent polygons to hide / vertices to weld) marks a
    // surface replacement authored in exact correspondence with a specific target shape — the signal
    // the follower-addon path uses to skip shape-follow projection. Generic followers carry an
    // EMPTY graft object, which does not count.
    bool graftPopulated = false;
    if (const auto graft = geomEntry->find("graft"); graft != geomEntry->end()) {
        const auto populated = [](const nlohmann::json& g, const char* key) {
            const auto it = g.find(key);
            return it != g.end() && it->value("count", 0) > 0;
        };
        graftPopulated = populated(*graft, "hidden_polys") || populated(*graft, "vertex_pairs");
    }

    // Pre-morph copy of the base cage: follower addons are authored against THIS shape, so their
    // shape-follow projection (followParentShape) needs base positions + the morph deltas below.
    std::vector<glm::vec3> baseCagePositions;
    if (includeAddons) {
        baseCagePositions = geo.positions;
    }

    // Apply the preset's dialed shape morphs to the base vertices — this is what turns the generic
    // base figure into the specific character. A character rarely dials the shape morphs directly; it
    // dials a *control* whose driver formulas drive the real head/body morphs, so resolveDialedMorphs()
    // walks that graph and returns each reached morph file + weight. We then bake its sparse deltas
    // (position += weight · delta). Controls/scales resolve to no deltas and no-op; a morph that
    // fails to load is skipped rather than aborting the whole import.
    float figureScale = 1.0f;
    JointCenterOffsets jointCenterOffsets; // per-bone rest-origin shifts the same morphs drive
    const std::vector<DialedMorph> dialedMorphs =
        resolveDialedMorphs(root, resolver, presetDir, figureScale, jointCenterOffsets);
    // Load every reached morph document up front, across cores — a character preset can reach
    // hundreds of morph files, and their serial read+inflate+parse was a large slice of import
    // time. The bake loop below then hits the resolver's cache.
    {
        std::vector<std::string> morphUris;
        morphUris.reserve(dialedMorphs.size());
        for (const DialedMorph& dialed : dialedMorphs) {
            morphUris.push_back(dialed.url);
        }
        resolver.prefetchDocuments(morphUris, presetDir);
    }
    for (const DialedMorph& dialed : dialedMorphs) {
        const ResolvedUri ru = resolver.resolve(dialed.url, presetDir);
        if (!ru.resolved()) {
            continue;
        }
        std::shared_ptr<const FigureDocument> morphDoc;
        try {
            morphDoc = resolver.loadDocument(dialed.url, presetDir);
        } catch (const std::exception&) {
            continue;
        }
        for (const auto& [vertexIndex, delta] : parseMorphDeltas(morphDoc->root(), ru.fragment)) {
            if (vertexIndex < geo.positions.size()) {
                geo.positions[vertexIndex] += dialed.weight * delta;
            }
        }
    }

    // UV set (referenced by the geometry, resolved relative to the base file).
    UvSet uv;
    if (!geo.defaultUvSetUri.empty()) {
        const ResolvedUri uru = resolver.resolve(geo.defaultUvSetUri, baseDir);
        if (uru.resolved()) {
            std::shared_ptr<const FigureDocument> uvDoc = resolver.loadDocument(geo.defaultUvSetUri, baseDir);
            uv = parseUvSet(uvDoc->root(), uru.fragment);
        }
    }

    FigureData out;
    out.figureScale = figureScale;
    out.graftPopulated = graftPopulated;

    // Skeleton + skin weights (from the base file's node_library + its SkinBinding modifier), parsed
    // *before* assembly so each de-indexed render vertex can carry its base vertex's joint weights.
    const nlohmann::json& baseRoot = baseDoc ? baseDoc->root() : root;
    if (const auto nodeLib = baseRoot.find("node_library"); nodeLib != baseRoot.end()) {
        out.bones = parseSkeleton(*nodeLib);
    }

    // Shift each joint's rest origin by the morph-driven center_point adjustments, so the skeleton
    // matches the character's morphed proportions (a base-figure rig around a morphed mesh would pivot
    // limbs at the wrong places, and the joint overlay would float off the body). The offsets are in
    // the same native units as origin; the cm->world scale is applied later, uniformly, in toModelData.
    if (!jointCenterOffsets.empty()) {
        for (FigureBone& bone : out.bones) {
            const auto it = jointCenterOffsets.find(bone.name);
            if (it != jointCenterOffsets.end()) {
                bone.origin += it->second;
            }
        }
    }

    if (!out.bones.empty()) {
        // Twist bones sit mid-limb where there's no real joint; keep their axial twist but forbid the
        // mid-bone bend the figure would otherwise allow. Done after the origins (incl. morph-driven
        // offsets) are final, so each bone's length direction reflects the character's proportions.
        lockTwistBoneBendAxes(out.bones);

        std::unordered_map<std::string, int> boneNameToIndex;
        for (int i = 0; i < static_cast<int>(out.bones.size()); ++i) {
            boneNameToIndex.emplace(out.bones[i].name, i);
        }
        if (const auto modLib = baseRoot.find("modifier_library");
            modLib != baseRoot.end() && modLib->is_array()) {
            for (const auto& mod : *modLib) {
                if (const auto skin = mod.find("skin"); skin != mod.end()) {
                    const int vc =
                        skin->value("vertex_count", static_cast<int>(geo.positions.size()));
                    out.vertexSkins = parseSkinWeights(*skin, vc, boneNameToIndex);
                    break;
                }
            }
        }

        // Pose correctives: auto-applied morphs driven by these joints' rotations. Discovered
        // structurally (see discoverCorrectives), so they work across figure generations that name
        // their corrective files differently. Only meaningful once we have a skeleton to drive them.
        std::unordered_set<std::string> boneNames;
        boneNames.reserve(out.bones.size());
        for (const FigureBone& b : out.bones) {
            boneNames.insert(b.name);
        }
        out.correctives = discoverCorrectives(resolver, baseDir, boneNames, dialedMorphs);
    }

    // Catmull-Clark subdivision: smooth the low-resolution base cage into the render mesh. Positions
    // follow the smooth rules while UVs (per corner, so seams stay put) and skin weights refine
    // linearly, then the result is assembled exactly like the un-subdivided path — so skinning and pose
    // correctives run unchanged, just on more vertices. Because Catmull-Clark is a *linear* operator,
    // each corrective's sparse base-cage deltas are carried onto the subdivided cage by the SAME
    // stencils (subdivideDeltaField), so the runtime re-morph needs no change. Level 1 (4x faces) is a
    // good editing-time smoothness; raise for finer at a vertex-count cost.
    constexpr int kSubdivisionLevels = 1;
    if (kSubdivisionLevels > 0) {
        SubdivisionResult sub = subdivideFigure(geo, uv, out.vertexSkins, kSubdivisionLevels);
        out.meshes = std::move(sub.meshes);
        // Carry each corrective's sparse delta field onto the subdivided cage. Every corrective is
        // independent (subdivideDeltaField only reads the shared topology), and a figure ships ~100+
        // of them, each a full-mesh stencil pass — the dominant slice of the subdivision's import
        // cost — so run them across cores.
        parallelFor(static_cast<int>(out.correctives.size()), [&](int ci) {
            PoseCorrective& pc = out.correctives[static_cast<std::size_t>(ci)];
            std::vector<glm::vec3> field(geo.positions.size(), glm::vec3(0.0f));
            for (const auto& [index, delta] : pc.deltas) {
                if (index < field.size()) {
                    field[index] += delta;
                }
            }
            const std::vector<glm::vec3> subdivided = sub.subdivideDeltaField(field);
            std::vector<std::pair<uint32_t, glm::vec3>> dense;
            for (uint32_t i = 0; i < subdivided.size(); ++i) {
                if (glm::dot(subdivided[i], subdivided[i]) > 1e-12f) {
                    dense.emplace_back(i, subdivided[i]);
                }
            }
            pc.deltas = std::move(dense);
        });
    } else {
        out.meshes = assembleFigureMeshes(geo, uv, out.vertexSkins);
    }

    // Materials come from the preset's scene (a base .dsf on its own carries none -> zones keep the
    // default base color). Texture URIs are resolved to on-disk paths inside.
    applySceneMaterials(out, root, resolver, presetDir);

    // Fitted follower addons (anime eyeballs, lashes, …) the preset declares in its post-load block.
    // Characters built this way deliberately hide the standard surfaces the follower replaces (e.g.
    // every stock eye surface dialed to cutout 0), so WITHOUT this step such a figure imports with
    // empty eye sockets. Each addon is a self-describing wearable figure: load it through this same
    // pipeline, apply its materials preset, and merge it into the figure. Failures skip just that
    // addon — a missing follower must not sink the character import.
    if (includeAddons) {
        // The morph deltas the followers must track (morphed − base cage). All-zero for an
        // unmorphed base figure, in which case the projection is skipped entirely.
        std::vector<glm::vec3> cageDeltas(baseCagePositions.size(), glm::vec3(0.0f));
        bool anyCageDelta = false;
        for (std::size_t i = 0; i < baseCagePositions.size() && i < geo.positions.size(); ++i) {
            cageDeltas[i] = geo.positions[i] - baseCagePositions[i];
            anyCageDelta = anyCageDelta || glm::dot(cageDeltas[i], cageDeltas[i]) > 1e-8f;
        }

        // Only VISIBLE zones contribute projection deltas. A stylized preset hides the stock
        // surfaces its addons replace (cutout 0) AND its morph typically stashes that hidden
        // geometry (shrinks the stock eyeballs into the head) — followers near the eye region
        // would sample those garbage deltas and land across the face. Clear shells (opacity
        // ~0.05) are excluded with them; the surrounding visible skin supplies sane deltas.
        std::vector<uint8_t> eligibleCageVerts(baseCagePositions.size(), 0);
        {
            std::unordered_map<std::string, float> zoneOpacity;
            for (const FigureMesh& m : out.meshes) {
                zoneOpacity[m.materialZone] = m.material.opacity;
            }
            for (const GeometryData::Face& f : geo.faces) {
                float opacity = 1.0f; // zones without a parsed material default to visible
                if (f.material >= 0 && f.material < static_cast<int>(geo.materialZones.size())) {
                    const auto it = zoneOpacity.find(geo.materialZones[static_cast<std::size_t>(f.material)]);
                    if (it != zoneOpacity.end()) {
                        opacity = it->second;
                    }
                }
                if (opacity <= 0.06f) {
                    continue;
                }
                for (int c = 0; c < f.count; ++c) {
                    if (f.v[static_cast<std::size_t>(c)] < eligibleCageVerts.size()) {
                        eligibleCageVerts[f.v[static_cast<std::size_t>(c)]] = 1;
                    }
                }
            }
        }

        for (const AddonRef& addonRef : findPostLoadAddons(root)) {
            try {
                const ResolvedUri location = resolver.resolve(addonRef.assetUri, presetDir);
                if (!location.resolved()) {
                    continue;
                }
                FigureData addon = loadFigureFile(location.path, contentRoots, false);
                for (const std::string& presetUri : addonRef.presetUris) {
                    try {
                        const std::shared_ptr<const FigureDocument> presetDoc =
                            resolver.loadDocument(presetUri, presetDir);
                        // A materials preset comes in either form; each applier no-ops when its
                        // section is absent (the anime-eye presets use the animation-track form),
                        // so a non-material preset document just passes through harmlessly.
                        applySceneMaterials(addon, presetDoc->root(), resolver, presetDir);
                        applyAnimationMaterials(addon, presetDoc->root(), resolver, presetDir);
                    } catch (const std::exception&) {
                        // Unresolvable preset: the follower keeps its own materials.
                    }
                }
                // Shape-follow only the followers authored against the BASE figure. A populated
                // graft marks a vendor-fitted in-place replacement (e.g. an anime character's
                // custom eyeballs) — projecting the morph onto those double-shifts them off the
                // face, verified both ways on real content.
                if (anyCageDelta && !addon.graftPopulated) {
                    followParentShape(addon, baseCagePositions, cageDeltas, eligibleCageVerts);
                }
                mergeAddonFigure(out, std::move(addon));
            } catch (const std::exception&) {
                continue;
            }
        }
    }

    return out;
}

} // namespace

FigureData FigureImporter::load(const std::string& path,
                                const std::vector<std::string>& contentRoots) const {
    return loadFigureFile(path, contentRoots, /*includeAddons=*/true);
}

} // namespace pose
