#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <unordered_set>
#include <vector>

#include "interp/r3000.h"

namespace gt2 {

// PS1 Geometry Transformation Engine (COP2), written from the psx-spx description: 44-bit MAC overflow
// flags, IR/SZ/SXY saturation, UNR division, register read/write quirks.
// STATUS: not yet compared against an emulator trace - treat as unverified.
class Gte final : public Cop2 {
public:
    uint32_t ReadData(uint32_t reg) override;
    void WriteData(uint32_t reg, uint32_t value) override;
    uint32_t ReadControl(uint32_t reg) override;
    void WriteControl(uint32_t reg, uint32_t value) override;
    void Execute(uint32_t command) override;

    std::map<uint32_t, uint64_t> commandCounts; // by function number (command & 0x3F)

    // Scene capture for the native renderer: every perspective transform (RTPS/RTPT) is recorded as
    // "input vertex under transform N". The inputs are the very vertices stored in the game's model
    // files, so the host can recognise WHAT is being drawn by matching them against parsed assets.
    struct CapturedTransform {
        int16_t rt[3][3];
        int32_t tr[3];
        uint16_t h;
        int32_t ofx, ofy;
        uint32_t firstVertex, vertexCount;
    };
    struct CapturedVertex { int16_t x, y, z; };
    bool captureEnabled = false;
    std::vector<CapturedTransform> capturedTransforms;
    std::vector<CapturedVertex> capturedVertices;
    // Screen positions produced this frame (x | y << 16, 11 bits each): lets the GPU side tell projected
    // 3D polygons from 2D HUD primitives.
    // The game submits a frame's primitives right AFTER the flip that ends the capture interval, so the
    // previous interval's set is what classifies them.
    std::unordered_set<uint32_t> projectedXy, projectedXyPrevious;
    bool IsProjected(uint32_t xyKey) const { return projectedXy.count(xyKey) || projectedXyPrevious.count(xyKey); }
    void ClearCapture() {
        capturedTransforms.clear();
        capturedVertices.clear();
        projectedXyPrevious = std::move(projectedXy);
        projectedXy.clear();
        transformDirty_ = true;
    }

private:
    using Vec3 = std::array<int32_t, 3>;

    int64_t CheckMac(int index, int64_t value); // index 1..3: sets 44-bit overflow flags, returns sign-extended value
    void SetMac(int index, int64_t value);
    void SetMac0(int64_t value);
    void SetIr(int index, int32_t value, bool lm);
    void SetIr0(int32_t value);
    void SetMacAndIr(int index, int64_t value, bool lm) { SetMac(index, value); SetIr(index, mac_[index], lm); }
    void PushSxy(int32_t x, int32_t y);
    void PushSz(int32_t value);
    void PushColor();
    uint32_t Divide(uint16_t h, uint16_t sz3);

    void Rtp(int vectorIndex, bool last);
    void MultiplyMatrix(const int16_t m[3][3], const Vec3& v, const int32_t* translation, bool lm);
    void Mvmva(uint32_t command);
    void NormalColor(int vectorIndex, int mode); // mode 0 NCS, 1 NCCS, 2 NCDS
    void ColorFromIr(int mode);                  // shared tail of NC*/CC/CDP
    void InterpolateToFarColor(int64_t mac1, int64_t mac2, int64_t mac3);

    int16_t v_[3][3] = {};          // V0..V2
    uint8_t rgbc_[4] = {};
    uint16_t otz_ = 0;
    int16_t ir_[4] = {};            // IR0..IR3
    int16_t sxy_[3][2] = {};        // SXY0..SXY2
    uint16_t sz_[4] = {};           // SZ0..SZ3
    uint8_t rgbFifo_[3][4] = {};
    uint32_t res1_ = 0;
    int32_t mac_[4] = {};           // MAC0..MAC3
    int32_t lzcs_ = 0;

    int16_t rt_[3][3] = {}, llm_[3][3] = {}, lcm_[3][3] = {};
    int32_t tr_[3] = {}, bk_[3] = {}, fc_[3] = {};
    int32_t ofx_ = 0, ofy_ = 0;
    uint16_t h_ = 0;
    int16_t dqa_ = 0, zsf3_ = 0, zsf4_ = 0;
    int32_t dqb_ = 0;
    uint32_t flag_ = 0;
    int sf_ = 12;
    bool lm_ = false;
    bool transformDirty_ = true;
};

} // namespace gt2
