#pragma once
#include "gt2view/scene_types.h"
#include "gt2view/decoded_texture_cache.h"
#include <GLES3/gl3.h>
#include <memory>

namespace gt2view {
// Device backend only. Scene construction, materials and HD UI stay shared.
class WebGlSceneRenderer final : public SceneRendererState {
public:
    struct Size { uint32_t width = 0, height = 0; };
    struct CullingStats { uint32_t tested = 0, culled = 0; };
    WebGlSceneRenderer();
    ~WebGlSceneRenderer();
    WebGlSceneRenderer(const WebGlSceneRenderer&) = delete;
    WebGlSceneRenderer& operator=(const WebGlSceneRenderer&) = delete;
    void SetVertices(uint32_t first, const std::vector<SceneVertex>& vertices);
    void UploadVram(uint32_t firstRow, uint32_t rowCount, const uint16_t* words);
    void UploadExternalTexture(uint32_t first, uint32_t count, const uint32_t* rgba) override;
    void Draw(const std::vector<DrawItem>& items, const std::string& shot = {}, size_t sceneItems = 0);
    void SetOptions(const RenderOptions& value) { options_ = value; }
    const RenderOptions& Options() const { return options_; }
    Size Extent() const { return extent_; }
    Size StereoExtent() const { return {}; }
    float AspectRatio() const { return extent_.height ? float(extent_.width) / float(extent_.height) : 1.0f; }
    uint32_t EffectiveMsaa() const { return samples_; }
    int PresentMode() const { return 2; } // Browser presentation follows display refresh.
    int Foveation() const { return 0; }
    void SetFoveation(int level);
    uint32_t CachedMaterials() const { return decoded_.hits; }
    uint32_t UncachedMaterials() const { return decoded_.misses; }
    double GpuMilliseconds() const { return -1; }
    const CullingStats& LastCulling() const { return culling_; }
    float clearColor[3] = {0.227f, 0.243f, 0.275f};
private:
    struct Target {
        GLuint framebuffer = 0, color = 0, depth = 0;
        Size size;
        uint32_t samples = 1;
    };
    void DestroyTarget(Target& target);
    void EnsureTarget(Target& target, Size size, uint32_t samples, bool texture);
    void BindTextures(bool mirror);
    void DrawItems(const std::vector<DrawItem>& items, size_t first, size_t last, size_t sceneItems, Size size, bool mirror);
    void PrepareMaterials(const std::vector<DrawItem>& items);
    GLuint program_ = 0, vao_ = 0, vertices_ = 0, rects_ = 0;
    GLuint vram_ = 0, external_ = 0, materials_ = 0, pages_ = 0, black_ = 0;
    GLint mvp_ = -1, paint_ = -1, brake_ = -1, stp_ = -1, optionsUniform_ = -1;
    Size extent_;
    Target scene_, resolve_, mirror_;
    uint32_t samples_ = 1;
    int maxSamples_ = 1, maxTexture_ = 0;
    std::vector<SceneVertex> shadow_;
    std::unordered_map<uint64_t, std::vector<SceneVertex>> ranges_;
    DecodedTextureCache decoded_;
    CullingStats culling_;
};
}
