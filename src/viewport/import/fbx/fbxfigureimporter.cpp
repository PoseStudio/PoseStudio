/**
 * @file fbxfigureimporter.cpp
 * @brief The FBX figure reader (see fbxfigureimporter.h): ufbx -> ModelData.
 */

#include "fbxfigureimporter.h"
#include "specgloss.h"

#include <ufbx.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace pose {

namespace {

namespace fs = std::filesystem;

glm::vec3 toGlm(const ufbx_vec3& v) {
    return glm::vec3(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z));
}

glm::mat4 toGlm(const ufbx_matrix& m) {
    glm::mat4 r(1.0f);
    r[0] = glm::vec4(static_cast<float>(m.m00), static_cast<float>(m.m10), static_cast<float>(m.m20), 0.0f);
    r[1] = glm::vec4(static_cast<float>(m.m01), static_cast<float>(m.m11), static_cast<float>(m.m21), 0.0f);
    r[2] = glm::vec4(static_cast<float>(m.m02), static_cast<float>(m.m12), static_cast<float>(m.m22), 0.0f);
    r[3] = glm::vec4(static_cast<float>(m.m03), static_cast<float>(m.m13), static_cast<float>(m.m23), 1.0f);
    return r;
}

std::string toStd(const ufbx_string& s) {
    return std::string(s.data, s.length);
}

/// An FBX texture: embedded content first (as bytes), else the paths the file gives, through the
/// shared finder (the file's own path, relative to the FBX or the folder above, then by name).
TextureSource resolveTexture(TextureFinder& finder, const ufbx_texture* tex, RiggedImportInfo* info) {
    TextureSource out;
    if (tex == nullptr) {
        return out;
    }
    if (tex->type != UFBX_TEXTURE_FILE && tex->file_textures.count > 0) {
        tex = tex->file_textures.data[0]; // (a layered or shader texture: its first file)
    }
    if (tex->content.size > 0 && tex->content.data != nullptr) {
        const auto* bytes = static_cast<const uint8_t*>(tex->content.data);
        out.encoded.assign(bytes, bytes + tex->content.size);
        if (info != nullptr) {
            ++info->embeddedTextures;
        }
        return out;
    }
    std::string shown = toStd(tex->relative_filename);
    if (shown.empty()) {
        shown = toStd(tex->filename);
    }
    return finder.resolve({toStd(tex->absolute_filename), toStd(tex->filename), toStd(tex->relative_filename)}, info, shown);
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

const char* axisName(ufbx_coordinate_axis a) {
    switch (a) {
    case UFBX_COORDINATE_AXIS_POSITIVE_X: return "+X";
    case UFBX_COORDINATE_AXIS_NEGATIVE_X: return "-X";
    case UFBX_COORDINATE_AXIS_POSITIVE_Y: return "+Y";
    case UFBX_COORDINATE_AXIS_NEGATIVE_Y: return "-Y";
    case UFBX_COORDINATE_AXIS_POSITIVE_Z: return "+Z";
    case UFBX_COORDINATE_AXIS_NEGATIVE_Z: return "-Z";
    default: return "?";
    }
}

} // namespace

ModelData FbxFigureImporter::load(const std::string& path, RiggedImportInfo* info) const {
    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    opts.generate_missing_normals = true;
    opts.ignore_animation = true;
    opts.load_external_files = false;
    opts.filename.data = path.c_str();
    opts.filename.length = path.size();

    ufbx_error  error;
    ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &error);
    if (scene == nullptr) {
        char buffer[1024];
        ufbx_format_error(buffer, sizeof(buffer), &error);
        throw std::runtime_error(std::string("FBX: ") + buffer);
    }
    struct SceneGuard {
        ufbx_scene* s;
        ~SceneGuard() { ufbx_free_scene(s); }
    } guard{scene};

    if (info != nullptr) {
        char axes[128];
        std::snprintf(axes, sizeof(axes), "up %s, front %s, right %s; unit %.4g m", axisName(scene->settings.axes.up),
                      axisName(scene->settings.axes.front), axisName(scene->settings.axes.right),
                      static_cast<double>(scene->settings.unit_meters));
        info->sourceAxes = axes;
    }

    // ---- THE SKELETON: the nodes that are bones (a bone attribute, or a skin cluster's target)
    // and their ancestors up to the scene root, in depth-first order with parents first.
    const std::size_t nodeCount = scene->nodes.count;
    std::vector<char> wanted(nodeCount, 0);
    for (std::size_t i = 0; i < nodeCount; ++i) {
        const ufbx_node* node = scene->nodes.data[i];
        if (node->bone != nullptr && !node->is_root) {
            wanted[i] = 1;
        }
    }
    std::unordered_map<const ufbx_node*, const ufbx_skin_cluster*> clusterOf;
    for (std::size_t s = 0; s < scene->skin_deformers.count; ++s) {
        const ufbx_skin_deformer* skin = scene->skin_deformers.data[s];
        for (std::size_t c = 0; c < skin->clusters.count; ++c) {
            const ufbx_skin_cluster* cluster = skin->clusters.data[c];
            if (cluster->bone_node != nullptr && !cluster->bone_node->is_root) {
                wanted[cluster->bone_node->typed_id] = 1;
                clusterOf.emplace(cluster->bone_node, cluster);
            }
        }
    }
    for (std::size_t i = 0; i < nodeCount; ++i) {
        if (!wanted[i]) {
            continue;
        }
        for (const ufbx_node* p = scene->nodes.data[i]->parent; p != nullptr && !p->is_root; p = p->parent) {
            wanted[p->typed_id] = 1;
        }
    }
    // PRUNE the skinless leaves: a bone no cluster binds and no wanted child hangs from is no joint
    // — an exporter's "_end" marker at a chain's tip, a leaf control. Left in, a marker standing at
    // ankle height was read as a foot and planted on the floor, and the leg could not move. (A
    // skinless bone WITH children stays: the hips' root, a chain's link.)
    std::vector<char> prunedLeaf(nodeCount, 0);
    for (bool pruned = true; pruned;) {
        pruned = false;
        for (std::size_t i = 0; i < nodeCount; ++i) {
            const ufbx_node* node = scene->nodes.data[i];
            if (!wanted[i] || clusterOf.count(node) != 0) {
                continue;
            }
            bool wantedChild = false;
            for (std::size_t c = 0; c < node->children.count && !wantedChild; ++c) {
                wantedChild = wanted[node->children.data[c]->typed_id] != 0;
            }
            if (!wantedChild) {
                wanted[i] = 0;
                prunedLeaf[i] = 1;
                pruned = true;
                if (info != nullptr) {
                    ++info->prunedLeaves;
                }
            }
        }
    }
    // ... but a pruned leaf still says where its parent's TIP lies — the one thing a bone with no
    // joint below it (a single-bone toe, a fingertip) does not say for itself (ModelBone::tip).
    std::unordered_map<const ufbx_node*, const ufbx_node*> tipNodeOf;
    for (std::size_t i = 0; i < nodeCount; ++i) {
        const ufbx_node* node = scene->nodes.data[i];
        if (!wanted[i]) {
            continue;
        }
        for (std::size_t c = 0; c < node->children.count; ++c) {
            const ufbx_node* child = node->children.data[c];
            if (prunedLeaf[child->typed_id] && tipNodeOf.count(node) == 0) {
                tipNodeOf.emplace(node, child);
            }
        }
    }
    std::unordered_map<const ufbx_node*, const ufbx_matrix*> bindPoseOf;
    for (std::size_t p = 0; p < scene->poses.count; ++p) {
        const ufbx_pose* pose = scene->poses.data[p];
        if (!pose->is_bind_pose) {
            continue;
        }
        for (std::size_t b = 0; b < pose->bone_poses.count; ++b) {
            const ufbx_bone_pose& bp = pose->bone_poses.data[b];
            if (bp.bone_node != nullptr) {
                bindPoseOf.emplace(bp.bone_node, &bp.bone_to_world);
            }
        }
    }
    std::vector<const ufbx_node*> boneNodes;
    std::vector<int>              boneParent;
    std::unordered_map<const ufbx_node*, int> boneIndexOf;
    {
        // Depth-first from the root's children, parents before children.
        struct Frame {
            const ufbx_node* node;
            int              parent;
        };
        std::vector<Frame> stack;
        const ufbx_node* root = scene->root_node;
        // The FIGURE's tree first — of the root's children, the one with the most wanted nodes
        // below it: the bone list's first root is the figure's anatomical root to the posing
        // engine, and a shadow-catcher plane with a bone attribute, listed before the figure,
        // made itself that root and the figure a second tree outside the body.
        std::vector<std::pair<std::size_t, const ufbx_node*>> tops;
        for (std::size_t c = 0; c < root->children.count; ++c) {
            const ufbx_node* top = root->children.data[c];
            std::size_t      count = 0;
            std::vector<const ufbx_node*> walk{top};
            while (!walk.empty()) {
                const ufbx_node* nd = walk.back();
                walk.pop_back();
                count += wanted[nd->typed_id] ? 1 : 0;
                for (std::size_t k = 0; k < nd->children.count; ++k) {
                    walk.push_back(nd->children.data[k]);
                }
            }
            tops.emplace_back(count, top);
        }
        std::stable_sort(tops.begin(), tops.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (std::size_t c = tops.size(); c-- > 0;) {
            stack.push_back({tops[c].second, -1});
        }
        while (!stack.empty()) {
            const Frame f = stack.back();
            stack.pop_back();
            if (!wanted[f.node->typed_id]) {
                continue;
            }
            const int index = static_cast<int>(boneNodes.size());
            boneNodes.push_back(f.node);
            boneParent.push_back(f.parent);
            boneIndexOf.emplace(f.node, index);
            for (std::size_t c = f.node->children.count; c-- > 0;) {
                stack.push_back({f.node->children.data[c], index});
            }
        }
    }
    const int boneCount = static_cast<int>(boneNodes.size());
    std::vector<RawSkeletonBone> bones(boneNodes.size());
    std::vector<glm::mat4>       boneWorld(boneNodes.size(), glm::mat4(1.0f)); // (each bone's BIND frame in the world)
    static const bool            kBoneTrace = std::getenv("POSESTUDIO_FBX_BONE_TRACE") != nullptr; // where each bone's place came from
    for (int i = 0; i < boneCount; ++i) {
        const ufbx_node* node = boneNodes[static_cast<std::size_t>(i)];
        RawSkeletonBone& bone = bones[static_cast<std::size_t>(i)];
        glm::mat4        world = toGlm(node->node_to_world);
        const char*      source = "node";
        if (const auto it = clusterOf.find(node); it != clusterOf.end()) {
            world = toGlm(it->second->bind_to_world); // (what the skin is bound against)
            source = "cluster";
        } else if (const auto bp = bindPoseOf.find(node); bp != bindPoseOf.end()) {
            world = toGlm(*bp->second);
            source = "bind pose";
        } else if (boneParent[static_cast<std::size_t>(i)] >= 0) {
            // A bone no cluster binds and no bind pose lists RIDES ITS PARENT, as a marker does:
            // its offset in the parent's frame as the file stands, carried to the parent's bind.
            // Read at its own node_to_world it stood where the file's CURRENT pose put it — a
            // biped export whose feet are pointed in the saved pose had its clusterless toe bone,
            // the ball of the foot, 11cm under the floor at a bind whose skin rests on it
            // (2026-11-03): every floor phase read a joint 111mm through the floor.
            const int        p = boneParent[static_cast<std::size_t>(i)];
            const ufbx_node* parent = boneNodes[static_cast<std::size_t>(p)];
            const glm::mat4  local = glm::inverse(toGlm(parent->node_to_world)) * toGlm(node->node_to_world);
            world = boneWorld[static_cast<std::size_t>(p)] * local;
            source = "parent";
        }
        boneWorld[static_cast<std::size_t>(i)] = world;
        bone.name = toStd(node->name);
        bone.parent = boneParent[static_cast<std::size_t>(i)];
        bone.position = glm::vec3(world[3]);
        if (const auto tip = tipNodeOf.find(node); tip != tipNodeOf.end()) {
            // The marker rides its parent: its offset in the parent's frame, carried to the bind.
            const glm::mat4 local = glm::inverse(toGlm(node->node_to_world)) * toGlm(tip->second->node_to_world);
            bone.tip = glm::vec3((world * local)[3]);
            bone.hasTip = true;
        }
        if (kBoneTrace) {
            const glm::vec3 own(toGlm(node->node_to_world)[3]);
            std::fprintf(stderr, "[fbx-bone] %3d %-28s %-9s at (%.3f %.3f %.3f) node (%.3f %.3f %.3f)%s\n", i, bone.name.c_str(), source,
                         bone.position.x, bone.position.y, bone.position.z, own.x, own.y, own.z, bone.hasTip ? " +tip" : "");
        }
    }
    const auto attachBone = [&](const ufbx_node* node) {
        for (const ufbx_node* n = node; n != nullptr && !n->is_root; n = n->parent) {
            if (const auto it = boneIndexOf.find(n); it != boneIndexOf.end()) {
                return it->second;
            }
        }
        return 0;
    };

    // ---- THE MESHES.
    ModelData     model;
    TextureFinder textures(fs::path(path).parent_path());
    std::string   skinningMethod = boneCount > 0 ? "rigid" : "none";
    for (std::size_t mi = 0; mi < scene->meshes.count; ++mi) {
        const ufbx_mesh* mesh = scene->meshes.data[mi];
        if (mesh->instances.count == 0 || mesh->num_faces == 0) {
            continue;
        }
        const ufbx_node*          inst = mesh->instances.data[0];
        const ufbx_skin_deformer* skin = mesh->skin_deformers.count > 0 ? mesh->skin_deformers.data[0] : nullptr;
        if (mesh->instances.count > 1 && info != nullptr) {
            info->notes.push_back("mesh '" + toStd(mesh->name) + "' is instanced " + std::to_string(mesh->instances.count) + " times: the first instance imported");
        }
        // Per logical vertex: the influences (up to four, normalized) and the matrix that puts the
        // geometry at its BIND position in the world (the clusters' bind matrices blended by weight).
        const std::size_t          nv = mesh->num_vertices;
        std::vector<glm::uvec4>    joints(nv, glm::uvec4(0));
        std::vector<glm::vec4>     weights(nv, glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
        std::vector<glm::mat4>     toWorld(nv, toGlm(inst->geometry_to_world));
        bool                       skinned = false;
        if (skin != nullptr && boneCount > 0) {
            if (skin->skinning_method == UFBX_SKINNING_METHOD_DUAL_QUATERNION) {
                skinningMethod = "dual quaternion";
            } else if (skinningMethod != "dual quaternion") {
                skinningMethod = "linear";
            }
            std::vector<glm::mat4> clusterToWorld(skin->clusters.count, glm::mat4(1.0f));
            std::vector<int>       clusterBone(skin->clusters.count, -1);
            for (std::size_t c = 0; c < skin->clusters.count; ++c) {
                const ufbx_skin_cluster* cluster = skin->clusters.data[c];
                clusterToWorld[c] = toGlm(cluster->bind_to_world) * toGlm(cluster->geometry_to_bone);
                if (const auto it = boneIndexOf.find(cluster->bone_node); it != boneIndexOf.end()) {
                    clusterBone[c] = it->second;
                }
            }
            struct Influence {
                int   bone;
                float w;
                int   cluster;
            };
            std::vector<Influence> infl;
            for (std::size_t v = 0; v < nv && v < skin->vertices.count; ++v) {
                const ufbx_skin_vertex& sv = skin->vertices.data[v];
                infl.clear();
                for (uint32_t k = 0; k < sv.num_weights; ++k) {
                    const ufbx_skin_weight& sw = skin->weights.data[sv.weight_begin + k];
                    if (sw.cluster_index >= skin->clusters.count || clusterBone[sw.cluster_index] < 0 || sw.weight <= 0.0) {
                        continue;
                    }
                    infl.push_back({clusterBone[sw.cluster_index], static_cast<float>(sw.weight), static_cast<int>(sw.cluster_index)});
                }
                if (infl.empty()) {
                    joints[v] = glm::uvec4(static_cast<unsigned>(attachBone(inst)));
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
                glm::mat4 blend(0.0f);
                for (std::size_t k = 0; k < infl.size(); ++k) {
                    const float w = infl[k].w / total;
                    joints[v][static_cast<glm::length_t>(k)] = static_cast<unsigned>(infl[k].bone);
                    weights[v][static_cast<glm::length_t>(k)] = w;
                    blend += clusterToWorld[static_cast<std::size_t>(infl[k].cluster)] * w;
                }
                for (std::size_t k = infl.size(); k < 4; ++k) {
                    joints[v][static_cast<glm::length_t>(k)] = joints[v][0];
                    weights[v][static_cast<glm::length_t>(k)] = 0.0f;
                }
                toWorld[v] = blend;
            }
            skinned = true;
        } else if (boneCount > 0) {
            const unsigned bone = static_cast<unsigned>(attachBone(inst));
            for (std::size_t v = 0; v < nv; ++v) {
                joints[v] = glm::uvec4(bone);
            }
        }
        if (info != nullptr) {
            (skinned ? info->skinnedMeshes : info->rigidMeshes) += 1;
        }
        // Per material part: a MeshData.
        std::vector<uint32_t>   tri(std::max<std::size_t>(3, mesh->max_face_triangles * 3));
        const bool              haveNormals = mesh->vertex_normal.exists;
        const bool              haveUv = mesh->vertex_uv.exists;
        const std::size_t       partCount = mesh->material_parts.count > 0 ? mesh->material_parts.count : 1;
        for (std::size_t pi = 0; pi < partCount; ++pi) {
            const ufbx_mesh_part* part = mesh->material_parts.count > 0 ? &mesh->material_parts.data[pi] : nullptr;
            const ufbx_material*  mat = nullptr;
            if (part != nullptr && part->index < mesh->materials.count) {
                mat = mesh->materials.data[part->index];
            } else if (part == nullptr && mesh->materials.count > 0) {
                mat = mesh->materials.data[0];
            }
            MeshData out;
            out.normalsGenerated = mesh->generated_normals; // (the file had none: the import smooths by angle, smoothnormals.h)
            std::unordered_map<CornerKey, uint32_t, CornerKeyHash> corners;
            const std::size_t faceCount = part != nullptr ? part->num_faces : mesh->num_faces;
            for (std::size_t fi = 0; fi < faceCount; ++fi) {
                const uint32_t  faceIndex = part != nullptr ? part->face_indices.data[fi] : static_cast<uint32_t>(fi);
                const ufbx_face face = mesh->faces.data[faceIndex];
                if (face.num_indices < 3) {
                    continue;
                }
                const uint32_t triangles = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
                for (uint32_t t = 0; t < triangles * 3; ++t) {
                    const uint32_t  index = tri[t];
                    const uint32_t  v = mesh->vertex_indices.data[index];
                    const CornerKey key{v, haveNormals ? mesh->vertex_normal.indices.data[index] : 0u,
                                        haveUv ? mesh->vertex_uv.indices.data[index] : 0u};
                    auto it = corners.find(key);
                    if (it == corners.end()) {
                        Vertex vert;
                        const glm::mat4& m = toWorld[v];
                        vert.pos = glm::vec3(m * glm::vec4(toGlm(mesh->vertex_position.values.data[mesh->vertex_position.indices.data[index]]), 1.0f));
                        glm::vec3 n(0.0f, 1.0f, 0.0f);
                        if (haveNormals) {
                            n = glm::mat3(m) * toGlm(mesh->vertex_normal.values.data[key.normal]);
                            const float len = glm::length(n);
                            n = len > 1e-12f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
                        }
                        vert.normal = n;
                        if (haveUv) {
                            const ufbx_vec2 uv = mesh->vertex_uv.values.data[key.uv];
                            vert.uv = glm::vec2(static_cast<float>(uv.x), 1.0f - static_cast<float>(uv.y)); // (V origin bottom-left -> top-left)
                        } else {
                            vert.uv = glm::vec2(0.0f);
                        }
                        vert.joints = joints[v];
                        vert.weights = weights[v];
                        it = corners.emplace(key, static_cast<uint32_t>(out.vertices.size())).first;
                        out.vertices.push_back(vert);
                    }
                    out.indices.push_back(it->second);
                }
            }
            if (out.indices.size() < 3) {
                continue;
            }
            // The material: base color and maps from the PBR view of the material (ufbx unifies
            // the vendor materials), the classic FBX channels as the fallback.
            out.roughness = 0.65f;
            out.specularWeight = 0.6f;
            out.specularWeightWithMap = 0.6f;
            if (mat != nullptr) {
                if (info != nullptr) {
                    ++info->materials;
                }
                out.name = toStd(mat->name); // the Materials tab's zone name
                const ufbx_material_map& base = mat->pbr.base_color.has_value || mat->pbr.base_color.texture ? mat->pbr.base_color : mat->fbx.diffuse_color;
                if (base.has_value) {
                    out.baseColor = glm::vec3(srgbToLinear(static_cast<float>(base.value_vec4.x)), srgbToLinear(static_cast<float>(base.value_vec4.y)),
                                              srgbToLinear(static_cast<float>(base.value_vec4.z)));
                }
                const ufbx_texture* diffuseTex = mat->pbr.base_color.texture != nullptr ? mat->pbr.base_color.texture : mat->fbx.diffuse_color.texture;
                out.diffuse = resolveTexture(textures, diffuseTex, info);
                if (!out.diffuse.empty()) {
                    out.baseColor = glm::vec3(1.0f); // (the map carries the color)
                }
                const ufbx_texture* normalTex = mat->pbr.normal_map.texture != nullptr ? mat->pbr.normal_map.texture : mat->fbx.normal_map.texture;
                if (normalTex != nullptr) {
                    out.normal = resolveTexture(textures, normalTex, info);
                    out.normalMode = out.normal.empty() ? 0 : 1;
                } else if (mat->fbx.bump.texture != nullptr) {
                    out.normal = resolveTexture(textures, mat->fbx.bump.texture, info);
                    out.normalMode = out.normal.empty() ? 0 : 2;
                    if (mat->fbx.bump_factor.has_value) {
                        out.normalStrength = std::max(0.1f, std::min(8.0f, static_cast<float>(mat->fbx.bump_factor.value_real)));
                    }
                }
                if (mat->pbr.roughness.has_value) {
                    out.roughness = std::max(0.05f, std::min(1.0f, static_cast<float>(mat->pbr.roughness.value_real)));
                } else if (mat->pbr.glossiness.has_value) {
                    out.roughness = std::max(0.05f, std::min(1.0f, 1.0f - static_cast<float>(mat->pbr.glossiness.value_real)));
                }
                out.lobe1Roughness = out.roughness;
                out.lobe2Roughness = out.roughness;
                if (mat->pbr.roughness.texture != nullptr) {
                    out.roughnessMap = resolveTexture(textures, mat->pbr.roughness.texture, info);
                    if (!out.roughnessMap.empty() && !mat->pbr.roughness.has_value) {
                        out.roughness = 1.0f; // (the map carries it; the value is the factor where there is one)
                    }
                }
                if (mat->pbr.metalness.has_value) {
                    out.metalness = std::max(0.0f, std::min(1.0f, static_cast<float>(mat->pbr.metalness.value_real)));
                }
                if (mat->pbr.metalness.texture != nullptr) {
                    // The metalness MAP (the Materials tab, 2026-11-09): read into the parameter
                    // image's green channel by the decode layer; the value is its factor.
                    out.metalnessMap = resolveTexture(textures, mat->pbr.metalness.texture, info);
                    if (!out.metalnessMap.empty() && !mat->pbr.metalness.has_value) {
                        out.metalness = 1.0f;
                    }
                }
                // THE ONE MATERIAL MODEL (specgloss.h): a SPECULAR / GLOSSINESS material — 3ds Max's
                // PBR spec/gloss shader, whose specular color IS a reflectance — is converted by the
                // standard solve (the scalars here, the maps per texel in the decode layer); a PHONG
                // or Lambert material's specular color is a highlight, not a reflectance, and gives
                // only the specular WEIGHT by its grey (Blender's and Maya's default 0.5 = the
                // standard dielectric; black = matte). A material with a metalness of its own is
                // left as it is.
                if (mat->shader_type == UFBX_SHADER_3DS_MAX_PBR_SPEC_GLOSS && !mat->pbr.metalness.has_value &&
                    mat->pbr.metalness.texture == nullptr) {
                    const ufbx_material_map& spec = mat->pbr.specular_color;
                    const glm::vec3 specColor = spec.has_value ? glm::vec3(srgbToLinear(static_cast<float>(spec.value_vec4.x)),
                                                                           srgbToLinear(static_cast<float>(spec.value_vec4.y)),
                                                                           srgbToLinear(static_cast<float>(spec.value_vec4.z)))
                                                               : glm::vec3(0.04f);
                    const float specFactor = mat->pbr.specular_factor.has_value ? static_cast<float>(mat->pbr.specular_factor.value_real) : 1.0f;
                    const glm::vec3 diffuse = base.has_value ? out.baseColor : glm::vec3(0.8f);
                    const MetallicRoughness mr = convertSpecGloss(out.diffuse.empty() ? diffuse : glm::vec3(0.8f), specColor * specFactor,
                                                                  1.0f - out.roughness);
                    out.convertSpecGloss = true;
                    out.specularColorFactor = specColor * specFactor;
                    out.specularColorMap = resolveTexture(textures, spec.texture, info);
                    out.glossinessFactor = 1.0f - out.roughness;
                    if (out.diffuse.empty() && out.specularColorMap.empty()) {
                        out.baseColor = mr.baseColor; // the scalar solve: no maps to bake
                        out.metalness = mr.metalness;
                    }
                    out.specularWeight = 1.0f; // (the solve's dielectric is the standard one)
                    out.specularWeightWithMap = 1.0f;
                    if (info != nullptr) {
                        info->notes.push_back("material '" + out.name + "' converted from specular / glossiness to metallic / roughness");
                    }
                } else if (!mat->pbr.metalness.has_value && mat->pbr.metalness.texture == nullptr &&
                           (mat->shader_type == UFBX_SHADER_FBX_PHONG || mat->shader_type == UFBX_SHADER_BLENDER_PHONG ||
                            mat->shader_type == UFBX_SHADER_FBX_LAMBERT || mat->shader_type == UFBX_SHADER_UNKNOWN) &&
                           mat->fbx.specular_color.has_value) {
                    const ufbx_vec4& ks = mat->fbx.specular_color.value_vec4;
                    const float     factor = mat->fbx.specular_factor.has_value ? static_cast<float>(mat->fbx.specular_factor.value_real) : 1.0f;
                    const float weight = phongSpecularWeight(glm::vec3(static_cast<float>(ks.x), static_cast<float>(ks.y), static_cast<float>(ks.z)), factor);
                    out.specularWeight = weight;
                    out.specularWeightWithMap = weight;
                    if (mat->fbx.specular_color.texture != nullptr) {
                        out.specMask = resolveTexture(textures, mat->fbx.specular_color.texture, info); // the highlight's map as the mask
                    }
                }
                float opacity = 1.0f;
                if (mat->pbr.opacity.has_value) {
                    opacity = static_cast<float>(mat->pbr.opacity.value_real);
                } else if (mat->fbx.transparency_factor.has_value) {
                    opacity = 1.0f - static_cast<float>(mat->fbx.transparency_factor.value_real);
                    // Degenerate 0/1 opacity needs a fallback: some exporters write
                    // TransparencyFactor inverted (1.0 = opaque) or only as a template default,
                    // with the real alpha in TransparentColor (black = opaque) — the factor
                    // alone says nothing there. A factor of exactly 0 stays opaque though: in
                    // the format's own terms transparency is the factor scaled by the colour, so
                    // 0 is opaque whatever the colour says.
                    if (opacity == 0.0f || opacity == 1.0f) {
                        opacity = mat->fbx.transparency_factor.value_real == 1.0f
                                      ? 1.0f - static_cast<float>(mat->fbx.transparency_color.value_vec4.x)
                                      : 1.0f;
                    }
                }
                out.opacity = std::max(0.0f, std::min(1.0f, opacity));
                if (mat->pbr.opacity.texture != nullptr) {
                    out.opacityMask = resolveTexture(textures, mat->pbr.opacity.texture, info);
                } else if (mat->fbx.transparency_color.texture != nullptr) {
                    out.opacityMask = resolveTexture(textures, mat->fbx.transparency_color.texture, info);
                }
                // THE CANONICAL MATERIAL's further channels, from ufbx's unified PBR view (every
                // vendor shader it knows maps onto it; a channel the file does not author keeps the
                // neutral default). A classic Phong / Lambert material's specular color is its
                // highlight, read above as the weight — never a tint here.
                const auto pbrReal = [](const ufbx_material_map& m, float& value, float lo, float hi) {
                    if (m.has_value) {
                        value = std::max(lo, std::min(hi, static_cast<float>(m.value_real)));
                    }
                };
                const auto pbrColor = [](const ufbx_material_map& m, glm::vec3& value) {
                    if (m.has_value) {
                        value = glm::vec3(static_cast<float>(m.value_vec3.x), static_cast<float>(m.value_vec3.y), static_cast<float>(m.value_vec3.z));
                    }
                };
                {
                    glm::vec3  e(1.0f);
                    float      factor = 0.0f;
                    const bool hasColor = mat->pbr.emission_color.has_value || mat->pbr.emission_color.texture != nullptr;
                    pbrColor(mat->pbr.emission_color, e);
                    if (mat->pbr.emission_factor.has_value) {
                        factor = std::max(0.0f, static_cast<float>(mat->pbr.emission_factor.value_real));
                    } else if (hasColor) {
                        factor = 1.0f;
                    }
                    const float peak = std::max(e.x, std::max(e.y, e.z));
                    if (factor > 0.0f && peak > 0.0f) {
                        out.emissionColor = e / peak;
                        out.emission = factor * peak;
                        if (mat->pbr.emission_color.texture != nullptr) {
                            out.emissionMap = resolveTexture(textures, mat->pbr.emission_color.texture, info);
                        }
                    }
                }
                const bool pbrShader = mat->shader_type != UFBX_SHADER_UNKNOWN && mat->shader_type != UFBX_SHADER_FBX_LAMBERT &&
                                       mat->shader_type != UFBX_SHADER_FBX_PHONG && mat->shader_type != UFBX_SHADER_3DS_MAX_PBR_SPEC_GLOSS;
                pbrReal(mat->pbr.specular_ior, out.ior, 1.0f, 3.0f);
                if (pbrShader) {
                    pbrColor(mat->pbr.specular_color, out.specularColor);
                }
                pbrReal(mat->pbr.specular_anisotropy, out.anisotropy, 0.0f, 1.0f);
                if (mat->pbr.specular_rotation.has_value) {
                    out.anisotropyRotation = static_cast<float>(mat->pbr.specular_rotation.value_real) * 360.0f; // (turns)
                }
                pbrReal(mat->pbr.diffuse_roughness, out.diffuseRoughness, 0.0f, 1.0f);
                if (mat->pbr.diffuse_roughness.texture != nullptr) {
                    out.diffuseRoughnessMap = resolveTexture(textures, mat->pbr.diffuse_roughness.texture, info);
                    if (!out.diffuseRoughnessMap.empty() && !mat->pbr.diffuse_roughness.has_value) {
                        out.diffuseRoughness = 1.0f; // (the map carries it)
                    }
                }
                pbrReal(mat->pbr.transmission_factor, out.transmission, 0.0f, 1.0f);
                pbrColor(mat->pbr.transmission_color, out.transmissionColor);
                pbrReal(mat->pbr.transmission_depth, out.transmissionDepth, 0.0f, 1e6f);
                pbrReal(mat->pbr.transmission_roughness, out.transmissionRoughness, 0.0f, 1.0f); // (its own, where authored)
                pbrReal(mat->pbr.transmission_dispersion, out.dispersion, 0.0f, 1.0f);
                pbrColor(mat->pbr.subsurface_color, out.subsurfaceColor);
                pbrColor(mat->pbr.subsurface_radius, out.subsurfaceRadiusColor);
                pbrReal(mat->pbr.subsurface_scale, out.subsurfaceRadius, 0.0f, 100.0f);
                pbrReal(mat->pbr.subsurface_anisotropy, out.subsurfaceAnisotropy, -1.0f, 1.0f);
                if (mat->pbr.subsurface_factor.texture != nullptr) {
                    out.translucencyWeightMap = resolveTexture(textures, mat->pbr.subsurface_factor.texture, info);
                }
                pbrReal(mat->pbr.sheen_factor, out.sheen, 0.0f, 1.0f);
                pbrColor(mat->pbr.sheen_color, out.sheenColor);
                pbrReal(mat->pbr.sheen_roughness, out.sheenRoughness, 0.0f, 1.0f);
                pbrReal(mat->pbr.coat_factor, out.topCoatWeight, 0.0f, 1.0f);
                pbrReal(mat->pbr.coat_roughness, out.topCoatRoughness, 0.0f, 1.0f);
                pbrColor(mat->pbr.coat_color, out.coatColor);
                pbrReal(mat->pbr.coat_ior, out.coatIor, 1.0f, 3.0f);
                const auto pbrMap = [&](const ufbx_material_map& m, TextureSource& slot) {
                    if (m.texture != nullptr) {
                        slot = resolveTexture(textures, m.texture, info);
                    }
                };
                pbrMap(mat->pbr.coat_factor, out.coatWeightMap);
                pbrMap(mat->pbr.coat_color, out.coatColorMap);
                pbrMap(mat->pbr.coat_roughness, out.coatRoughnessMap);
                pbrMap(mat->pbr.coat_ior, out.coatReflectivityMap);
                pbrMap(mat->pbr.coat_normal, out.coatBumpMap);
                pbrMap(mat->pbr.specular_color, out.specularColorSlotMap);
                pbrMap(mat->pbr.specular_anisotropy, out.anisotropyMap);
                pbrMap(mat->pbr.specular_rotation, out.anisotropyRotationMap);
                pbrMap(mat->pbr.ambient_occlusion, out.aoMap);
                pbrMap(mat->pbr.transmission_color, out.transmissionColorMap);
                pbrMap(mat->pbr.transmission_factor, out.transmissionWeightMap);
                pbrReal(mat->pbr.coat_anisotropy, out.coatAnisotropy, 0.0f, 1.0f);
                pbrReal(mat->pbr.coat_rotation, out.coatAnisotropyRotation, 0.0f, 360.0f);
                pbrMap(mat->pbr.coat_anisotropy, out.coatAnisotropyMap);
                pbrMap(mat->pbr.coat_rotation, out.coatAnisotropyRotationMap);
                pbrMap(mat->pbr.sheen_factor, out.sheenWeightMap);
                pbrMap(mat->pbr.sheen_color, out.sheenColorMap);
                pbrMap(mat->pbr.sheen_roughness, out.sheenRoughnessMap);
                pbrMap(mat->pbr.thin_film_factor, out.thinFilmWeightMap);
                pbrMap(mat->pbr.thin_film_thickness, out.thinFilmThicknessMap);
                // Displacement: the unified map, else the classic one, its factor as the strength.
                if (mat->pbr.displacement_map.texture != nullptr) {
                    out.displacementMap = resolveTexture(textures, mat->pbr.displacement_map.texture, info);
                } else if (mat->fbx.displacement.texture != nullptr) {
                    out.displacementMap = resolveTexture(textures, mat->fbx.displacement.texture, info);
                }
                if (!out.displacementMap.empty() && mat->fbx.displacement_factor.has_value) {
                    out.displacementStrength = std::clamp(static_cast<float>(mat->fbx.displacement_factor.value_real), -100.0f, 100.0f);
                }
                pbrReal(mat->pbr.thin_film_factor, out.thinFilm, 0.0f, 1.0f);
                pbrReal(mat->pbr.thin_film_thickness, out.thinFilmThickness, 0.0f, 2000.0f);
                pbrReal(mat->pbr.thin_film_ior, out.thinFilmIor, 1.0f, 3.0f);
                out.thinWalled = mat->features.thin_walled.enabled;
            }
            model.meshes.push_back(std::move(out));
        }
    }
    if (model.meshes.empty()) {
        throw std::runtime_error("FBX: the file holds no mesh");
    }

    // ---- FACING, CENTRE AND FLOOR, and the raw bones: the step every rigged reader shares.
    finishRiggedModel(model, bones, info);

    // The skinning method is the rig's: a file authored for linear blend skinning is blended
    // linearly in the viewport (MeshData::linearSkinning; the native figures stay dual quaternion).
    static const bool kForceDq = std::getenv("POSESTUDIO_FBX_DQ_SKINNING") != nullptr; // A/B: dual quaternions as before
    if (!kForceDq && skinningMethod == "linear") {
        for (MeshData& mesh : model.meshes) {
            mesh.linearSkinning = true;
        }
    }
    if (info != nullptr) {
        info->skinningMethod = skinningMethod;
    }
    return model;
}

} // namespace pose
