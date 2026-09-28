#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

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
constexpr uint32_t kMirrorTexture = 1u << 21; // Normalized UVs into the shared rear-view target.
constexpr uint32_t kReconstructedUi = 1u << 20;
constexpr uint32_t kUiMap = 1u << 19;
constexpr uint32_t kSmoothUi = 1u << 18;
constexpr uint32_t kHdUi = 1u << 17; // Offline enlarged indices; the palette stays in native VRAM.
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

// Which space DrawItem::mvp leaves its vertices in (docs/research/vr_port_plan.md, M2).
// Desktop draws use the item's matrix as-is. The mirror-source tag selects a
// separate mono pass on both desktop and stereo; the other tags select stereo transforms.
//   kScreen: mvp is the whole world -> clip matrix (today's 2D path and every desktop frame)
//   kWorld:  mvp maps the object into the reference space the draw list was built in (world - refEye); the renderer
//            applies the eye's view-projection
//   kSky:    the same, with the eye translation dropped - the backdrop sits at infinity and is identical in both eyes
//   kMirrorSource: full rear-camera clip matrix, drawn only into the shared mirror texture
enum DrawSpace : uint32_t { kSpaceWorld = 0, kSpaceSky = 1, kSpaceScreen = 2, kSpaceMirrorSource = 3 };

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
constexpr uint32_t kOptionSmoothTextures = 1, kOptionAffine = 2, kOptionMipmaps = 8;

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
    bool mipmaps = true;
    bool smoothTextures = false; // bilinear filtering of the decoded PS1 texels (CLUT-aware, inside the polygon's UV
                                 // rectangle, STP class kept); false = the PS1's nearest texel (exact)
    bool affine = false;         // screen-linear (PS1 GPU) texture / colour interpolation; false = perspective-correct
    bool vsync = true;           // FIFO presentation; false = MAILBOX (or IMMEDIATE when the surface has no MAILBOX)
    bool operator==(const RenderOptions&) const = default;
};

// Shared resource addresses and HD UI preparation for every graphics backend.
class SceneRendererState {
public:
    virtual ~SceneRendererState() = default;
    // Native menu/font and profiler occupy reserved ranges above the original scene/HUD buffers.
    static constexpr uint32_t kNativeUiVertexBase = 1 << 20, kNativeUiVertexLimit = 16384;
    static constexpr uint32_t kProfilerVertexBase = kNativeUiVertexBase + kNativeUiVertexLimit;
    static constexpr uint32_t kVrDrivingVertexBase = kProfilerVertexBase + 8192, kVrDrivingVertexLimit = 40960;
    static constexpr uint32_t kCockpitBodyVertexBase = kVrDrivingVertexBase + kVrDrivingVertexLimit;
    static constexpr uint32_t kCockpitBodyVertexStride = 8192, kCockpitBodyVertexLimit = 8 * kCockpitBodyVertexStride;
    static constexpr uint32_t kCockpitVertexBase = kCockpitBodyVertexBase + kCockpitBodyVertexLimit, kCockpitVertexLimit = 32768;
    static constexpr uint32_t kMaxVertices = kCockpitVertexBase + kCockpitVertexLimit;
    static constexpr uint32_t kNativeFontRow = 2048, kNativeFontRows = 144;
    static constexpr uint32_t kMenuReflectionRow = kNativeFontRow + kNativeFontRows;
    static constexpr uint32_t kVramWidth = 1024, kVramRows = kMenuReflectionRow + 512;
    static constexpr uint32_t kVrHandTexelBase = 1u << 22;
    static constexpr uint32_t kMovieTexelBase = kVrHandTexelBase + (1u << 20);
    static constexpr uint32_t kHdMenuTexelBase = kMovieTexelBase + 2048 * 2048;
    static constexpr uint32_t kHdUiTexelBase = kHdMenuTexelBase + 2048 * 2048;
    static constexpr uint32_t kHdUiSlots = 16, kHdUiPageTexels = 1024 * 1024 / 2;
    static constexpr uint32_t kExternalTexels = kHdUiTexelBase + kHdUiSlots * kHdUiPageTexels;

    void ApplyHdUi(std::vector<SceneVertex>& vertices);
    virtual void UploadExternalTexture(uint32_t first, uint32_t count, const uint32_t* rgba) = 0;
protected:
    RenderOptions options_;
    std::vector<uint16_t> vramShadow_;
    struct HdUiSlot { std::string key; uint64_t used=0; bool reconstructed=false; };
    std::vector<HdUiSlot> hdUiSlots_;
    std::unordered_set<std::string> hdUiAssets_, hdFontAssets_;
    std::unordered_map<uint64_t,int> hdUiPages_;
    uint64_t hdUiGeneration_=~uint64_t(0), hdUiFrame_=0;
    bool hdUiEnabled_=false;
};
} // namespace gt2view
