/**
 * @file vulkancommon.h
 * @brief Small, header-only utilities shared across every Vulkan module.
 *
 * Deliberately tiny: just the things every rendering file needs (error checking,
 * a human-readable VkResult string, the global frames-in-flight constant). Keep
 * heavyweight helpers in their own translation units so this stays cheap to include.
 *
 * NOTE: This subsystem links the Vulkan loader directly (CMake's Vulkan::Vulkan),
 * so we call vkXxx functions normally. QVulkanInstance is used only to create the
 * VkInstance and the per-window VkSurfaceKHR — see VulkanContext / VulkanWindow.
 */

#ifndef VULKANCOMMON_H
#define VULKANCOMMON_H

#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>

namespace pose {

/// How many frames the CPU is allowed to record ahead of the GPU. Two gives good
/// overlap without the input latency of triple-buffering the command stream.
inline constexpr int kMaxFramesInFlight = 2;

/// Maps a VkResult to its enum name for diagnostics. Only the codes this app can
/// realistically hit are spelled out; anything else falls back to the raw integer.
inline std::string vkResultString(VkResult result) {
    switch (result) {
        case VK_SUCCESS:                        return "VK_SUCCESS";
        case VK_NOT_READY:                      return "VK_NOT_READY";
        case VK_TIMEOUT:                        return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
        default:                                return "VkResult(" + std::to_string(static_cast<int>(result)) + ")";
    }
}

/// Thrown by VK_CHECK on any non-success VkResult. Caught at the VulkanWindow
/// boundary, which tears the renderer down and reports the failure rather than
/// letting a half-initialised device leak.
class VulkanError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief Aborts the current operation (by throwing VulkanError) if @p result is not
 *        VK_SUCCESS, tagging the message with the call site.
 *
 * Use the VK_CHECK macro rather than calling this directly so __FILE__/__LINE__ are
 * captured automatically.
 */
inline void vkCheckImpl(VkResult result, const char* expr, const char* file, int line) {
    if (result != VK_SUCCESS) {
        throw VulkanError(std::string("Vulkan call failed: ") + expr + " -> " +
                          vkResultString(result) + " (" + file + ":" + std::to_string(line) + ")");
    }
}

} // namespace pose

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): a macro is the only way to capture the call site.
#define VK_CHECK(expr) ::pose::vkCheckImpl((expr), #expr, __FILE__, __LINE__)

#endif // VULKANCOMMON_H
