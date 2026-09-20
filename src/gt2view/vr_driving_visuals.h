#pragma once
#include "gt2view/vk_scene_renderer.h"
#include "platform/xr/vr_driving.h"
#include <span>
namespace gt2view {
std::span<const uint8_t> VrHandAsset(int index);
class VrDrivingVisuals {
public:
    explicit VrDrivingVisuals(VkSceneRenderer& renderer);
    void Append(std::vector<DrawItem>& items, const gt2::vr::TrackedControllers& tracking,
                const gt2::vr::DrivingController& controller, const gt2::vr::DrivingSettings& settings, const float matrix[16]);
private:
    struct Vertex { float position[4][3], normal[4][3], u, v; };
    struct Mesh { std::vector<Vertex> vertices; std::vector<uint16_t> indices; };
    VkSceneRenderer& renderer_;
    Mesh meshes_[2];
    std::vector<SceneVertex> posed_[2];
    uint32_t textureWidth_ = 0, textureHeight_ = 0;
    std::vector<SceneVertex> vertices_, wheel_;
    int wheelRadius_ = -1;
    int gripStep_[2]{-1,-1}, triggerStep_[2]{-1,-1};
};
}
