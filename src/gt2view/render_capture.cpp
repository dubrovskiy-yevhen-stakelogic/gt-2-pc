#include "gt2view/vk_scene_renderer.h"
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <cstring>

namespace gt2view {
// Private benchmark captures contain game assets; write only to the user's chosen path.
// Version 4 adds native model rotation; versions 1 through 3 remain readable.
static_assert(sizeof(SceneVertex) == 44 && sizeof(DrawItem) == 136 && offsetof(DrawItem,cabinDepth)==112 && offsetof(DrawItem,softParticle)==116);
void VkSceneRenderer::SaveCapture(const std::string& path, const std::vector<DrawItem>& items, size_t sceneItems) {
    WaitFrame();
    FlushFrameUploads();
    uint32_t vertices = 0;
    for (const auto& item : items) {
        if (uint64_t(item.firstVertex) + item.vertexCount > kMaxVertices) throw std::runtime_error("capture: invalid draw range");
        vertices = std::max(vertices, item.firstVertex + item.vertexCount);
    }
    std::ofstream out(path, std::ios::binary);
    const uint32_t header[] = {0x43543247, 4, vertices, kVramWidth * kVramRows, kExternalTexels, uint32_t(items.size()), uint32_t(sceneItems)};
    auto write = [&](const void* data, size_t bytes) { out.write(static_cast<const char*>(data), std::streamsize(bytes)); };
    write(header, sizeof(header)); write(clearColor, sizeof(clearColor));
    // Captures store portable external-texture addresses, never atlas offsets
    // belonging to this renderer instance. Replay can promote them again.
    const uint32_t prefix=std::min(vertices,kModelSetVertexBase);
    write(vertexBuffer_.mapped, size_t(prefix) * sizeof(SceneVertex));
    const auto* mapped=static_cast<const SceneVertex*>(vertexBuffer_.mapped);
    std::vector<SceneVertex> models(mapped+prefix,mapped+vertices);
    for (auto& vertex:models) if (vertex.flags & kModelAtlasTexture) {
        uint32_t source=0;
        if (!modelAtlas_.Source(vertex.page,source)) throw std::runtime_error("capture: missing model atlas source");
        vertex.page=source;vertex.flags&=~kModelAtlasTexture;
    }
    write(models.data(), models.size()*sizeof(SceneVertex));
    write(textureBuffer_.mapped, size_t(header[3]) * sizeof(uint32_t));
    write(externalBuffer_.mapped, size_t(header[4]) * sizeof(uint32_t));
    write(items.data(), items.size() * sizeof(DrawItem));
    if (!out) throw std::runtime_error("cannot write render capture: " + path);
}
void VkSceneRenderer::LoadCapture(const std::string& path, std::vector<DrawItem>& items, size_t& sceneItems) {
    WaitFrame();
    FlushFrameUploads();
    std::ifstream in(path, std::ios::binary);
    auto read = [&](void* data, size_t bytes) {
        if (!in.read(static_cast<char*>(data), std::streamsize(bytes))) throw std::runtime_error("truncated render capture: " + path);
    };
    uint32_t h[7]{}; read(h, sizeof(h));
    if (h[0] != 0x43543247 || (h[1] < 1 || h[1] > 4) || h[2] > kMaxVertices || (h[3] > kVramWidth * kVramRows || h[3] < kVramWidth * 2048) ||
        (h[4] != kExternalTexels && h[4] != kVrHandTexelBase) || h[5] > 100000 || h[6] > h[5]) throw std::runtime_error("invalid render capture header");
    read(clearColor, sizeof(clearColor));
    std::vector<SceneVertex> vertices(h[2]); read(vertices.data(), vertices.size() * sizeof(SceneVertex));
    std::vector<uint32_t> textureWords(h[3]);
    read(textureWords.data(), textureWords.size() * sizeof(uint32_t));
    std::memcpy(textureBuffer_.mapped, textureWords.data(), textureWords.size() * sizeof(uint32_t));
    // Keep CPU palette decoding consistent with the restored GPU contents.
    // Older captures can contain fewer rows than the current renderer.
    for (size_t i = 0; i < textureWords.size(); ++i)
        vramShadow_[i] = uint16_t(textureWords[i]);
    read(externalBuffer_.mapped, size_t(h[4]) * sizeof(uint32_t));
    modelSourcePixels_.clear();modelAtlas_=ModelTextureAtlas{};
    const auto* external=static_cast<const uint32_t*>(externalBuffer_.mapped);
    for (size_t i=kModelSetVertexBase;i<vertices.size();++i) {
        const auto& vertex=vertices[i];
        if (!(vertex.flags & kExternalTexture) || vertex.page<kModelSetTexelBase) continue;
        const uint64_t count=uint64_t(vertex.clut&65535u)*(vertex.clut>>16);
        if (uint64_t(vertex.page)+count>h[4]) throw std::runtime_error("capture: model texture out of range");
        if (!modelSourcePixels_.count(vertex.page))
            modelSourcePixels_[vertex.page].assign(external+vertex.page,external+vertex.page+size_t(count));
    }
    items.assign(h[5],DrawItem{});
    for(auto& item:items)read(&item,h[1]==1?112:h[1]==2?116:h[1]==3?120:sizeof(DrawItem));
    sceneItems = h[6];
    for (const auto& item : items)
        if (uint64_t(item.firstVertex) + item.vertexCount > vertices.size()) throw std::runtime_error("invalid capture draw range");
    decoded_.Invalidate(0, kVramRows);
    vramKnown_.clear(); // capture loading bypasses the ordinary VRAM upload API
    SetVertices(0, vertices);
    // Dynamic ranges can start at any vertex offset. Rebuild triangle UV bounds
    // with each draw's original alignment, rather than the capture buffer's zero.
    for (const auto& item : items) {
        std::vector<SceneVertex> range(vertices.begin() + item.firstVertex,
                                       vertices.begin() + item.firstVertex + item.vertexCount);
        SetVertices(item.firstVertex, range);
    }
}
}
