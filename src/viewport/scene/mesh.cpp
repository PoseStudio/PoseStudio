/**
 * @file mesh.cpp
 * @brief Mesh upload, material packing, texture slots, and draw. See mesh.h.
 */

#include "mesh.h"

#include "descriptorutil.h"
#include "modeldata.h"
#include "tangentgen.h"
#include "textureuploadcache.h"
#include "vertex.h"
#include "vulkancommands.h"
#include "vulkancontext.h"
#include "vulkanimage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>

namespace pose {

namespace {

/// Whether a slot's texture is a COLOR (sampled through an sRGB view) or data (linear).
bool slotIsSrgb(MaterialMap map) {
    return map == MaterialMap::BaseColor || map == MaterialMap::Opacity || map == MaterialMap::Translucency ||
           map == MaterialMap::Emission || map == MaterialMap::CoatColor || map == MaterialMap::SpecularColor ||
           map == MaterialMap::TransmissionColor || map == MaterialMap::SheenColor;
}

std::size_t idx(MaterialMap map) {
    return static_cast<std::size_t>(map);
}

/// The file's name without its folders (either separator).
std::string baseName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

Mesh::Mesh(VulkanContext& context, const MeshData& data, VkDescriptorSetLayout materialSetLayout,
           VkDescriptorPool materialPool, const VulkanTexture& fallbackDiffuse,
           const VulkanTexture& fallbackNormal, TextureUploadCache& uploads, ImmediateBatch& batch,
           const std::vector<uint32_t>* correctiveRanges, uint32_t materialIndex)
    : m_context(&context), m_fallbackDiffuse(&fallbackDiffuse), m_fallbackNormal(&fallbackNormal),
      m_indexCount(static_cast<uint32_t>(data.indices.size())), m_opacity(data.opacity),
      m_hasOpacityMask(data.hasOpacityMask), m_materialIndex(materialIndex) {
    // Bind-pose centroid (the transparent pass's back-to-front sort key) and the joint carrying
    // most of the mesh's skin weight (which carries that centroid through the pose). An unskinned
    // mesh's vertices all weight joint 0 fully, so it lands on the identity joint.
    if (!data.vertices.empty()) {
        constexpr uint32_t kMaxJointIndex = 4096; // sanity bound on a corrupt index
        glm::vec3 sum(0.0f);
        std::vector<float> jointWeight; // summed skin weight per joint index
        for (const Vertex& v : data.vertices) {
            sum += v.pos;
            for (int k = 0; k < 4; ++k) {
                const uint32_t joint = v.joints[k];
                const float weight = v.weights[k];
                if (weight <= 0.0f || joint >= kMaxJointIndex) {
                    continue;
                }
                if (joint >= jointWeight.size()) {
                    jointWeight.resize(static_cast<std::size_t>(joint) + 1, 0.0f);
                }
                jointWeight[joint] += weight;
            }
        }
        m_centroid = sum / static_cast<float>(data.vertices.size());
        if (!jointWeight.empty()) {
            m_dominantJoint = static_cast<uint32_t>(
                std::max_element(jointWeight.begin(), jointWeight.end()) - jointWeight.begin());
        }
    }

    const VkDeviceSize vertexBytes = sizeof(Vertex) * data.vertices.size();
    const VkDeviceSize indexBytes = sizeof(uint32_t) * data.indices.size();
    // Record all uploads into the shared batch instead of submitting per buffer/texture. A mesh
    // some pose corrective touches uploads a COPY of its vertices with the Model's per-vertex
    // corrective ranges stamped in (Vertex::correctiveRange) — the importer's data stays
    // pristine (the ground samples and bounds read it after this), and the batch copies the
    // temporary into its staging buffer before returning. (A one-time whole-array copy per
    // touched mesh at import; not worth avoiding.)
    // (The uploaded vertices are KEPT, with the indices: a render engine is handed the mesh
    // itself — see cpuVertices().)
    m_cpuVertices = data.vertices;
    if (correctiveRanges != nullptr && correctiveRanges->size() == data.vertices.size()) {
        for (std::size_t i = 0; i < m_cpuVertices.size(); ++i) {
            m_cpuVertices[i].correctiveRange = (*correctiveRanges)[i];
        }
    }
    m_cpuIndices = data.indices;
    m_vertexBuffer = createDeviceLocalBuffer(context, m_cpuVertices.data(), vertexBytes,
                                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, batch);
    m_indexBuffer = createDeviceLocalBuffer(context, data.indices.data(), indexBytes,
                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT, batch);
    // THE SMOOTHING: a mesh the import smoothed by angle (its file brought no smoothing of its
    // own) arrives smoothed, with the file's geometry beside it — set aside before the occlusion
    // bake, so not complete yet (prepareFileGeometry).
    m_smoothApplied = data.smoothByAngle;
    m_smoothAngleApplied = data.smoothAngle;
    if (data.smoothByAngle && !data.fileVertices.empty() && data.fileIndices.size() == data.indices.size()) {
        m_fileVertices = data.fileVertices;
        m_fileIndices = data.fileIndices;
        m_fileComplete = false;
    }

    // THE TEXTURE SLOTS as imported: per slot the plain decoded image (what the Materials tab
    // replaces or clears) and the UPLOADED texture the slot drives — the shared uploads of the
    // decode layer's derived images: the diffuse with the cutout baked into its alpha (BaseColor
    // and Opacity both drive it), the roughness/metalness parameter map (Roughness and Metalness
    // both), the maps themselves for the rest. Absent => the shared fallbacks, so the shader's
    // per-texel multiplies are no-ops for scalar-only materials.
    const int normalMode = data.normalImage ? data.normalMode : 0; // 0 none / 1 normal / 2 bump
    auto plain = [&](MaterialMap map, const std::shared_ptr<const DecodedImage>& image, const std::string& path, int channel = 0) {
        m_slotPlain[idx(map)] = image;
        m_importedSlotPlain[idx(map)] = image;
        m_slotChannel[idx(map)] = channel;
        m_importedSlotChannel[idx(map)] = channel;
        m_importedSlotPath[idx(map)] = image ? path : std::string();
        m_maps.present[idx(map)] = image != nullptr;
        m_material.maps[idx(map)].source = image ? kImportedTexture : "";
    };
    // The base color's plain image: the decode layer's, or the uploaded image itself where no
    // cutout was baked (an older ModelData, a mesh with no mask).
    plain(MaterialMap::BaseColor, data.diffusePlainImage ? data.diffusePlainImage : (data.hasOpacityMask ? nullptr : data.diffuseImage),
          data.diffuse.path);
    plain(MaterialMap::Opacity, data.opacityMaskImage, data.opacityMask.path);
    plain(MaterialMap::Normal, normalMode == 1 ? data.normalImage : nullptr, data.normal.path);
    plain(MaterialMap::Bump, normalMode == 2 ? data.normalImage : data.bumpImage, normalMode == 2 ? data.normal.path : data.bumpMap.path);
    plain(MaterialMap::Roughness, data.roughnessPlainImage, data.roughnessMap.path, data.roughnessChannel);
    plain(MaterialMap::Metalness, data.metalnessPlainImage, data.metalnessMap.path, data.metalnessChannel);
    plain(MaterialMap::SpecularMask, data.specMaskImage, data.specMask.path);
    plain(MaterialMap::Translucency, data.translucencyImage, data.translucencyMap.path);
    plain(MaterialMap::PoreDetail, data.detailNormalImage, data.detailNormalMap.path);
    plain(MaterialMap::Emission, data.emissionImage, data.emissionMap.path);
    // The slots after this line are stored-only (kept for the tab and the project, no texture) but
    // the seven materialMapIsRendered names, whose uploads follow below.
    plain(MaterialMap::DiffuseRoughness, data.diffuseRoughnessImage, data.diffuseRoughnessMap.path);
    plain(MaterialMap::CoatWeight, data.coatWeightImage, data.coatWeightMap.path);
    plain(MaterialMap::CoatColor, data.coatColorImage, data.coatColorMap.path);
    plain(MaterialMap::CoatRoughness, data.coatRoughnessImage, data.coatRoughnessMap.path);
    plain(MaterialMap::CoatReflectivity, data.coatReflectivityImage, data.coatReflectivityMap.path);
    plain(MaterialMap::CoatBump, data.coatBumpImage, data.coatBumpMap.path);
    plain(MaterialMap::TranslucencyWeight, data.translucencyWeightImage, data.translucencyWeightMap.path);
    plain(MaterialMap::SpecularColor, data.specularColorSlotImage, data.specularColorSlotMap.path);
    plain(MaterialMap::Anisotropy, data.anisotropyImage, data.anisotropyMap.path);
    plain(MaterialMap::AnisotropyRotation, data.anisotropyRotationImage, data.anisotropyRotationMap.path);
    plain(MaterialMap::Lobe2Roughness, data.lobe2RoughnessImage, data.lobe2RoughnessMap.path);
    plain(MaterialMap::LobeRatio, data.lobeRatioImage, data.lobeRatioMap.path);
    plain(MaterialMap::DetailWeight, data.detailWeightImage, data.detailWeightMap.path);
    plain(MaterialMap::DetailRoughness, data.detailRoughnessImage, data.detailRoughnessMap.path);
    plain(MaterialMap::Displacement, data.displacementImage, data.displacementMap.path);
    plain(MaterialMap::AmbientOcclusion, data.aoImage, data.aoMap.path);
    plain(MaterialMap::TransmissionColor, data.transmissionColorImage, data.transmissionColorMap.path);
    plain(MaterialMap::TransmissionWeight, data.transmissionWeightImage, data.transmissionWeightMap.path);
    plain(MaterialMap::CoatAnisotropy, data.coatAnisotropyImage, data.coatAnisotropyMap.path);
    plain(MaterialMap::CoatAnisotropyRotation, data.coatAnisotropyRotationImage, data.coatAnisotropyRotationMap.path);
    plain(MaterialMap::SheenWeight, data.sheenWeightImage, data.sheenWeightMap.path);
    plain(MaterialMap::SheenColor, data.sheenColorImage, data.sheenColorMap.path);
    plain(MaterialMap::SheenRoughness, data.sheenRoughnessImage, data.sheenRoughnessMap.path);
    plain(MaterialMap::ThinFilmWeight, data.thinFilmWeightImage, data.thinFilmWeightMap.path);
    plain(MaterialMap::ThinFilmThickness, data.thinFilmThicknessImage, data.thinFilmThicknessMap.path);
    // A slot that is a cutout alone: the baked diffuse exists with no base-color map.
    if (data.hasOpacityMask && !data.diffusePlainImage) {
        m_maps.present[idx(MaterialMap::BaseColor)] = false;
        m_material.maps[idx(MaterialMap::BaseColor)].source = "";
    }

    // The uploads (or reuses — see TextureUploadCache).
    const std::shared_ptr<VulkanTexture> diffuse = uploadShared(context, uploads, data.diffuseImage, /*srgb=*/true, batch);
    m_slotTexture[idx(MaterialMap::BaseColor)] = diffuse;
    m_slotTexture[idx(MaterialMap::Opacity)] = diffuse;
    const std::shared_ptr<VulkanTexture> detail = uploadShared(context, uploads, data.normalImage, /*srgb=*/false, batch);
    if (normalMode == 1) {
        m_slotTexture[idx(MaterialMap::Normal)] = detail;
        m_slotTexture[idx(MaterialMap::Bump)] = uploadShared(context, uploads, data.bumpImage, /*srgb=*/false, batch);
    } else if (normalMode == 2) {
        m_slotTexture[idx(MaterialMap::Bump)] = detail;
    }
    const std::shared_ptr<VulkanTexture> params = uploadShared(context, uploads, data.roughnessImage, /*srgb=*/false, batch);
    m_slotTexture[idx(MaterialMap::Roughness)] = params;
    m_slotTexture[idx(MaterialMap::Metalness)] = params;
    m_slotTexture[idx(MaterialMap::SpecularMask)] = uploadShared(context, uploads, data.specMaskImage, /*srgb=*/false, batch);
    m_slotTexture[idx(MaterialMap::Translucency)] = uploadShared(context, uploads, data.translucencyImage, /*srgb=*/true, batch);
    m_slotTexture[idx(MaterialMap::PoreDetail)] = uploadShared(context, uploads, data.detailNormalImage, /*srgb=*/false, batch);
    m_slotTexture[idx(MaterialMap::Emission)] = uploadShared(context, uploads, data.emissionImage, /*srgb=*/true, batch);
    // The parameter map carries the ambient occlusion (B) and transmission weight (A) maps too;
    // the LAYER map packs the coat weight, coat roughness, sheen weight and thin-film thickness
    // (modelimportservice's buildPackedMap); the sheen color map binds on its own.
    m_slotTexture[idx(MaterialMap::AmbientOcclusion)] = params;
    m_slotTexture[idx(MaterialMap::TransmissionWeight)] = params;
    const std::shared_ptr<VulkanTexture> layer = uploadShared(context, uploads, data.layerMapImage, /*srgb=*/false, batch);
    m_slotTexture[idx(MaterialMap::CoatWeight)] = layer;
    m_slotTexture[idx(MaterialMap::CoatRoughness)] = layer;
    m_slotTexture[idx(MaterialMap::SheenWeight)] = layer;
    m_slotTexture[idx(MaterialMap::ThinFilmThickness)] = layer;
    m_slotTexture[idx(MaterialMap::SheenColor)] = uploadShared(context, uploads, data.sheenColorImage, /*srgb=*/true, batch);
    m_normalMode = normalMode;

    // The canonical material: the imported values, kept as the restore target, and the values as
    // they stand, packed for the GPU (Model::uploadMaterialsIfDirty). specularWeight is the
    // per-material specular (F0) weight the material parser derived from the active specular
    // lobe's reflectivity (1 = the standard 4% dielectric; skin ~0.5 → ~2%; OBJ meshes default
    // to 1), or an importer's conversion of a specular / glossiness source.
    m_name = data.name;
    MaterialParams imported = m_material; // (the map sources set above)
    imported.baseColor = data.baseColor;
    imported.opacity = data.opacity;
    imported.roughness = data.roughness;
    imported.metalness = data.metalness;
    imported.specular = data.specularWeight;
    imported.normalStrength = normalMode == 1 ? data.normalStrength : 1.0f;
    imported.bumpStrength = normalMode == 2 ? data.normalStrength : data.bumpStrength;
    imported.translucency = data.translucencyWeight;
    imported.topCoat = data.topCoatWeight;
    imported.topCoatRoughness = data.topCoatRoughness;
    imported.detailWeight = data.detailWeight;
    imported.detailTiles = data.detailTiles;
    imported.detailTilesV = data.detailTilesV;
    imported.detailOffset = data.detailOffset;
    imported.detailRoughness = data.detailRoughness;
    imported.displacementStrength = data.displacementStrength;
    imported.displacementMidLevel = data.displacementMidLevel;
    imported.displacementSubdiv = data.displacementSubdiv;
    imported.aoWeight = data.aoWeight;
    imported.lobe1Roughness = data.lobe1Roughness;
    imported.lobe2Roughness = data.lobe2Roughness;
    imported.lobeRatio = data.lobeRatio;
    // The canonical model's further channels, as the reader filled them (neutral defaults else).
    imported.diffuseRoughness = data.diffuseRoughness;
    imported.specularColor = data.specularColor;
    imported.ior = data.ior;
    imported.anisotropy = data.anisotropy;
    imported.anisotropyRotation = data.anisotropyRotation;
    imported.transmission = data.transmission;
    imported.transmissionColor = data.transmissionColor;
    imported.transmissionDepth = data.transmissionDepth;
    imported.transmissionRoughness = data.transmissionRoughness;
    imported.dispersion = data.dispersion;
    imported.thinWalled = data.thinWalled;
    imported.subsurfaceColor = data.subsurfaceColor;
    imported.subsurfaceRadius = data.subsurfaceRadius;
    imported.subsurfaceRadiusColor = data.subsurfaceRadiusColor;
    imported.subsurfaceAnisotropy = data.subsurfaceAnisotropy;
    imported.sheen = data.sheen;
    imported.sheenColor = data.sheenColor;
    imported.sheenRoughness = data.sheenRoughness;
    imported.coatColor = data.coatColor;
    imported.coatIor = data.coatIor;
    imported.coatBumpStrength = data.coatBumpStrength;
    imported.topCoatEnabled = data.topCoatEnabled;
    imported.coatAnisotropy = data.coatAnisotropy;
    imported.coatAnisotropyRotation = data.coatAnisotropyRotation;
    imported.uvRotation = data.uvRotation;
    imported.doubleSided = data.doubleSided;
    imported.smoothByAngle = data.smoothByAngle;
    imported.smoothAngle = data.smoothAngle;
    imported.thinFilm = data.thinFilm;
    imported.thinFilmThickness = data.thinFilmThickness;
    imported.thinFilmIor = data.thinFilmIor;
    imported.emissionColor = data.emissionColor;
    imported.emission = data.emission;
    imported.uvTiling = data.uvTiling;
    imported.uvOffset = data.uvOffset;
    m_importedMaterial = imported;
    m_material = imported;
    packGpu();

    // Allocate this mesh's set-1 descriptor from the owning Model's pool and bind the slots.
    m_materialSet = allocateDescriptorSet(context.device(), materialPool, materialSetLayout);
    rebindAll();
}

void Mesh::rebindAll() {
    const auto tex = [&](MaterialMap map, const VulkanTexture& fallback) -> const VulkanTexture& {
        const std::shared_ptr<VulkanTexture>& t = m_slotTexture[idx(map)];
        return t ? *t : fallback;
    };
    // binding 1 (the detail map) is the normal map, or the bump map when there is no normal map.
    const VulkanTexture& detail = m_normalMode == 1 ? tex(MaterialMap::Normal, *m_fallbackNormal)
                                  : m_normalMode == 2 ? tex(MaterialMap::Bump, *m_fallbackNormal)
                                                      : *m_fallbackNormal;
    // The white fallback is numerically identical sampled as sRGB or linear (1.0 either way), so
    // the diffuse fallback doubles for the parameter maps.
    const VulkanTexture& diffuse = tex(MaterialMap::BaseColor, *m_fallbackDiffuse);
    const VulkanTexture& params = tex(MaterialMap::Roughness, *m_fallbackDiffuse);
    const VulkanTexture& specMask = tex(MaterialMap::SpecularMask, *m_fallbackDiffuse);
    const VulkanTexture& trans = tex(MaterialMap::Translucency, *m_fallbackDiffuse);
    const VulkanTexture& pore = tex(MaterialMap::PoreDetail, *m_fallbackNormal);
    const VulkanTexture& bump = m_normalMode == 1 ? tex(MaterialMap::Bump, *m_fallbackNormal) : *m_fallbackNormal;
    const VulkanTexture& emission = tex(MaterialMap::Emission, *m_fallbackDiffuse);
    const VulkanTexture& layer = tex(MaterialMap::CoatWeight, *m_fallbackDiffuse); // (the four packed slots share it)
    const VulkanTexture& sheenColor = tex(MaterialMap::SheenColor, *m_fallbackDiffuse);
    const std::array<VkDescriptorImageInfo, kMaterialBindingCount> imageInfos = {
        sampledImageInfo(diffuse.imageView(), diffuse.sampler()),
        sampledImageInfo(detail.imageView(), detail.sampler()),
        sampledImageInfo(params.imageView(), params.sampler()),
        sampledImageInfo(specMask.imageView(), specMask.sampler()),
        sampledImageInfo(trans.imageView(), trans.sampler()),
        sampledImageInfo(pore.imageView(), pore.sampler()),
        sampledImageInfo(bump.imageView(), bump.sampler()),
        sampledImageInfo(emission.imageView(), emission.sampler()),
        sampledImageInfo(layer.imageView(), layer.sampler()),
        sampledImageInfo(sheenColor.imageView(), sheenColor.sampler()),
    };
    writeCombinedImageSamplers(m_context->device(), m_materialSet, 0, imageInfos.data(),
                               static_cast<uint32_t>(imageInfos.size()));
}

void Mesh::refreshDetailMode() {
    m_normalMode = m_slotTexture[idx(MaterialMap::Normal)] ? 1 : m_slotTexture[idx(MaterialMap::Bump)] ? 2 : 0;
}

void Mesh::packGpu() {
    const MaterialParams& m = m_material;
    m_opacity = m.opacity;
    m_transmissive = m.transmissionEnabled && m.transmission > 0.001f;
    MaterialGpu& g = m_gpu;
    g.baseColor = glm::vec4(m.baseEnabled ? m.baseColor : glm::vec3(0.0f), m.opacity); // (Base off: a black dielectric)
    // z = metalness; w = the specular (F0) weight (1 = standard 4% dielectric).
    // material.y packs the detail mode + its strength (floor = mode, fract×8 = strength) — the
    // push block's packing, kept so the shader's arithmetic is unchanged. Mode 2 (a bump map
    // alone on the detail binding) takes the bump strength.
    const float detailStrength = !m.geometryEnabled ? 0.0f : m_normalMode == 2 ? m.bumpStrength : m.normalStrength; // (Geometry off: the maps at rest)
    const float modePacked =
        static_cast<float>(m_normalMode) +
        std::clamp(detailStrength, 0.0f, 7.9f) * 0.125f * (m_normalMode > 0 ? 1.0f : 0.0f);
    // (The overlays' metal baked into the bound map — setMetalBaked — the map IS the metalness.)
    const float metalness = m_metalBaked >= 0.0f ? 1.0f : m.metalness;
    g.material = glm::vec4(m.roughness, modePacked, m.baseEnabled ? metalness : 0.0f, m.specularEnabled ? m.specular : 0.0f);
    // The layer switches: an off layer is its weight at 0 for the shader, its value kept.
    g.material2 = glm::vec4(m.lobe1Roughness, m.lobe2Roughness, m.lobeRatio, m.subsurfaceEnabled ? m.translucency : 0.0f);
    // material3.w packs the micro-detail layer: integer part = UV tiling, fraction = weight;
    // 0 = no detail layer.
    const float detailPacked =
        (m_slotTexture[idx(MaterialMap::PoreDetail)] && m.detailTiles >= 1.0f && m.detailWeight > 0.0f && m.detailEnabled)
            ? std::floor(m.detailTiles) + std::min(m.detailWeight, 0.99f)
            : 0.0f;
    g.material3 = glm::vec4(m.topCoatEnabled ? m.topCoat : 0.0f, m.topCoatRoughness, 0.0f, detailPacked);
    // The canonical material's further channels (material.glsl).
    g.emission = glm::vec4(m.emissionColor * (m.emissionEnabled ? m.emission : 0.0f), m.geometryEnabled ? m.bumpStrength : 0.0f);
    g.uvTransform = m.tilingEnabled ? glm::vec4(m.uvTiling, m.uvOffset) : glm::vec4(1.0f, 1.0f, 0.0f, 0.0f); // (Tiling off: as painted)
    g.specularColor = glm::vec4(m.specularColor, m.specularEnabled ? m.anisotropy : 0.0f);
    g.transmission = glm::vec4(m.transmissionEnabled ? m.transmission : 0.0f, m.transmissionDepth,
                               m.thinWalled ? 1.0f : 0.0f, m.ior);
    g.transmissionColor = glm::vec4(m.transmissionColor, m.dispersion);
    g.subsurface = glm::vec4(m.subsurfaceColor, m.subsurfaceRadius);
    g.sheen = glm::vec4(m.sheenEnabled ? m.sheen : 0.0f, m.sheenRoughness, glm::radians(m.anisotropyRotation), m.coatIor);
    g.sheenColor = glm::vec4(m.sheenColor, m.thinFilmThickness);
    g.coatColor = glm::vec4(m.coatColor, m.thinFilmIor);
    // extra.w: a bump map bound BESIDE the normal map (mesh.frag applies its grain over the mapped
    // normal at the bump strength, emission.w).
    const bool bumpBeside = m_normalMode == 1 && m_slotTexture[idx(MaterialMap::Bump)] != nullptr;
    // extra.z: the ambient occlusion MAP's weight (the parameter map's blue; Geometry off = the map at rest).
    g.extra = glm::vec4(m.diffuseRoughness, m.thinFilmEnabled ? m.thinFilm : 0.0f, m.geometryEnabled ? m.aoWeight : 0.0f,
                        bumpBeside ? 1.0f : 0.0f);
    // The detail layer's placement (the shader reads its tiling from here, the count material3.w
    // packs for x — the same floor — so a square layer at no offset renders bit-identically) and
    // the last four scalars the shader draws: the detail roughness multiplier, the coat's
    // anisotropy with its rotation, and the UV rotation (degrees → radians).
    const float detailTilesV = m.detailTilesV > 0.0f ? m.detailTilesV : m.detailTiles;
    g.detail = glm::vec4(std::floor(m.detailTiles), std::floor(detailTilesV), m.detailOffset.x, m.detailOffset.y);
    g.extra2 = glm::vec4(m.detailEnabled ? m.detailRoughness : 1.0f, m.topCoatEnabled ? m.coatAnisotropy : 0.0f,
                         glm::radians(m.coatAnisotropyRotation), m.tilingEnabled ? glm::radians(m.uvRotation) : 0.0f);
}

bool Mesh::setMaterial(const MaterialParams& params) {
    // The VALUES: the texture slots are setSlotImage's alone, so the mesh keeps its own map
    // sources whatever the params carry (a linked zone's, a project's, an undo snapshot's whose
    // slots applyMaterialTextures has already put right).
    MaterialParams next = params;
    next.maps = m_material.maps;
    next.overlays = m_material.overlays; // (the overlays are the mesh's own too: setOverlays)
    if (next == m_material) {
        return false;
    }
    m_material = std::move(next);
    packGpu();
    return true;
}

void Mesh::prepareFileGeometry() {
    if (m_fileVertices.empty()) {
        // Shaded as imported until now: the uploaded geometry is the file's, with everything on it.
        m_fileVertices = m_cpuVertices;
        m_fileIndices = m_cpuIndices;
        m_fileComplete = true;
        return;
    }
    if (m_fileComplete || m_fileIndices.size() != m_cpuIndices.size()) {
        m_fileComplete = true;
        return;
    }
    // Set aside by the import before the occlusion was baked, the tangents made and the
    // corrective ranges stamped: each file vertex takes them from the uploaded vertices at its
    // corners (the occlusion their mean), and gets tangents for its own normals.
    std::vector<float>    occlusion(m_fileVertices.size(), 0.0f);
    std::vector<uint32_t> corners(m_fileVertices.size(), 0);
    for (std::size_t k = 0; k < m_fileIndices.size(); ++k) {
        const uint32_t file = m_fileIndices[k];
        const uint32_t now = m_cpuIndices[k];
        if (file >= m_fileVertices.size() || now >= m_cpuVertices.size()) {
            continue;
        }
        occlusion[file] += m_cpuVertices[now].ao;
        ++corners[file];
        m_fileVertices[file].correctiveRange = m_cpuVertices[now].correctiveRange;
    }
    for (std::size_t i = 0; i < m_fileVertices.size(); ++i) {
        if (corners[i] > 0) {
            m_fileVertices[i].ao = occlusion[i] / static_cast<float>(corners[i]);
        }
    }
    computeTangents(m_fileVertices, m_fileIndices);
    m_fileComplete = true;
}

void Mesh::uploadGeometry() {
    m_indexCount = static_cast<uint32_t>(m_cpuIndices.size());
    m_vertexBuffer = createDeviceLocalBuffer(*m_context, m_cpuVertices.data(), sizeof(Vertex) * m_cpuVertices.size(),
                                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    m_indexBuffer = createDeviceLocalBuffer(*m_context, m_cpuIndices.data(), sizeof(uint32_t) * m_cpuIndices.size(),
                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

void Mesh::setSmoothedGeometry(std::vector<Vertex> vertices, std::vector<uint32_t> indices, float angle) {
    if (vertices.empty() || indices.size() < 3) {
        return;
    }
    m_cpuVertices = std::move(vertices);
    m_cpuIndices = std::move(indices);
    markSmoothed(angle);
    uploadGeometry();
}

void Mesh::restoreFileGeometry() {
    m_smoothApplied = false;
    if (m_fileVertices.empty()) {
        return; // (the uploaded geometry is the file's already)
    }
    prepareFileGeometry();
    m_cpuVertices = std::move(m_fileVertices);
    m_cpuIndices = std::move(m_fileIndices);
    m_fileVertices.clear();
    m_fileIndices.clear();
    m_fileComplete = true;
    uploadGeometry();
}

std::shared_ptr<const DecodedImage> Mesh::slotImage(MaterialMap map, bool imported) const {
    return imported ? m_importedSlotPlain[idx(map)] : m_slotPlain[idx(map)];
}

int Mesh::slotChannel(MaterialMap map, bool imported) const {
    return imported ? m_importedSlotChannel[idx(map)] : m_slotChannel[idx(map)];
}

std::string Mesh::slotLabel(MaterialMap map) const {
    const std::string& source = m_material.maps[idx(map)].source;
    if (source.empty()) {
        return std::string();
    }
    if (source == kImportedTexture) {
        const std::string& path = m_importedSlotPath[idx(map)];
        return path.empty() ? std::string("embedded") : baseName(path);
    }
    return baseName(textureSourcePath(source));
}

std::string Mesh::slotPath(MaterialMap map) const {
    const std::string& source = m_material.maps[idx(map)].source;
    if (source.empty()) {
        return std::string();
    }
    return source == kImportedTexture ? m_importedSlotPath[idx(map)] : textureSourcePath(source);
}

void Mesh::setMetalBaked(float scalar) {
    if (scalar == m_metalBaked) {
        return;
    }
    m_metalBaked = scalar;
    packGpu();
}

bool Mesh::setOverlays(const std::vector<MaterialOverlay>& overlays) {
    if (m_material.overlays == overlays) {
        return false;
    }
    m_material.overlays = overlays;
    return true;
}

void Mesh::setSlotImage(MaterialMap map, const std::shared_ptr<const DecodedImage>& plain, int channel,
                        const std::shared_ptr<const DecodedImage>& gpuImage, const std::string& source, SlotUploads* uploads) {
    m_slotPlain[idx(map)] = plain;
    m_slotChannel[idx(map)] = channel;
    m_material.maps[idx(map)].source = plain ? source : std::string();
    m_maps.present[idx(map)] = plain != nullptr;
    // The texture the slot drives: the one another zone of the model already uploaded from this
    // very image (the zones of one map share it), else uploaded on its own submit (a one-off:
    // the caller waited for the device so the descriptor can be rewritten).
    std::shared_ptr<VulkanTexture> texture;
    if (materialMapIsRendered(map) && gpuImage && gpuImage->width > 0 && gpuImage->height > 0 && !gpuImage->pixels.empty()) {
        const bool srgb = slotIsSrgb(map);
        if (uploads != nullptr) {
            const auto it = uploads->entries.find(gpuImage.get());
            if (it != uploads->entries.end() && it->second.srgb == srgb && it->second.image.lock() == gpuImage) {
                texture = it->second.texture.lock();
            }
        }
        if (!texture) {
            texture = std::make_shared<VulkanTexture>(*m_context, gpuImage->pixels.data(), gpuImage->width, gpuImage->height, srgb);
            if (uploads != nullptr) {
                if (uploads->entries.size() >= 64) {
                    // (Entries whose image or texture has gone are dead weight: drop them.)
                    for (auto e = uploads->entries.begin(); e != uploads->entries.end();) {
                        e = (e->second.image.expired() || e->second.texture.expired()) ? uploads->entries.erase(e) : std::next(e);
                    }
                }
                uploads->entries[gpuImage.get()] = SlotUploads::Entry{gpuImage, texture, srgb};
            }
        }
    }
    switch (map) {
    case MaterialMap::BaseColor:
    case MaterialMap::Opacity:
        // One baked image for both: the base color with the cutout in its alpha.
        m_slotTexture[idx(MaterialMap::BaseColor)] = texture;
        m_slotTexture[idx(MaterialMap::Opacity)] = texture;
        if (map == MaterialMap::Opacity) {
            m_hasOpacityMask = plain != nullptr;
        }
        break;
    case MaterialMap::Roughness:
    case MaterialMap::Metalness:
    case MaterialMap::AmbientOcclusion:
    case MaterialMap::TransmissionWeight:
        // The parameter map, packed from the four slots' plain images.
        m_slotTexture[idx(MaterialMap::Roughness)] = texture;
        m_slotTexture[idx(MaterialMap::Metalness)] = texture;
        m_slotTexture[idx(MaterialMap::AmbientOcclusion)] = texture;
        m_slotTexture[idx(MaterialMap::TransmissionWeight)] = texture;
        break;
    case MaterialMap::CoatWeight:
    case MaterialMap::CoatRoughness:
    case MaterialMap::SheenWeight:
    case MaterialMap::ThinFilmThickness:
        // The layer map, likewise.
        m_slotTexture[idx(MaterialMap::CoatWeight)] = texture;
        m_slotTexture[idx(MaterialMap::CoatRoughness)] = texture;
        m_slotTexture[idx(MaterialMap::SheenWeight)] = texture;
        m_slotTexture[idx(MaterialMap::ThinFilmThickness)] = texture;
        break;
    default:
        m_slotTexture[idx(map)] = texture;
        break;
    }
    refreshDetailMode();
    rebindAll();
    packGpu();
}

void Mesh::record(VkCommandBuffer cmd, VkPipelineLayout layout, const glm::mat4& model,
                  MeshDrawKind kind, int highlightJoint, int highlightTwin) const {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &m_materialSet, 0,
                            nullptr); // set 1 = this mesh's textures

    // meta.x = this mesh's material entry; meta.y packs the draw kind with the selected joint +
    // its highlight twin (see the declaration): kind + 8·(joint+1) + 8192·(twin+1), every field
    // a small exact integer.
    const int packedKind = static_cast<int>(kind) +
                           8 * (std::clamp(highlightJoint, -1, 1022) + 1) +
                           8192 * (std::clamp(highlightTwin, -1, 1022) + 1);
    MeshPushConstants push;
    push.model = model;
    push.meta = glm::vec4(static_cast<float>(m_materialIndex), static_cast<float>(packedKind), 0.0f, 0.0f);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);

    const VkBuffer vertexBuffer = m_vertexBuffer.handle();
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

void Mesh::recordDepth(VkCommandBuffer cmd) const {
    const VkBuffer vertexBuffer = m_vertexBuffer.handle();
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

} // namespace pose
