/**
 * @file usdfigureimporter.cpp
 * @brief The USD figure reader (see usdfigureimporter.h): LightUSD + its render-scene layer
 *        ("Tydra") -> ModelData.
 */

#include "usdfigureimporter.h"
#include "specgloss.h"

#include <lightusd.hh>
#include <tydra/render-data-converter.hh>
#include <tydra/render-data.hh>
#include <tydra/scene-access.hh>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace pose {

namespace {

namespace fs = std::filesystem;
namespace lu = lightusd;
namespace ty = lightusd::tydra;

/// USD's matrices are row-vector (p' = p · M, the translation in the last ROW); glm's are
/// column-vector. The column-vector equivalent is the transpose, and glm stores column i where
/// USD stores row i — so the storage is copied element for element.
glm::mat4 toGlm(const lu::value::matrix4d& m) {
    glm::mat4 g(1.0f);
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            g[i][j] = static_cast<float>(m.m[i][j]);
        }
    }
    return g;
}

glm::vec3 toGlm(const ty::vec3& v) {
    return glm::vec3(v[0], v[1], v[2]);
}

/// Reads element @p e of a vertex attribute as @p n floats (zero-filled past its components).
/// Returns false when the attribute has no such element.
bool readAttr(const ty::VertexAttribute& a, std::size_t e, float* out, std::size_t n) {
    const std::size_t stride = a.stride_bytes();
    if (stride == 0) {
        return false;
    }
    std::size_t index = e;
    if (a.is_indexed()) {
        if (e >= a.indices.size()) {
            return false;
        }
        index = a.indices[e];
    }
    const std::size_t offset = index * stride;
    if (offset + stride > a.data.size()) {
        return false;
    }
    const std::size_t comps = std::min<std::size_t>(n, a.format_size() / sizeof(float));
    for (std::size_t k = 0; k < n; ++k) {
        out[k] = 0.0f;
    }
    if (a.format == ty::VertexAttributeFormat::Float || a.format == ty::VertexAttributeFormat::Vec2 || a.format == ty::VertexAttributeFormat::Vec3 ||
        a.format == ty::VertexAttributeFormat::Vec4) {
        std::memcpy(out, a.data.data() + offset, comps * sizeof(float));
        return true;
    }
    return false;
}

struct CornerKey {
    uint32_t vertex;
    uint32_t normal;
    uint32_t uv;
    bool operator==(const CornerKey& o) const { return vertex == o.vertex && normal == o.normal && uv == o.uv; }
};
struct CornerKeyHash {
    std::size_t operator()(const CornerKey& k) const {
        std::size_t h = k.vertex;
        h = h * 1000003u ^ k.normal;
        h = h * 1000003u ^ k.uv;
        return h;
    }
};

/// The textures of a USD file: a USDZ package's come out of the package (its asset map), a plain
/// file's are found beside it through the shared finder.
class UsdTextures {
public:
    UsdTextures(const std::string& path, bool usdz, RiggedImportInfo* info) : m_finder(fs::path(path).parent_path()), m_info(info) {
        if (!usdz) {
            return;
        }
        std::string warn, err;
        if (!lu::ReadUSDZAssetInfoFromFile(path, &m_asset, &warn, &err)) {
            if (info != nullptr) {
                info->notes.push_back("the USDZ package's asset table could not be read: " + err);
            }
            return;
        }
        m_packaged = true;
        for (const auto& [name, span] : m_asset.asset_map) {
            m_byName.emplace(lowerCopy(fs::path(name).filename().string()), name);
        }
    }

    TextureSource resolve(const std::string& identifier) {
        TextureSource out;
        if (identifier.empty()) {
            if (m_info != nullptr) {
                ++m_info->missingTextures;
            }
            return out;
        }
        if (m_packaged) {
            auto it = m_asset.asset_map.find(identifier);
            if (it == m_asset.asset_map.end()) {
                const auto byName = m_byName.find(lowerCopy(fs::path(identifier).filename().string()));
                if (byName != m_byName.end()) {
                    it = m_asset.asset_map.find(byName->second);
                }
            }
            if (it != m_asset.asset_map.end()) {
                const auto [offset, size] = it->second;
                const uint8_t* base = m_asset.is_mmaped() ? m_asset.addr : m_asset.data.data();
                const std::size_t total = m_asset.is_mmaped() ? m_asset.size : m_asset.data.size();
                if (base != nullptr && offset + size <= total && size > 0) {
                    out.encoded.assign(base + offset, base + offset + size);
                    if (m_info != nullptr) {
                        ++m_info->embeddedTextures;
                    }
                    return out;
                }
            }
            // (not in the package: a USDZ may still refer to a file beside it)
        }
        return m_finder.resolve({identifier}, m_info, identifier);
    }

private:
    TextureFinder                                m_finder;
    RiggedImportInfo*                            m_info;
    lu::USDZAsset                                m_asset;
    bool                                         m_packaged = false;
    std::unordered_map<std::string, std::string> m_byName;
};

int channelIndex(ty::UVTexture::Channel c) {
    switch (c) {
    case ty::UVTexture::Channel::G: return 1;
    case ty::UVTexture::Channel::B: return 2;
    case ty::UVTexture::Channel::A: return 3;
    default: return 0;
    }
}

} // namespace

ModelData UsdFigureImporter::load(const std::string& path, RiggedImportInfo* info) const {
    const bool        usdz = lu::IsUSDZ(path);
    lu::Stage         stage;
    lu::USDLoadOptions options;
    options.max_memory_limit_in_mb = 8192;
    options.max_allowed_asset_size_in_mb = 8192;
    options.load_sublayers = true;
    options.load_references = true;
    options.load_payloads = true;
    options.do_composition = true;
    std::string warn, err;
    const bool  loaded = usdz ? lu::LoadUSDZFromFile(path, &stage, &warn, &err, options) : lu::LoadUSDFromFile(path, &stage, &warn, &err, options);
    if (!loaded) {
        throw std::runtime_error("USD: " + (err.empty() ? std::string("the file could not be read") : err));
    }
    if (!warn.empty() && info != nullptr && warn.find("Threading is disabled") == std::string::npos) {
        info->notes.push_back("while reading: " + warn.substr(0, 300));
    }

    // Units and axes: the stage's metadata, with the format's own fallbacks (centimetres, Y up).
    const bool   unitAuthored = stage.metas().metersPerUnit.authored();
    const bool   upAuthored = stage.metas().upAxis.authored();
    const double metersPerUnit = unitAuthored ? stage.metas().metersPerUnit.get_value() : 0.01;
    const bool   zUp = upAuthored && stage.metas().upAxis.get_value() == lu::Axis::Z;
    if (info != nullptr) {
        char axes[96];
        std::snprintf(axes, sizeof(axes), "up %s%s; unit %.4g m%s", zUp ? "+Z" : "+Y", upAuthored ? "" : " (default)", metersPerUnit,
                      unitAuthored ? "" : " (default)");
        info->sourceAxes = axes;
    }
    const float unit = static_cast<float>(metersPerUnit);
    // A Z-up stage turned Y-up: (x, y, z) -> (x, z, -y).
    const glm::mat3 axisTurn = zUp ? glm::mat3(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f)) : glm::mat3(1.0f);
    const auto toWorldUnits = [&](const glm::vec3& p) { return axisTurn * (p * unit); };
    const auto toWorldDir = [&](const glm::vec3& n) { return axisTurn * n; };

    // The render-scene layer: meshes triangulated, every UV set, textures left ENCODED (their
    // identifiers alone — the Qt layer decodes, as for every format).
    ty::RenderSceneConverterEnv env(stage);
    env.scene_config.load_texture_assets = false;
    env.mesh_config.triangulate = true;
    env.mesh_config.extract_all_texcoords = true;
    env.mesh_config.keep_triangulation_intermediates = true;
    env.material_config.allow_missing_asset = true;
    env.material_config.allow_texture_load_failure = true;
    env.usd_filename = path;
    env.set_search_paths({fs::path(path).parent_path().string()});
    ty::RenderSceneConverter converter;
    ty::RenderScene          scene;
    if (!converter.ConvertToRenderScene(env, &scene)) {
        throw std::runtime_error("USD: " + (converter.GetError().empty() ? std::string("the scene could not be converted") : converter.GetError()));
    }
    if (!converter.GetWarning().empty() && info != nullptr) {
        info->notes.push_back("while converting: " + converter.GetWarning().substr(0, 300));
    }

    // ---- THE NODES: the world transform of every mesh and skeleton prim (the first node that
    // carries each wins).
    std::unordered_map<int, glm::mat4> meshWorld;
    std::unordered_map<int, glm::mat4> skelWorld;
    {
        std::vector<const ty::Node*> stack;
        for (const ty::Node& n : scene.nodes) {
            stack.push_back(&n);
        }
        static const bool kNodeTrace = std::getenv("POSESTUDIO_USD_NODE_TRACE") != nullptr; // the render-scene node tree
        while (!stack.empty()) {
            const ty::Node* n = stack.back();
            stack.pop_back();
            if (kNodeTrace) {
                const glm::mat4 g = toGlm(n->global_matrix);
                std::fprintf(stderr, "[usd-node] %-40s type %d id %d at (%.3f %.3f %.3f) scale %.4f\n", n->abs_path.c_str(), static_cast<int>(n->nodeType),
                             n->id, g[3][0], g[3][1], g[3][2], glm::length(glm::vec3(g[0])));
            }
            if (n->nodeType == ty::NodeType::Mesh && n->id >= 0) {
                meshWorld.emplace(n->id, toGlm(n->global_matrix));
            } else if (n->nodeType == ty::NodeType::Skeleton) {
                // (a Skeleton node carries no id: matched to the skeleton by its prim path)
                for (std::size_t s = 0; s < scene.skeletons.size(); ++s) {
                    if (scene.skeletons[s].abs_path == n->abs_path) {
                        skelWorld.emplace(static_cast<int>(s), toGlm(n->global_matrix));
                    }
                }
            }
            for (const ty::Node& c : n->children) {
                stack.push_back(&c);
            }
        }
    }

    // ---- THE SKELETON: the one with the most joints; its joints depth-first, parents first.
    int skelIndex = -1;
    for (std::size_t s = 0; s < scene.skeletons.size(); ++s) {
        if (skelIndex < 0 || scene.skeletons[s].num_joints() > scene.skeletons[static_cast<std::size_t>(skelIndex)].num_joints()) {
            skelIndex = static_cast<int>(s);
        }
    }
    if (scene.skeletons.size() > 1 && info != nullptr) {
        info->notes.push_back("the file holds " + std::to_string(scene.skeletons.size()) + " skeletons: the largest is the figure's, the others' meshes import rigid");
    }
    const glm::mat4 skelXform = skelIndex >= 0 && skelWorld.count(skelIndex) != 0 ? skelWorld[skelIndex] : glm::mat4(1.0f);
    struct Joint {
        const ty::SkelNode* node;
        int                 parent;
    };
    std::vector<Joint>       joints; // (bone order)
    std::unordered_map<int, int> boneOfJoint; // skeleton joint index -> bone index
    if (skelIndex >= 0) {
        const ty::SkelHierarchy& skel = scene.skeletons[static_cast<std::size_t>(skelIndex)];
        std::vector<Joint>       stack{{&skel.root_node, -1}};
        while (!stack.empty()) {
            const Joint j = stack.back();
            stack.pop_back();
            const int index = static_cast<int>(joints.size());
            joints.push_back(j);
            if (j.node->joint_id >= 0) {
                boneOfJoint.emplace(j.node->joint_id, index);
            }
            for (std::size_t c = j.node->children.size(); c-- > 0;) {
                stack.push_back({&j.node->children[c], index});
            }
        }
    }
    // The weight each joint carries over every mesh bound to this skeleton: a leaf joint nothing
    // is skinned to is an exporter's end marker, left out and kept as its parent's tip.
    std::vector<double> jointWeight(joints.size(), 0.0);
    for (const ty::RenderMesh& mesh : scene.meshes) {
        if (mesh.skel_id != skelIndex || skelIndex < 0) {
            continue;
        }
        const ty::JointAndWeight& jw = mesh.joint_and_weights;
        for (std::size_t i = 0; i < jw.jointIndices.size() && i < jw.jointWeights.size(); ++i) {
            const auto it = boneOfJoint.find(jw.jointIndices[i]);
            if (it != boneOfJoint.end() && jw.jointWeights[i] > 0.0f) {
                jointWeight[static_cast<std::size_t>(it->second)] += jw.jointWeights[i];
            }
        }
    }
    std::vector<char> keep(joints.size(), 1);
    for (bool pruned = true; pruned;) {
        pruned = false;
        for (std::size_t i = 0; i < joints.size(); ++i) {
            if (!keep[i] || jointWeight[i] > 0.0) {
                continue;
            }
            bool keptChild = false;
            for (std::size_t c = 0; c < joints.size() && !keptChild; ++c) {
                keptChild = keep[c] && joints[c].parent == static_cast<int>(i);
            }
            if (!keptChild) {
                keep[i] = 0;
                pruned = true;
                if (info != nullptr) {
                    ++info->prunedLeaves;
                }
            }
        }
    }
    std::vector<RawSkeletonBone> bones;
    std::vector<int>             boneOfJointNode(joints.size(), -1); // joint list index -> bone index
    for (std::size_t i = 0; i < joints.size(); ++i) {
        const ty::SkelNode* node = joints[i].node;
        const glm::mat4     bindWorld = skelXform * toGlm(node->bind_transform);
        const glm::vec3     position = toWorldUnits(glm::vec3(bindWorld[3]));
        if (!keep[i]) {
            const int p = joints[i].parent;
            if (p >= 0 && boneOfJointNode[static_cast<std::size_t>(p)] >= 0) {
                RawSkeletonBone& parent = bones[static_cast<std::size_t>(boneOfJointNode[static_cast<std::size_t>(p)])];
                if (!parent.hasTip) {
                    parent.tip = position;
                    parent.hasTip = true;
                }
            }
            continue;
        }
        RawSkeletonBone bone;
        // The joint's NAME is the last element of its path ("root/hips/spine/chest" names the
        // chest): the skeleton layer hands the whole path back as the name where the file
        // authors no joint names, and a role read off "spine" in every chest's name is wrong.
        bone.name = !node->joint_name.empty() ? node->joint_name : node->joint_path;
        if (const std::size_t slash = bone.name.find_last_of('/'); slash != std::string::npos) {
            bone.name = bone.name.substr(slash + 1);
        }
        bone.parent = joints[i].parent >= 0 ? boneOfJointNode[static_cast<std::size_t>(joints[i].parent)] : -1;
        bone.position = position;
        boneOfJointNode[i] = static_cast<int>(bones.size());
        bones.push_back(std::move(bone));
    }
    const int boneCount = static_cast<int>(bones.size());
    const auto boneOfSkelJoint = [&](int jointIndex) {
        const auto it = boneOfJoint.find(jointIndex);
        if (it == boneOfJoint.end()) {
            return -1;
        }
        return boneOfJointNode[static_cast<std::size_t>(it->second)];
    };

    // ---- THE MESHES.
    ModelData   model;
    UsdTextures textures(path, usdz, info);
    bool        anySkinned = false;
    bool        notedMaterialX = false, notedShapes = false;
    for (std::size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        const ty::RenderMesh& mesh = scene.meshes[mi];
        const std::vector<uint32_t>& fvi = mesh.faceVertexIndices();
        const std::vector<uint32_t>& fvc = mesh.faceVertexCounts();
        if (mesh.points.empty() || fvi.empty()) {
            continue;
        }
        if (!mesh.targets.empty() && info != nullptr && !notedShapes) {
            info->notes.push_back("blend shapes are not read");
            notedShapes = true;
        }
        const bool skinned = skelIndex >= 0 && mesh.skel_id == skelIndex && !mesh.joint_and_weights.jointIndices.empty() &&
                             mesh.joint_and_weights.elementSize > 0;
        anySkinned = anySkinned || skinned;
        if (info != nullptr) {
            (skinned ? info->skinnedMeshes : info->rigidMeshes) += 1;
        }
        // Where each point stands in the world at bind: a skinned mesh through the geometry bind
        // transform and the skeleton's transform, a rigid one through its prim's.
        glm::mat4 toWorld = meshWorld.count(static_cast<int>(mi)) != 0 ? meshWorld[static_cast<int>(mi)] : glm::mat4(1.0f);
        if (skinned) {
            toWorld = skelXform * toGlm(mesh.joint_and_weights.geomBindTransform);
        }
        const glm::mat3 normalXform = glm::inverseTranspose(glm::mat3(toWorld));
        // Per point: the influences, normalized, as bone indices.
        const std::size_t       np = mesh.points.size();
        std::vector<glm::uvec4> pointJoints(np, glm::uvec4(0));
        std::vector<glm::vec4>  pointWeights(np, glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
        if (skinned) {
            const ty::JointAndWeight& jw = mesh.joint_and_weights;
            const std::size_t         es = static_cast<std::size_t>(jw.elementSize);
            struct Influence {
                int   bone;
                float w;
            };
            std::vector<Influence> infl;
            for (std::size_t p = 0; p < np; ++p) {
                infl.clear();
                for (std::size_t k = 0; k < es; ++k) {
                    const std::size_t i = p * es + k;
                    if (i >= jw.jointIndices.size() || i >= jw.jointWeights.size() || jw.jointWeights[i] <= 0.0f) {
                        continue;
                    }
                    const int bone = boneOfSkelJoint(jw.jointIndices[i]);
                    if (bone >= 0) {
                        infl.push_back({bone, jw.jointWeights[i]});
                    }
                }
                if (infl.empty()) {
                    continue;
                }
                std::sort(infl.begin(), infl.end(), [](const Influence& a, const Influence& b) { return a.w > b.w; });
                if (infl.size() > 4) {
                    infl.resize(4);
                }
                float total = 0.0f;
                for (const Influence& in : infl) {
                    total += in.w;
                }
                for (std::size_t k = 0; k < 4; ++k) {
                    pointJoints[p][static_cast<glm::length_t>(k)] = static_cast<unsigned>(k < infl.size() ? infl[k].bone : infl[0].bone);
                    pointWeights[p][static_cast<glm::length_t>(k)] = k < infl.size() ? infl[k].w / total : 0.0f;
                }
            }
        }
        // The triangles, and which material each belongs to (a GeomSubset's, else the mesh's).
        const std::size_t faceCount = fvc.size();
        std::vector<int>  faceMaterial(faceCount, mesh.material_id);
        for (const auto& [name, subset] : mesh.material_subsetMap) {
            for (const int f : subset.indices()) {
                if (f >= 0 && static_cast<std::size_t>(f) < faceCount) {
                    faceMaterial[static_cast<std::size_t>(f)] = subset.material_id;
                }
            }
        }
        std::map<int, std::vector<std::size_t>> facesOfMaterial;
        for (std::size_t f = 0; f < faceCount; ++f) {
            facesOfMaterial[faceMaterial[f]].push_back(f);
        }
        // The UV set the material's base map samples (its primvar name), else the first.
        const auto uvSlotFor = [&](int materialId) -> const ty::VertexAttribute* {
            const ty::VertexAttribute* first = nullptr;
            for (const auto& [slot, attr] : mesh.texcoords) {
                if (first == nullptr) {
                    first = &attr;
                }
            }
            if (materialId >= 0 && static_cast<std::size_t>(materialId) < scene.materials.size()) {
                const ty::RenderMaterial& m = scene.materials[static_cast<std::size_t>(materialId)];
                if (m.surfaceShader.has_value() && m.surfaceShader->diffuseColor.is_texture()) {
                    const int tid = m.surfaceShader->diffuseColor.texture_id;
                    if (tid >= 0 && static_cast<std::size_t>(tid) < scene.textures.size()) {
                        const std::string& varname = scene.textures[static_cast<std::size_t>(tid)].varname_uv;
                        for (const auto& [slot, attr] : mesh.texcoords) {
                            if (!varname.empty() && attr.name == varname) {
                                return &attr;
                            }
                        }
                    }
                }
            }
            return first;
        };
        const std::vector<uint32_t>& fvMap = mesh.triangulatedToOrigFaceVertexIndexMap; // (a facevarying attribute's element for a triangulated corner)
        const auto corner = [&](std::size_t i) { return i < fvMap.size() ? fvMap[i] : static_cast<uint32_t>(i); };
        for (const auto& [materialId, faces] : facesOfMaterial) {
            MeshData                   out;
            const ty::VertexAttribute* uvAttr = uvSlotFor(materialId);
            const ty::VertexAttribute& nAttr = mesh.normals;
            const bool                 haveNormals = !nAttr.empty();
            const bool                 haveUv = uvAttr != nullptr && !uvAttr->empty();
            std::unordered_map<CornerKey, uint32_t, CornerKeyHash> corners;
            std::size_t fvStart = 0; // (the first face-vertex of each face: faces are consecutive)
            std::vector<std::size_t> faceStart(faceCount, 0);
            for (std::size_t f = 0; f < faceCount; ++f) {
                faceStart[f] = fvStart;
                fvStart += fvc[f];
            }
            for (const std::size_t f : faces) {
                const uint32_t count = fvc[f];
                if (count < 3) {
                    continue;
                }
                // (triangulated meshes have three; a non-triangulated one is fanned)
                for (uint32_t t = 1; t + 1 < count; ++t) {
                    const std::size_t cornerIndices[3] = {faceStart[f], faceStart[f] + t, faceStart[f] + t + 1};
                    for (const std::size_t ci : cornerIndices) {
                        if (ci >= fvi.size()) {
                            continue;
                        }
                        const uint32_t p = fvi[ci];
                        if (p >= np) {
                            continue;
                        }
                        const uint32_t nIndex = !haveNormals ? 0u : (nAttr.is_facevarying() ? corner(ci) : (nAttr.is_vertex() ? p : 0u));
                        const uint32_t uvIndex = !haveUv ? 0u : (uvAttr->is_facevarying() ? corner(ci) : (uvAttr->is_vertex() ? p : 0u));
                        const CornerKey key{p, nIndex, uvIndex};
                        auto            it = corners.find(key);
                        if (it == corners.end()) {
                            Vertex vert;
                            vert.pos = toWorldUnits(glm::vec3(toWorld * glm::vec4(toGlm(mesh.points[p]), 1.0f)));
                            glm::vec3 n(0.0f);
                            if (haveNormals) {
                                float nn[3];
                                if (readAttr(nAttr, nAttr.is_facevarying() ? corner(ci) : (nAttr.is_vertex() ? p : 0u), nn, 3)) {
                                    n = toWorldDir(normalXform * glm::vec3(nn[0], nn[1], nn[2]));
                                }
                            }
                            const float len = glm::length(n);
                            vert.normal = len > 1e-12f ? n / len : glm::vec3(0.0f);
                            vert.uv = glm::vec2(0.0f);
                            if (haveUv) {
                                float uv[2];
                                if (readAttr(*uvAttr, uvAttr->is_facevarying() ? corner(ci) : (uvAttr->is_vertex() ? p : 0u), uv, 2)) {
                                    vert.uv = glm::vec2(uv[0], 1.0f - uv[1]); // (USD's origin is the bottom-left)
                                }
                            }
                            vert.joints = pointJoints[p];
                            vert.weights = pointWeights[p];
                            it = corners.emplace(key, static_cast<uint32_t>(out.vertices.size())).first;
                            out.vertices.push_back(vert);
                        }
                        out.indices.push_back(it->second);
                    }
                }
            }
            if (out.indices.size() < 3) {
                continue;
            }
            fillMissingNormals(out);
            // The material: UsdPreviewSurface.
            out.roughness = 0.65f;
            out.specularWeight = 0.6f;
            out.specularWeightWithMap = 0.6f;
            if (materialId >= 0 && static_cast<std::size_t>(materialId) < scene.materials.size()) {
                const ty::RenderMaterial& m = scene.materials[static_cast<std::size_t>(materialId)];
                if (info != nullptr) {
                    ++info->materials;
                }
                out.name = m.name; // the Materials tab's zone name
                const auto textureOf = [&](int tid, int* channel) -> TextureSource {
                    if (tid < 0 || static_cast<std::size_t>(tid) >= scene.textures.size()) {
                        return TextureSource();
                    }
                    const ty::UVTexture& t = scene.textures[static_cast<std::size_t>(tid)];
                    if (channel != nullptr) {
                        *channel = channelIndex(t.connectedOutputChannel);
                    }
                    if (t.texture_image_id < 0 || static_cast<std::size_t>(t.texture_image_id) >= scene.images.size()) {
                        return TextureSource();
                    }
                    return textures.resolve(scene.images[static_cast<std::size_t>(t.texture_image_id)].asset_identifier);
                };
                if (m.surfaceShader.has_value()) {
                    const ty::PreviewSurfaceShader& s = *m.surfaceShader;
                    out.baseColor = glm::vec3(s.diffuseColor.value[0], s.diffuseColor.value[1], s.diffuseColor.value[2]); // (linear)
                    int diffuseImage = -1;
                    if (s.diffuseColor.is_texture()) {
                        out.diffuse = textureOf(s.diffuseColor.texture_id, nullptr);
                        if (!out.diffuse.empty()) {
                            out.baseColor = glm::vec3(1.0f);
                            diffuseImage = static_cast<int>(scene.textures[static_cast<std::size_t>(s.diffuseColor.texture_id)].texture_image_id);
                        }
                    }
                    out.roughness = std::max(0.05f, std::min(1.0f, s.roughness.value));
                    if (s.roughness.is_texture()) {
                        int channel = 0;
                        out.roughnessMap = textureOf(s.roughness.texture_id, &channel);
                        out.roughnessChannel = channel == 3 ? 0 : channel;
                        if (!out.roughnessMap.empty()) {
                            out.roughness = 1.0f; // (the map carries it)
                        }
                    }
                    out.lobe1Roughness = out.roughness;
                    out.lobe2Roughness = out.roughness;
                    out.metalness = std::max(0.0f, std::min(1.0f, s.metallic.value));
                    if (s.metallic.is_texture()) {
                        // The metalness MAP (the Materials tab, 2026-11-09): read into the parameter
                        // image's green channel by the decode layer, whichever channel its output names.
                        int channel = 0;
                        out.metalnessMap = textureOf(s.metallic.texture_id, &channel);
                        out.metalnessChannel = channel == 3 ? 0 : channel;
                        if (!out.metalnessMap.empty()) {
                            out.metalness = 1.0f; // (the map carries it)
                        }
                    }
                    if (s.useSpecularWorkflow) {
                        // THE ONE MATERIAL MODEL (specgloss.h): a preview surface in the SPECULAR
                        // workflow gives its reflectance at normal incidence as a color; the standard
                        // solve turns it into a metalness (the scalars here, the maps per texel in
                        // the decode layer).
                        const glm::vec3 specular(s.specularColor.value[0], s.specularColor.value[1], s.specularColor.value[2]);
                        const float     glossiness = 1.0f - std::max(0.0f, std::min(1.0f, s.roughness.value));
                        const glm::vec3 diffuse = out.diffuse.empty() ? out.baseColor : glm::vec3(0.8f);
                        const MetallicRoughness mr = convertSpecGloss(diffuse, specular, glossiness);
                        out.convertSpecGloss = true;
                        out.specularColorFactor = s.specularColor.is_texture() ? glm::vec3(1.0f) : specular;
                        if (s.specularColor.is_texture()) {
                            out.specularColorMap = textureOf(s.specularColor.texture_id, nullptr);
                        }
                        out.glossinessFactor = glossiness;
                        if (out.diffuse.empty() && out.specularColorMap.empty()) {
                            out.baseColor = mr.baseColor; // the scalar solve: no maps to bake
                            out.metalness = mr.metalness;
                        }
                        out.specularWeight = 1.0f;
                        out.specularWeightWithMap = 1.0f;
                        if (info != nullptr) {
                            info->notes.push_back("material '" + out.name + "' converted from the specular workflow to metallic / roughness");
                        }
                    }
                    if (s.normal.is_texture()) {
                        out.normal = textureOf(s.normal.texture_id, nullptr);
                        out.normalMode = out.normal.empty() ? 0 : 1;
                    }
                    out.opacity = std::max(0.0f, std::min(1.0f, s.opacity.value));
                    if (s.opacity.is_texture()) {
                        const int  tid = s.opacity.texture_id;
                        const bool fromDiffuse = tid >= 0 && static_cast<std::size_t>(tid) < scene.textures.size() && diffuseImage >= 0 &&
                                                 scene.textures[static_cast<std::size_t>(tid)].texture_image_id == diffuseImage &&
                                                 scene.textures[static_cast<std::size_t>(tid)].connectedOutputChannel == ty::UVTexture::Channel::A;
                        if (fromDiffuse) {
                            out.opacityFromDiffuseAlpha = true;
                        } else {
                            out.opacityMask = textureOf(tid, nullptr);
                        }
                        out.opacity = 1.0f;
                    }
                    // The canonical material's further channels a preview surface carries: the
                    // emissive color (and its map), the index of refraction, the clearcoat.
                    {
                        const glm::vec3 e(s.emissiveColor.value[0], s.emissiveColor.value[1], s.emissiveColor.value[2]);
                        const float     peak = std::max(e.x, std::max(e.y, e.z));
                        if (s.emissiveColor.is_texture()) {
                            out.emissionMap = textureOf(s.emissiveColor.texture_id, nullptr);
                            out.emissionColor = glm::vec3(1.0f);
                            out.emission = out.emissionMap.empty() ? 0.0f : 1.0f;
                        } else if (peak > 0.0f) {
                            out.emissionColor = e / peak;
                            out.emission = peak;
                        }
                    }
                    out.ior = std::max(1.0f, std::min(3.0f, s.ior.value));
                    if (s.clearcoat.value > 0.0f) {
                        out.topCoatWeight = std::min(1.0f, s.clearcoat.value);
                        out.topCoatRoughness = std::max(0.0f, std::min(1.0f, s.clearcoatRoughness.value));
                    }
                } else if (m.openPBRShader.has_value()) {
                    const ty::OpenPBRSurfaceShader& s = *m.openPBRShader;
                    out.baseColor = glm::vec3(s.base_color.value[0], s.base_color.value[1], s.base_color.value[2]);
                    if (s.base_color.is_texture()) {
                        out.diffuse = textureOf(s.base_color.texture_id, nullptr);
                        if (!out.diffuse.empty()) {
                            out.baseColor = glm::vec3(1.0f);
                        }
                    }
                    out.roughness = std::max(0.05f, std::min(1.0f, s.specular_roughness.value));
                    out.lobe1Roughness = out.roughness;
                    out.lobe2Roughness = out.roughness;
                    out.metalness = std::max(0.0f, std::min(1.0f, s.base_metalness.value));
                } else if (info != nullptr && !notedMaterialX) {
                    info->notes.push_back("a material without a UsdPreviewSurface (a MaterialX network, say) renders with its color alone");
                    notedMaterialX = true;
                }
            }
            model.meshes.push_back(std::move(out));
        }
    }
    if (model.meshes.empty()) {
        throw std::runtime_error("USD: the file holds no mesh");
    }

    // ---- UNITS, once more: a stage that says metres and stands a character 20 m tall and more
    // was written in centimetres (a converted rig whose scale sat on a prim the skeleton layer
    // does not carry): brought down by a hundred, as the glTF reader does.
    if (boneCount >= 3) {
        float minY = std::numeric_limits<float>::max(), maxY = -std::numeric_limits<float>::max();
        for (const RawSkeletonBone& b : bones) {
            minY = std::min(minY, b.position.y);
            maxY = std::max(maxY, b.position.y);
        }
        if (maxY - minY > 20.0f) {
            for (RawSkeletonBone& b : bones) {
                b.position *= 0.01f;
                b.tip *= 0.01f;
            }
            for (MeshData& mesh : model.meshes) {
                for (Vertex& v : mesh.vertices) {
                    v.pos *= 0.01f;
                }
            }
            if (info != nullptr) {
                info->notes.push_back("the skeleton stands over 20 m tall: read as centimetres and scaled by 0.01");
            }
        }
    }

    // ---- FACING, CENTRE AND FLOOR, and the raw bones: the step every rigged reader shares.
    finishRiggedModel(model, bones, info);

    // UsdSkel is linear blend skinning.
    if (anySkinned) {
        for (MeshData& mesh : model.meshes) {
            mesh.linearSkinning = true;
        }
    }
    if (info != nullptr) {
        info->skinningMethod = anySkinned ? "linear" : (boneCount > 0 ? "rigid" : "none");
    }
    return model;
}

} // namespace pose
