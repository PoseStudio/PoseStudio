/**
 * @file mesh.h
 * @brief GPU-resident geometry of one material group (Mesh), its material and texture slots, and
 *        the push-constant blocks the mesh shaders read.
 *
 * A Mesh owns device-local vertex + index buffers, its canonical material (materialparams.h —
 * the values as imported and as they stand, packed for the GPU into the owning Model's material
 * buffer), and its TEXTURE SLOTS (MaterialMap): per slot the plain decoded image the material
 * carries now and the one it was imported with, and the eight sampled textures the slots drive
 * through its own descriptor set (set 1: diffuse with the cutout baked into its alpha, the detail
 * normal/bump, the roughness/metalness parameter map, spec mask, translucency, pore normal, a
 * bump beside a normal, emission — each falling back to a shared 1x1 texture when absent). A
 * Model (model.h) groups the meshes of one imported file. Qt-free.
 */

#ifndef MESH_H
#define MESH_H

#include "materialparams.h" // MaterialParams / MaterialMaps / MaterialMap (the Materials tab's values and slots)
#include "vulkanbuffer.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pose {

class ImmediateBatch;
class VulkanContext;
class VulkanTexture;
struct DecodedImage;
struct MeshData;
struct TextureUploadCache; // per-model dedup of texture uploads (textureuploadcache.h)

/// Per-draw push-constant block for the mesh shaders. Must stay byte-identical to the
/// `push_constant` block in mesh.vert / mesh.frag: the model matrix and `meta` — x the mesh's
/// index into its model's MATERIAL BUFFER (MaterialGpu below), y the draw kind (MeshDrawKind)
/// packed with the selected joint + its highlight twin as kind + 8·(joint+1) + 8192·(twin+1) —
/// see Mesh::record. The material values rode in this block until 2026-11-09, when it was full
/// (128 bytes, the guaranteed minimum); they live in the buffer now, with room to grow.
struct MeshPushConstants {
    glm::mat4 model;
    glm::vec4 meta;
};

/// One mesh's material as the shaders read it: an entry of the model's per-frame-in-flight
/// material storage buffer (set 2 binding 3; Model::uploadMaterialsIfDirty), packed by
/// Mesh::packGpu from its MaterialParams. Must stay byte-identical to `MaterialGpu` in
/// shaders/include/material.glsl (sixteen vec4s). The first four carry the values exactly as the
/// push block used to (the packed detail mode/strength and micro-detail fields included), so the
/// shader's arithmetic is unchanged; the rest are the canonical material's further channels.
struct MaterialGpu {
    glm::vec4 baseColor;         // rgb tint; a = opacity (1 = opaque)
    glm::vec4 material;          // x roughness (ratio-blended), y normalMode packed with the detail strength
                                 //   (floor = mode, fract×8 = strength), z metalness, w specular (F0) weight
    glm::vec4 material2;         // x lobe1 roughness, y lobe2 roughness, z lobe ratio (1 = single-lobe),
                                 //   w translucency weight
    glm::vec4 material3;         // x top-coat weight (0 = none), y top-coat roughness, z unused,
                                 //   w micro-detail packed as floor(tiles) + fract(weight) (0 = none)
    glm::vec4 emission;          // rgb emission radiance factor (linear colour × luminance), w bump strength
                                 //   (a bump map beside a normal map)
    glm::vec4 uvTransform;       // xy tiling, zw offset
    glm::vec4 specularColor;     // rgb specular tint, w anisotropy
    glm::vec4 transmission;      // x weight, y depth, z thin-walled (0/1), w index of refraction
    glm::vec4 transmissionColor; // rgb, w dispersion
    glm::vec4 subsurface;        // rgb colour, w radius scale
    glm::vec4 sheen;             // x weight, y roughness, z anisotropy rotation, w coat ior
    glm::vec4 sheenColor;        // rgb, w thin-film thickness
    glm::vec4 coatColor;         // rgb, w thin-film ior
    glm::vec4 extra;             // x diffuse roughness, y thin-film weight, z unused, w = 1 when a bump
                                 //   map is bound beside the normal map (mesh.frag applies it)
    glm::vec4 detail;            // the detail layer's placement: xy tiling, zw offset (repeats)
    glm::vec4 extra2;            // x detail roughness multiplier, y coat anisotropy, z its rotation
                                 //   (radians), w the UV rotation (radians)
};
static_assert(sizeof(MaterialGpu) == 16 * sizeof(glm::vec4), "MaterialGpu must match material.glsl");

/// What a Mesh::record draw is for — pushed as meta.y's low bits. Opaque/Transparent select the
/// alpha semantics (SSS mask vs blend opacity) in the lit passes; Wire is the wireframe overlay
/// (every edge in a flat grey from the UBO's wire level, through the LINE-polygon-mode
/// pipeline); DepthOnly is the hidden-line surface fill (its pipeline masks every colour write,
/// so the shader returns at once — the draw exists to lay down depth).
/// What a draw IS, for the shader (pushed in meta.y's low bits): the lit opaque pass, the
/// alpha-blended transparent pass, the wireframe edges, the hidden-line depth fill, and the
/// TRANSMISSION pair — a transmissive zone's two draws in the transparent pass (the frame
/// behind it multiplied by what it lets through, then its own light added; see
/// ScenePipelines::transmitMultiply / transmitAdd).
enum class MeshDrawKind { Opaque = 0, Transparent = 1, Wire = 2, DepthOnly = 3, TransmitMultiply = 4, TransmitAdd = 5 };

/// Push-constant block for the position-only projected passes — the depth-only shadow pass and
/// the selection-outline mask pass, which share shadow.vert. Must stay byte-identical to that
/// shader's `push_constant` block (two mat4s = 128 bytes, the guaranteed push-constant minimum).
struct ShadowPushConstants {
    glm::mat4 viewProj; // the fitted light matrix (shadow) or the camera's (outline mask)
    glm::mat4 model;
};

/// The set-1 bindings (mesh.frag's samplers), in order.
enum MaterialBinding : uint32_t {
    kBindDiffuse = 0,     // sRGB base colour, the cutout mask in its alpha
    kBindDetail = 1,      // the normal map (mode 1), or the bump map when there is no normal map (mode 2)
    kBindParams = 2,      // the PARAMETER map: roughness (R), metalness (G), ambient occlusion (B), transmission weight (A)
    kBindSpecMask = 3,
    kBindTranslucency = 4,
    kBindPoreNormal = 5,
    kBindBump = 6,        // a bump map CARRIED BESIDE a normal map
    kBindEmission = 7,
    kBindLayer = 8,       // the LAYER map: coat weight (R), coat roughness (G), sheen weight (B), thin-film thickness (A)
    kBindSheenColor = 9,  // sRGB sheen colour map
    kMaterialBindingCount = 10
};

/// One material group of a model: device-local vertex/index buffers, its canonical material and
/// texture slots, and its ten texture maps bound through its own set-1 descriptor.
class Mesh {
public:
    /// @param materialSetLayout  The shared set-1 layout (Scene owns it).
    /// @param materialPool       The owning Model's pool to allocate this mesh's set from.
    /// @param fallbackDiffuse    Shared 1x1 white texture, bound when the mesh has no diffuse map.
    /// @param fallbackNormal     Shared 1x1 flat-normal texture, bound when the mesh has no detail map.
    /// @param uploads            The owning Model's texture-upload cache: meshes sharing a
    ///                           DecodedImage share one VulkanTexture (figure zones commonly sample
    ///                           the same atlas maps — uploading per mesh multiplied GPU memory and
    ///                           import time by the sharing factor).
    /// @param batch              Upload batch the mesh's buffers/textures record into (the Model
    ///                           flushes it once for all its meshes — one submit per import).
    /// @param correctiveRanges   Per-vertex packed pose-corrective ranges (Vertex::correctiveRange,
    ///                           one per data.vertices entry) the Model resolved, or nullptr when
    ///                           no corrective touches this mesh. They're stamped into the vertex
    ///                           data as it uploads — the importer's copy stays pristine.
    /// @param materialIndex      This mesh's index in the owning Model — its entry in the model's
    ///                           material buffer (pushed as meta.x).
    Mesh(VulkanContext& context, const MeshData& data, VkDescriptorSetLayout materialSetLayout,
         VkDescriptorPool materialPool, const VulkanTexture& fallbackDiffuse,
         const VulkanTexture& fallbackNormal, TextureUploadCache& uploads, ImmediateBatch& batch,
         const std::vector<uint32_t>* correctiveRanges = nullptr, uint32_t materialIndex = 0);

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&&) noexcept = default;
    Mesh& operator=(Mesh&&) noexcept = default;

    /// Binds set 1 (this mesh's textures), pushes (model, meta), binds buffers, and draws. The
    /// caller has already bound the matching pipeline and the per-frame camera set (set 0) and
    /// the model's pose + material set (set 2). @p kind tells the shader what the draw is (alpha
    /// semantics / wire / depth fill); @p highlightJoint / @p highlightTwin (-1 = none) are the
    /// SELECTED joint and its twist partner — the vertex stage sums the vertex's skin weights on
    /// them into the highlight amount the fragment tints in the selection colour.
    void record(VkCommandBuffer cmd, VkPipelineLayout layout, const glm::mat4& model,
                MeshDrawKind kind, int highlightJoint = -1, int highlightTwin = -1) const;

    /// Depth-only draw for the shadow pass: just buffers + drawIndexed — no material set, no push
    /// (the Model pushes the light matrices once for all its meshes).
    void recordDepth(VkCommandBuffer cmd) const;

    /// True if this mesh is (partly) see-through and must be drawn in the transparent pass. An
    /// opacity mask baked into the diffuse alpha makes a mesh transparent even at scalar opacity 1
    /// (a lash/brow card's shape exists only in the mask).
    bool isTransparent() const { return m_opacity < 0.999f || m_hasOpacityMask || m_transmissive; }
    /// True if the zone TRANSMITS light (a transmission weight above 0 with the layer on —
    /// glass): drawn in the transparent pass as two draws, the TransmitMultiply and TransmitAdd
    /// kinds through ScenePipelines' transmission pair (see Scene::record).
    bool isTransmissive() const { return m_transmissive; }
    /// True if the mesh's shape is (partly) defined by an opacity mask in its diffuse alpha — a
    /// lash/brow card, an older figure's eye-shell fade. Material-less passes can't honour the
    /// mask, so they skip such meshes (their solid card geometry is not the shape the user sees).
    bool hasOpacityMask() const { return m_hasOpacityMask; }

    /// The material zone's name (MeshData::name) — the Materials tab's label and a project's key.
    const std::string& name() const { return m_name; }
    /// THE MATERIALS TAB: the editable material values as they stand, the values the mesh was
    /// IMPORTED with (the tab's restore target), and which maps it carries now.
    const MaterialParams& material() const { return m_material; }
    const MaterialParams& importedMaterial() const { return m_importedMaterial; }
    const MaterialMaps&   materialMaps() const { return m_maps; }
    /// Replaces the material VALUES — the texture slots are setSlotImage's, and a params' map
    /// sources are IGNORED (the mesh keeps its own): the GPU entry is repacked
    /// (the owning Model uploads it with the next frame's material buffer), and isTransparent()
    /// may change (the Model re-partitions its meshes). True if anything changed.
    bool setMaterial(const MaterialParams& params);
    /// The material as the shaders read it (the Model copies it into its material buffer).
    const MaterialGpu& gpu() const { return m_gpu; }

    // --- THE TEXTURE SLOTS (2026-11-09; the tab's Textures group) ---
    /// The plain decoded image slot @p map carries now (or @p imported: as imported) — null =
    /// none — and the channel a packed map's value rides in (roughness / metalness).
    std::shared_ptr<const DecodedImage> slotImage(MaterialMap map, bool imported = false) const;
    int                                 slotChannel(MaterialMap map, bool imported = false) const;
    /// What is bound in slot @p map, for the tab: the file's name (an import's path or a chosen
    /// file's), "embedded" for an imported image without a path, "" for none.
    std::string slotLabel(MaterialMap map) const;
    /// The slot's file on disk: a chosen file's path, the imported map's, or "" (no map, or an
    /// embedded image). What the tab's "Open File Location" opens.
    std::string slotPath(MaterialMap map) const;
    /// Replaces slot @p map: @p plain is the new plain image (null = none) and @p channel its
    /// channel; @p gpuImage the image to UPLOAD for the binding the slot drives — derived by the
    /// caller (the base colour with the cutout baked into its alpha, the roughness/metalness
    /// parameter map; the plain image itself for the rest; null = the fallback) — and @p source
    /// what the material records for the slot ("" / kImportedTexture / a path). Uploads on its
    /// own submit and rewrites this mesh's descriptor set: THE CALLER HAS WAITED FOR THE DEVICE.
    /// The detail mode follows the Normal / Bump slots, the cutout flag the Opacity slot.
    void setSlotImage(MaterialMap map, const std::shared_ptr<const DecodedImage>& plain, int channel,
                      const std::shared_ptr<const DecodedImage>& gpuImage, const std::string& source);

    /// Bind-pose centroid (local space) — the transparent pass's back-to-front sort key, carried
    /// through the dominant joint's pose by Model::posedCentroid.
    const glm::vec3& centroid() const { return m_centroid; }
    /// The joint carrying most of this mesh's summed skin weight (0 for an unskinned mesh) —
    /// whose skin transform moves the centroid with the pose for the transparency sort.
    uint32_t dominantJoint() const { return m_dominantJoint; }

private:
    /// Packs m_material into m_gpu, the shaders' view of it (and sets m_opacity, m_transmissive) — at construction
    /// from the imported values, and again by setMaterial / setSlotImage.
    void packGpu();
    /// m_normalMode from the Normal / Bump slots' textures (1 normal, 2 bump alone, 0 none).
    void refreshDetailMode();
    /// Writes every binding of m_materialSet from m_slotTexture (the fallbacks where null).
    void rebindAll();

    VulkanContext*       m_context = nullptr;
    const VulkanTexture* m_fallbackDiffuse = nullptr;
    const VulkanTexture* m_fallbackNormal = nullptr;

    VulkanBuffer                   m_vertexBuffer;
    VulkanBuffer                   m_indexBuffer;
    uint32_t                       m_indexCount = 0;
    std::string                    m_name;              // the zone's name (MeshData::name)
    MaterialParams                 m_material;          // the values as they stand (see material())
    MaterialParams                 m_importedMaterial;  // … and as imported (the restore target)
    MaterialMaps                   m_maps;              // which maps the mesh carries now
    int                            m_normalMode = 0;    // 0 none / 1 normal / 2 bump alone (see refreshDetailMode)
    glm::vec3                      m_centroid{0.0f};   // bind-pose local centroid (transparency sort)
    uint32_t                       m_dominantJoint = 0; // see dominantJoint()
    float                          m_opacity = 1.0f;
    bool                           m_hasOpacityMask = false;
    bool                           m_transmissive = false; // see isTransmissive (set by packGpu)
    // The material as packed for the GPU (see gpu()) and this mesh's index in its model's
    // material buffer, which record() pushes.
    MaterialGpu                    m_gpu{};
    uint32_t                       m_materialIndex = 0;
    // The texture slots: the plain images now and as imported, their channels, the import's
    // file paths (the tab's labels), and per slot the UPLOADED texture it drives (BaseColor and
    // Opacity share the baked diffuse; Roughness and Metalness share the parameter map). Textures
    // are shared_ptr: at import meshes sampling the same DecodedImage share one upload (see the
    // TextureUploadCache note on the constructor).
    std::array<std::shared_ptr<const DecodedImage>, kMaterialMapCount> m_slotPlain{};
    std::array<std::shared_ptr<const DecodedImage>, kMaterialMapCount> m_importedSlotPlain{};
    std::array<int, kMaterialMapCount>                                 m_slotChannel{};
    std::array<int, kMaterialMapCount>                                 m_importedSlotChannel{};
    std::array<std::string, kMaterialMapCount>                         m_importedSlotPath{};
    std::array<std::shared_ptr<VulkanTexture>, kMaterialMapCount>      m_slotTexture{};
    VkDescriptorSet                m_materialSet = VK_NULL_HANDLE; // set 1; owned by the Model's pool
};

} // namespace pose

#endif // MESH_H
