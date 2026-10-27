#pragma once
#include <functional>

#include "platform/xr/vr_controls.h"
#include <string>
#include <vector>
#include "gt2view/scene_renderer.h"
#include "gt2view/hud_visibility.h"
#include "platform/xr/vr_driving.h"
#include "cockpit_camera.h"
namespace gt2::career { struct CareerSave; struct CareerData; struct NewGameDefaults; }
namespace gt2game {
class GameWindow;
class FrameProfiler;
bool FrameProfilerEnabled();
void AppendFrameProfiler(gt2view::SceneRenderer& renderer, std::vector<gt2view::DrawItem>& items, const FrameProfiler& stats);
void LoadOverlaySettings(const std::string& baseSettingsPath);
int AdaptivePedalStrength();
int OverlayRefreshRate();
int OverlayFoveation();
const gt2::vr::DrivingSettings& OverlayDrivingSettings();
const gt2::vr::ControlBindings& OverlayControlBindings();
const gt2::vr::ControlBindings& OverlayDesktopBindings();
bool OverlayDesktopCustomBindings();
float OverlayIntroLowering();
const CockpitSettings& OverlayCockpitSettings();
CockpitSettings OverlayCockpitSettings(const std::string& car, bool gt4);
void SetOverlayCockpitCar(const std::string& car, bool requiredGt4 = false);
int OverlayRaceView(); // -1 before the player has selected a view; 0 cockpit, 1/2 chase, 3 hidden car.
void RememberRaceView(int view);
bool OverlayGt4ModelSet();
const gt2view::HudVisibility& OverlayHudVisibility();
bool OverlayMetricUnits(bool fallback);
int OverlayVrScale(); // -1 unless the player saved an eye scale
int OverlayRumbleStrength(); // 0..100 percent
void ShowPcOverlay(GameWindow& window, const std::vector<gt2view::DrawItem>& background, size_t sceneCount);
void SetArcadeGarageCheatContext(std::function<std::string()> action);
void SetSimulationCheatContext(gt2::career::CareerSave* save, const gt2::career::CareerData* data, const std::string& path = {}, const gt2::career::NewGameDefaults* defaults = nullptr);
bool TakeCampaignChangeRequest();
std::string SelectGameDisc(const std::string& root, const std::string& preferred, bool vr = true,
                           bool deterministic = false, const std::string& script = {}, const std::string& shot = {}, bool sound = true,
                           bool playStartup = true);
bool TakeGameChangeRequest();
void SetDiscImportAvailable(bool available);
bool TakeDiscImportRequest();
bool ConsoleStartupHandled();
bool PlayStationIntroEnabled();
void PrepareNativeUi(gt2view::SceneRenderer& renderer);
void AppendSkipHint(gt2view::SceneRenderer& renderer, std::vector<gt2view::DrawItem>& items);
}
