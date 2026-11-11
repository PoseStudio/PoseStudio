/**
 * @file vulkancontext.cpp
 * @brief Implementation of the device layer. See vulkancontext.h for the contract.
 */

#include "vulkancontext.h"

#include "samplercache.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

namespace pose {

namespace {
/// Device extensions every rendering device must support. Swapchain is the only hard
/// requirement at this stage; add to this list (not ad-hoc at call sites) as features grow.
const std::array<const char*, 1> kRequiredDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

/// MoltenVK (the Vulkan-on-Metal layer every macOS/iOS build runs through) implements only a
/// *subset* of Vulkan and advertises VK_KHR_portability_subset to say so. The spec REQUIRES
/// that, whenever a physical device exposes this extension, the logical device created from it
/// must enable it — otherwise vkCreateDevice is non-conformant and may fail. Desktop
/// Windows/Linux drivers don't expose it, so we probe for it and enable it only when present
/// (a true no-op off Apple). Using the string literal avoids pulling in the beta headers
/// (VK_ENABLE_BETA_EXTENSIONS) solely for the name macro.
constexpr const char* kPortabilitySubsetExtension = "VK_KHR_portability_subset";
} // namespace

VulkanContext::VulkanContext(VkInstance instance, VkSurfaceKHR surface, uint32_t apiVersion,
                             const VulkanContextOptions& options)
    : m_instance(instance), m_surface(surface) {
    pickPhysicalDevice(options.preferredDevice);
    createLogicalDevice(options.extraDeviceExtensions);
    createAllocator(apiVersion);
    m_samplers = std::make_unique<SamplerCache>(m_device);

    // Pick the MSAA level once: the highest of {4x, 2x} the device supports for BOTH color and depth
    // framebuffers, else single-sample. 4x is the quality/perf sweet spot for a viewport; the HDR
    // scene pass, the outline mask, and every scene pipeline are built against this count.
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
    const VkSampleCountFlags counts =
        props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
    for (VkSampleCountFlagBits bit : {VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_2_BIT}) {
        if (counts & bit) {
            m_sampleCount = bit;
            break;
        }
    }
}

VulkanContext::~VulkanContext() {
    // Reverse construction order. The samplers must go before the device that owns them (a
    // member would otherwise be destroyed AFTER this body, i.e. after vkDestroyDevice). The
    // instance and surface are borrowed (owned by QVulkanInstance), so we never destroy them here.
    m_samplers.reset();
    if (m_allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(m_allocator);
    }
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
    }
}

const VkFormatProperties& VulkanContext::formatProperties(VkFormat format) const {
    auto it = m_formatProperties.find(format);
    if (it == m_formatProperties.end()) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &props);
        it = m_formatProperties.emplace(format, props).first;
    }
    return it->second;
}

bool VulkanContext::findQueueFamilies(VkPhysicalDevice device, uint32_t& graphicsFamily,
                                      uint32_t& presentFamily) const {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    // We look for a single family that can do both graphics and present. The vast
    // majority of desktop GPUs expose one, which keeps the device/swapchain setup
    // free of cross-family ownership transfers. A split-family fallback can be added
    // later if a real device needs it.
    for (uint32_t i = 0; i < count; ++i) {
        if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            continue;
        }
        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &presentSupport);
        if (presentSupport == VK_TRUE) {
            graphicsFamily = i;
            presentFamily = i;
            return true;
        }
    }
    return false;
}

void VulkanContext::pickPhysicalDevice(VkPhysicalDevice preferred) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) {
        throw VulkanError("No Vulkan-capable physical devices found.");
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    // Prefer a discrete GPU, but accept any device that has a graphics+present queue
    // family and the required extensions. Score-based selection lets a future "let the
    // user choose their GPU" preference slot in without reworking the loop. The headset
    // runtime's device (VulkanContextOptions::preferredDevice) outscores everything when it
    // qualifies: the headset can only be driven from it.
    VkPhysicalDevice best = VK_NULL_HANDLE;
    int bestScore = -1;
    for (VkPhysicalDevice device : devices) {
        uint32_t g = 0, p = 0;
        if (!findQueueFamilies(device, g, p)) {
            continue;
        }

        // Verify required device extensions are all present.
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> available(extCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, available.data());
        std::set<std::string> missing(kRequiredDeviceExtensions.begin(), kRequiredDeviceExtensions.end());
        for (const auto& ext : available) {
            missing.erase(ext.extensionName);
        }
        if (!missing.empty()) {
            continue;
        }

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device, &props);
        int score = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 1000 : 0;
        if (preferred != VK_NULL_HANDLE && device == preferred) {
            score += 10000;
        }
        if (score > bestScore) {
            bestScore = score;
            best = device;
            m_graphicsFamily = g;
            m_presentFamily = p;
        }
    }

    if (best == VK_NULL_HANDLE) {
        throw VulkanError("No physical device with a graphics+present queue and swapchain support.");
    }
    m_physicalDevice = best;
    m_preferredDeviceUsed = preferred != VK_NULL_HANDLE && best == preferred;

    // Plain stderr (not qInfo) keeps the rendering/ core free of Qt — see CLAUDE.md.
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
    std::fprintf(stderr, "[Vulkan] Using device: %s (API %u.%u)%s\n", props.deviceName,
                 VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                 preferred == VK_NULL_HANDLE ? "" : (m_preferredDeviceUsed ? " [the headset's device]"
                                                                             : " [NOT the headset's device]"));
}

void VulkanContext::createLogicalDevice(const std::vector<std::string>& extraExtensions) {
    const float priority = 1.0f;
    // Use a set so a graphics==present family only produces one create-info entry.
    std::set<uint32_t> uniqueFamilies = {m_graphicsFamily, m_presentFamily};
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        queueInfos.push_back(qi);
    }

    VkPhysicalDeviceFeatures features{};
    // Per-attachment blend/write-mask states: the HDR scene pass has two color attachments
    // (diffuse + the specular the SSS blur must not smear) with different write masks per
    // pipeline. Universally supported on desktop hardware (and by MoltenVK); guard anyway so an
    // exotic device degrades gracefully at pipeline creation (VulkanPipeline collapses the two
    // attachments to one shared state — see supportsIndependentBlend()) rather than failing here.
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(m_physicalDevice, &supported);
    features.independentBlend = supported.independentBlend;
    m_supportsIndependentBlend = supported.independentBlend == VK_TRUE;
    // Wireframe rasterization (the wireframe shade modes' LINE polygon mode). Universal on
    // desktop GPUs and MoltenVK; a device without it simply gets no wire pipelines (the wire
    // modes then draw their surface fill alone — see Scene).
    features.fillModeNonSolid = supported.fillModeNonSolid;
    m_supportsWireframe = supported.fillModeNonSolid == VK_TRUE;

    // Start from the hard requirements, then append VK_KHR_portability_subset if the chosen
    // device exposes it (mandatory on MoltenVK — see the constant's note above).
    std::vector<const char*> enabledExtensions(kRequiredDeviceExtensions.begin(),
                                               kRequiredDeviceExtensions.end());
    {
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> available(extCount);
        vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, available.data());
        for (const auto& ext : available) {
            if (std::strcmp(ext.extensionName, kPortabilitySubsetExtension) == 0) {
                enabledExtensions.push_back(kPortabilitySubsetExtension);
                break;
            }
        }

        // The headset runtime's extensions (VulkanContextOptions): all of them or none — a
        // partially satisfied runtime cannot drive the device, and the desktop needs none of
        // them. Already-listed names are skipped (vkCreateDevice rejects duplicates).
        const auto supported = [&available](const std::string& name) {
            for (const auto& ext : available) {
                if (name == ext.extensionName) {
                    return true;
                }
            }
            return false;
        };
        bool allSupported = true;
        for (const std::string& name : extraExtensions) {
            if (!supported(name)) {
                std::fprintf(stderr, "[Vulkan] device lacks %s (the headset runtime requires it)\n", name.c_str());
                allSupported = false;
            }
        }
        if (allSupported) {
            for (const std::string& name : extraExtensions) {
                bool listed = false;
                for (const char* enabled : enabledExtensions) {
                    if (name == enabled) {
                        listed = true;
                        break;
                    }
                }
                if (!listed) {
                    enabledExtensions.push_back(name.c_str()); // extraExtensions outlives vkCreateDevice below
                }
            }
            m_extraExtensionsEnabled = true;
        }
    }

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    ci.pQueueCreateInfos = queueInfos.data();
    ci.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    ci.ppEnabledExtensionNames = enabledExtensions.data();
    ci.pEnabledFeatures = &features;

    VK_CHECK(vkCreateDevice(m_physicalDevice, &ci, nullptr, &m_device));

    vkGetDeviceQueue(m_device, m_graphicsFamily, 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_presentFamily, 0, &m_presentQueue);
}

void VulkanContext::createAllocator(uint32_t apiVersion) {
    // VMA is configured to call the statically-linked loader symbols (see vma_impl.cpp),
    // so no function-pointer table is needed here.
    VmaAllocatorCreateInfo ci{};
    ci.instance = m_instance;
    ci.physicalDevice = m_physicalDevice;
    ci.device = m_device;
    ci.vulkanApiVersion = apiVersion;
    VK_CHECK(vmaCreateAllocator(&ci, &m_allocator));
}

} // namespace pose
