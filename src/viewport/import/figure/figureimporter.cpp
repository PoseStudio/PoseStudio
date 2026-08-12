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
#include "subdivision.h"
#include "uriresolver.h"
#include "uvparser.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
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

    std::vector<PoseCorrective> correctives;
    std::unordered_set<std::string> seenIds;
    for (const std::string& dir : dirs) {
        for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (!it->is_regular_file(ec) || it->path().extension() != ".dsf") {
                continue;
            }
            try {
                const FigureDocument doc = FigureDocument::loadFromFile(it->path().string());
                const auto ml = doc.root().find("modifier_library");
                if (ml == doc.root().end() || !ml->is_array()) {
                    continue;
                }
                for (const auto& mod : *ml) {
                    PoseCorrective pc;
                    if (parseCorrective(mod, ctx, pc) && seenIds.insert(pc.id).second) {
                        correctives.push_back(std::move(pc));
                    }
                }
            } catch (const std::exception&) {
                continue; // unreadable/!gzip/!json — skip this file
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

} // namespace

FigureData FigureImporter::load(const std::string& path,
                                const std::vector<std::string>& contentRoots) const {
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
        for (PoseCorrective& pc : out.correctives) {
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
        }
    } else {
        out.meshes = assembleFigureMeshes(geo, uv, out.vertexSkins);
    }

    // Materials come from the preset's scene (a base .dsf on its own carries none -> zones keep the
    // default base color). Resolve each zone's texture URIs to on-disk paths.
    if (root.contains("scene") && root["scene"].contains("materials")) {
        static const nlohmann::json kNoLibrary = nlohmann::json::array();
        const nlohmann::json& materialLibrary =
            root.contains("material_library") ? root["material_library"] : kNoLibrary;
        const nlohmann::json& imageLibrary =
            root.contains("image_library") ? root["image_library"] : kNoLibrary;
        const std::unordered_map<std::string, MaterialRefs> refs =
            parseMaterials(root["scene"]["materials"], materialLibrary, imageLibrary);
        for (FigureMesh& mesh : out.meshes) {
            const auto it = refs.find(mesh.materialZone);
            if (it == refs.end()) {
                continue;
            }
            const MaterialRefs& ref = it->second;
            mesh.material.baseColor = ref.baseColor;
            mesh.material.roughness = ref.roughness;
            mesh.material.opacity = ref.opacity;
            if (!ref.diffuseImageUri.empty()) {
                mesh.material.diffuseMapPath = resolver.resolve(ref.diffuseImageUri, presetDir).path;
            }
            if (!ref.normalImageUri.empty()) {
                mesh.material.normalMapPath = resolver.resolve(ref.normalImageUri, presetDir).path;
            }
            if (!ref.bumpImageUri.empty()) {
                mesh.material.bumpMapPath = resolver.resolve(ref.bumpImageUri, presetDir).path;
            }
        }
    }

    return out;
}

} // namespace pose
