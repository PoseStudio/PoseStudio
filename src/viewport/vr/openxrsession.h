/**
 * @file openxrsession.h
 * @brief One VR session on the headset: the reference spaces, the per-eye swapchains the
 *        renderer draws into, the session state machine, and the frame loop primitives.
 *
 * Created on demand (View → VR Mode) from the startup OpenXrSystem and the application's
 * Vulkan device, and destroyed when VR Mode ends. What it owns on the Vulkan side is only what
 * the runtime hands over — the swapchain images — plus the views, framebuffers and the one
 * render pass the fullscreen composite draws them through; the eye-sized scene targets belong
 * to VulkanRenderer (beginVr / drawVrFrame), which never sees an OpenXR type: it receives this
 * session's render pass, extent and framebuffers, and each eye's matrices as a VrEyeView.
 *
 * THE FRAME LOOP (driven by VulkanWindow, vulkanwindow_vr.cpp): pollEvents() on a timer runs
 * the session state machine — begin() when the runtime says READY, end() on STOPPING, and a
 * Gone event when the session or the runtime is lost. While running, a dedicated thread blocks
 * in waitFrame() (the ONE call that may run off the GUI thread — OpenXR designs xrWaitFrame for
 * exactly that, so the GUI thread never stalls on the headset's pacing) and hands the predicted
 * display time to the GUI thread, which does beginFrame -> locateViews -> per eye
 * acquireEye / render / releaseEye -> endFrame, strictly one frame at a time.
 *
 * Spaces: STAGE (floor-level, the play-space centre) when the runtime offers it — a figure on
 * y = 0 then stands on the user's real floor at true scale — else LOCAL (eye-level) with the
 * floor offset applied by the caller (floorLevel()). Pure OpenXR + Vulkan + GLM + std, no Qt.
 */

#ifndef OPENXRSESSION_H
#define OPENXRSESSION_H

#include "openxrinput.h"
#include "vreyeview.h"
#include "vulkanhandles.h"

#include <vulkan/vulkan.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pose {

class OpenXrSystem;

class OpenXrSession {
public:
    /// The application's Vulkan objects the runtime renders through (XrGraphicsBindingVulkanKHR).
    /// The queue is the one the renderer submits on: the runtime uses it only inside the OpenXR
    /// calls the GUI thread makes, so the two never race.
    struct VulkanBinding {
        VkInstance       instance = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice         device = VK_NULL_HANDLE;
        uint32_t         queueFamilyIndex = 0;
        uint32_t         queueIndex = 0;
    };

    /// What pollEvents() asks the caller to do.
    enum class Event {
        None,
        Ready,    ///< The runtime is ready for begin().
        Stopping, ///< The runtime wants the session ended: stop waiting on frames, then end().
        Gone      ///< The session or the runtime is lost: destroy this session.
    };

    /// Creates the session, its spaces and swapchains. @p renderScale scales the runtime's
    /// recommended per-eye resolution; @p wantDepth adds a depth swapchain per eye and submits
    /// the frame's depth with it (hasDepth() says whether that came together — the runtime
    /// must offer the depth-layer extension and a depth format). Null with @p failReason set on
    /// failure.
    static std::unique_ptr<OpenXrSession> create(const OpenXrSystem& system, const VulkanBinding& binding,
                                                 float renderScale, bool wantDepth, std::string& failReason);
    ~OpenXrSession();

    OpenXrSession(const OpenXrSession&) = delete;
    OpenXrSession& operator=(const OpenXrSession&) = delete;

    // --- The state machine (GUI thread) --------------------------------------------------------
    Event pollEvents();
    bool  begin(); ///< xrBeginSession (after a Ready event). False on failure.
    void  end();   ///< xrEndSession (after a Stopping event, with the frame thread stopped).
    bool  running() const { return m_running; }
    /// Asks the runtime to end the session: a Stopping event follows, then Gone.
    void  requestExit();

    // --- The frame loop ------------------------------------------------------------------------
    /// Blocks until the runtime wants the next frame rendered. May run on a thread of its own.
    /// False when the runtime refuses (the session is no longer running). @p displayPeriod is
    /// the headset's frame period (nanoseconds) — the frame-time readout's yardstick.
    bool waitFrame(int64_t& predictedDisplayTime, bool& shouldRender, int64_t& displayPeriod);
    bool beginFrame();
    /// The two eyes' matrices at @p time through @p worldFromStage (false while the headset is
    /// not tracked — render nothing then). The poses are kept for endFrame's layer.
    bool locateViews(int64_t time, const glm::mat4& worldFromStage, VrEyeView out[2]);
    /// The head's pose in the session's space at @p time (false while untracked) — the
    /// recentring input.
    bool headPose(int64_t time, glm::vec3& position, glm::quat& orientation) const;
    /// The controllers this frame (OpenXrInput), poses in the session's space. False without
    /// controllers (no profile accepted) or when the runtime refuses the sync.
    bool syncInput(int64_t time, VrInput& out);
    /// A short vibration on hand @p hand (0 left, 1 right); no-op without controllers.
    void hapticPulse(int hand, float seconds, float amplitude);
    bool hasControllers() const { return m_input != nullptr; }
    /// Acquires (and waits for) eye @p eye's next swapchain image — and its depth image, with a
    /// depth swapchain; its index selects the framebuffer to render into. False on failure.
    bool acquireEye(int eye, uint32_t& imageIndex);
    /// Hands the rendered image(s) back (after the submit that renders them).
    void releaseEye(int eye);
    /// Ends the frame with the projection layer when @p rendered (its depth attached when the
    /// session has depth), else with no layer.
    void endFrame(int64_t time, bool rendered);

    // --- What the renderer draws into ----------------------------------------------------------
    VkRenderPass  renderPass() const { return m_renderPass.get(); }
    VkFramebuffer framebuffer(int eye, uint32_t imageIndex) const { return m_framebuffers[eye][imageIndex].get(); }
    VkExtent2D    eyeExtent() const { return m_extent; }
    /// The depth swapchains (create's wantDepth, when the runtime offers the depth layer and a
    /// depth format): the depth-only pass over their images and, after acquireEye, the
    /// framebuffer of the acquired depth image.
    bool          hasDepth() const { return m_depth; }
    VkRenderPass  depthRenderPass() const { return m_depthRenderPass.get(); }
    VkFramebuffer depthFramebuffer(int eye) const {
        return m_depth ? m_depthFramebuffers[eye][m_depthIndex[eye]].get() : VK_NULL_HANDLE;
    }
    /// Whether the swapchain format is an sRGB one (the store encodes) — else the composite
    /// must encode in the shader.
    bool          formatIsSrgb() const { return m_formatIsSrgb; }
    /// Whether the session's space is floor-level (STAGE). An eye-level (LOCAL) fallback needs
    /// the caller to lift the world by the user's eye height.
    bool          floorLevel() const { return m_floorLevel; }

private:
    OpenXrSession() = default;
    bool createSpaces(const OpenXrSystem& system);
    bool createSwapchains(const OpenXrSystem& system, float renderScale, std::string& failReason);
    /// The per-eye depth swapchains + their pass and framebuffers; false (depth off, the
    /// session still fine) when the runtime offers no depth format.
    bool createDepthSwapchains();

    XrInstance m_instance = XR_NULL_HANDLE; // borrowed from the system
    XrSession  m_session = XR_NULL_HANDLE;
    XrSpace    m_space = XR_NULL_HANDLE;     // STAGE, or LOCAL as the fallback
    XrSpace    m_viewSpace = XR_NULL_HANDLE; // the head
    bool       m_floorLevel = false;
    XrEnvironmentBlendMode m_blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    XrSessionState m_state = XR_SESSION_STATE_UNKNOWN;
    bool           m_running = false;
    std::unique_ptr<OpenXrInput> m_input; // the controllers; null when the runtime offers none

    VkDevice   m_device = VK_NULL_HANDLE; // borrowed
    VkFormat   m_format = VK_FORMAT_UNDEFINED;
    bool       m_formatIsSrgb = false;
    VkExtent2D m_extent{};

    // Per eye: the runtime's swapchain, its images (owned by the runtime), our views over them,
    // and a framebuffer per image for the composite pass. Declaration order = reverse
    // destruction order: framebuffers and views before the pass, the pass before the swapchains.
    XrSwapchain                                m_swapchains[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::vector<XrSwapchainImageVulkanKHR>     m_images[2];
    UniqueRenderPass                           m_renderPass;
    std::vector<UniqueImageView>               m_imageViews[2];
    std::vector<UniqueFramebuffer>             m_framebuffers[2];

    // The depth swapchains (optional), mirroring the color ones; m_depthIndex is the image
    // acquired for the frame in flight.
    bool                                   m_depth = false;
    VkFormat                               m_depthFormat = VK_FORMAT_UNDEFINED;
    XrSwapchain                            m_depthSwapchains[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::vector<XrSwapchainImageVulkanKHR> m_depthImages[2];
    UniqueRenderPass                       m_depthRenderPass;
    std::vector<UniqueImageView>           m_depthViews[2];
    std::vector<UniqueFramebuffer>         m_depthFramebuffers[2];
    uint32_t                               m_depthIndex[2] = {0, 0};

    // The eyes as last located (pose + fov): endFrame's projection layer describes the images
    // with the same poses they were rendered from.
    XrView m_located[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};

    static constexpr float kNearPlane = 0.05f;  // metres — a hand held up to the face
    static constexpr float kFarPlane = 1000.0f; // the orbit camera's far plane
};

} // namespace pose

#endif // OPENXRSESSION_H
