/**
 * @file openxrsystem.cpp
 * @brief Implementation of OpenXrSystem. See openxrsystem.h.
 */

#include "openxrsystem.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <type_traits>

namespace pose {

namespace {

/// Splits the runtime's space-separated extension list.
std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> words;
    std::istringstream in(text);
    std::string word;
    while (in >> word) {
        words.push_back(word);
    }
    return words;
}

/// Reads a runtime-filled, space-separated extension string through the two-call pattern
/// (@p query is one of the PFN_xrGetVulkan*ExtensionsKHR entry points).
template <class Query>
std::vector<std::string> queryExtensionList(Query query, XrInstance instance, XrSystemId system) {
    if (query == nullptr) {
        return {};
    }
    uint32_t size = 0;
    if (XR_FAILED(query(instance, system, 0, &size, nullptr)) || size == 0) {
        return {};
    }
    std::string buffer(size, '\0');
    if (XR_FAILED(query(instance, system, size, &size, buffer.data()))) {
        return {};
    }
    buffer.resize(std::strlen(buffer.c_str()));
    return splitWords(buffer);
}

/// XrVersion (16.16.32 packed) -> the Vulkan API version packing.
uint32_t vulkanVersionFromXr(XrVersion v) {
    return VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(v), XR_VERSION_MINOR(v), 0);
}

} // namespace

std::string OpenXrSystem::resultName(XrInstance instance, XrResult result) {
    if (instance != XR_NULL_HANDLE) {
        char buffer[XR_MAX_RESULT_STRING_SIZE] = {};
        if (XR_SUCCEEDED(xrResultToString(instance, result, buffer))) {
            return buffer;
        }
    }
    switch (result) { // the codes that can occur before an instance exists
    case XR_ERROR_RUNTIME_UNAVAILABLE:     return "XR_ERROR_RUNTIME_UNAVAILABLE";
    case XR_ERROR_RUNTIME_FAILURE:         return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
    case XR_ERROR_EXTENSION_NOT_PRESENT:   return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case XR_ERROR_INSTANCE_LOST:           return "XR_ERROR_INSTANCE_LOST";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
    case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
    default:                               return "XrResult(" + std::to_string(static_cast<int>(result)) + ")";
    }
}

std::unique_ptr<OpenXrSystem> OpenXrSystem::create(std::string& failReason) {
    // --- The instance extension we need: the Vulkan graphics binding. ---------------------------
    uint32_t extensionCount = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
    if (XR_FAILED(result)) {
        failReason = result == XR_ERROR_RUNTIME_UNAVAILABLE
                         ? "No OpenXR runtime is installed. Install the Meta Quest Link app (for a Quest), "
                           "SteamVR, or Virtual Desktop, and make it the active OpenXR runtime."
                         : "The OpenXR loader could not list the runtime's extensions (" +
                               resultName(XR_NULL_HANDLE, result) + ").";
        return nullptr;
    }
    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    if (extensionCount > 0) {
        xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data());
    }
    bool hasVulkan = false;
    for (const XrExtensionProperties& ext : extensions) {
        if (std::strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) {
            hasVulkan = true;
            break;
        }
    }
    if (!hasVulkan) {
        failReason = "The active OpenXR runtime does not support Vulkan applications (" XR_KHR_VULKAN_ENABLE_EXTENSION_NAME
                     " is missing).";
        return nullptr;
    }

    // --- The instance. OpenXR 1.1 first; a 1.0-only runtime gets the 1.0 API. -------------------
    std::unique_ptr<OpenXrSystem> system(new OpenXrSystem());
    const char* enabled[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "PoseStudio");
    info.applicationInfo.applicationVersion = 1;
    std::snprintf(info.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "PoseStudio");
    info.applicationInfo.engineVersion = 1;
    info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = enabled;
    result = xrCreateInstance(&info, &system->m_instance);
    if (result == XR_ERROR_API_VERSION_UNSUPPORTED) {
        info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        result = xrCreateInstance(&info, &system->m_instance);
    }
    if (XR_FAILED(result)) {
        failReason = result == XR_ERROR_RUNTIME_UNAVAILABLE
                         ? "The OpenXR runtime could not be started. Is the Meta Quest Link app (or SteamVR / "
                           "Virtual Desktop) installed and set as the active OpenXR runtime?"
                         : "The OpenXR runtime refused to start (" + resultName(XR_NULL_HANDLE, result) + ").";
        return nullptr;
    }

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(system->m_instance, &props))) {
        system->m_runtimeName = props.runtimeName;
    }

    // --- The headset. Absent = a normal outcome (not connected, Link not active). ---------------
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    result = xrGetSystem(system->m_instance, &systemInfo, &system->m_systemId);
    if (XR_FAILED(result)) {
        failReason = result == XR_ERROR_FORM_FACTOR_UNAVAILABLE
                         ? "No headset was found when PoseStudio started (runtime: " + system->m_runtimeName +
                               "). Connect the headset first (Quest Link or Air Link, Virtual Desktop, or "
                               "SteamVR), then restart PoseStudio."
                         : "The headset could not be opened (" +
                               resultName(system->m_instance, result) + ").";
        return nullptr;
    }
    XrSystemProperties systemProps{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(system->m_instance, system->m_systemId, &systemProps))) {
        system->m_systemName = systemProps.systemName;
    }

    // --- XR_KHR_vulkan_enable's entry points + the Vulkan version the runtime accepts. ----------
    const auto load = [&](const char* name, auto& fn) {
        PFN_xrVoidFunction raw = nullptr;
        if (XR_FAILED(xrGetInstanceProcAddr(system->m_instance, name, &raw)) || raw == nullptr) {
            return false;
        }
        fn = reinterpret_cast<std::decay_t<decltype(fn)>>(raw);
        return true;
    };
    if (!load("xrGetVulkanInstanceExtensionsKHR", system->m_getInstanceExtensions) ||
        !load("xrGetVulkanDeviceExtensionsKHR", system->m_getDeviceExtensions) ||
        !load("xrGetVulkanGraphicsDeviceKHR", system->m_getGraphicsDevice) ||
        !load("xrGetVulkanGraphicsRequirementsKHR", system->m_getGraphicsRequirements)) {
        failReason = "The OpenXR runtime (" + system->m_runtimeName + ") did not provide its Vulkan entry points.";
        return nullptr;
    }
    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    result = system->m_getGraphicsRequirements(system->m_instance, system->m_systemId, &requirements);
    if (XR_FAILED(result)) {
        failReason = "The OpenXR runtime could not report its Vulkan requirements (" +
                     resultName(system->m_instance, result) + ").";
        return nullptr;
    }
    system->m_minApiVersion = vulkanVersionFromXr(requirements.minApiVersionSupported);
    system->m_maxApiVersion = vulkanVersionFromXr(requirements.maxApiVersionSupported);

    // Plain stderr (not qInfo): this module is Qt-free like the rendering core.
    std::fprintf(stderr, "[vr] OpenXR runtime: %s; headset: %s; Vulkan %u.%u-%u.%u\n",
                 system->m_runtimeName.c_str(), system->m_systemName.c_str(),
                 VK_API_VERSION_MAJOR(system->m_minApiVersion), VK_API_VERSION_MINOR(system->m_minApiVersion),
                 VK_API_VERSION_MAJOR(system->m_maxApiVersion), VK_API_VERSION_MINOR(system->m_maxApiVersion));
    return system;
}

OpenXrSystem::~OpenXrSystem() {
    if (m_instance != XR_NULL_HANDLE) {
        xrDestroyInstance(m_instance);
    }
}

std::vector<std::string> OpenXrSystem::vulkanInstanceExtensions() const {
    return queryExtensionList(m_getInstanceExtensions, m_instance, m_systemId);
}

std::vector<std::string> OpenXrSystem::vulkanDeviceExtensions() const {
    return queryExtensionList(m_getDeviceExtensions, m_instance, m_systemId);
}

VkPhysicalDevice OpenXrSystem::vulkanPhysicalDevice(VkInstance instance) const {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    if (m_getGraphicsDevice == nullptr ||
        XR_FAILED(m_getGraphicsDevice(m_instance, m_systemId, instance, &device))) {
        return VK_NULL_HANDLE;
    }
    return device;
}

} // namespace pose
