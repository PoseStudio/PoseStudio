/**
 * @file fbxfigureimporter.cpp
 * @brief The FBX figure reader (see fbxfigureimporter.h): ufbx -> ModelData.
 */

#include "fbxfigureimporter.h"

#include <ufbx.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
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

std::string lowerCopy(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool isImageFile(const fs::path& p) {
    static const char* const kExt[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".tif", ".tiff", ".webp", ".exr", ".hdr", ".dds", ".psd", ".gif"};
    const std::string ext = lowerCopy(p.extension().string());
    for (const char* e : kExt) {
        if (ext == e) {
            return true;
        }
    }
    return false;
}

/// sRGB -> linear (the renderer's base colour is linear; authored colours are sRGB).
float srgbToLinear(float c) {
    c = std::max(0.0f, std::min(1.0f, c));
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

/// Finds a texture's file: embedded content first, then the paths the file gives (absolute, or
/// relative to the FBX), then a file of that name anywhere under the FBX's folder.
class TextureFinder {
public:
    explicit TextureFinder(fs::path dir) : m_dir(std::move(dir)) {}

    TextureSource resolve(const ufbx_texture* tex, FbxImportInfo* info) {
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
        const std::string candidates[] = {toStd(tex->absolute_filename), toStd(tex->filename), toStd(tex->relative_filename)};
        std::error_code   ec;
        for (const std::string& c : candidates) {
            if (c.empty()) {
                continue;
            }
            const fs::path p(c);
            if (p.is_absolute() && fs::is_regular_file(p, ec)) {
                return found(p, info);
            }
            const fs::path rel = m_dir / p;
            if (fs::is_regular_file(rel, ec)) {
                return found(rel, info);
            }
            // (... or against the folder ABOVE the file's: exports keep "textures/" beside a
            // "formats/" folder, and write the path relative to the project.)
            const fs::path above = m_dir.parent_path() / p;
            if (!m_dir.parent_path().empty() && fs::is_regular_file(above, ec)) {
                return found(above, info);
            }
        }
        for (const std::string& c : candidates) {
            if (c.empty()) {
                continue;
            }
            const std::string name = lowerCopy(fs::path(c).filename().string());
            if (name.empty()) {
                continue;
            }
            index();
            const auto it = m_byName.find(name);
            if (it != m_byName.end()) {
                return found(it->second, info);
            }
        }
        if (info != nullptr) {
            ++info->missingTextures;
            std::string shown = toStd(tex->relative_filename);
            if (shown.empty()) {
                shown = toStd(tex->filename);
            }
            if (m_missingNoted.insert(shown).second && m_missingNoted.size() <= 8) {
                info->notes.push_back("texture not found: " + shown);
            }
        }
        return out;
    }

private:
    TextureSource found(const fs::path& p, FbxImportInfo* info) {
        TextureSource out;
        out.path = p.string();
        if (info != nullptr) {
            ++info->externalTextures;
        }
        return out;
    }

    void index() {
        if (m_indexed) {
            return;
        }
        m_indexed = true;
        std::error_code ec;
        // The file's own folder, deep (an export's texture tree is a few folders deep), then the
        // folder above it, shallower (a "textures/" beside a "formats/"; a library root is not what
        // sits beside a character — the walk is capped either way).
        const fs::path above = m_dir.parent_path();
        const struct { fs::path dir; int depth; } roots[] = {{m_dir, 8}, {above, 3}};
        std::size_t files = 0;
        for (const auto& r : roots) {
            if (r.dir.empty() || !fs::is_directory(r.dir, ec)) {
                continue;
            }
            fs::recursive_directory_iterator it(r.dir, fs::directory_options::skip_permission_denied, ec);
            const fs::recursive_directory_iterator end;
            for (; it != end && files < 20000; it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                if (it.depth() > r.depth) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(ec) || !isImageFile(it->path())) {
                    continue;
                }
                ++files;
                m_byName.emplace(lowerCopy(it->path().filename().string()), it->path()); // (the first found wins: the file's own folder)
            }
        }
    }

    fs::path                                   m_dir;
    bool                                       m_indexed = false;
    std::unordered_map<std::string, fs::path>  m_byName;
    std::unordered_set<std::string>            m_missingNoted;
};

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

ModelData FbxFigureImporter::load(const std::string& path, FbxImportInfo* info) const {
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
    std::vector<glm::vec3> bonePos(boneNodes.size(), glm::vec3(0.0f));
    std::vector<glm::mat4> boneWorld(boneNodes.size(), glm::mat4(1.0f)); // (each bone's BIND frame in the world)
    std::vector<glm::vec3> tipPos(boneNodes.size(), glm::vec3(0.0f)); // (the pruned end marker's place, in the bind)
    std::vector<char>      hasTip(boneNodes.size(), 0);
    static const bool      kBoneTrace = std::getenv("POSESTUDIO_FBX_BONE_TRACE") != nullptr; // where each bone's place came from
    for (int i = 0; i < boneCount; ++i) {
        const ufbx_node* node = boneNodes[static_cast<std::size_t>(i)];
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
        bonePos[static_cast<std::size_t>(i)] = glm::vec3(world[3]);
        if (const auto tip = tipNodeOf.find(node); tip != tipNodeOf.end()) {
            // The marker rides its parent: its offset in the parent's frame, carried to the bind.
            const glm::mat4 local = glm::inverse(toGlm(node->node_to_world)) * toGlm(tip->second->node_to_world);
            tipPos[static_cast<std::size_t>(i)] = glm::vec3((world * local)[3]);
            hasTip[static_cast<std::size_t>(i)] = 1;
        }
        if (kBoneTrace) {
            const glm::vec3 own(toGlm(node->node_to_world)[3]);
            std::fprintf(stderr, "[fbx-bone] %3d %-28s %-9s at (%.3f %.3f %.3f) node (%.3f %.3f %.3f)%s\n", i, toStd(node->name).c_str(), source,
                         bonePos[static_cast<std::size_t>(i)].x, bonePos[static_cast<std::size_t>(i)].y, bonePos[static_cast<std::size_t>(i)].z, own.x,
                         own.y, own.z, hasTip[static_cast<std::size_t>(i)] ? " +tip" : "");
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
    std::size_t   totalVertices = 0;
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
            // The material: base colour and maps from the PBR view of the material (ufbx unifies
            // the vendor materials), the classic FBX channels as the fallback.
            out.roughness = 0.65f;
            out.specularWeight = 0.6f;
            out.specularWeightWithMap = 0.6f;
            if (mat != nullptr) {
                if (info != nullptr) {
                    ++info->materials;
                }
                const ufbx_material_map& base = mat->pbr.base_color.has_value || mat->pbr.base_color.texture ? mat->pbr.base_color : mat->fbx.diffuse_color;
                if (base.has_value) {
                    out.baseColor = glm::vec3(srgbToLinear(static_cast<float>(base.value_vec4.x)), srgbToLinear(static_cast<float>(base.value_vec4.y)),
                                              srgbToLinear(static_cast<float>(base.value_vec4.z)));
                }
                const ufbx_texture* diffuseTex = mat->pbr.base_color.texture != nullptr ? mat->pbr.base_color.texture : mat->fbx.diffuse_color.texture;
                out.diffuse = textures.resolve(diffuseTex, info);
                if (!out.diffuse.empty()) {
                    out.baseColor = glm::vec3(1.0f); // (the map carries the colour)
                }
                const ufbx_texture* normalTex = mat->pbr.normal_map.texture != nullptr ? mat->pbr.normal_map.texture : mat->fbx.normal_map.texture;
                if (normalTex != nullptr) {
                    out.normal = textures.resolve(normalTex, info);
                    out.normalMode = out.normal.empty() ? 0 : 1;
                } else if (mat->fbx.bump.texture != nullptr) {
                    out.normal = textures.resolve(mat->fbx.bump.texture, info);
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
                    out.roughnessMap = textures.resolve(mat->pbr.roughness.texture, info);
                }
                if (mat->pbr.metalness.has_value) {
                    out.metalness = std::max(0.0f, std::min(1.0f, static_cast<float>(mat->pbr.metalness.value_real)));
                }
                float opacity = 1.0f;
                if (mat->pbr.opacity.has_value) {
                    opacity = static_cast<float>(mat->pbr.opacity.value_real);
                } else if (mat->fbx.transparency_factor.has_value) {
                    opacity = 1.0f - static_cast<float>(mat->fbx.transparency_factor.value_real);
                }
                out.opacity = std::max(0.0f, std::min(1.0f, opacity));
                if (mat->pbr.opacity.texture != nullptr) {
                    out.opacityMask = textures.resolve(mat->pbr.opacity.texture, info);
                } else if (mat->fbx.transparency_color.texture != nullptr) {
                    out.opacityMask = textures.resolve(mat->fbx.transparency_color.texture, info);
                }
            }
            totalVertices += out.vertices.size();
            model.meshes.push_back(std::move(out));
        }
    }
    if (model.meshes.empty()) {
        throw std::runtime_error("FBX: the file holds no mesh");
    }

    // ---- FACING, CENTRE AND FLOOR. The figure faces +z with her left at +x, stands over the origin,
    // and her lowest point rests on y = 0 — what every reader of the bind pose takes for granted.
    float yawDeg = 0.0f;
    if (boneCount >= 3) {
        float     minY = std::numeric_limits<float>::max();
        float     maxY = -std::numeric_limits<float>::max();
        glm::vec3 mean(0.0f);
        for (const glm::vec3& p : bonePos) {
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
            mean += p;
        }
        mean /= static_cast<float>(boneCount);
        const float height = maxY - minY;
        // THE LATERAL AXIS is the skeleton's widest horizontal spread (a standing figure is wider
        // than deep, arms out or down), read off the bone positions' covariance in the floor plane
        // — exact to a fraction of a degree where the toes alone (splayed, few) are not; the toes
        // give the SIGN: they point forward.
        float sxx = 0.0f, sxz = 0.0f, szz = 0.0f;
        for (const glm::vec3& p : bonePos) {
            sxx += (p.x - mean.x) * (p.x - mean.x);
            sxz += (p.x - mean.x) * (p.z - mean.z);
            szz += (p.z - mean.z) * (p.z - mean.z);
        }
        const float lateralAngle = 0.5f * std::atan2(2.0f * sxz, sxx - szz); // the major axis, from +x toward +z
        glm::vec3   lateral(std::cos(lateralAngle), 0.0f, std::sin(lateralAngle));
        glm::vec3   forward = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), lateral); // one of the two perpendiculars
        glm::vec3   toes(0.0f);
        if (height > 0.2f) {
            const float lowY = minY + 0.12f * height;
            for (int i = 0; i < boneCount; ++i) {
                const int p = boneParent[static_cast<std::size_t>(i)];
                if (p < 0) {
                    continue;
                }
                const glm::vec3& a = bonePos[static_cast<std::size_t>(p)];
                const glm::vec3& b = bonePos[static_cast<std::size_t>(i)];
                if (a.y < lowY && b.y < lowY) {
                    toes += glm::vec3(b.x - a.x, 0.0f, b.z - a.z);
                }
            }
        }
        const float spread = std::abs(sxx - szz) + std::abs(sxz);
        if (spread > 1.0e-4f && glm::length(toes) > 0.01f * std::max(height, 0.2f)) {
            if (glm::dot(forward, toes) < 0.0f) {
                forward = -forward;
            }
            yawDeg = glm::degrees(std::atan2(forward.x, forward.z));
        } else if (glm::length(toes) > 0.01f * std::max(height, 0.2f)) {
            yawDeg = glm::degrees(std::atan2(toes.x, toes.z)); // (no clear spread: the toes alone)
        } else if (info != nullptr) {
            info->notes.push_back("the figure's facing could not be read off the skeleton: assumed to face +z");
        }
    }
    if (std::abs(yawDeg) > 0.5f) {
        const glm::mat4 turn = glm::rotate(glm::mat4(1.0f), glm::radians(-yawDeg), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat3 turn3(turn);
        for (glm::vec3& p : bonePos) {
            p = turn3 * p;
        }
        for (glm::vec3& p : tipPos) {
            p = turn3 * p;
        }
        for (MeshData& mesh : model.meshes) {
            for (Vertex& v : mesh.vertices) {
                v.pos = turn3 * v.pos;
                v.normal = turn3 * v.normal;
            }
        }
    }
    glm::vec3 centre(0.0f);
    if (boneCount > 0) {
        // Over the HIPS: the first bone (parents first) with two children or more — the body's
        // midline runs through it, and the engine measures the sides from it.
        std::vector<int> childCount(boneNodes.size(), 0);
        for (int i = 0; i < boneCount; ++i) {
            const int p = boneParent[static_cast<std::size_t>(i)];
            if (p >= 0) {
                ++childCount[static_cast<std::size_t>(p)];
            }
        }
        int hips = -1;
        for (int i = 0; i < boneCount && hips < 0; ++i) {
            if (childCount[static_cast<std::size_t>(i)] >= 2) {
                hips = i;
            }
        }
        if (hips >= 0) {
            centre = bonePos[static_cast<std::size_t>(hips)];
        } else {
            for (const glm::vec3& p : bonePos) {
                centre += p;
            }
            centre /= static_cast<float>(boneCount);
        }
    } else {
        std::size_t count = 0;
        for (const MeshData& mesh : model.meshes) {
            for (const Vertex& v : mesh.vertices) {
                centre += v.pos;
                ++count;
            }
        }
        centre /= static_cast<float>(std::max<std::size_t>(1, count));
    }
    float lowest = std::numeric_limits<float>::max();
    float highest = -std::numeric_limits<float>::max();
    for (const MeshData& mesh : model.meshes) {
        for (const Vertex& v : mesh.vertices) {
            lowest = std::min(lowest, v.pos.y);
            highest = std::max(highest, v.pos.y);
        }
    }
    const glm::vec3 shift(-centre.x, -lowest, -centre.z);
    for (MeshData& mesh : model.meshes) {
        for (Vertex& v : mesh.vertices) {
            v.pos += shift;
        }
    }
    for (glm::vec3& p : bonePos) {
        p += shift;
    }
    for (glm::vec3& p : tipPos) {
        p += shift;
    }

    // ---- THE BONES, raw: names, hierarchy, bind positions. Frames and limits are the conformer's.
    model.bones.reserve(boneNodes.size());
    std::unordered_set<std::string> usedNames;
    for (int i = 0; i < boneCount; ++i) {
        ModelBone bone;
        bone.name = toStd(boneNodes[static_cast<std::size_t>(i)]->name);
        if (bone.name.empty()) {
            bone.name = "bone_" + std::to_string(i);
        }
        for (int suffix = 2; !usedNames.insert(bone.name).second; ++suffix) {
            bone.name = toStd(boneNodes[static_cast<std::size_t>(i)]->name) + "_" + std::to_string(suffix);
        }
        bone.parent = boneParent[static_cast<std::size_t>(i)];
        bone.bindGlobal = glm::translate(glm::mat4(1.0f), bonePos[static_cast<std::size_t>(i)]);
        bone.rotationOrder = "XYZ";
        bone.tip = tipPos[static_cast<std::size_t>(i)];
        bone.hasTip = hasTip[static_cast<std::size_t>(i)] != 0;
        model.bones.push_back(std::move(bone));
    }

    // The skinning method is the rig's: a file authored for linear blend skinning is blended
    // linearly in the viewport (MeshData::linearSkinning; the native figures stay dual quaternion).
    static const bool kForceDq = std::getenv("POSESTUDIO_FBX_DQ_SKINNING") != nullptr; // A/B: dual quaternions as before
    if (!kForceDq && skinningMethod == "linear") {
        for (MeshData& mesh : model.meshes) {
            mesh.linearSkinning = true;
        }
    }
    if (info != nullptr) {
        info->bones = boneCount;
        info->heightMetres = highest - lowest;
        info->yawDegrees = yawDeg;
        info->groundedMm = -lowest * 1000.0f;
        info->skinningMethod = skinningMethod;
        if (info->heightMetres < 1.2f || info->heightMetres > 2.4f) {
            char line[160];
            std::snprintf(line, sizeof(line), "the figure stands %.2f m tall after the unit conversion: an unusual height for a human", static_cast<double>(info->heightMetres));
            info->notes.push_back(line);
        }
        if (totalVertices == 0) {
            info->notes.push_back("no vertices");
        }
    }
    return model;
}

} // namespace pose
