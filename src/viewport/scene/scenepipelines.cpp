/**
 * @file scenepipelines.cpp
 * @brief Construction of the scene's layouts, sets, fallbacks, and mesh pipelines. See
 *        scenepipelines.h.
 */

#include "scenepipelines.h"

#include "cameraubo.h" // sizeof(CameraUbo)
#include "descriptorutil.h"
#include "iblmaps.h"
#include "mesh.h" // MeshPushConstants
#include "shadowmap.h"
#include "splatcloud.h" // SplatPushConstants
#include "vertex.h"
#include "vulkancommon.h" // kMaxFramesInFlight
#include "vulkancontext.h"
#include "vulkanimage.h"
#include "vulkanpipeline.h"

#include <array>

namespace pose {

namespace {

/// A fresh mesh-pass PipelineConfig: the shared state every variant starts from (push block,
/// depth test + write, opaque, both HDR colour attachments declared, the mesh vertex layout, the
/// four set layouts). Each variant then sets only its own deltas, so no state leaks from one
/// variant's configuration into the next.
PipelineConfig meshBaseConfig(const std::vector<VkDescriptorSetLayout>& layouts,
                              const VkVertexInputBindingDescription& binding,
                              const std::array<VkVertexInputAttributeDescription, 8>& attrs) {
    PipelineConfig config;
    config.pushConstantSize = sizeof(MeshPushConstants);
    config.pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    config.depthTestEnable = true;
    config.depthWriteEnable = true;
    config.blendEnable = false;
    // The HDR pass has a second colour attachment: the SPECULAR target the SSS blur must not
    // smear. Only the opaque pipeline writes it (mesh.frag routes the PBR specular there); every
    // other variant masks it off.
    config.colorAttachmentCount = 2;
    config.writeColorAttachment1 = false;
    // No back-face culling (PipelineConfig's default): imported OBJ winding is inconsistent in
    // the wild, so we'd risk an invisible/inside-out model. The fragment shader instead flips the
    // normal per gl_FrontFacing for correct two-sided lighting. Revisit once we normalize winding
    // on import.
    config.vertexBindings.assign(1, binding);
    config.vertexAttributes.assign(attrs.begin(), attrs.end());
    config.descriptorSetLayouts = layouts;
    return config;
}

} // namespace

ScenePipelines::ScenePipelines(VulkanContext& context, VkRenderPass renderPass, VkRenderPass photoRenderPass,
                               const std::vector<char>& vertSpirv,
                               const std::vector<char>& fragSpirv,
                               const std::vector<char>& backgroundVertSpirv,
                               const std::vector<char>& backgroundFragSpirv,
                               const std::vector<char>& splatVertSpirv,
                               const std::vector<char>& splatFragSpirv)
    : m_device(context.device()) {
    // --- Layouts (see the member comments for what each set carries) ---
    m_cameraSetLayout = createDescriptorSetLayout(
        m_device, {descriptorBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)});
    m_materialSetLayout = createDescriptorSetLayout(
        m_device, uniformBindings(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_SHADER_STAGE_FRAGMENT_BIT));
    m_poseSetLayout = createDescriptorSetLayout(
        m_device, uniformBindings(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT));
    m_iblSetLayout = createDescriptorSetLayout(
        m_device, uniformBindings(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  VK_SHADER_STAGE_FRAGMENT_BIT));
    m_splatSetLayout = createDescriptorSetLayout(
        m_device, uniformBindings(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT));

    // --- The scene-wide IBL/shadow set: one static set, its bindings written by
    // writeShadowMap (once) and writeEnvironment (per environment). ---
    m_iblPool = createDescriptorPool(m_device, {poolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4)}, 1);
    m_iblSet = allocateDescriptorSet(m_device, m_iblPool.get(), m_iblSetLayout.get());

    // --- The per-frame camera UBO sets ---
    m_cameraPool = createDescriptorPool(
        m_device, {poolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxFramesInFlight)}, kMaxFramesInFlight);
    m_cameraSets.resize(kMaxFramesInFlight);
    allocateDescriptorSets(m_device, m_cameraPool.get(), m_cameraSetLayout.get(), kMaxFramesInFlight,
                           m_cameraSets.data());
    m_cameraBuffers.reserve(kMaxFramesInFlight);
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        m_cameraBuffers.emplace_back(createMappedUniformBuffer(context, sizeof(CameraUbo)));
        const VkDescriptorBufferInfo info = bufferInfo(m_cameraBuffers[i].handle(), sizeof(CameraUbo));
        writeBufferDescriptors(m_device, m_cameraSets[i], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &info, 1);
    }

    // --- Fallback textures ---
    // 1x1 opaque-white fallback texture so untextured meshes can share the same pipeline/shader:
    // sampling white and multiplying by baseColor just yields baseColor.
    const uint8_t white[4] = {255, 255, 255, 255};
    m_fallbackDiffuse = std::make_unique<VulkanTexture>(context, white, 1, 1);
    // 1x1 flat-normal fallback (tangent-space +Z, i.e. no perturbation) for meshes with no detail
    // map. Linear, not sRGB — normal/bump data must be sampled verbatim.
    const uint8_t flatNormal[4] = {128, 128, 255, 255};
    m_fallbackNormal = std::make_unique<VulkanTexture>(context, flatNormal, 1, 1, /*srgb=*/false);

    // --- The mesh pipelines: one shader pair, one layout set, several fixed-function variants ---
    // set 0 = camera, set 1 = material textures, set 2 = per-model pose data,
    // set 3 = IBL maps (prefiltered specular cubemap + BRDF LUT) + the shadow map.
    const std::vector<VkDescriptorSetLayout> layouts = {
        m_cameraSetLayout.get(), m_materialSetLayout.get(), m_poseSetLayout.get(), m_iblSetLayout.get()};
    const VkVertexInputBindingDescription binding = Vertex::bindingDescription();
    const auto attrs = Vertex::attributeDescriptions();

    // Opaque: the only variant that writes the specular attachment (see meshBaseConfig).
    {
        PipelineConfig config = meshBaseConfig(layouts, binding, attrs);
        config.writeColorAttachment1 = true;
        m_opaque = std::make_unique<VulkanPipeline>(context, renderPass, vertSpirv, fragSpirv, config);
    }

    // Transparent variant for see-through material zones (eye moisture, cornea, later hair/lashes):
    // alpha-blend over what's already there, and keep depth *testing* (so opaque geometry in front
    // still occludes) but disable depth *writes* (so overlapping transparent layers don't reject
    // each other and nothing behind the mostly-clear surface gets occluded). Drawn after the opaque
    // pass. Same shaders/layout — only the blend + depth-write state differs.
    {
        PipelineConfig config = meshBaseConfig(layouts, binding, attrs);
        config.blendEnable = true;
        config.depthWriteEnable = false;
        config.colorWriteAlpha = false; // don't clobber the SSS mask under blended shells/cards
        // The specular attachment stays masked: blended shells keep their specular in-line (mask ≈ 0).
        m_transparent =
            std::make_unique<VulkanPipeline>(context, renderPass, vertSpirv, fragSpirv, config);
    }

    // Wireframe (the wireframe shade modes): the same shaders and layout rasterized as triangle
    // EDGES (LINE polygon mode), depth-tested and written, pulled a hair toward the camera by a
    // negative depth bias so a surface's own edges win over its coplanar fill in the overlay
    // modes. Alpha and the specular attachment are masked — a wire is neither skin nor a glint.
    // Skipped on a device without fillModeNonSolid; the wire modes then show their surface alone
    // (a Clay surface where the row has none — see Scene::record).
    if (context.supportsWireframe()) {
        PipelineConfig config = meshBaseConfig(layouts, binding, attrs);
        config.colorWriteAlpha = false;
        config.polygonMode = VK_POLYGON_MODE_LINE;
        config.depthBiasConstant = -1.0f;
        config.depthBiasSlope = -1.0f;
        m_wire = std::make_unique<VulkanPipeline>(context, renderPass, vertSpirv, fragSpirv, config);
    }

    // Hidden-line surface fill: depth test + write with EVERY colour write masked, so the
    // surface occludes the grid and the wires behind it while the framebuffer keeps the viewport's
    // clear colour — the classic hidden-line drawing once the wire pass draws over it.
    {
        PipelineConfig config = meshBaseConfig(layouts, binding, attrs);
        config.colorWriteRgb = false;
        config.colorWriteAlpha = false;
        m_hiddenLine =
            std::make_unique<VulkanPipeline>(context, renderPass, vertSpirv, fragSpirv, config);
    }

    // HDRI backdrop: a fullscreen triangle sampling the environment cube along the eye ray, drawn
    // FIRST in the main pass with depth test/write off so everything else renders over it. Its own
    // two-set layout: 0 = camera UBO, 1 = the scene-wide IBL/shadow set (it samples binding 0).
    {
        PipelineConfig config;
        config.depthTestEnable = false;
        config.depthWriteEnable = false;
        config.blendEnable = false;
        config.colorAttachmentCount = 2; // the spec target is present but masked off (writeColorAttachment1 false)
        config.descriptorSetLayouts = {m_cameraSetLayout.get(), m_iblSetLayout.get()};
        m_background = std::make_unique<VulkanPipeline>(context, renderPass, backgroundVertSpirv,
                                                        backgroundFragSpirv, config);
    }

    // GAUSSIAN SPLATS (splatcloud.h): a four-vertex triangle strip instanced once a splat (no
    // vertex buffer; gl_InstanceIndex is the sorted slot), blended far-first into the PHOTO PASS
    // (hdrtarget.h: the layer, single-sample, over the resolved scene depth) — depth-tested
    // against the opaque geometry, never written (a translucent cloud must not occlude) — IN THE
    // TRAINERS' OWN COLOUR SPACE: a splat's colour as the file has it, display-referred,
    // accumulated premultiplied with its coverage (the standard "over" blend), which the
    // composite lays over the tonemapped scene. The first cut blended the splats into the HDR
    // colour itself through the inverse tonemap, and the convex inverse weighed every bright
    // splat more than it was trained to: skin read as speckled, oily and sparkling, a sunlit
    // scene blew out; the second drew the layer as a third attachment of the scene pass, at its
    // MSAA, and four blends a pixel of every quad were the whole of a frame's GPU time (nothing
    // of the splats touches the HDR colour, the SSS mask or the specular either way; a figure
    // standing inside a capture is shaded as ever and covered by the composite). Its own two-set
    // layout: 0 = camera UBO, 1 = the cloud's buffers; the push block is splat.vert's.
    {
        PipelineConfig config;
        config.pushConstantSize = sizeof(SplatPushConstants);
        config.pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT;
        config.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        config.blendEnable = true;
        config.depthTestEnable = true;
        config.depthWriteEnable = false;
        config.singleSample = true;
        config.colorAttachmentCount = 1;
        config.descriptorSetLayouts = {m_cameraSetLayout.get(), m_splatSetLayout.get()};
        m_splat = std::make_unique<VulkanPipeline>(context, photoRenderPass, splatVertSpirv, splatFragSpirv, config);
    }
}

ScenePipelines::~ScenePipelines() = default;

void ScenePipelines::writeShadowMap(const ShadowMap& shadowMap) {
    // Bindings 2 + 3 = the shadow map (comparison + raw-depth samplers), written once
    // (writeEnvironment rewrites only 0/1).
    const VkDescriptorImageInfo infos[2] = {shadowMap.descriptorInfo(), shadowMap.rawDescriptorInfo()};
    writeCombinedImageSamplers(m_device, m_iblSet, 2, infos, 2);
}

void ScenePipelines::writeEnvironment(const IblMaps& maps) {
    const VkDescriptorImageInfo infos[2] = {maps.specularInfo(), maps.brdfInfo()};
    writeCombinedImageSamplers(m_device, m_iblSet, 0, infos, 2);
}

} // namespace pose
