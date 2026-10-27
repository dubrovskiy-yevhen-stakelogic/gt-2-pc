#include "gt2view/vk_scene_renderer.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <stdexcept>

namespace gt2view {
void VkSceneRenderer::ResetModelAtlasForAudit() {
    BeginModelTextures();
}
void VkSceneRenderer::BeginModelTextures() {
    WaitFrame();
    modelAtlas_={};modelSourcePixels_.clear();modelSourceSizes_.clear();nativeTextureSources_.clear();modelRanges_.clear();
}
void VkSceneRenderer::PromoteModelTextures(uint32_t first,const std::vector<SceneVertex>& vertices) {
    auto promoted=vertices;
    size_t textured=0,hardware=0;
    for (auto& vertex:promoted) {
        if (!(vertex.flags & kExternalTexture)) continue;
        modelSourceSizes_[vertex.page]=vertex.clut;
        if(vertex.flags & kModelNative)nativeTextureSources_.insert(vertex.page);
        ++textured;
        const auto it=modelSourcePixels_.find(vertex.page);
        uint32_t origin=0;
        if (it!=modelSourcePixels_.end() && modelAtlas_.Insert(vertex.page,vertex.clut&65535u,vertex.clut>>16,it->second,origin,nativeTextureSources_.contains(vertex.page))) {
            vertex.page=origin;vertex.flags|=kModelAtlasTexture;
            ++hardware;
        }
    }
    if (modelAtlas_.dirty) {
        WaitFrame();
        if (!modelFullSize_) {
            DestroyImage(modelImage_);DestroyBuffer(modelUpload_);
            modelImage_=CreateImage({ModelTextureAtlas::kSide,ModelTextureAtlas::kSide},VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
            modelUpload_=CreateBuffer(size_t(ModelTextureAtlas::kSide)*ModelTextureAtlas::kSide*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            VkDescriptorImageInfo info{decodedSampler_,modelImage_.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstBinding=7;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&info;
            for (auto set:{descSet_,mirrorSourceSet_}) { write.dstSet=set;vkUpdateDescriptorSets(device_,1,&write,0,nullptr); }
            modelFullSize_=true;
        }
        std::memcpy(modelUpload_.mapped,modelAtlas_.pixels.data(),modelAtlas_.pixels.size()*4);
        modelAtlas_.dirty=false;modelPending_=true;
    }
    SetVerticesRaw(first,promoted);
    std::printf("GT4 texture atlas: %zu/%zu textured vertices use hardware sampling\n",hardware,textured);
}
void VkSceneRenderer::UploadModelImage() {
    if (modelFullSize_ && modelAtlas_.dirty) {
        std::memcpy(modelUpload_.mapped,modelAtlas_.pixels.data(),modelAtlas_.pixels.size()*4);
        modelAtlas_.dirty=false;modelPending_=true;
    }
    if (!modelPending_) return;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.image=modelImage_.image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    const uint32_t side=modelFullSize_?ModelTextureAtlas::kSide:1;
    copy.imageExtent={side,side,1};
    vkCmdCopyBufferToImage(cmd_,modelUpload_.buffer,modelImage_.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    modelPending_=false;
}
void VkSceneRenderer::PrepareFrameTextures(const std::vector<DrawItem>& items,size_t sceneItems) {
    if(!nativeEnvironmentPending_){PrepareDecodedTextures(items,sceneItems);return;}
    std::vector<DrawItem> combined;
    for(const auto& face:nativeEnvironmentFaces_)combined.insert(combined.end(),face.begin(),face.end());
    const size_t captureCount=combined.size();combined.insert(combined.end(),items.begin(),items.end());
    // Begin() clears upload requests. Preparing capture and main separately
    // marked capture pages valid on CPU while dropping their only GPU upload.
    PrepareDecodedTextures(combined,captureCount+std::min(sceneItems,items.size()));
    if (!decoded_.uploads.empty()) environmentCodedRanges_.clear();
    size_t offset=0;
    for(size_t face=0;face<3;++face) {
        const size_t end=offset+nativeEnvironmentFaces_[face].size();
        nativeEnvironmentCached_[face].assign(cachedDraws_.begin()+ptrdiff_t(offset),cachedDraws_.begin()+ptrdiff_t(end));offset=end;
    }
    const auto* vertices=static_cast<const SceneVertex*>(vertexBuffer_.mapped);
    for(size_t face=0;face<3;++face)for(size_t draw=0;draw<nativeEnvironmentFaces_[face].size();++draw) {
        if(!nativeEnvironmentCached_[face][draw])continue;
        const auto& item=nativeEnvironmentFaces_[face][draw];
        const uint64_t range=(uint64_t(item.firstVertex)<<32)|item.vertexCount;
        auto [found,added]=environmentCodedRanges_.try_emplace(range,true);
        if(added) {
            std::vector<uint32_t> codes(item.vertexCount,0);
            for(uint32_t v=0;v<item.vertexCount;++v) {
                const auto& vertex=vertices[item.firstVertex+v];
                // Car palette/mirror/normal-bearing vertices need the general
                // shader. Track geometry has none of these dynamic attributes.
                if(vertex.flags & (kCarPaint|kModelNative|kModelReflection)) {found->second=false;break;}
                if(!(vertex.flags & kTextured))continue;
                const uint32_t key=vertex.clut|(((vertex.flags>>8)&3u)<<28);
                uint32_t at=DecodedTextureCache::Bucket(vertex.page,key);
                for(uint32_t probe=0;probe<16;++probe,at=(at+1)&4095u) {
                    const auto& e=decoded_.table[at];if(!e[3])break;
                    if(e[0]==vertex.page && e[1]==key){codes[v]=e[2];break;}
                }
                if(!codes[v]) {found->second=false;break;}
            }
            if(found->second)WriteBuffer(environmentCodes_,size_t(item.firstVertex)*sizeof(uint32_t),codes.data(),codes.size()*sizeof(uint32_t));
        }
        if(!found->second)nativeEnvironmentCached_[face][draw]=0;
    }
    cachedDraws_.erase(cachedDraws_.begin(),cachedDraws_.begin()+ptrdiff_t(captureCount));
}
void VkSceneRenderer::PrepareDecodedTextures(const std::vector<DrawItem>& items, size_t sceneItems) {
    decoded_.Begin();
    cachedDraws_.assign(items.size(), 0);
    const size_t decodedItems = cacheMenuTextures_ ? items.size() : std::min(sceneItems, items.size());
    if (!decodedEnabled_ || !decodedItems) { std::memset(decodedTable_.mapped, 0, sizeof(decoded_.table)); return; }
    if (!decodedUpload_.buffer) {
        decodedUpload_ = CreateBuffer(size_t(DecodedTextureCache::kLayers) * DecodedTextureCache::kTexels * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        DestroyImage(decodedImage_);
        decodedImage_ = CreateImage({256, 256}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, DecodedTextureCache::kLayers, 9);
        VkDescriptorImageInfo info{decodedSampler_, decodedImage_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descSet_; write.dstBinding = 3; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        write.dstSet = mirrorSourceSet_;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        // The source pass's harmless array descriptor must follow this image's
        // lifetime too; its initial 1px array has just been destroyed.
        write.dstBinding=12;vkUpdateDescriptorSets(device_,1,&write,0,nullptr);
        decodedInitialized_ = false;
    }
    const auto* vertices = static_cast<const SceneVertex*>(vertexBuffer_.mapped);
    // Pin all materials, admit soft particles first, then prepare ordinary draws.
    // A busy scene can exceed capacity; smoke must not fall back to dependent
    // VRAM reads for every overlapping translucent fragment.
    for (unsigned pass = 0; pass < 3; ++pass) {
        for (size_t draw = 0; draw < decodedItems; ++draw) {
            const auto& item = items[draw];
            if ((pass == 1 && !item.softParticle) || (pass == 2 && item.softParticle)) continue;
            if (uint64_t(item.firstVertex) + item.vertexCount > kMaxVertices) continue;
            if (item.firstVertex >= kVrDrivingVertexBase &&
                uint64_t(item.firstVertex) + item.vertexCount <= uint64_t(kVrDrivingVertexBase) + kVrDrivingVertexLimit) {
                cachedDraws_[draw] = 1;
                continue;
            }
            const uint64_t range = (uint64_t(item.firstVertex) << 32) | item.vertexCount;
            auto [it, added] = materialRanges_.try_emplace(range);
            if (added) {
                auto& materials = it->second;
                for (uint32_t i = 0; i < item.vertexCount; i += 3) {
                    const auto& v = vertices[item.firstVertex + i];
                    if (v.flags & kModelAtlasTexture) continue;
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
                if (pass == 0) decoded_.Protect(uint32_t(key), clutDepth, item.softParticle != 0);
                else decoded_.Prepare(uint32_t(key), clutDepth, vramShadow_.data(), kVramRows, item.softParticle != 0);
            }
            cachedDraws_[draw] = supported && decoded_.misses == misses;
        }
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
    write.dstSet = mirrorSourceSet_;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    write.dstBinding = 6;
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
