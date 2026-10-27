#pragma once
#include "gt2view/procedural_cockpit.h"
#include "gt2view/shading_rate.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>
namespace {
void Check(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Vulkan error " + std::to_string(result));
}
struct Device {
    gt2view::VkContext::Existing existing;
    Device() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "GT2 cockpit corpus"; app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app;
        Check(vkCreateInstance(&create, nullptr, &existing.instance));
        uint32_t count = 0; Check(vkEnumeratePhysicalDevices(existing.instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(existing.instance, &count, devices.data()));
        for (auto device : devices) {
            VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(device, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_3) continue;
            existing.physical = device;
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) break;
        }
        if (!existing.physical) throw std::runtime_error("no Vulkan 1.3 device");
        VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(existing.physical, &properties);
        std::printf("GPU: %s\n", properties.deviceName);
        vkGetPhysicalDeviceQueueFamilyProperties(existing.physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(existing.physical, &count, queues.data());
        while (existing.queueFamily < count && !(queues[existing.queueFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++existing.queueFamily;
        if (existing.queueFamily == count) throw std::runtime_error("no graphics queue");
        const float priority = 1;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = existing.queueFamily; queue.queueCount = 1; queue.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; v13.dynamicRendering = VK_TRUE;
        VkPhysicalDeviceMultiviewFeatures multiview{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
        multiview.multiview = VK_TRUE; multiview.pNext = &v13;
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR rate{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
        existing.shadingRate = gt2view::ShadingRateFeatures(existing.physical, rate);
        const char* extension = VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME;
        if (existing.shadingRate) v13.pNext = &rate;
        VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device.pNext = &multiview; device.queueCreateInfoCount = 1; device.pQueueCreateInfos = &queue;
        if (existing.shadingRate) { device.enabledExtensionCount = 1; device.ppEnabledExtensionNames = &extension; }
        Check(vkCreateDevice(existing.physical, &device, nullptr, &existing.device));
        vkGetDeviceQueue(existing.device, existing.queueFamily, 0, &existing.queue);
    }
    ~Device() {
        if (existing.device) vkDestroyDevice(existing.device, nullptr);
        if (existing.instance) vkDestroyInstance(existing.instance, nullptr);
    }
};

[[maybe_unused]] std::array<float,16> View(const std::array<float,3>& eye, float yawDegrees, float pitchDegrees) {
    constexpr float radians = 3.14159265358979323846f / 180;
    const float yaw = yawDegrees * radians, pitch = pitchDegrees * radians;
    const float right[] = {std::cos(yaw), 0, std::sin(yaw)};
    const float up[] = {-std::sin(yaw)*std::sin(pitch), std::cos(pitch), std::cos(yaw)*std::sin(pitch)};
    const float forward[] = {std::sin(yaw)*std::cos(pitch), std::sin(pitch), -std::cos(yaw)*std::cos(pitch)};
    const float focal = 1 / std::tan(95.f * radians * .5f);
    std::array<float,16> matrix{};
    for (size_t axis = 0; axis < 3; ++axis) {
        matrix[axis*4] = right[axis] * focal;
        matrix[axis*4+1] = -up[axis] * focal;
        matrix[axis*4+3] = forward[axis];
        matrix[12] -= right[axis] * focal * eye[axis];
        matrix[13] += up[axis] * focal * eye[axis];
        matrix[15] -= forward[axis] * eye[axis];
    }
    matrix[14] = .025f; // Infinite reversed-Z projection, matching the renderer's depth convention.
    return matrix;
}

[[maybe_unused]] gt2view::DrawItem Checker(gt2view::VkSceneRenderer& renderer) {
    std::vector<gt2view::SceneVertex> vertices;
    constexpr float colours[2][3] = {{.10f, .70f, .90f}, {.85f, .64f, .12f}};
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
        for (const auto& corner : std::array<std::array<int,2>,6>{{{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}}}) {
            gt2view::SceneVertex vertex{};
            vertex.pos[0] = -1.f + float(x+corner[0])*.25f;
            vertex.pos[1] = -1.f + float(y+corner[1])*.25f;
            std::copy_n(colours[(x+y)&1], 3, vertex.color); vertices.push_back(vertex);
        }
    }
    renderer.SetVertices(0, vertices);
    gt2view::DrawItem item; item.vertexCount = uint32_t(vertices.size());
    item.mvp[0] = item.mvp[5] = item.mvp[10] = item.mvp[15] = 1;
    return item;
}
}
