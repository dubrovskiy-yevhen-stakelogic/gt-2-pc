#pragma once
#include "gt2view/cockpit_fit.h"
#include "gt2view/scene_renderer.h"
#include <array>

namespace gt2view {

class ProceduralCockpit {
public:
    explicit ProceduralCockpit(SceneRenderer& renderer);
    void Append(std::vector<DrawItem>& items, const CockpitFit& fit, const float* carMvp,
                float steeringRadians, float speedKph, float rpm, int gear, bool showWheel = true,
                float seatHeight = 0, float seatBack = 0, const std::array<float,3>* steeringHub = nullptr,
                bool showCabin = true);
    void AppendMirror(std::vector<DrawItem>& items, const float* carMvp, float scale = 1.f);
    // Populated only by the owned-data geometry audit.
    std::array<uint32_t,2> SeatRangeForAudit() const { return auditSeatRange_; }
    const auto& DriverSeatForAudit() const { return auditSeat_; }
    const auto& CowlSealForAudit() const { return auditCowl_; }
    const auto& WheelLinersForAudit() const { return auditWheelLiners_; }
    const auto& FasciaForAudit() const { return auditFascia_; }
    const auto& InstrumentFacesForAudit() const { return auditInstrumentFaces_; }
    // The dashboard owns this anchor. Seat/tracking adjustments must not move it.
    const auto& WheelCenter() const { return wheelCenter_; }

private:
    void Build(const CockpitFit& fit);
    void BuildMirror(float scale);
    void UpdateInstruments(float speedKph, float rpm, int gear);

    SceneRenderer& renderer_;
    CockpitFit fitKey_{};
    std::array<float, 4> speedometer_{};
    bool built_ = false;
    uint32_t cabinCount_ = 0, wheelCount_ = 0, instrumentCount_ = 0;
    uint32_t backingCount_ = 0;
    uint32_t mirrorCount_ = 0;
    uint32_t wheelWellFirst_ = 0, wheelWellCount_ = 0;
    float instrumentScale_=1.f;
    float dialX_[2]{}, dialY_ = 0, dialZ_ = 0;
    std::array<float, 3> wheelCenter_{};
    std::array<float, 2> seatOffset_{};
    std::array<float,3> steeringHub_{};
    std::array<float,3> mirrorCenter_{};
    float mirrorScale_ = 0;
    bool interactiveWheel_ = false;
    std::vector<SceneVertex> auditSeat_;
    std::array<uint32_t,2> auditSeatRange_{};
    std::vector<SceneVertex> auditCowl_;
    std::vector<SceneVertex> auditWheelLiners_;
    std::vector<SceneVertex> auditFascia_;
    std::vector<SceneVertex> auditInstrumentFaces_;
    int speedStep_ = -1, rpmStep_ = -1, gear_ = -100;
};

} // namespace gt2view
