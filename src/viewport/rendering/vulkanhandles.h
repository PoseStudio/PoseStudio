/**
 * @file vulkanhandles.h
 * @brief Move-only RAII owners for raw Vulkan device-level handles.
 *
 * A constructor that creates several Vulkan objects and then throws (a VK_CHECK on a later
 * step) never runs its destructor, so every handle created before the throw leaks until the
 * device is destroyed — validation then reports them at vkDestroyDevice. Holding each handle in
 * one of these owners makes cleanup automatic and exception-safe: members are destroyed in
 * reverse declaration order, whether the enclosing constructor finishes or not, so the
 * hand-written "destroy everything in the right order" destructors can go.
 *
 * Only handles whose destroy function has the uniform (device, handle, allocator) shape are
 * covered here — including the swapchain and command pool, whose KHR/core destroy entry points
 * share it; VMA-backed buffers/images have their own owners (VulkanBuffer, AttachmentImage).
 * The owner never creates anything — call the vkCreate* function as usual and hand the
 * result over with `reset(device, handle)` or the constructor.
 */

#ifndef VULKANHANDLES_H
#define VULKANHANDLES_H

#include <vulkan/vulkan.h>

#include <utility>

namespace pose {

/**
 * @class UniqueDeviceHandle
 * @brief Owns one Vulkan handle destroyed with `Destroy(device, handle, nullptr)`.
 * @tparam Handle  The Vulkan handle type (VkDescriptorPool, VkSampler, ...).
 * @tparam Destroy The matching vkDestroy* entry point (a static-loader function, so its address
 *                 is a constant expression).
 */
template <typename Handle, void(VKAPI_PTR* Destroy)(VkDevice, Handle, const VkAllocationCallbacks*)>
class UniqueDeviceHandle {
public:
    UniqueDeviceHandle() = default;
    UniqueDeviceHandle(VkDevice device, Handle handle) : m_device(device), m_handle(handle) {}
    ~UniqueDeviceHandle() { reset(); }

    UniqueDeviceHandle(const UniqueDeviceHandle&) = delete;
    UniqueDeviceHandle& operator=(const UniqueDeviceHandle&) = delete;

    UniqueDeviceHandle(UniqueDeviceHandle&& other) noexcept
        : m_device(other.m_device), m_handle(other.m_handle) {
        other.m_handle = VK_NULL_HANDLE;
    }
    UniqueDeviceHandle& operator=(UniqueDeviceHandle&& other) noexcept {
        if (this != &other) {
            reset();
            m_device = other.m_device;
            m_handle = other.m_handle;
            other.m_handle = VK_NULL_HANDLE;
        }
        return *this;
    }

    /// Destroys the current handle (if any) and takes ownership of `handle`.
    void reset(VkDevice device = VK_NULL_HANDLE, Handle handle = VK_NULL_HANDLE) {
        if (m_handle != VK_NULL_HANDLE) Destroy(m_device, m_handle, nullptr);
        m_device = device;
        m_handle = handle;
    }

    /// Gives up ownership without destroying (for handing a handle to another owner).
    Handle release() { return std::exchange(m_handle, Handle{VK_NULL_HANDLE}); }

    Handle get() const { return m_handle; }
    operator Handle() const { return m_handle; }
    explicit operator bool() const { return m_handle != VK_NULL_HANDLE; }

private:
    VkDevice m_device = VK_NULL_HANDLE;
    Handle   m_handle = VK_NULL_HANDLE;
};

using UniqueDescriptorSetLayout = UniqueDeviceHandle<VkDescriptorSetLayout, vkDestroyDescriptorSetLayout>;
using UniqueDescriptorPool      = UniqueDeviceHandle<VkDescriptorPool, vkDestroyDescriptorPool>;
using UniquePipelineLayout      = UniqueDeviceHandle<VkPipelineLayout, vkDestroyPipelineLayout>;
using UniquePipeline            = UniqueDeviceHandle<VkPipeline, vkDestroyPipeline>;
using UniqueRenderPass          = UniqueDeviceHandle<VkRenderPass, vkDestroyRenderPass>;
using UniqueFramebuffer         = UniqueDeviceHandle<VkFramebuffer, vkDestroyFramebuffer>;
using UniqueImageView           = UniqueDeviceHandle<VkImageView, vkDestroyImageView>;
using UniqueSampler             = UniqueDeviceHandle<VkSampler, vkDestroySampler>;
using UniqueShaderModule        = UniqueDeviceHandle<VkShaderModule, vkDestroyShaderModule>;
using UniqueSemaphore           = UniqueDeviceHandle<VkSemaphore, vkDestroySemaphore>;
using UniqueFence               = UniqueDeviceHandle<VkFence, vkDestroyFence>;
using UniqueCommandPool         = UniqueDeviceHandle<VkCommandPool, vkDestroyCommandPool>;
using UniqueSwapchain           = UniqueDeviceHandle<VkSwapchainKHR, vkDestroySwapchainKHR>;
using UniqueQueryPool           = UniqueDeviceHandle<VkQueryPool, vkDestroyQueryPool>;

} // namespace pose

#endif // VULKANHANDLES_H
