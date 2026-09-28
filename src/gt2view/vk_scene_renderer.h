#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>

#include "gt2view/scene_types.h"
#include "gt2view/vk_context.h"
#include "gt2view/draw_culling.h"
#include "gt2view/decoded_texture_cache.h"

namespace gt2view {

// Depth: reversed Z (clear 0, GREATER_OR_EQUAL, z_ndc = zNear / distance with an infinite far plane) - projections
// must put zNear into the clip z row (see ReversedZ in the callers); 2D layers use z = 1 (always in front, later
// quads over earlier ones).
//
// Minimal Vulkan 1.3 (dynamic rendering) renderer: one vertex buffer, one PS1-VRAM buffer, N draws.
// One frame in flight.
//
// The instance / surface / device belong to the VkContext handed in (gt2view/vk_context.h): this class owns the
// window's swapchain and records the frame into a RenderTarget. The XR path (M1 / M2) will build a context from the
// runtime's instance and device and supply the RenderTargets of an XrSwapchain (colour, optional depth, two array
// layers) instead of the swapchain, which is the only piece of this class tied to a window.
class VkSceneRenderer : public SceneRendererState {
public:
    static constexpr VkFormat kSceneDepthFormat = VK_FORMAT_D32_SFLOAT;
    explicit VkSceneRenderer(VkContext& context);
    // Offscreen (the XR path, M1): no window swapchain - every frame is recorded into one image of `extent` and
    // `format` (a *_UNORM format: the frame's bytes are the window path's, see SaveScreenshot), which Draw leaves in
    // VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL for the caller to copy into an XrSwapchain image. Present does not exist
    // here; the caller submits the frame to the compositor.
    VkSceneRenderer(VkContext& context, VkExtent2D extent, VkFormat format);
    ~VkSceneRenderer();
    VkSceneRenderer(const VkSceneRenderer&) = delete;
    VkSceneRenderer& operator=(const VkSceneRenderer&) = delete;

    // Replaces vertices [first, first + count).
    void SetVertices(uint32_t first, const std::vector<SceneVertex>& vertices);
    // Build the next XR frame on the CPU while the previous GPU submission finishes.
    // Draw/DrawStereo commit these writes after the frame fence, before recording.
    void BeginFrameUploads() { deferUploads_ = true; }
    // Native hands have fixed UV/materials and do not use PS1 atlas rectangles.
    void SetHandVertices(uint32_t offset, const std::vector<SceneVertex>& vertices);
    void UploadHandTexture(uint32_t width, uint32_t height, const uint32_t* rgba);
    // Copies `rowCount` rows of 1024 16-bit words into the VRAM buffer starting at `firstRow`.
    void UploadVram(uint32_t firstRow, uint32_t rowCount, const uint16_t* words);
    // Copies `count` RGBA8 texels (R in the low byte) into the external texture store at `firstTexel`
    // (kExternalTexture vertices address it; see scene_assets.h UseExternalCar).
    void UploadExternalTexture(uint32_t firstTexel, uint32_t count, const uint32_t* rgba);

    // If screenshotPath is not empty the presented image is also saved as PNG. items[0, sceneItems) are the scene
    // (RenderOptions apply to them: an offscreen target at the scene scale with MSAA, resolved and scaled into the
    // window, then the remaining items at the window's resolution); sceneItems = 0: every item as a 2D / native layer.
    void Draw(const std::vector<DrawItem>& items, const std::string& screenshotPath = {}, size_t sceneItems = 0);

    // Takes effect from the next Draw (a changed MSAA / scale recreates the scene targets, vsync the swapchain).
    void SetOptions(const RenderOptions& options);
    const RenderOptions& Options() const { return options_; }
    // The MSAA sample count actually used (the request clamped to the device's colour / depth limits).
    uint32_t EffectiveMsaa() const { return uint32_t(ClampSamples(options_.msaa)); }
    // The present mode in use (VK_PRESENT_MODE_*), for the frame-rate log.
    VkPresentModeKHR PresentMode() const { return presentMode_; }

    // Offscreen mode (the XR path): the image the last Draw recorded, in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, and
    // a wait for its queue submission to have completed (before copying it into a compositor swapchain image).
    bool Offscreen() const { return offscreen_; }
    VkImage OffscreenImage() const { return images_.empty() ? VK_NULL_HANDLE : images_[0]; }
    VkExtent2D Extent() const { return extent_; }
    void WaitFrame();
    void SetFoveation(int level);
    int Foveation() const { return rateImage_.image ? foveation_ : 0; }
    void EnableDecodedTextures(bool enabled) { decodedEnabled_ = enabled; }
    uint32_t CachedMaterials() const { return decoded_.hits; }
    uint32_t UncachedMaterials() const { return decoded_.misses; }
    double GpuMilliseconds() const { return gpuMs_; } // -1 when queue timestamps are unavailable
    void SaveCapture(const std::string& path, const std::vector<DrawItem>& items, size_t sceneItems);
    void LoadCapture(const std::string& path, std::vector<DrawItem>& items, size_t& sceneItems);

    // ---- stereo (docs/research/vr_port_plan.md, M2) ----
    // Creates the two-layer colour and depth images the race is rendered into in VR, of `extent` per eye and
    // `format` (the *_UNORM twin of the compositor's swapchain format, as in offscreen mode). `multiview` = one pass
    // with VkRenderingInfo::viewMask 0b11 instead of one pass per eye. Call once (it recreates on a change).
    void CreateStereoTarget(VkExtent2D extent, VkFormat format, bool multiview);
    bool StereoReady() const { return stereoReady_; }
    bool StereoMultiview() const { return stereoMultiview_; }
    VkExtent2D StereoExtent() const { return stereoExtent_; }
    // The two-layer images the last DrawStereo recorded, both left in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL.
    VkImage StereoImage() const { return externalStereoImage_ ? externalStereoImage_ : stereoColor_.image; }
    void SetExternalStereoImage(VkImage image);
    VkImage StereoDepthImage() const { return stereoDepth_.image; }
    VkFormat StereoDepthFormat() const { return kSceneDepthFormat; }
    // The per-eye matrices of the frame about to be recorded (vr_rig.h View::worldVP / skyVP).
    void SetStereoViews(const StereoViews& views) { stereoViews_ = views; }
    // Records `items` into the stereo target, once for both eyes. `screenshotPath` saves the left eye as PNG.
    void DrawStereo(const std::vector<DrawItem>& items, const std::string& screenshotPath = {}, size_t sceneItems = 0);

    struct CullingStats { uint32_t tested = 0, culled = 0; };
    const CullingStats& LastCulling() const { return culling_; }

    float clearColor[3] = {0.227f, 0.243f, 0.275f};

    float AspectRatio() const { return extent_.height ? float(extent_.width) / float(extent_.height) : 1.0f; }

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    struct PendingWrite { Buffer* buffer = nullptr; size_t offset = 0; std::vector<uint8_t> bytes; };
    std::vector<PendingWrite> pendingWrites_;
    size_t pendingWriteCount_ = 0;
    bool deferUploads_ = false;
    std::vector<float> rectScratch_;
    std::vector<bool> vramKnown_;
    void WriteBuffer(Buffer& buffer, size_t offset, const void* data, size_t bytes);
    void FlushFrameUploads(); // caller has waited for the frame fence
    void CreateSwapchain();
    void CreateOffscreen(VkExtent2D extent, VkFormat format); // the XR path's single target, instead of a swapchain
    void DestroySwapchain();
    void CreatePipeline();
    // `stereo` = the stereo vertex shader (scene_stereo.vert); `viewMask` != 0 = its multiview variant.
    void CreatePipelineSet(VkSampleCountFlagBits samples, VkPipeline out[6], bool stereo = false, uint32_t viewMask = 0, bool cachedOnly = false, bool cachedHud = false);
    void DestroyPipelineSet(VkPipeline set[6]);
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;   // the whole image (2D, or 2D_ARRAY when layers > 1)
        VkImageView layer[2] = {};           // one view per array layer (the two-pass stereo path)
        uint32_t layers = 1, mipLevels = 1;
        bool ownsImage = true;
    };
    Image CreateImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits samples, VkImageAspectFlags aspect,
                      uint32_t layers = 1, uint32_t mipLevels = 1);
    void DestroyImage(Image& image);
    void DestroyStereoTarget();
    void PrepareDecodedTextures(const std::vector<DrawItem>& items, size_t sceneItems);
    void UploadDecodedTextures();
    // Records `items` once per eye into the stereo target (`layer` < 0 = one multiview pass).
    void RecordStereoPass(const std::vector<DrawItem>& items, size_t sceneItems, int layer);
    void RecordMirrorPass(const std::vector<DrawItem>& items, size_t sceneItems);
    void CreateSceneTargets();  // the offscreen scene target for options_ at the window's extent
    void DestroySceneTargets();
    VkSampleCountFlagBits ClampSamples(uint32_t requested) const;
    // Records items[first, last) into the current dynamic rendering (target of `extent`), with `pipelines` (sample count
    // of the target); items below `sceneItems` get the scene's option bits.
    void RecordItems(const std::vector<DrawItem>& items, size_t first, size_t last, VkExtent2D extent, const VkPipeline pipelines[6], size_t sceneItems);
    // The swapchain image `imageIndex` with the window's depth buffer, as a target for BeginTargetRendering.
    RenderTarget WindowTarget(uint32_t imageIndex) const;
    // Begins dynamic rendering into `target` (colour loaded or cleared, depth cleared and not stored).
    void BeginTargetRendering(const RenderTarget& target, VkAttachmentLoadOp colorLoad);
    void DrawScenePass(const std::vector<DrawItem>& items, size_t sceneItems, uint32_t imageIndex);
    void FinishFrame(uint32_t imageIndex, const std::string& screenshotPath); // capture, present
    Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage);
    void DestroyBuffer(Buffer& b);
    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const;
    void SaveScreenshot(const std::string& path);
    void SaveReadback(const std::string& path, VkExtent2D extent, VkFormat format);

    VkContext& context_;
    // Copies of the context's handles (it owns them and outlives the renderer).
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    PFN_vkCmdBeginRendering cmdBeginRendering_ = nullptr;
    PFN_vkCmdEndRendering cmdEndRendering_ = nullptr;
    uint32_t queueFamily_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat colorFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;

    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkQueryPool gpuQueries_ = VK_NULL_HANDLE;
    uint32_t timestampBits_ = 0;
    double timestampNs_ = 0, gpuMs_ = -1;
    bool gpuQueryPending_ = false;
    VkSemaphore acquired_ = VK_NULL_HANDLE, rendered_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descSet_ = VK_NULL_HANDLE;
    VkDescriptorSet mirrorSourceSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipelines_[6] = {}; // [0..3] PS1 blend modes, [4] opaque, [5] UI coverage
    VkPipeline cachedMenuPipelines_[6] = {}; // Mono menu cars: the texture-only fragment path.
    VkPipeline msaaPipelines_[6] = {}; // the same for the scene target's sample count (msaaSamples_ > 1)
    VkSampleCountFlagBits msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;


    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool blitLinear_ = false;       // the colour format supports linear-filtered blits
    // The scene target (only when the options need it): colour at the scene scale (resolve target of the MSAA colour),
    // the MSAA colour, the scene's depth.
    VkExtent2D sceneExtent_{};
    Image sceneColor_, sceneMsaa_, sceneDepth_;
    static constexpr VkExtent2D kMirrorExtent{480, 128};
    Image mirrorColor_, mirrorDepth_;
    bool mirrorInitialized_ = false, recordMirror_ = false;
    bool sceneTargets_ = false;
    // Offscreen mode (the XR path): the one colour image the frames are recorded into (also images_[0] / views_[0]).
    bool offscreen_ = false;
    Image offscreenTarget_;
    // Stereo (M2): the two-layer colour / depth the race is rendered into in VR, plus the multisampled colour when
    // the graphics options ask for MSAA (resolved into stereoColor_ in the pass).
    bool stereoReady_ = false, stereoMultiview_ = true;
    VkExtent2D stereoExtent_{};
    VkFormat stereoFormat_ = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits stereoSamples_ = VK_SAMPLE_COUNT_1_BIT;
    Image stereoColor_, stereoMsaa_, stereoDepth_;
    Image rateImage_; Buffer rateUpload_;
    VkExtent2D rateExtent_{}, rateTexel_{};
    int foveation_ = 0; bool rateUploaded_ = false;
    void CreateRateImage();
    void UploadRateImage();
    VkImage externalStereoImage_ = VK_NULL_HANDLE;
    std::unordered_map<VkImage, Image> externalStereoViews_;
    VkPipeline cachedPipelines_[6] = {};
    VkPipeline cachedHudPipelines_[6] = {}; // Cached 2D materials keep full-rate shading and HUD clipping.
    std::vector<uint8_t> cachedDraws_;
    VkPipeline stereoPipelines_[6] = {}; // the stereo shader at the stereo target's sample count
    bool recordStereo_ = false;
    uint32_t recordEye_ = 0;             // the eye index RecordItems pushes (the two-pass path)
    uint32_t recordLayers_ = 1;          // array layers of the target being recorded (vkCmdClearAttachments)
    StereoViews stereoViews_;
    bool decodedEnabled_ = true;
    bool cacheMenuTextures_ = false; // Metal also decodes palette textures outside the race scene.
    bool cachedMenuShader_ = false;
    DecodedTextureCache decoded_;
    std::unordered_map<uint64_t, std::vector<uint64_t>> materialRanges_;
    Image decodedImage_, handImage_;
    Buffer handUpload_;
    bool handPending_ = false;
    VkExtent2D handExtent_{1,1};
    void UploadHandImage();
    Buffer decodedUpload_, decodedTable_;
    VkSampler decodedSampler_ = VK_NULL_HANDLE;
    bool decodedInitialized_ = false;
    Buffer viewBuffer_; // the uniform buffer the stereo shader reads (set 0, binding 2)

    // Per-vertex UV rectangle of each vertex's triangle (min u, min v, max u, max v; texel units), filled by SetVertices
    // for the smooth texture filter (vertex binding 1).
    DrawBoundsCache boundsCache_;
    CullingStats culling_;
    Buffer vertexBuffer_, rectBuffer_, textureBuffer_, externalBuffer_, readback_;
};

} // namespace gt2view
