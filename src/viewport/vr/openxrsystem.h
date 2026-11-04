/**
 * @file openxrsystem.h
 * @brief The OpenXR instance + the headset ("system") found at startup, and the Vulkan
 *        requirements the headset runtime negotiates through XR_KHR_vulkan_enable.
 *
 * Created BEFORE the application's Vulkan instance (ViewportWidget, when VR support is enabled in
 * Preferences): the runtime dictates which Vulkan instance and device extensions must be enabled,
 * which physical device the headset is driven by, and the API version range it accepts — none of
 * which can be changed once the device exists. So the negotiation is a startup matter and the
 * answers are read here by ViewportWidget (instance extensions, API version) and VulkanContext
 * (physical device, device extensions). The session itself (OpenXrSession) is created later, on
 * demand, from this system.
 *
 * Failure is a normal outcome — no runtime installed, no headset connected — and is reported as
 * a user-readable reason rather than an exception; the application then runs without VR. The
 * OpenXR loader is linked statically (CMake); the active runtime is whatever the loader finds
 * registered on the machine (Meta Quest Link, SteamVR, Virtual Desktop, Monado, ...). Pure
 * OpenXR + Vulkan + std, no Qt.
 */

#ifndef OPENXRSYSTEM_H
#define OPENXRSYSTEM_H

#include <vulkan/vulkan.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h> // XR_KHR_vulkan_enable (XR_USE_GRAPHICS_API_VULKAN is project-wide)

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pose {

class OpenXrSystem {
public:
    /// Creates the instance (XR_KHR_vulkan_enable), finds the head-mounted display, and reads the
    /// runtime's Vulkan requirements. Null with @p failReason set (in the user's words) when any
    /// step fails: no runtime, a runtime without the Vulkan extension, no headset present.
    static std::unique_ptr<OpenXrSystem> create(std::string& failReason);
    ~OpenXrSystem();

    OpenXrSystem(const OpenXrSystem&) = delete;
    OpenXrSystem& operator=(const OpenXrSystem&) = delete;

    XrInstance         instance() const { return m_instance; }
    XrSystemId         systemId() const { return m_systemId; }
    const std::string& runtimeName() const { return m_runtimeName; }
    const std::string& systemName() const { return m_systemName; }

    /// The Vulkan INSTANCE extensions the runtime requires (enable them on QVulkanInstance).
    std::vector<std::string> vulkanInstanceExtensions() const;
    /// The Vulkan DEVICE extensions the runtime requires (VulkanContext enables them).
    std::vector<std::string> vulkanDeviceExtensions() const;
    /// The physical device the headset is driven by, for @p instance (VulkanContext must create
    /// its logical device on it). VK_NULL_HANDLE if the runtime cannot say.
    VkPhysicalDevice vulkanPhysicalDevice(VkInstance instance) const;
    /// The Vulkan API version range the runtime accepts (VK_MAKE_API_VERSION-packed).
    uint32_t minVulkanApiVersion() const { return m_minApiVersion; }
    uint32_t maxVulkanApiVersion() const { return m_maxApiVersion; }

    /// A user-readable name for an OpenXR result ("XR_ERROR_RUNTIME_UNAVAILABLE"), with the
    /// instance's own spelling when one exists.
    static std::string resultName(XrInstance instance, XrResult result);

private:
    OpenXrSystem() = default;

    XrInstance  m_instance = XR_NULL_HANDLE;
    XrSystemId  m_systemId = XR_NULL_SYSTEM_ID;
    std::string m_runtimeName;
    std::string m_systemName;
    uint32_t    m_minApiVersion = 0;
    uint32_t    m_maxApiVersion = 0;

    // XR_KHR_vulkan_enable entry points (loaded through xrGetInstanceProcAddr).
    PFN_xrGetVulkanInstanceExtensionsKHR   m_getInstanceExtensions = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR     m_getDeviceExtensions = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR       m_getGraphicsDevice = nullptr;
    PFN_xrGetVulkanGraphicsRequirementsKHR m_getGraphicsRequirements = nullptr;
};

} // namespace pose

#endif // OPENXRSYSTEM_H
