#pragma once
#include <cstdint>
#include <algorithm>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace gt2 {

// Software PS1 GPU: executes the game's own GP0/GP1 command stream into a
// 1024x512 15-bit VRAM image. Goal: a faithful enough picture to see what the guest is doing - not
// pixel-exact parity with any other rasteriser. Not implemented: dithering. 24-bit display: DisplayRgba only.
class Gpu {
public:
    static constexpr int kVramWidth = 1024, kVramHeight = 512;

    Gpu() : vram_(size_t(kVramWidth) * kVramHeight, 0), touched_(size_t(kVramWidth) * kVramHeight, 0) {}

    void WriteGp0(uint32_t word);
    void WriteGp1(uint32_t word);
    uint32_t ReadStatus(bool oddField) const;
    uint32_t ReadData();

    const std::vector<uint16_t>& Vram() const { return vram_; }
    int DisplayWidth() const;
    int DisplayHeight() const { return (displayMode_ & 0x24) == 0x24 ? 480 : 240; }
    bool DisplayEnabled() const { return displayEnabled_; }
    bool Display24() const { return (displayMode_ & 0x10) != 0; } // GP1(08h) bit 4: 24-bit colour

    // RGBA8 copy of the visible display area.
    std::vector<uint8_t> DisplayRgba(int& width, int& height) const;

    // HUD-only mode (second GPU instance used by the native renderer): polygons whose every vertex is a
    // GTE projection of this frame are 3D scene geometry and are skipped; what remains is the 2D layer.
    // `touched` marks pixels drawn since the last flip so the layer can be composited with transparency.
    std::function<bool(uint32_t xyKey)> isProjected; // set = HUD-only mode
    std::function<void()> onDisplayFlip;             // GP1(05h) changed the display start
    uint32_t backgroundColor = 0;                    // HUD-only mode: colour (BGR) of the game's sky band
    // HUD-only mode, draw lists: PS1 primitives arrive far -> near -> HUD, so inside one list everything up
    // to the last GTE-projected polygon is the 3D layer (sky, ground, smoke included) and is not drawn.
    void BeginList() { inList_ = isProjected != nullptr; }
    void EndList();
    // RGBA8 of the display area; alpha 255 only where something was drawn since the last ClearTouched().
    std::vector<uint8_t> DisplayRgbaTouched(int& width, int& height) const;
    void ClearTouched() { std::fill(touched_.begin(), touched_.end(), uint8_t(0)); }

    uint64_t primitivesDrawn = 0;
    bool skip3dRaster = false; // profiling / native-scene mode: polygons are parsed but not rasterised

private:
    struct Vertex { int x, y, r, g, b, u, v; };

    void ExecuteCommand();
    size_t CommandLength(uint32_t first) const;
    void DrawTriangle(const Vertex& a, const Vertex& b, const Vertex& c, bool textured, bool gouraud, bool semi, bool raw);
    void DrawRectangle(int x, int y, int w, int h, int u, int v, uint32_t color, bool textured, bool semi, bool raw);
    void DrawLine(Vertex a, Vertex b, bool semi);
    void PlotPixel(int x, int y, int r, int g, int b, bool semi, bool setMask);
    uint16_t Texel(int u, int v) const;
    void FillRect(uint32_t color, int x, int y, int w, int h);

    std::vector<uint16_t> vram_;
    std::vector<uint8_t> touched_;
    struct Deferred { std::vector<uint32_t> words; bool projectedPolygon; };
    std::vector<Deferred> deferred_;
    bool inList_ = false, skipDraw_ = false, backgroundSeen_ = false;
    bool IsProjectedPolygon(const std::vector<uint32_t>& words) const;
    std::vector<uint32_t> command_;
    size_t expected_ = 0;
    bool polyline_ = false;

    // CPU -> VRAM / VRAM -> CPU transfers.
    struct Transfer { int x = 0, y = 0, w = 0, h = 0; int64_t remaining = 0, index = 0; } upload_;
    std::deque<uint32_t> readback_;

    // Draw state (GP0 E1-E6).
    uint16_t drawMode_ = 0;
    uint16_t clut_ = 0;
    int texWindowMaskX_ = 0, texWindowMaskY_ = 0, texWindowOffX_ = 0, texWindowOffY_ = 0;
    int areaLeft_ = 0, areaTop_ = 0, areaRight_ = 1023, areaBottom_ = 511;
    int offsetX_ = 0, offsetY_ = 0;
    bool forceMask_ = false, checkMask_ = false;

    // Display state (GP1).
    bool displayEnabled_ = false;
    int displayX_ = 0, displayY_ = 0;
    uint32_t displayMode_ = 0, dmaDirection_ = 0;
};

} // namespace gt2
