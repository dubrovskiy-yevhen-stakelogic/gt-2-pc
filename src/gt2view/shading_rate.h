#pragma once
#include <vulkan/vulkan.h>
#include <cstring>
#include <vector>
namespace gt2view {
// Keep feature negotiation identical for the XR device and standalone renderer tests.
inline bool ShadingRateFeatures(VkPhysicalDevice physical, VkPhysicalDeviceFragmentShadingRateFeaturesKHR& enabled) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data());
    bool found = false;
    for (auto& e : extensions) if (!std::strcmp(e.extensionName, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)) found = true;
    if (!found) return false;
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext = &available;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!available.attachmentFragmentShadingRate || !available.pipelineFragmentShadingRate) return false;
    enabled = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    enabled.attachmentFragmentShadingRate = enabled.pipelineFragmentShadingRate = VK_TRUE;
    return true;
}
}
