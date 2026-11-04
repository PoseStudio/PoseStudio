/**
 * @file openxrsession.cpp
 * @brief Implementation of OpenXrSession. See openxrsession.h.
 */

#include "openxrsession.h"

#include "attachmentimage.h"   // createImageView
#include "openxrsystem.h"
#include "renderpassbuilder.h" // RenderPassBuilder, createFramebuffer
#include "vrmath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pose {

namespace {

glm::vec3 toGlm(const XrVector3f& v) {
    return {v.x, v.y, v.z};
}

glm::quat toGlm(const XrQuaternionf& q) {
    return glm::quat(q.w, q.x, q.y, q.z);
}

constexpr XrViewConfigurationType kViewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
constexpr int64_t kImageWaitNanoseconds = 1'000'000'000; // a second: far beyond any frame period

} // namespace

std::unique_ptr<OpenXrSession> OpenXrSession::create(const OpenXrSystem& system, const VulkanBinding& binding,
                                                     float renderScale, std::string& failReason) {
    std::unique_ptr<OpenXrSession> session(new OpenXrSession());
    session->m_instance = system.instance();
    session->m_device = binding.device;

    XrGraphicsBindingVulkanKHR graphics{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    graphics.instance = binding.instance;
    graphics.physicalDevice = binding.physicalDevice;
    graphics.device = binding.device;
    graphics.queueFamilyIndex = binding.queueFamilyIndex;
    graphics.queueIndex = binding.queueIndex;

    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &graphics;
    info.systemId = system.systemId();
    XrResult result = xrCreateSession(system.instance(), &info, &session->m_session);
    if (XR_FAILED(result)) {
        failReason = "The headset session could not be created (" +
                     OpenXrSystem::resultName(system.instance(), result) + ").";
        return nullptr;
    }

    // The compositor's blend mode: the first the system offers (OPAQUE on every PC headset).
    uint32_t blendCount = 0;
    xrEnumerateEnvironmentBlendModes(system.instance(), system.systemId(), kViewConfig, 0, &blendCount, nullptr);
    if (blendCount > 0) {
        std::vector<XrEnvironmentBlendMode> modes(blendCount);
        xrEnumerateEnvironmentBlendModes(system.instance(), system.systemId(), kViewConfig, blendCount,
                                         &blendCount, modes.data());
        session->m_blendMode = modes.front();
    }

    if (!session->createSpaces(system)) {
        failReason = "The headset session has no usable reference space.";
        return nullptr;
    }
    if (!session->createSwapchains(system, renderScale, failReason)) {
        return nullptr;
    }
    // The controllers: declared and attached before the session begins. A runtime without any
    // of the controller families leaves the headset view-only — not a failure.
    std::string inputReason;
    session->m_input = OpenXrInput::create(system.instance(), session->m_session, inputReason);
    if (!session->m_input) {
        std::fprintf(stderr, "[vr] no controllers: %s\n", inputReason.c_str());
    }
    return session;
}

bool OpenXrSession::syncInput(int64_t time, VrInput& out) {
    return m_input && m_input->sync(time, m_space, out);
}

void OpenXrSession::hapticPulse(int hand, float seconds, float amplitude) {
    if (m_input) {
        m_input->pulse(hand, seconds, amplitude);
    }
}

OpenXrSession::~OpenXrSession() {
    // Our Vulkan objects over the runtime's images go first (RAII members, destroyed in reverse
    // declaration order AFTER this body — so reset them explicitly before the swapchains they
    // wrap are destroyed).
    m_input.reset(); // its action spaces belong to the session
    for (int eye = 0; eye < 2; ++eye) {
        m_framebuffers[eye].clear();
        m_imageViews[eye].clear();
    }
    m_renderPass.reset();
    for (XrSwapchain& swapchain : m_swapchains) {
        if (swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(swapchain);
            swapchain = XR_NULL_HANDLE;
        }
    }
    if (m_viewSpace != XR_NULL_HANDLE) {
        xrDestroySpace(m_viewSpace);
    }
    if (m_space != XR_NULL_HANDLE) {
        xrDestroySpace(m_space);
    }
    if (m_session != XR_NULL_HANDLE) {
        // Allowed in any session state: a running session is ended by its destruction.
        xrDestroySession(m_session);
    }
}

bool OpenXrSession::createSpaces(const OpenXrSystem& system) {
    (void)system;
    uint32_t count = 0;
    xrEnumerateReferenceSpaces(m_session, 0, &count, nullptr);
    std::vector<XrReferenceSpaceType> types(count);
    if (count > 0) {
        xrEnumerateReferenceSpaces(m_session, count, &count, types.data());
    }
    const bool hasStage = std::find(types.begin(), types.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != types.end();
    const bool hasLocal = std::find(types.begin(), types.end(), XR_REFERENCE_SPACE_TYPE_LOCAL) != types.end();
    if (!hasStage && !hasLocal) {
        return false;
    }

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f; // identity
    spaceInfo.referenceSpaceType = hasStage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (XR_FAILED(xrCreateReferenceSpace(m_session, &spaceInfo, &m_space))) {
        return false;
    }
    m_floorLevel = hasStage;

    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (XR_FAILED(xrCreateReferenceSpace(m_session, &spaceInfo, &m_viewSpace))) {
        return false;
    }
    std::fprintf(stderr, "[vr] reference space: %s\n", hasStage ? "STAGE (floor level)" : "LOCAL (eye level)");
    return true;
}

bool OpenXrSession::createSwapchains(const OpenXrSystem& system, float renderScale, std::string& failReason) {
    // --- The per-eye resolution: the runtime's recommendation, scaled. ------------------------
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(system.instance(), system.systemId(), kViewConfig, 0, &viewCount, nullptr);
    if (viewCount < 2) {
        failReason = "The headset does not offer a stereo view configuration.";
        return false;
    }
    std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(system.instance(), system.systemId(), kViewConfig, viewCount, &viewCount,
                                      views.data());
    const XrViewConfigurationView& view = views[0]; // both eyes share a size on every PC headset
    const float scale = std::clamp(renderScale, 0.25f, 2.0f);
    const auto scaled = [scale](uint32_t recommended, uint32_t maximum) {
        const uint32_t wanted = static_cast<uint32_t>(std::lround(recommended * scale));
        return std::clamp(wanted, 64u, std::max(64u, maximum));
    };
    m_extent.width = scaled(view.recommendedImageRectWidth, view.maxImageRectWidth);
    m_extent.height = scaled(view.recommendedImageRectHeight, view.maxImageRectHeight);

    // --- The swapchain format: an sRGB colour format when offered (the store then encodes, as
    // the desktop swapchain's does), else a UNORM one with the composite encoding in-shader. ----
    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    if (formatCount > 0) {
        xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data());
    }
    const auto offers = [&formats](VkFormat format) {
        return std::find(formats.begin(), formats.end(), static_cast<int64_t>(format)) != formats.end();
    };
    m_format = VK_FORMAT_UNDEFINED;
    for (VkFormat candidate : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB}) {
        if (offers(candidate)) {
            m_format = candidate;
            m_formatIsSrgb = true;
            break;
        }
    }
    if (m_format == VK_FORMAT_UNDEFINED) {
        for (VkFormat candidate : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}) {
            if (offers(candidate)) {
                m_format = candidate;
                m_formatIsSrgb = false;
                break;
            }
        }
    }
    if (m_format == VK_FORMAT_UNDEFINED) {
        failReason = "The headset runtime offers no 8-bit RGBA swapchain format.";
        return false;
    }

    // --- The composite's render pass over the runtime's images: one single-sample colour
    // attachment, loaded DONT_CARE (the fullscreen composite overwrites every pixel) and left in
    // COLOR_ATTACHMENT_OPTIMAL — the layout XR_KHR_vulkan_enable hands the images over in and
    // wants them back in. The dependencies: the pass's writes after whatever touched the image
    // before (the runtime's reads of the previous frame), and made available to the runtime's
    // reads that follow the submit. ---------------------------------------------------------
    VkSubpassDependency before{};
    before.srcSubpass = VK_SUBPASS_EXTERNAL;
    before.dstSubpass = 0;
    before.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    before.srcAccessMask = 0;
    before.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    before.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency after{};
    after.srcSubpass = 0;
    after.dstSubpass = VK_SUBPASS_EXTERNAL;
    after.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    after.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    after.dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    m_renderPass = RenderPassBuilder()
                       .color(m_format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                              VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
                       .dependency(before)
                       .dependency(after)
                       .build(m_device);

    // --- One swapchain per eye, its images wrapped in views + framebuffers. --------------------
    for (int eye = 0; eye < 2; ++eye) {
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = static_cast<int64_t>(m_format);
        info.sampleCount = 1;
        info.width = m_extent.width;
        info.height = m_extent.height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        XrResult result = xrCreateSwapchain(m_session, &info, &m_swapchains[eye]);
        if (XR_FAILED(result)) {
            failReason = "The headset's swapchain could not be created (" +
                         OpenXrSystem::resultName(m_instance, result) + ").";
            return false;
        }
        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(m_swapchains[eye], 0, &imageCount, nullptr);
        m_images[eye].assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        result = xrEnumerateSwapchainImages(m_swapchains[eye], imageCount, &imageCount,
                                            reinterpret_cast<XrSwapchainImageBaseHeader*>(m_images[eye].data()));
        if (XR_FAILED(result) || imageCount == 0) {
            failReason = "The headset's swapchain images could not be read (" +
                         OpenXrSystem::resultName(m_instance, result) + ").";
            return false;
        }
        for (const XrSwapchainImageVulkanKHR& image : m_images[eye]) {
            m_imageViews[eye].push_back(createImageView(m_device, image.image, VK_IMAGE_VIEW_TYPE_2D, m_format,
                                                        VK_IMAGE_ASPECT_COLOR_BIT));
            const VkImageView view = m_imageViews[eye].back().get();
            m_framebuffers[eye].push_back(createFramebuffer(m_device, m_renderPass.get(), &view, 1, m_extent));
        }
    }
    std::fprintf(stderr, "[vr] per-eye target %ux%u (recommended %ux%u, scale %.2f), %s, %zu images per eye\n",
                 m_extent.width, m_extent.height, view.recommendedImageRectWidth,
                 view.recommendedImageRectHeight, scale, m_formatIsSrgb ? "sRGB" : "UNORM (shader-encoded)",
                 m_images[0].size());
    return true;
}

OpenXrSession::Event OpenXrSession::pollEvents() {
    Event event = Event::None;
    XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(m_instance, &buffer) == XR_SUCCESS) {
        switch (buffer.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&buffer);
            if (changed.session == m_session) {
                m_state = changed.state;
                switch (m_state) {
                case XR_SESSION_STATE_READY:        event = Event::Ready; break;
                case XR_SESSION_STATE_STOPPING:     event = Event::Stopping; break;
                case XR_SESSION_STATE_EXITING:
                case XR_SESSION_STATE_LOSS_PENDING: event = Event::Gone; break;
                default:                            break; // SYNCHRONIZED / VISIBLE / FOCUSED / IDLE: keep going
                }
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            event = Event::Gone;
            break;
        default:
            break;
        }
        buffer = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
    return event;
}

bool OpenXrSession::begin() {
    XrSessionBeginInfo info{XR_TYPE_SESSION_BEGIN_INFO};
    info.primaryViewConfigurationType = kViewConfig;
    const XrResult result = xrBeginSession(m_session, &info);
    if (XR_FAILED(result)) {
        std::fprintf(stderr, "[vr] xrBeginSession failed: %s\n",
                     OpenXrSystem::resultName(m_instance, result).c_str());
        return false;
    }
    m_running = true;
    return true;
}

void OpenXrSession::end() {
    if (m_running) {
        xrEndSession(m_session);
        m_running = false;
    }
}

void OpenXrSession::requestExit() {
    xrRequestExitSession(m_session);
}

bool OpenXrSession::waitFrame(int64_t& predictedDisplayTime, bool& shouldRender) {
    XrFrameWaitInfo info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState state{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(m_session, &info, &state))) {
        return false;
    }
    predictedDisplayTime = state.predictedDisplayTime;
    shouldRender = state.shouldRender == XR_TRUE;
    return true;
}

bool OpenXrSession::beginFrame() {
    XrFrameBeginInfo info{XR_TYPE_FRAME_BEGIN_INFO};
    return XR_SUCCEEDED(xrBeginFrame(m_session, &info)); // XR_FRAME_DISCARDED is a success code
}

bool OpenXrSession::locateViews(int64_t time, const glm::mat4& worldFromStage, VrEyeView out[2]) {
    XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
    info.viewConfigurationType = kViewConfig;
    info.displayTime = time;
    info.space = m_space;
    XrViewState state{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    if (XR_FAILED(xrLocateViews(m_session, &info, &state, 2, &count, views)) || count < 2) {
        return false;
    }
    const XrViewStateFlags needed = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    if ((state.viewStateFlags & needed) != needed) {
        return false;
    }
    for (int eye = 0; eye < 2; ++eye) {
        m_located[eye] = views[eye];
        const XrFovf& fov = views[eye].fov;
        out[eye].projection = vrProjectionFromFov(fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown,
                                                  kNearPlane, kFarPlane);
        out[eye].view = vrViewFromEyePose(worldFromStage, toGlm(views[eye].pose.position),
                                          toGlm(views[eye].pose.orientation));
    }
    return true;
}

bool OpenXrSession::headPose(int64_t time, glm::vec3& position, glm::quat& orientation) const {
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(m_viewSpace, m_space, time, &location))) {
        return false;
    }
    const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((location.locationFlags & needed) != needed) {
        return false;
    }
    position = toGlm(location.pose.position);
    orientation = toGlm(location.pose.orientation);
    return true;
}

bool OpenXrSession::acquireEye(int eye, uint32_t& imageIndex) {
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(m_swapchains[eye], &acquire, &imageIndex))) {
        return false;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = kImageWaitNanoseconds;
    if (xrWaitSwapchainImage(m_swapchains[eye], &wait) != XR_SUCCESS) {
        // The image stays acquired: release it unrendered so the swapchain's bookkeeping balances.
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(m_swapchains[eye], &release);
        return false;
    }
    return imageIndex < m_framebuffers[eye].size();
}

void OpenXrSession::releaseEye(int eye) {
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(m_swapchains[eye], &release);
}

void OpenXrSession::endFrame(int64_t time, bool rendered) {
    XrCompositionLayerProjectionView projectionViews[2];
    for (int eye = 0; eye < 2; ++eye) {
        projectionViews[eye] = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        projectionViews[eye].pose = m_located[eye].pose;
        projectionViews[eye].fov = m_located[eye].fov;
        projectionViews[eye].subImage.swapchain = m_swapchains[eye];
        projectionViews[eye].subImage.imageRect.offset = {0, 0};
        projectionViews[eye].subImage.imageRect.extent = {static_cast<int32_t>(m_extent.width),
                                                          static_cast<int32_t>(m_extent.height)};
        projectionViews[eye].subImage.imageArrayIndex = 0;
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = m_space;
    layer.viewCount = 2;
    layer.views = projectionViews;
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};

    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    info.displayTime = time;
    info.environmentBlendMode = m_blendMode;
    info.layerCount = rendered ? 1u : 0u;
    info.layers = rendered ? layers : nullptr;
    const XrResult result = xrEndFrame(m_session, &info);
    if (XR_FAILED(result)) {
        std::fprintf(stderr, "[vr] xrEndFrame failed: %s\n", OpenXrSystem::resultName(m_instance, result).c_str());
    }
}

} // namespace pose
