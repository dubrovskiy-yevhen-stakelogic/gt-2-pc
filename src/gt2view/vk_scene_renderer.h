#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gt2view/vk_context.h"
#include "gt2view/draw_culling.h"
#include "gt2view/decoded_texture_cache.h"

namespace gt2view {

struct SceneVertex {
    float pos[3];
    float texel[2];
    float color[3];   // polygon colour / 255
    uint32_t page;    // pageX | pageY << 16, in VRAM words
    uint32_t clut;    // clutX | clutY << 16
    uint32_t flags;   // kTextured | kRawTexture | kCarPaint | depth << 8
};

constexpr uint32_t kTextured = 1, kRawTexture = 2, kCarPaint = 4, kOverlay = 8;
// kExternalTexture: the texel coordinates address an RGBA8 image in the external texture store (mod meshes)
// instead of the PS1 VRAM: page = first texel of the image, clut = width | height << 16, repeat wrapping,
// alpha < 0.5 discards, colour = texel x vertex colour.
constexpr uint32_t kExternalTexture = 16;
// Course polygons: kSemiTransparent = the PS1 code's semi-transparency bit (the texels with the STP bit blend, the
// others are opaque - see DrawItem::stpPass); kCullBack = drawn only when front-facing (the original's NCLIP test,
// polygon word0 bit 31: front = counter-clockwise on screen, y down); bits 12-13 = the ordering-table tier as
// "nearness" 0..3 (3 - ((word0 >> 27) & 3), the original's OT offset of 16 entries per step): coplanar decals are
// drawn on top of the surface they lie on by a small relative depth offset (vertex shader).
constexpr uint32_t kSemiTransparent = 1u << 10, kCullBack = 1u << 11, kNearTierShift = 12;
// kIntegerModulate: textured 2D (menus) - the colour modulation in the GPU's integer form, (5-bit texel * 8-bit
// colour) >> 7, so that the frame equals the console's (gt2view/menu_view.h).
constexpr uint32_t kIntegerModulate = 1u << 14;
constexpr uint32_t kHandTexture = 1u << 16; // Dedicated hardware-filtered hand albedo.
constexpr uint32_t kClampTextureRect = 1u << 15; // Sprite atlas edges must not sample the neighbouring image.

// PS1 semi-transparency modes (GPU tpage bits 5-6): 0 = B/2 + F/2, 1 = B + F, 2 = B - F, 3 = B + F/4.
constexpr uint32_t kBlendOpaque = 0xFF;

// Which space DrawItem::mvp leaves its vertices in (docs/research/vr_port_plan.md, M2). Only the stereo path reads
// it: with no stereo views set (every desktop frame) the item's matrix is used exactly as it is, so the window path
// keeps its former arithmetic to the bit.
//   kScreen: mvp is the whole world -> clip matrix (today's 2D path and every desktop frame)
//   kWorld:  mvp maps the object into the reference space the draw list was built in (world - refEye); the renderer
//            applies the eye's view-projection
//   kSky:    the same, with the eye translation dropped - the backdrop sits at infinity and is identical in both eyes
enum DrawSpace : uint32_t { kSpaceWorld = 0, kSpaceSky = 1, kSpaceScreen = 2 };

struct DrawItem {
    uint32_t firstVertex = 0, vertexCount = 0;
    float mvp[16] = {};
    uint32_t space = kSpaceScreen;
    uint32_t paint = 0, brakeLit = 0;
    uint32_t blend = kBlendOpaque; // kBlendOpaque or a PS1 mode; blended items write no depth
    // Semi-transparent textured polygons (PS1: only texels with the STP bit blend): 0 = draw every fragment,
    // 1 = the opaque part (texels without STP; untextured fragments are skipped), 2 = the blended part (textured:
    // texels with STP only). A semi-transparent range is drawn twice: pass 1 with the opaque items, pass 2 blended.
    uint32_t stpPass = 0;
    // Optional scissor rectangle in window fractions (x0, y0, x1, y1; 0..1, y down); x1 <= x0 = the whole window
    // (the PS1 drawing area of a separate draw environment, e.g. the menus' car viewport).
    float scissor[4] = {0, 0, 0, 0};
    // Non-zero: the depth buffer is cleared (to the far value) inside the scissor rectangle before this item - a
    // sub-view drawn over the scene (the rear-view mirror).
    uint32_t clearDepth = 0;
};

struct FrameParams {
    float mvp[16]; // column-major
    uint32_t paint = 0;
    uint32_t brakeLit = 0;
    uint32_t stpPass = 0;
    uint32_t options = 0; // kOption* of the scene items (RenderOptions), 0 for the 2D layers
    uint32_t space = kSpaceScreen; // DrawItem::space (the stereo shader; the desktop shader does not declare it)
    uint32_t eye = 0;              // the two-pass stereo path's array layer
    uint32_t padding[2] = {};
    float hudClip[4] = {};
};
static_assert(offsetof(FrameParams, hudClip) == 96 && sizeof(FrameParams) == 112);
constexpr uint32_t kOptionSmoothTextures = 1, kOptionAffine = 2;

// The two eyes of one stereo frame (docs/research/vr_port_plan.md, M2; src/platform/xr/vr_rig.h builds them). The
// renderer uploads them into a uniform buffer the stereo vertex shader indexes by gl_ViewIndex (multiview) or by the
// eye push constant (two passes).
struct StereoViews {
    float worldVP[2][16] = {}; // reference space (world - refEye) -> clip, per eye
    float hudVP[2][16] = {};
    float skyVP[2][16] = {};   // the same with the eye translation removed
};

// Modern presentation options (ours; docs/formats/modern_graphics.md). They change only how the frame is drawn, never
// what is drawn: the default-constructed value is today's output (the `--vanilla` preset) and takes exactly the
// former code path. They apply to the "scene" items of a frame (Draw's sceneItems: the 3D view of a race, with the
// rear-view mirror); the 2D layers after them (HUD, panels) and every screen that passes no scene count (menus, title)
// keep the native window resolution and the PS1's nearest texels.
struct RenderOptions {
    uint32_t sceneWidth = 0, sceneHeight = 0;
    float sceneScale = 1.0f;     // internal resolution of the scene, times the window (0.5 .. 2)
    uint32_t msaa = 1;           // multisampling of the scene: 1 (off), 2, 4, 8 (clamped to the GPU's limit)
    bool smoothTextures = false; // bilinear filtering of the decoded PS1 texels (CLUT-aware, inside the polygon's UV
                                 // rectangle, STP class kept); false = the PS1's nearest texel (exact)
    bool affine = false;         // screen-linear (PS1 GPU) texture / colour interpolation; false = perspective-correct
    bool vsync = true;           // FIFO presentation; false = MAILBOX (or IMMEDIATE when the surface has no MAILBOX)
    bool operator==(const RenderOptions&) const = default;
};

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
class VkSceneRenderer {
public:
    static constexpr VkFormat kSceneDepthFormat = VK_FORMAT_D32_SFLOAT;
    // Native menu/font and profiler occupy reserved ranges above the original scene/HUD buffers.
    static constexpr uint32_t kNativeUiVertexBase = 1 << 20, kNativeUiVertexLimit = 16384;
    static constexpr uint32_t kProfilerVertexBase = kNativeUiVertexBase + kNativeUiVertexLimit;
    static constexpr uint32_t kVrDrivingVertexBase = kProfilerVertexBase + 8192, kVrDrivingVertexLimit = 40960;
    static constexpr uint32_t kMaxVertices = kVrDrivingVertexBase + kVrDrivingVertexLimit;
    static constexpr uint32_t kNativeFontRow = 2048, kNativeFontRows = 144;
    static constexpr uint32_t kMenuReflectionRow = kNativeFontRow + kNativeFontRows;
    static constexpr uint32_t kVramWidth = 1024, kVramRows = kMenuReflectionRow + 512;
    static constexpr uint32_t kVrHandTexelBase = 1u << 22;
    static constexpr uint32_t kMovieTexelBase = kVrHandTexelBase - 640 * 512;
    static constexpr uint32_t kExternalTexels = kVrHandTexelBase + (1u << 20);          // RGBA8 texels of the external texture store (16 MiB)

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
    std::vector<uint16_t> vramShadow_;
    std::vector<bool> vramKnown_;
    void WriteBuffer(Buffer& buffer, size_t offset, const void* data, size_t bytes);
    void FlushFrameUploads(); // caller has waited for the frame fence
    void CreateSwapchain();
    void CreateOffscreen(VkExtent2D extent, VkFormat format); // the XR path's single target, instead of a swapchain
    void DestroySwapchain();
    void CreatePipeline();
    // `stereo` = the stereo vertex shader (scene_stereo.vert); `viewMask` != 0 = its multiview variant.
    void CreatePipelineSet(VkSampleCountFlagBits samples, VkPipeline out[5], bool stereo = false, uint32_t viewMask = 0, bool cachedOnly = false, bool cachedHud = false);
    void DestroyPipelineSet(VkPipeline set[5]);
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;   // the whole image (2D, or 2D_ARRAY when layers > 1)
        VkImageView layer[2] = {};           // one view per array layer (the two-pass stereo path)
        uint32_t layers = 1;
        bool ownsImage = true;
    };
    Image CreateImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits samples, VkImageAspectFlags aspect,
                      uint32_t layers = 1);
    void DestroyImage(Image& image);
    void DestroyStereoTarget();
    void PrepareDecodedTextures(const std::vector<DrawItem>& items);
    void UploadDecodedTextures();
    // Records `items` once per eye into the stereo target (`layer` < 0 = one multiview pass).
    void RecordStereoPass(const std::vector<DrawItem>& items, size_t sceneItems, int layer);
    void CreateSceneTargets();  // the offscreen scene target for options_ at the window's extent
    void DestroySceneTargets();
    VkSampleCountFlagBits ClampSamples(uint32_t requested) const;
    // Records items[first, last) into the current dynamic rendering (target of `extent`), with `pipelines` (sample count
    // of the target); items below `sceneItems` get the scene's option bits.
    void RecordItems(const std::vector<DrawItem>& items, size_t first, size_t last, VkExtent2D extent, const VkPipeline pipelines[5], size_t sceneItems);
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
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipelines_[5] = {}; // [0..3] PS1 blend modes, [4] opaque
    VkPipeline msaaPipelines_[5] = {}; // the same for the scene target's sample count (msaaSamples_ > 1)
    VkSampleCountFlagBits msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;

    RenderOptions options_;
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool blitLinear_ = false;       // the colour format supports linear-filtered blits
    // The scene target (only when the options need it): colour at the scene scale (resolve target of the MSAA colour),
    // the MSAA colour, the scene's depth.
    VkExtent2D sceneExtent_{};
    Image sceneColor_, sceneMsaa_, sceneDepth_;
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
    VkPipeline cachedPipelines_[5] = {};
    VkPipeline cachedHudPipelines_[5] = {}; // Cached 2D materials keep full-rate shading and HUD clipping.
    std::vector<uint8_t> cachedDraws_;
    VkPipeline stereoPipelines_[5] = {}; // the stereo shader at the stereo target's sample count
    bool recordStereo_ = false;
    uint32_t recordEye_ = 0;             // the eye index RecordItems pushes (the two-pass path)
    uint32_t recordLayers_ = 1;          // array layers of the target being recorded (vkCmdClearAttachments)
    StereoViews stereoViews_;
#ifdef __ANDROID__
    bool decodedEnabled_ = true;
#else
    bool decodedEnabled_ = false; // Desktop GPUs retain their faster direct VRAM path.
#endif
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
