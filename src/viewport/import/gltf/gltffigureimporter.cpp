/**
 * @file gltffigureimporter.cpp
 * @brief The glTF figure reader (see gltffigureimporter.h): cgltf -> ModelData. The single TU
 *        that compiles cgltf's implementation.
 */

#include "gltffigureimporter.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace pose {

namespace {

namespace fs = std::filesystem;

const char* resultName(cgltf_result r) {
    switch (r) {
    case cgltf_result_success: return "success";
    case cgltf_result_data_too_short: return "the file is truncated";
    case cgltf_result_unknown_format: return "not a glTF or GLB file";
    case cgltf_result_invalid_json: return "the JSON is malformed";
    case cgltf_result_invalid_gltf: return "the glTF is invalid";
    case cgltf_result_invalid_options: return "invalid options";
    case cgltf_result_file_not_found: return "a file it refers to was not found";
    case cgltf_result_io_error: return "the file could not be read";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "a glTF 1.0 file (only glTF 2.0 is supported)";
    default: return "unknown error";
    }
}

glm::mat4 worldOf(const cgltf_node* node) {
    float m[16];
    cgltf_node_transform_world(node, m);
    return glm::make_mat4(m);
}

/// A base64 data URI's payload (the part after the comma), decoded. Empty on a malformed string.
std::vector<uint8_t> decodeBase64(const char* text) {
    static const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    int                  bits = 0;
    unsigned             acc = 0;
    for (const char* p = text; *p != '\0' && *p != '"'; ++p) {
        if (*p == '=') {
            break;
        }
        const int v = value(*p);
        if (v < 0) {
            continue; // whitespace
        }
        acc = (acc << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFFu));
        }
    }
    return out;
}

/// A glTF image: embedded in a buffer view (GLB, or a .gltf whose buffer holds it), a base64 data
/// URI, or a file beside the glTF (its URI percent-decoded), found through the shared finder.
TextureSource resolveImage(TextureFinder& finder, const cgltf_image* image, RiggedImportInfo* info) {
    TextureSource out;
    if (image == nullptr) {
        return out;
    }
    if (image->buffer_view != nullptr) {
        const cgltf_buffer_view* view = image->buffer_view;
        const uint8_t*           base = view->data != nullptr ? static_cast<const uint8_t*>(view->data)
                                                              : (view->buffer != nullptr && view->buffer->data != nullptr
                                                                     ? static_cast<const uint8_t*>(view->buffer->data) + view->offset
                                                                     : nullptr);
        if (base != nullptr && view->size > 0) {
            out.encoded.assign(base, base + view->size);
            if (info != nullptr) {
                ++info->embeddedTextures;
            }
            return out;
        }
    }
    if (image->uri != nullptr) {
        if (std::strncmp(image->uri, "data:", 5) == 0) {
            const char* comma = std::strchr(image->uri, ',');
            if (comma != nullptr) {
                out.encoded = decodeBase64(comma + 1);
                if (!out.encoded.empty()) {
                    if (info != nullptr) {
                        ++info->embeddedTextures;
                    }
                    return out;
                }
            }
            if (info != nullptr) {
                ++info->missingTextures;
                info->notes.push_back("an embedded (data URI) image could not be decoded");
            }
            return out;
        }
        std::string uri(image->uri);
        cgltf_decode_uri(uri.data()); // ("%20" and friends; shortens in place)
        uri.resize(std::strlen(uri.c_str()));
        return finder.resolve({uri}, info, uri);
    }
    if (info != nullptr) {
        ++info->missingTextures;
    }
    return out;
}

/// The image a texture samples. A texture that only carries a Basis Universal or WebP image
/// through its extension has no image the viewport can decode; WebP decodes when Qt's image
/// formats add-on is present, so it is taken as a fallback.
const cgltf_image* imageOf(const cgltf_texture* texture) {
    if (texture == nullptr) {
        return nullptr;
    }
    if (texture->image != nullptr) {
        return texture->image;
    }
    if (texture->has_webp && texture->webp_image != nullptr) {
        return texture->webp_image;
    }
    return nullptr;
}

bool readFloats(const cgltf_accessor* acc, cgltf_size i, float* out, cgltf_size n) {
    return acc != nullptr && i < acc->count && cgltf_accessor_read_float(acc, i, out, n);
}

} // namespace

ModelData GltfFigureImporter::load(const std::string& path, RiggedImportInfo* info) const {
    cgltf_options options = {};
    cgltf_data*   data = nullptr;
    cgltf_result  r = cgltf_parse_file(&options, path.c_str(), &data);
    if (r != cgltf_result_success) {
        throw std::runtime_error(std::string("glTF: ") + resultName(r));
    }
    struct DataGuard {
        cgltf_data* d;
        ~DataGuard() { cgltf_free(d); }
    } guard{data};

    // What the file REQUIRES that the reader has not got: compressed geometry, compressed textures.
    for (cgltf_size i = 0; i < data->extensions_required_count; ++i) {
        const char* ext = data->extensions_required[i];
        if (std::strcmp(ext, "KHR_draco_mesh_compression") == 0 || std::strcmp(ext, "EXT_meshopt_compression") == 0) {
            throw std::runtime_error(std::string("glTF: the file's meshes are compressed with ") + ext +
                                     ", which this reader does not decode; export it without mesh compression");
        }
        if (std::strcmp(ext, "KHR_texture_basisu") == 0) {
            throw std::runtime_error("glTF: the file's textures are Basis Universal (KHR_texture_basisu), which this reader does not decode; "
                                     "export it with PNG or JPEG textures");
        }
    }
    r = cgltf_load_buffers(&options, data, path.c_str());
    if (r != cgltf_result_success) {
        throw std::runtime_error(std::string("glTF: loading the buffers failed: ") + resultName(r));
    }
    if (info != nullptr) {
        info->sourceAxes = data->file_type == cgltf_file_type_glb ? "GLB container; metres, Y up" : "glTF JSON; metres, Y up";
    }

    // ---- THE SCENE'S TOP NODES (the default scene; the first; else every parentless node).
    std::vector<const cgltf_node*> tops;
    const cgltf_scene*             scene = data->scene != nullptr ? data->scene : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
    if (scene != nullptr) {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
            tops.push_back(scene->nodes[i]);
        }
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; ++i) {
            if (data->nodes[i].parent == nullptr) {
                tops.push_back(&data->nodes[i]);
            }
        }
    }
    const auto nodeIndex = [&](const cgltf_node* n) { return static_cast<std::size_t>(n - data->nodes); };

    // The nodes that carry a mesh, in scene order.
    std::vector<const cgltf_node*> meshNodes;
    {
        std::vector<const cgltf_node*> stack(tops.rbegin(), tops.rend());
        while (!stack.empty()) {
            const cgltf_node* n = stack.back();
            stack.pop_back();
            if (n->mesh != nullptr) {
                meshNodes.push_back(n);
            }
            for (cgltf_size c = n->children_count; c-- > 0;) {
                stack.push_back(n->children[c]);
            }
        }
    }

    // ---- THE SKELETON: every joint of every skin and their ancestors up to the top, in depth-first
    // order with parents first (the top with the most joints below it first: the figure's tree).
    // THE BIND IS THE NODES AS THE FILE STANDS: glTF keeps no bind pose apart from the skins'
    // inverse bind matrices, and exporters define those against different spaces (one against
    // the skin's world, another against the mesh node's local space); what every viewer shows at
    // the default pose is the vertices skinned by jointWorld · IBM with the joints where the
    // file's nodes put them, and that is what is read — a file whose nodes are saved posed
    // imports in that pose (its animation is not read).
    std::vector<char> wanted(data->nodes_count, 0);
    std::vector<char> isJoint(data->nodes_count, 0);
    for (cgltf_size s = 0; s < data->skins_count; ++s) {
        const cgltf_skin& skin = data->skins[s];
        for (cgltf_size j = 0; j < skin.joints_count; ++j) {
            wanted[nodeIndex(skin.joints[j])] = 1;
            isJoint[nodeIndex(skin.joints[j])] = 1;
        }
    }
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        if (!wanted[i]) {
            continue;
        }
        for (const cgltf_node* p = data->nodes[i].parent; p != nullptr; p = p->parent) {
            wanted[nodeIndex(p)] = 1;
        }
    }
    // The weight each joint carries, over every skinned mesh: a joint nothing is skinned to and no
    // wanted child hangs from is an exporter's end marker, not a joint (the FBX reader's rule).
    std::vector<double> jointWeightTotal(data->nodes_count, 0.0);
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& node = data->nodes[i];
        if (node.mesh == nullptr || node.skin == nullptr) {
            continue;
        }
        for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = node.mesh->primitives[pi];
            const cgltf_accessor*  jointsAcc = nullptr;
            const cgltf_accessor*  weightsAcc = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                if (prim.attributes[a].index != 0) {
                    continue;
                }
                if (prim.attributes[a].type == cgltf_attribute_type_joints) {
                    jointsAcc = prim.attributes[a].data;
                } else if (prim.attributes[a].type == cgltf_attribute_type_weights) {
                    weightsAcc = prim.attributes[a].data;
                }
            }
            if (jointsAcc == nullptr || weightsAcc == nullptr) {
                continue;
            }
            const cgltf_size n = std::min(jointsAcc->count, weightsAcc->count);
            for (cgltf_size v = 0; v < n; ++v) {
                cgltf_uint j[4] = {0, 0, 0, 0};
                float      w[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                if (!cgltf_accessor_read_uint(jointsAcc, v, j, 4) || !cgltf_accessor_read_float(weightsAcc, v, w, 4)) {
                    continue;
                }
                for (int k = 0; k < 4; ++k) {
                    if (w[k] > 0.0f && j[k] < node.skin->joints_count) {
                        jointWeightTotal[nodeIndex(node.skin->joints[j[k]])] += w[k];
                    }
                }
            }
        }
    }
    std::vector<char> prunedLeaf(data->nodes_count, 0);
    for (bool pruned = true; pruned;) {
        pruned = false;
        for (cgltf_size i = 0; i < data->nodes_count; ++i) {
            const cgltf_node& node = data->nodes[i];
            if (!wanted[i] || jointWeightTotal[i] > 0.0) {
                continue;
            }
            bool wantedChild = false;
            for (cgltf_size c = 0; c < node.children_count && !wantedChild; ++c) {
                wantedChild = wanted[nodeIndex(node.children[c])] != 0;
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
    std::unordered_map<const cgltf_node*, const cgltf_node*> tipNodeOf;
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& node = data->nodes[i];
        if (!wanted[i]) {
            continue;
        }
        for (cgltf_size c = 0; c < node.children_count; ++c) {
            const cgltf_node* child = node.children[c];
            if (prunedLeaf[nodeIndex(child)] && tipNodeOf.count(&node) == 0) {
                tipNodeOf.emplace(&node, child);
            }
        }
    }
    std::vector<const cgltf_node*>            boneNodes;
    std::vector<int>                          boneParent;
    std::unordered_map<const cgltf_node*, int> boneIndexOf;
    {
        struct Frame {
            const cgltf_node* node;
            int               parent;
        };
        std::vector<std::pair<std::size_t, const cgltf_node*>> sortedTops;
        for (const cgltf_node* top : tops) {
            std::size_t                    count = 0;
            std::vector<const cgltf_node*> walk{top};
            while (!walk.empty()) {
                const cgltf_node* nd = walk.back();
                walk.pop_back();
                count += wanted[nodeIndex(nd)] ? 1 : 0;
                for (cgltf_size k = 0; k < nd->children_count; ++k) {
                    walk.push_back(nd->children[k]);
                }
            }
            sortedTops.emplace_back(count, top);
        }
        std::stable_sort(sortedTops.begin(), sortedTops.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<Frame> stack;
        for (std::size_t c = sortedTops.size(); c-- > 0;) {
            stack.push_back({sortedTops[c].second, -1});
        }
        while (!stack.empty()) {
            const Frame f = stack.back();
            stack.pop_back();
            if (!wanted[nodeIndex(f.node)]) {
                // (an unwanted node's subtree may still hold wanted nodes only through a wanted
                // ancestor, which this one is not: nothing below it is a bone)
                continue;
            }
            const int index = static_cast<int>(boneNodes.size());
            boneNodes.push_back(f.node);
            boneParent.push_back(f.parent);
            boneIndexOf.emplace(f.node, index);
            for (cgltf_size c = f.node->children_count; c-- > 0;) {
                stack.push_back({f.node->children[c], index});
            }
        }
    }
    const int                    boneCount = static_cast<int>(boneNodes.size());
    std::vector<RawSkeletonBone> bones(boneNodes.size());
    static const bool            kBoneTrace = std::getenv("POSESTUDIO_GLTF_BONE_TRACE") != nullptr;
    for (int i = 0; i < boneCount; ++i) {
        const cgltf_node* node = boneNodes[static_cast<std::size_t>(i)];
        RawSkeletonBone&  bone = bones[static_cast<std::size_t>(i)];
        const glm::mat4   world = worldOf(node);
        const char*       source = isJoint[nodeIndex(node)] ? "joint" : "node";
        bone.name = node->name != nullptr ? node->name : "";
        bone.parent = boneParent[static_cast<std::size_t>(i)];
        bone.position = glm::vec3(world[3]);
        if (const auto tip = tipNodeOf.find(node); tip != tipNodeOf.end()) {
            const glm::mat4 local = glm::inverse(worldOf(node)) * worldOf(tip->second);
            bone.tip = glm::vec3((world * local)[3]);
            bone.hasTip = true;
        }
        if (kBoneTrace) {
            std::fprintf(stderr, "[gltf-bone] %3d %-28s %-7s at (%.3f %.3f %.3f)%s\n", i, bone.name.c_str(), source, bone.position.x,
                         bone.position.y, bone.position.z, bone.hasTip ? " +tip" : "");
        }
    }
    const auto attachBone = [&](const cgltf_node* node) {
        for (const cgltf_node* n = node; n != nullptr; n = n->parent) {
            if (const auto it = boneIndexOf.find(n); it != boneIndexOf.end()) {
                return it->second;
            }
        }
        return 0;
    };

    // ---- THE MESHES: every node that carries one, in scene order.
    ModelData     model;
    TextureFinder textures(fs::path(path).parent_path());
    bool          anySkinned = false;
    bool          notedTransform = false, notedMorphs = false, notedPoints = false, notedSecondSet = false;
    for (const cgltf_node* node : meshNodes) {
        const cgltf_mesh* mesh = node->mesh;
        const cgltf_skin* skin = node->skin;
        const glm::mat4   nodeWorld = worldOf(node);
        const glm::mat3   nodeNormal = glm::inverseTranspose(glm::mat3(nodeWorld));
        const bool        skinned = skin != nullptr && boneCount > 0;
        if (info != nullptr) {
            (skinned ? info->skinnedMeshes : info->rigidMeshes) += 1;
        }
        anySkinned = anySkinned || skinned;
        const unsigned rideBone = static_cast<unsigned>(attachBone(node));
        for (cgltf_size pi = 0; pi < mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles && prim.type != cgltf_primitive_type_triangle_strip &&
                prim.type != cgltf_primitive_type_triangle_fan) {
                if (info != nullptr && !notedPoints) {
                    info->notes.push_back("points and lines are not imported");
                    notedPoints = true;
                }
                continue;
            }
            if (prim.targets_count > 0 && info != nullptr && !notedMorphs) {
                info->notes.push_back("morph targets are not read");
                notedMorphs = true;
            }
            // The material first: which UV set its base map samples decides the texcoords read.
            MeshData out;
            out.roughness = 0.65f;
            out.specularWeight = 0.6f;
            out.specularWeightWithMap = 0.6f;
            int uvSet = 0;
            if (const cgltf_material* mat = prim.material; mat != nullptr) {
                if (info != nullptr) {
                    ++info->materials;
                }
                const cgltf_texture_view* baseView = nullptr;
                if (mat->has_pbr_metallic_roughness) {
                    const cgltf_pbr_metallic_roughness& pbr = mat->pbr_metallic_roughness;
                    out.baseColor = glm::vec3(pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2]); // (linear by the spec)
                    out.opacity = pbr.base_color_factor[3];
                    baseView = &pbr.base_color_texture;
                    out.roughness = std::max(0.05f, std::min(1.0f, pbr.roughness_factor));
                    out.metalness = std::max(0.0f, std::min(1.0f, pbr.metallic_factor));
                    if (pbr.metallic_roughness_texture.texture != nullptr) {
                        out.roughnessMap = resolveImage(textures, imageOf(pbr.metallic_roughness_texture.texture), info);
                        out.roughnessChannel = 1; // (roughness rides the green channel; metalness the blue)
                    }
                } else if (mat->has_pbr_specular_glossiness) {
                    const cgltf_pbr_specular_glossiness& sg = mat->pbr_specular_glossiness;
                    out.baseColor = glm::vec3(sg.diffuse_factor[0], sg.diffuse_factor[1], sg.diffuse_factor[2]);
                    out.opacity = sg.diffuse_factor[3];
                    baseView = &sg.diffuse_texture;
                    out.roughness = std::max(0.05f, std::min(1.0f, 1.0f - sg.glossiness_factor));
                }
                if (baseView != nullptr && baseView->texture != nullptr) {
                    out.diffuse = resolveImage(textures, imageOf(baseView->texture), info);
                    uvSet = baseView->texcoord;
                    if (!out.diffuse.empty()) {
                        out.baseColor = glm::vec3(1.0f); // (the map carries the colour; the factor is folded in by most exporters)
                    }
                    if (baseView->has_transform && info != nullptr && !notedTransform) {
                        info->notes.push_back("texture transforms (KHR_texture_transform) are not applied");
                        notedTransform = true;
                    }
                }
                if (mat->normal_texture.texture != nullptr) {
                    out.normal = resolveImage(textures, imageOf(mat->normal_texture.texture), info);
                    out.normalMode = out.normal.empty() ? 0 : 1;
                    out.normalStrength = std::max(0.1f, std::min(8.0f, mat->normal_texture.scale > 0.0f ? mat->normal_texture.scale : 1.0f));
                }
                out.lobe1Roughness = out.roughness;
                out.lobe2Roughness = out.roughness;
                if (mat->alpha_mode == cgltf_alpha_mode_opaque) {
                    out.opacity = 1.0f;
                } else if (!out.diffuse.empty()) {
                    out.opacityFromDiffuseAlpha = true; // (MASK or BLEND: the base map's alpha is the cutout)
                }
                out.opacity = std::max(0.0f, std::min(1.0f, out.opacity));
            }
            // The attributes.
            const cgltf_accessor* posAcc = nullptr;
            const cgltf_accessor* normAcc = nullptr;
            const cgltf_accessor* uvAcc = nullptr;
            const cgltf_accessor* uv0Acc = nullptr;
            const cgltf_accessor* jointsAcc = nullptr;
            const cgltf_accessor* weightsAcc = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                switch (attr.type) {
                case cgltf_attribute_type_position: posAcc = attr.data; break;
                case cgltf_attribute_type_normal: normAcc = attr.data; break;
                case cgltf_attribute_type_texcoord:
                    if (attr.index == uvSet) {
                        uvAcc = attr.data;
                    }
                    if (attr.index == 0) {
                        uv0Acc = attr.data;
                    }
                    break;
                case cgltf_attribute_type_joints:
                    if (attr.index == 0) {
                        jointsAcc = attr.data;
                    } else if (info != nullptr && !notedSecondSet) {
                        info->notes.push_back("only the first four skin influences per vertex are read (JOINTS_1 and up are ignored)");
                        notedSecondSet = true;
                    }
                    break;
                case cgltf_attribute_type_weights:
                    if (attr.index == 0) {
                        weightsAcc = attr.data;
                    }
                    break;
                default: break;
                }
            }
            if (uvAcc == nullptr) {
                uvAcc = uv0Acc;
            }
            if (posAcc == nullptr || posAcc->count == 0) {
                continue;
            }
            const bool primSkinned = skinned && jointsAcc != nullptr && weightsAcc != nullptr;
            // Per joint of the skin: jointWorld · IBM — what puts a vertex bound to that joint
            // where the file's nodes stand (the bind, see the skeleton above).
            std::vector<glm::mat4> skinMats;
            if (primSkinned) {
                skinMats.resize(skin->joints_count, glm::mat4(1.0f));
                for (cgltf_size k = 0; k < skin->joints_count; ++k) {
                    float     ibm[16];
                    glm::mat4 inverseBind(1.0f);
                    if (readFloats(skin->inverse_bind_matrices, k, ibm, 16)) {
                        inverseBind = glm::make_mat4(ibm);
                    }
                    skinMats[k] = worldOf(skin->joints[k]) * inverseBind;
                }
            }
            out.vertices.resize(posAcc->count);
            for (cgltf_size v = 0; v < posAcc->count; ++v) {
                Vertex& vert = out.vertices[v];
                float   p[3] = {0.0f, 0.0f, 0.0f};
                cgltf_accessor_read_float(posAcc, v, p, 3);
                glm::vec3 pos(p[0], p[1], p[2]);
                glm::vec3 n(0.0f);
                if (normAcc != nullptr && v < normAcc->count) {
                    float nn[3] = {0.0f, 0.0f, 0.0f};
                    cgltf_accessor_read_float(normAcc, v, nn, 3);
                    n = glm::vec3(nn[0], nn[1], nn[2]);
                }
                vert.joints = glm::uvec4(rideBone);
                vert.weights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
                glm::mat4 toWorld = nodeWorld;
                bool      blended = false;
                if (primSkinned && v < jointsAcc->count && v < weightsAcc->count) {
                    cgltf_uint j[4] = {0, 0, 0, 0};
                    float      w[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    cgltf_accessor_read_uint(jointsAcc, v, j, 4);
                    cgltf_accessor_read_float(weightsAcc, v, w, 4);
                    struct Influence {
                        int        bone;
                        float      w;
                        cgltf_uint joint;
                    };
                    Influence infl[4];
                    int       count = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (w[k] <= 0.0f || j[k] >= skin->joints_count) {
                            continue;
                        }
                        const auto it = boneIndexOf.find(skin->joints[j[k]]);
                        if (it == boneIndexOf.end()) {
                            continue;
                        }
                        infl[count++] = {it->second, w[k], j[k]};
                    }
                    if (count > 0) {
                        std::sort(infl, infl + count, [](const Influence& a, const Influence& b) { return a.w > b.w; });
                        float total = 0.0f;
                        for (int k = 0; k < count; ++k) {
                            total += infl[k].w;
                        }
                        glm::mat4 blend(0.0f);
                        for (int k = 0; k < 4; ++k) {
                            vert.joints[k] = static_cast<unsigned>(k < count ? infl[k].bone : infl[0].bone);
                            vert.weights[k] = k < count ? infl[k].w / total : 0.0f;
                            if (k < count) {
                                blend += skinMats[infl[k].joint] * (infl[k].w / total);
                            }
                        }
                        toWorld = blend;
                        blended = true;
                    }
                }
                // (a skinned vertex through its joints' bind matrices, a rigid one through its
                // node's transform; a skinned mesh's vertex no joint weighs rides the mesh node)
                pos = glm::vec3(toWorld * glm::vec4(pos, 1.0f));
                n = (blended ? glm::inverseTranspose(glm::mat3(toWorld)) : nodeNormal) * n;
                const float len = glm::length(n);
                vert.pos = pos;
                vert.normal = len > 1e-12f ? n / len : glm::vec3(0.0f);
                if (uvAcc != nullptr && v < uvAcc->count) {
                    float uv[2] = {0.0f, 0.0f};
                    cgltf_accessor_read_float(uvAcc, v, uv, 2);
                    vert.uv = glm::vec2(uv[0], uv[1]); // (glTF's origin is the top-left, as the renderer's)
                } else {
                    vert.uv = glm::vec2(0.0f);
                }
            }
            // The indices: as given, or sequential; strips and fans unrolled into a triangle list.
            std::vector<uint32_t> raw;
            if (prim.indices != nullptr) {
                raw.resize(prim.indices->count);
                cgltf_accessor_unpack_indices(prim.indices, raw.data(), sizeof(uint32_t), raw.size());
            } else {
                raw.resize(posAcc->count);
                for (std::size_t i = 0; i < raw.size(); ++i) {
                    raw[i] = static_cast<uint32_t>(i);
                }
            }
            const auto valid = [&](uint32_t i) { return i < out.vertices.size(); };
            if (prim.type == cgltf_primitive_type_triangles) {
                out.indices.reserve(raw.size());
                for (std::size_t i = 0; i + 2 < raw.size(); i += 3) {
                    if (valid(raw[i]) && valid(raw[i + 1]) && valid(raw[i + 2])) {
                        out.indices.insert(out.indices.end(), {raw[i], raw[i + 1], raw[i + 2]});
                    }
                }
            } else if (prim.type == cgltf_primitive_type_triangle_strip) {
                for (std::size_t i = 0; i + 2 < raw.size(); ++i) {
                    const uint32_t a = raw[i], b = raw[i + 1], c = raw[i + 2];
                    if (!valid(a) || !valid(b) || !valid(c)) {
                        continue;
                    }
                    if (i % 2 == 0) {
                        out.indices.insert(out.indices.end(), {a, b, c});
                    } else {
                        out.indices.insert(out.indices.end(), {b, a, c});
                    }
                }
            } else {
                for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
                    if (valid(raw[0]) && valid(raw[i]) && valid(raw[i + 1])) {
                        out.indices.insert(out.indices.end(), {raw[0], raw[i], raw[i + 1]});
                    }
                }
            }
            if (out.indices.size() < 3) {
                continue;
            }
            fillMissingNormals(out);
            model.meshes.push_back(std::move(out));
        }
    }
    if (model.meshes.empty()) {
        throw std::runtime_error("glTF: the file holds no triangle mesh");
    }

    // ---- UNITS: glTF is metres by definition, but an export in centimetres happens; a character
    // 20 m tall and more is one, and is brought down by a hundred.
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

    // glTF skins are linear blend skinning by definition.
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
