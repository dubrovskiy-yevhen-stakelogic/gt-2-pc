#include "gt2view/vk_scene_renderer.h"
#include "gt2view/foveation.h"
#include <algorithm>
#include <cstdio>

namespace gt2view {
void VkSceneRenderer::SetFoveation(int level) {
    level = std::clamp(level, 0, 3);
    if (level == foveation_) return;
    WaitFrame(); foveation_ = level;
    if (stereoReady_) CreateRateImage();
}
void VkSceneRenderer::CreateRateImage() {
    DestroyImage(rateImage_); DestroyBuffer(rateUpload_); rateUploaded_ = false;
    if (!foveation_ || !context_.ShadingRate()) return;
    auto getRates = reinterpret_cast<PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR>(vkGetInstanceProcAddr(context_.Instance(), "vkGetPhysicalDeviceFragmentShadingRatesKHR"));
    uint32_t count = 0;
    if (!getRates || getRates(physical_, &count, nullptr) != VK_SUCCESS) return;
    std::vector<VkPhysicalDeviceFragmentShadingRateKHR> rates(count, {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR});
    if (getRates(physical_, &count, rates.data()) != VK_SUCCESS) return;
    bool supported = false;
    for (const auto& rate : rates) if (rate.fragmentSize.width == 2 && rate.fragmentSize.height == 2 && (rate.sampleCounts & stereoSamples_)) supported = true;
    if (!supported) { std::printf("vulkan: 2x2 shading unavailable for %ux MSAA\n", unsigned(stereoSamples_)); return; }
    VkPhysicalDeviceFragmentShadingRatePropertiesKHR rateProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; properties.pNext = &rateProperties;
    vkGetPhysicalDeviceProperties2(physical_, &properties);
    rateTexel_ = rateProperties.maxFragmentShadingRateAttachmentTexelSize;
    rateExtent_ = {(stereoExtent_.width + rateTexel_.width - 1) / rateTexel_.width,
                   (stereoExtent_.height + rateTexel_.height - 1) / rateTexel_.height};
    rateImage_ = CreateImage(rateExtent_, VK_FORMAT_R8_UINT, VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    const size_t layerBytes = size_t(rateExtent_.width) * rateExtent_.height;
    rateUpload_ = CreateBuffer(layerBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto* bytes = static_cast<uint8_t*>(rateUpload_.mapped);
    for (uint32_t y = 0; y < rateExtent_.height; ++y) for (uint32_t x = 0; x < rateExtent_.width; ++x)
        bytes[y * rateExtent_.width + x] = FoveationRate(x, y, rateExtent_.width, rateExtent_.height, foveation_);
    std::printf("vulkan: foveation %d, full-resolution centre / 2x2 periphery, map %ux%u, tile %ux%u; HUD full rate\n",
        foveation_, rateExtent_.width, rateExtent_.height, rateTexel_.width, rateTexel_.height);
}
void VkSceneRenderer::UploadRateImage() {
    if (!rateImage_.image || rateUploaded_) return;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = rateImage_.image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {rateExtent_.width, rateExtent_.height, 1};
    vkCmdCopyBufferToImage(cmd_, rateUpload_.buffer, rateImage_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    rateUploaded_ = true;
}
}