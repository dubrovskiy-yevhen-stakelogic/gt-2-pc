#pragma once
#include "platform/xr/vr_controls.h"
#include <string>
#include <vector>
#include "gt2view/vk_scene_renderer.h"
#include "gt2view/hud_visibility.h"
#include "platform/xr/vr_driving.h"
namespace gt2::career { struct CareerSave; struct CareerData; }
namespace gt2game {
class GameWindow;
class FrameProfiler;
bool FrameProfilerEnabled();
void AppendFrameProfiler(gt2view::VkSceneRenderer& renderer, std::vector<gt2view::DrawItem>& items, const FrameProfiler& stats);
void LoadOverlaySettings(const std::string& baseSettingsPath);
int AdaptivePedalStrength();
int OverlayRefreshRate();
int OverlayFoveation();
const gt2::vr::DrivingSettings& OverlayDrivingSettings();
const gt2::vr::ControlBindings& OverlayControlBindings();
float OverlayIntroLowering();
const gt2view::HudVisibility& OverlayHudVisibility();
bool OverlayMetricUnits(bool fallback);
int OverlayVrScale(); // -1 unless the player saved an eye scale
int OverlayRumbleStrength(); // 0..100 percent
void ShowPcOverlay(GameWindow& window, const std::vector<gt2view::DrawItem>& background, size_t sceneCount);
void SetSimulationCheatContext(gt2::career::CareerSave* save, const gt2::career::CareerData* data, const std::string& path = {});
std::string SelectQuestDisc(const std::string& root, const std::string& preferred);
void PrepareNativeUi(gt2view::VkSceneRenderer& renderer);
void AppendSkipHint(gt2view::VkSceneRenderer& renderer, std::vector<gt2view::DrawItem>& items);
}
