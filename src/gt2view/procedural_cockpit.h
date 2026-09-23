#pragma once
#include "gt2view/cockpit_fit.h"
#include "gt2view/vk_scene_renderer.h"
#include <array>

namespace gt2view {

class ProceduralCockpit {
public:
    explicit ProceduralCockpit(VkSceneRenderer& renderer);
    void Append(std::vector<DrawItem>& items, const CockpitFit& fit, const float* carMvp,
                float steeringRadians, float speedKph, float rpm, int gear, bool showWheel = true,
                float seatHeight = 0, float seatBack = 0, const std::array<float,3>* steeringHub = nullptr);
    void AppendMirror(std::vector<DrawItem>& items, const float* carMvp, float scale = 1.f);

private:
    void Build(const CockpitFit& fit);
    void BuildMirror(float scale);
    void UpdateInstruments(float speedKph, float rpm, int gear);

    VkSceneRenderer& renderer_;
    std::array<float, 203> fitKey_{};
    bool built_ = false;
    uint32_t cabinCount_ = 0, wheelCount_ = 0, instrumentCount_ = 0;
    uint32_t mirrorCount_ = 0;
    float dialX_[2]{}, dialY_ = 0, dialZ_ = 0;
    std::array<float, 3> wheelCenter_{};
    std::array<float, 2> seatOffset_{};
    std::array<float,3> steeringHub_{};
    std::array<float,3> mirrorCenter_{};
    float mirrorScale_ = 0;
    bool interactiveWheel_ = false;
    int speedStep_ = -1, rpmStep_ = -1, gear_ = -100;
};

} // namespace gt2view
