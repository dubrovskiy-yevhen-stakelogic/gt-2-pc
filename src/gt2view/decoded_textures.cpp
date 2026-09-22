#include "gt2view/vk_scene_renderer.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace gt2view {
void VkSceneRenderer::PrepareDecodedTextures(const std::vector<DrawItem>& items, size_t sceneItems) {
    decoded_.Begin();
    cachedDraws_.assign(items.size(), 0);
    if (!decodedEnabled_ || !sceneItems) { std::memset(decodedTable_.mapped, 0, sizeof(decoded_.table)); return; }
    if (!decodedUpload_.buffer) {
        decodedUpload_ = CreateBuffer(size_t(DecodedTextureCache::kLayers) * DecodedTextureCache::kTexels * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        DestroyImage(decodedImage_);
        decodedImage_ = CreateImage({256, 256}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, DecodedTextureCache::kLayers, 9);
        VkDescriptorImageInfo info{decodedSampler_, decodedImage_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descSet_; write.dstBinding = 3; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr); decodedInitialized_ = false;
    }
    const auto* vertices = static_cast<const SceneVertex*>(vertexBuffer_.mapped);
    for (size_t draw = 0; draw < std::min(sceneItems, items.size()); ++draw) {
        const auto& item = items[draw];
        if (uint64_t(item.firstVertex) + item.vertexCount > kMaxVertices) continue;
        if (item.firstVertex >= kVrDrivingVertexBase) { cachedDraws_[draw] = 1; continue; }
        const uint64_t range = (uint64_t(item.firstVertex) << 32) | item.vertexCount;
        auto [it, added] = materialRanges_.try_emplace(range);
        if (added) {
            auto& materials = it->second;
            for (uint32_t i = 0; i < item.vertexCount; i += 3) {
                const auto& v = vertices[item.firstVertex + i];
                if (v.flags & (kOverlay | kExternalTexture | kHdUi)) { materials.push_back(~uint64_t(0)); continue; }
                if (!(v.flags & kTextured)) continue;
                const uint32_t clutDepth = v.clut | (((v.flags >> 8) & 3u) << 28);
                const uint64_t key = uint64_t(v.page) | (uint64_t(clutDepth) << 32) | ((v.flags & kCarPaint) ? uint64_t(1) << 63 : 0);
                materials.push_back(key);
            }
            std::sort(materials.begin(), materials.end());
            materials.erase(std::unique(materials.begin(), materials.end()), materials.end());
        }
        bool supported = true;
        const uint32_t misses = decoded_.misses;
        for (uint64_t key : it->second) {
            if (key == ~uint64_t(0)) { supported = false; continue; }
            uint32_t clutDepth = uint32_t(key >> 32) & 0x7fffffffu;
            if (key >> 63) {
                clutDepth += item.paint << 16;
                if ((clutDepth & 65535) == 224 && item.brakeLit) clutDepth += 16;
            }
            // Decode from the CPU copy: repeated palette reads from the mapped
            // GPU upload allocation can stall for seconds on discrete GPUs.
            decoded_.Prepare(uint32_t(key), clutDepth, vramShadow_.data(), kVramRows);
        }
        cachedDraws_[draw] = supported && decoded_.misses == misses;
    }
    std::memcpy(decodedTable_.mapped, decoded_.table.data(), sizeof(decoded_.table));
    auto* upload = static_cast<uint32_t*>(decodedUpload_.mapped);
    for (uint32_t layer : decoded_.uploads)
        std::memcpy(upload + size_t(layer) * DecodedTextureCache::kTexels,
            decoded_.pixels.data() + size_t(layer) * DecodedTextureCache::kTexels, size_t(DecodedTextureCache::kTexels) * 4);
}
void VkSceneRenderer::UploadDecodedTextures() {
    if (decodedInitialized_ && decoded_.uploads.empty()) return;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = decodedImage_.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, decodedImage_.mipLevels, 0, decodedImage_.layers};
    barrier.oldLayout = decodedInitialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = decodedInitialized_ ? VK_ACCESS_SHADER_READ_BIT : 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, decodedInitialized_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    std::vector<VkBufferImageCopy> copies;
    for (uint32_t layer : decoded_.uploads) {
        VkBufferImageCopy copy{};
        copy.bufferOffset = size_t(layer) * DecodedTextureCache::kTexels * 4;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1}; copy.imageExtent = {256, 256, 1};
        copies.push_back(copy);
    }
    if (!copies.empty()) vkCmdCopyBufferToImage(cmd_, decodedUpload_.buffer, decodedImage_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        uint32_t(copies.size()), copies.data());
    for (uint32_t level = 1; level <= decodedImage_.mipLevels; ++level) {
        barrier.subresourceRange.baseMipLevel = level - 1;
        barrier.subresourceRange.levelCount = 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        if (level == decodedImage_.mipLevels) break;
        std::vector<VkImageBlit> blits;
        for (uint32_t layer : decoded_.uploads) {
            VkImageBlit b{};
            b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, layer, 1};
            b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
            b.srcOffsets[1] = {int32_t(256u >> (level - 1)), int32_t(256u >> (level - 1)), 1};
            b.dstOffsets[1] = {int32_t(256u >> level), int32_t(256u >> level), 1};
            blits.push_back(b);
        }
        if (!blits.empty()) vkCmdBlitImage(cmd_, decodedImage_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            decodedImage_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(blits.size()), blits.data(), VK_FILTER_LINEAR);
    }
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = decodedImage_.mipLevels;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    decodedInitialized_ = true; decoded_.uploads.clear();
}
}

namespace gt2view {
void VkSceneRenderer::SetHandVertices(uint32_t offset, const std::vector<SceneVertex>& vertices) {
    if (uint64_t(offset) + vertices.size() > kVrDrivingVertexLimit) throw std::runtime_error("hand vertices exceed reserved range");
    boundsCache_.Invalidate(kVrDrivingVertexBase+offset, uint32_t(vertices.size()));
    WriteBuffer(vertexBuffer_, size_t(kVrDrivingVertexBase+offset) * sizeof(SceneVertex),
                vertices.data(), vertices.size() * sizeof(SceneVertex));
}
void VkSceneRenderer::UploadHandTexture(uint32_t width, uint32_t height, const uint32_t* rgba) {
    if (!width || !height || width > 2048 || height > 2048) throw std::runtime_error("invalid hand texture size");
    WaitFrame();
    DestroyImage(handImage_); DestroyBuffer(handUpload_);
    handImage_ = CreateImage({width, height}, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    handUpload_ = CreateBuffer(size_t(width)*height*4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(handUpload_.mapped, rgba, size_t(width)*height*4);
    VkDescriptorImageInfo info{decodedSampler_, handImage_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descSet_; write.dstBinding = 5; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    handPending_ = true; handExtent_ = {width,height};
}
void VkSceneRenderer::UploadHandImage() {
    if (!handPending_) return;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = handImage_.image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.imageExtent = {handExtent_.width,handExtent_.height,1};
    vkCmdCopyBufferToImage(cmd_,handUpload_.buffer,handImage_.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,0,nullptr,0,nullptr,1,&barrier);
    handPending_ = false;
}
}
