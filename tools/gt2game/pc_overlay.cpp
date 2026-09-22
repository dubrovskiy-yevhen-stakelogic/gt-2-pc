#include "pc_overlay.h"
#include "gt2formats/hd_media.h"
#include "game/shell/shared_vr_settings.h"
#include "movie_player.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include "game_window.h"
#include "graphics_options.h"
#include "game/pc_features.h"
#include "game/audio/pause.h"
#include "game/career/cheats.h"
#include <cstring>
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace gt2game {
namespace {
std::string settingsPath;
gt2::shell::VrSettings baseVrSettings;
int adaptive = 60, rumble = 50, vrScale = -1, vrRefresh = 72;
bool profiler = true;
bool playStationIntro = true;
bool consoleStartupHandled = false;
bool gameChangeAvailable = false, gameChangeRequested = false;
int foveation = 2;
int metricUnits = -1;
bool baseMetricUnits = true;
gt2view::HudVisibility hudVisibility;
gt2::vr::DrivingSettings drivingSettings;
gt2::vr::ControlBindings controlBindings;
gt2::vr::ControlBindings desktopBindings = gt2::vr::DesktopBindings();
bool desktopCustom = false;
int introLowerCm = 200;
struct HudSetting { const char* label; const char* key; bool gt2view::HudVisibility::*member; };
constexpr HudSetting hudSettings[] = {
    {"Course map", "vr_hud_map", &gt2view::HudVisibility::map},
    {"Lap counter and times", "vr_hud_lap", &gt2view::HudVisibility::lap},
    {"Records and best lap", "vr_hud_records", &gt2view::HudVisibility::records},
    {"Speed / RPM / gear", "vr_hud_gauges", &gt2view::HudVisibility::gauges},
    {"Turbo gauge", "vr_hud_turbo", &gt2view::HudVisibility::turbo},
    {"Tyre indicators", "vr_hud_tyres", &gt2view::HudVisibility::tyres},
    {"Rear-view mirror", "vr_hud_mirror", &gt2view::HudVisibility::mirror},
    {"Start countdown", "vr_hud_countdown", &gt2view::HudVisibility::countdown},
    {"Driving warnings", "vr_hud_warnings", &gt2view::HudVisibility::warnings},
    {"Split times and messages", "vr_hud_messages", &gt2view::HudVisibility::messages},
    {"Replay caption", "vr_hud_replay", &gt2view::HudVisibility::replay},
    {"Movie skip hint", "vr_hud_movie_hint", &gt2view::HudVisibility::movieHint}
};
constexpr uint32_t vertexBase = gt2view::VkSceneRenderer::kNativeUiVertexBase, fontRow = gt2view::VkSceneRenderer::kNativeFontRow;

// A system-font atlas is generated at runtime: the source kit carries no game font or artwork.
void UploadFont(gt2view::VkSceneRenderer& renderer) {
#ifdef _WIN32
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 256; info.bmiHeader.biHeight = -144;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HFONT font = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, FIXED_PITCH, L"Consolas");
    if (!dc || !bitmap || !font || !pixels) {
        if (font) DeleteObject(font); if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc);
        throw std::runtime_error("cannot create overlay font");
    }
    auto oldBitmap = SelectObject(dc, bitmap), oldFont = SelectObject(dc, font);
    PatBlt(dc, 0, 0, 256, 144, BLACKNESS);
    SetTextColor(dc, RGB(255, 255, 255)); SetBkColor(dc, RGB(0, 0, 0));
    for (int c = 32; c < 128; ++c) { const char ch = char(c); TextOutA(dc, ((c - 32) % 16) * 16, ((c - 32) / 16) * 24, &ch, 1); }
    GdiFlush();
    std::vector<uint16_t> atlas(1024 * 144);
    const auto* rgba = static_cast<const uint32_t*>(pixels);
    for (int y = 0; y < 144; ++y) for (int x = 0; x < 256; ++x) {
        const uint16_t v = uint16_t((rgba[y * 256 + x] & 255) >> 3);
        atlas[size_t(y) * 1024 + size_t(x)] = uint16_t(v | (v << 5) | (v << 10));
    }
    SelectObject(dc, oldFont); SelectObject(dc, oldBitmap); DeleteObject(font); DeleteObject(bitmap); DeleteDC(dc);
    renderer.UploadVram(fontRow, 144, atlas.data());
#else
    // Compact built-in 5x7 lettering; no platform font files are required on the headset.
    static constexpr uint8_t glyphs[][5] = {
        {0x3e,0x51,0x49,0x45,0x3e},{0,0x42,0x7f,0x40,0},{0x42,0x61,0x51,0x49,0x46},
        {0x21,0x41,0x45,0x4b,0x31},{0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
        {0x3c,0x4a,0x49,0x49,0x30},{1,0x71,9,5,3},{0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1e},
        {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
        {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,9,9,9,1},
        {0x3e,0x41,0x49,0x49,0x7a},{0x7f,8,8,8,0x7f},{0,0x41,0x7f,0x41,0},
        {0x20,0x40,0x41,0x3f,1},{0x7f,8,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
        {0x7f,2,0x0c,2,0x7f},{0x7f,4,8,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
        {0x7f,9,9,9,6},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,9,0x19,0x29,0x46},
        {0x46,0x49,0x49,0x49,0x31},{1,1,0x7f,1,1},{0x3f,0x40,0x40,0x40,0x3f},
        {0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,8,0x14,0x63},
        {7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43}
    };
    std::vector<uint16_t> atlas(1024 * 144);
    for (int c = 32; c < 128; ++c) {
        const int upper = c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
        const int index = upper >= '0' && upper <= '9' ? upper - '0' : upper >= 'A' && upper <= 'Z' ? upper - 'A' + 10 : -1;
        for (int x = 0; x < 5; ++x) for (int y = 0; y < 7; ++y) {
            bool pixel = index >= 0 && ((glyphs[index][x] >> y) & 1);
            if (c == '-') pixel = y == 3;
            if (c == '/') pixel = x + y == 5;
            if (c == ':') pixel = x == 2 && (y == 2 || y == 5);
            if (c == '.') pixel = x == 2 && y == 6;
            if (c == '%') pixel = x + y == 5 || (x == 0 && y < 2) || (x == 4 && y > 4);
            if (c == '(') pixel = x == (y == 0 || y == 6 ? 3 : 2);
            if (c == ')') pixel = x == (y == 0 || y == 6 ? 1 : 2);
            if (c == '+') pixel = y == 3 || (x == 2 && y > 0 && y < 6);
            if (!pixel) continue;
            for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx)
                atlas[size_t(((c - 32) / 16) * 24 + y * 2 + 4 + dy) * 1024 +
                      size_t(((c - 32) % 16) * 16 + x * 2 + 1 + dx)] = 0x7fff;
        }
    }
    renderer.UploadVram(fontRow, 144, atlas.data());
#endif
}

template<size_t N> int Cycle(int value, const int (&values)[N], int direction) {
    const auto it = std::find(std::begin(values), std::end(values), value);
    const int index = it == std::end(values) ? 0 : int(it - std::begin(values));
    return values[(index + direction + int(N)) % int(N)];
}

void Save() {
    const auto dir = std::filesystem::path(settingsPath).parent_path();
    if (!dir.empty()) std::filesystem::create_directories(dir);
    const std::string temp = settingsPath + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << "# Live overlay preferences; separate from earned progress.\n" << CurrentGraphics().Serialize();
        out << "hd_assets=" << int(gt2::hd::Enabled()) << '\n';
        out << "vr_ps1_intro=" << int(playStationIntro) << '\n';
        out << "adaptive=" << adaptive << "\nrumble=" << rumble << "\nunlock_courses=" << int(gt2::pc::unlockCourses)
            << "\nunlock_cars=" << int(gt2::pc::unlockCars) << "\n";
        if (metricUnits >= 0) out << "units=" << (metricUnits ? "kmh" : "mph") << '\n';
        if (vrScale >= 0) out << "vr_render_scale=" << vrScale << "\n";
        out << "vr_driving_mode=" << drivingSettings.mode << "\nvr_motion_hand=" << drivingSettings.motionHand
            << "\nvr_wheel_height=" << drivingSettings.wheelHeightCm << "\nvr_wheel_distance=" << drivingSettings.wheelDistanceCm
            << "\nvr_wheel_radius=" << drivingSettings.wheelRadiusCm << "\nvr_intro_lower_cm=" << introLowerCm << '\n';
        out << "pc_custom_bindings=" << int(desktopCustom) << "\npc_brake_reverse=" << int(desktopBindings.brakeReverse)
            << "\npc_steering_stick=" << desktopBindings.steeringStick << '\n';
        for (size_t i=0;i<desktopBindings.source.size();++i) out << "pc_binding_" << i << '=' << desktopBindings.source[i] << '\n';
        out << "vr_brake_reverse=" << int(controlBindings.brakeReverse) << "\nvr_steering_stick=" << controlBindings.steeringStick << '\n';
        for (size_t i=0;i<controlBindings.source.size();++i) out << "vr_binding_" << i << '=' << controlBindings.source[i] << '\n';
        for (const auto& setting : hudSettings) out << setting.key << '=' << int(hudVisibility.*setting.member) << '\n';
        out << "profiler=" << int(profiler) << "\n";
        out << "vr_foveation=" << foveation << "\n";
        out << "vr_refresh=" << vrRefresh << "\nunlock_sim_events=" << int(gt2::pc::unlockSimulationEvents) << "\n";
        out.flush();
        if (!out) throw std::runtime_error("cannot write overlay settings");
    }
#ifdef _WIN32
    if (!MoveFileExW(std::filesystem::path(temp).c_str(), std::filesystem::path(settingsPath).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("cannot replace overlay settings");
#else
    std::filesystem::rename(temp, settingsPath);
#endif
    if(!gt2::shell::sharedVrSettingsPath.empty())
        gt2::shell::WritePreferences(gt2::shell::sharedVrSettingsPath,
            baseVrSettings.Serialize()+gt2::shell::VrPreferences(gt2::shell::ReadPreferences(settingsPath)));
}
}

void LoadOverlaySettings(const std::string& basePath) {
    const auto wheelDirectory = gt2::shell::sharedVrSettingsPath.empty() ? std::filesystem::path(basePath).parent_path()
        : std::filesystem::path(gt2::shell::sharedVrSettingsPath).parent_path();
    gt2::input::wheel::LoadSettings((wheelDirectory / "wheel-settings.txt").string());
    adaptive = 60; rumble = 50; vrScale = -1; vrRefresh = 72; profiler = true; foveation = 2;
    hudVisibility = {}; drivingSettings = {}; controlBindings = {}; introLowerCm = 200;
    desktopBindings = gt2::vr::DesktopBindings(); desktopCustom = false;
    metricUnits = -1;
    playStationIntro = true;
    const auto base=gt2::shell::PcSettings::Load(basePath);
    baseMetricUnits = base.metric; baseVrSettings=base.vr;
    gt2::hd::SetEnabled(true);
    gt2::pc::SetUnlocks(false, false);
    gt2::pc::unlockSimulationEvents = false;
    settingsPath = basePath + ".overlay";
    std::istringstream in(gt2::shell::ReadPreferences(settingsPath)+"\n"+
        gt2::shell::VrPreferences(gt2::shell::ReadPreferences(gt2::shell::sharedVrSettingsPath)));
    auto graphics = CurrentGraphics();
    bool courses = false, cars = false;
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || line.empty() || line[0] == '#') continue;
        const auto key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "hd_assets") gt2::hd::SetEnabled(value != "0");
        else if (key == "vr_ps1_intro") playStationIntro = value != "0";
        else if (key == "units" && (value == "kmh" || value == "mph")) metricUnits = value == "kmh";
        else if (key == "pc_custom_bindings") desktopCustom = value == "1";
        else if (key == "pc_brake_reverse") desktopBindings.brakeReverse = value == "1";
        else if (key == "pc_steering_stick") desktopBindings.steeringStick = std::clamp(std::atoi(value.c_str()),0,1);
        else if (key.size()==12 && key.starts_with("pc_binding_") && key[11]>='0' && key[11]<='7') desktopBindings.source[size_t(key[11]-'0')] = std::clamp(std::atoi(value.c_str()),0,12);
        else if (key == "vr_brake_reverse") controlBindings.brakeReverse = value == "1";
        else if (key == "vr_steering_stick") controlBindings.steeringStick = std::clamp(std::atoi(value.c_str()),0,1);
        else if (key.size()==12 && key.substr(0,11)=="vr_binding_" && key[11]>='0' && key[11]<='7') controlBindings.source[size_t(key[11]-'0')] = std::clamp(std::atoi(value.c_str()),0,8);
        else if (key == "vr_driving_mode") drivingSettings.mode = std::clamp(std::atoi(value.c_str()),0,2);
        else if (key == "vr_motion_hand") drivingSettings.motionHand = std::clamp(std::atoi(value.c_str()),0,1);
        else if (key == "vr_wheel_height") drivingSettings.wheelHeightCm = std::clamp(std::atoi(value.c_str()),-60,0);
        else if (key == "vr_wheel_distance") drivingSettings.wheelDistanceCm = std::clamp(std::atoi(value.c_str()),20,75);
        else if (key == "vr_wheel_radius") drivingSettings.wheelRadiusCm = std::clamp(std::atoi(value.c_str()),8,40);
        else if (key == "vr_intro_lower_cm") introLowerCm = std::clamp(std::atoi(value.c_str()),0,2000);
        else if (key == "adaptive") adaptive = std::clamp(std::atoi(value.c_str()), 0, 100);
        else if (key == "rumble") { const int v = std::atoi(value.c_str()); rumble = v < 0 ? 50 : std::clamp(v, 0, 100); }
        else if (key == "vr_foveation") foveation = std::clamp(std::atoi(value.c_str()), 0, 3);
        else if (key == "profiler") profiler = value == "1";
        else if (key == "vr_refresh") vrRefresh = std::clamp(std::atoi(value.c_str()), 72, 120);
        else if (key == "vr_render_scale") vrScale = std::clamp(std::atoi(value.c_str()), 50, 200);
        else if (key == "unlock_sim_events") gt2::pc::unlockSimulationEvents = value == "1";
        else if (key == "unlock_courses") courses = value == "1";
        else if (key == "unlock_cars") cars = value == "1";
        else {
            bool hudKey = false;
            for (const auto& setting : hudSettings) if (key == setting.key) {
                hudVisibility.*setting.member = value == "1"; hudKey = true; break;
            }
            if (!hudKey) graphics.Parse(key, value);
        }
    }
    SetGraphicsFromOverlay(graphics);
    gt2::pc::SetUnlocks(courses, cars);
}
bool OverlayDesktopCustomBindings() { return desktopCustom; }
const gt2::vr::ControlBindings& OverlayDesktopBindings() { return desktopBindings; }
const gt2::vr::ControlBindings& OverlayControlBindings() { return controlBindings; }
const gt2::vr::DrivingSettings& OverlayDrivingSettings() { return drivingSettings; }
float OverlayIntroLowering() { return introLowerCm*.01f; }
const gt2view::HudVisibility& OverlayHudVisibility() { return hudVisibility; }
bool OverlayMetricUnits(bool fallback) { return metricUnits < 0 ? fallback : metricUnits != 0; }
int OverlayVrScale() { return vrScale; }
int OverlayFoveation() { return foveation; }
int OverlayRefreshRate() { return vrRefresh; }
int AdaptivePedalStrength() { return adaptive; }
int OverlayRumbleStrength() { return rumble; }

namespace {
struct NativeCanvas {
    std::vector<gt2view::SceneVertex> vertices;
    void Quad(float x, float y, float w, float h, uint32_t color, int glyph = -1) {
        const int cx[] = {0,1,1,0,1,0}, cy[] = {0,0,1,0,1,1};
        for (int k = 0; k < 6; ++k) {
            gt2view::SceneVertex v{};
            v.pos[0] = (x + float(cx[k]) * w) / 400 - 1;
            v.pos[1] = (y + float(cy[k]) * h) / 300 - 1;
            v.pos[2] = 1;
            v.color[0] = float((color >> 16) & 255) / 255;
            v.color[1] = float((color >> 8) & 255) / 255;
            v.color[2] = float(color & 255) / 255;
            if (glyph >= 0) {
                v.flags = gt2view::kTextured | (2u << 8); v.page = fontRow << 16;
                v.texel[0] = float(glyph % 16 * 16 + cx[k] * 16);
                v.texel[1] = float(glyph / 16 * 24 + cy[k] * 24);
            }
            vertices.push_back(v);
        }
    }
    void Text(float x, float y, const std::string& value, uint32_t color = 0xe2e8f0, float scale = 1) {
        for (unsigned char c : value) {
            if (c >= 32 && c < 128) Quad(x, y, 16 * scale, 24 * scale, color, int(c) - 32);
            x += 12 * scale;
        }
    }
    void Append(gt2view::VkSceneRenderer& renderer, std::vector<gt2view::DrawItem>& items, uint32_t base = vertexBase) {
        const size_t limit = base == vertexBase ? gt2view::VkSceneRenderer::kNativeUiVertexLimit : 8192;
        if (vertices.size() > limit) throw std::runtime_error("Native overlay vertex range exceeded");
        renderer.SetVertices(base, vertices);
        gt2view::DrawItem layer; layer.firstVertex = base; layer.vertexCount = uint32_t(vertices.size());
        layer.mvp[0] = layer.mvp[5] = layer.mvp[10] = layer.mvp[15] = 1;
        items.push_back(layer);
    }
};
void Panel(GameWindow& window, const std::string& title, const std::vector<std::string>& rows, int selected,
           const std::string& status, const std::string& footer) {
    NativeCanvas canvas;
    canvas.Quad(28, 24, 744, 552, 0x101c2a); canvas.Quad(28, 24, 744, 5, 0x42b6f5);
    canvas.Text(54, 48, title, 0xffffff);
    for (size_t i = 0; i < rows.size(); ++i) {
        const float y = 112 + float(i) * (rows.size()>9 ? 35.f : 40.f);
        if (int(i) == selected) canvas.Quad(44, y - 5, 711, 34, 0x234b68);
        canvas.Text(56, y, rows[i].substr(0, 57), int(i) == selected ? 0x8ed8f8 : 0xe2e8f0);
    }
    canvas.Text(54, 479, status.substr(0, 57), 0xa8b7c8);
    canvas.Text(54, 520, footer, 0xffffff);
    std::vector<gt2view::DrawItem> items; canvas.Append(window.Renderer(), items);
    window.EndFrame(items);
}
gt2::career::CareerSave* cheatSave = nullptr;
const gt2::career::CareerData* cheatData = nullptr;
std::string cheatPath;
std::vector<size_t> catalogue;
const gt2::career::CareerData* catalogueData = nullptr;

std::string ApplySimulationCheat(int action, size_t carIndex = 0) {
    using namespace gt2::career;
    if (!cheatSave || !cheatData) return "Enter GT Mode first. Cheats are disabled in races.";
    if (cheatPath.empty()) return "No career save path. No changes made.";
    try {
        auto candidate = *cheatSave;
        ApplyCheat(candidate, cheatData, action == 0 ? Cheat::GoldLicences : action == 1 ? Cheat::MaxCredits : Cheat::AddCar, carIndex);
        SaveCheat(cheatPath, *cheatSave, candidate);
        *cheatSave = std::move(candidate);
        return action == 0 ? "All licences: GOLD. Saved." : action == 1 ? "99,999,999 credits. Saved." : "Car added to garage. Saved.";
    } catch (const std::exception& e) { return std::string("Cheat failed: ") + e.what(); }
}

#ifdef _WIN32
void ShowWheelAdvanced(GameWindow& window) {
    using namespace gt2::input::wheel;
    auto& rig = window.Input().Wheel();
    auto& config = Config();
    int page = 0, selected = 0, axis = 0, buttonPage = 0, devicePage = 0;
    int capture = -1;
    std::vector<DeviceState> baseline;
    std::string message;
    std::string saveError;
    gt2::vr::MenuTriggers wheelTriggers;
    wheelTriggers.Begin(window.Pad().pressureL2 / 255.f, window.Pad().pressureR2 / 255.f);
    rig.Stop();
    while (!window.Closed() && window.BeginFrame()) {
        rig.Stop();
        const auto& devices = rig.Devices();
        auto find = [&](const std::string& id) -> const DeviceState* {
            for (const auto& d : devices) if (d.id == id && d.online) return &d;
            return nullptr;
        };
        auto save = [&] {
            try { config.automatic = false; config.useClutch = config.axes[Clutch].Valid(false); SaveSettings(); saveError.clear(); message = "Saved. " + rig.Status(); }
            catch (const std::exception& e) { saveError = std::string("Not saved: ") + e.what(); }
        };
        if (capture >= 0) {
            if (window.KeyPressed(gt2::keys::kEscape)) { capture = -1; message = "Assignment cancelled"; }
            else {
                bool learned = false;
                for (const auto& d : devices) if (d.online) {
                    const auto old = std::find_if(baseline.begin(), baseline.end(), [&](const auto& b) { return b.id == d.id && b.online; });
                    if (old == baseline.end()) continue;
                    if (capture == ButtonCount) {
                        int best = -1, distance = 8000;
                        for (int a = 0; a < kAxes; ++a) if (d.available[a] && std::abs(d.axes[a] - old->axes[a]) > distance) {
                            best = a; distance = std::abs(d.axes[a] - old->axes[a]);
                        }
                        if (best >= 0) {
                            auto& a = config.axes[axis]; a = {}; a.device = d.id; a.axis = best; a.rest = old->axes[best];
                            a.end = d.axes[best] < a.rest ? -32768 : 32767;
                            a.right = a.end < a.rest ? 32767 : -32768;
                            learned = true;
                        }
                    } else {
                        for (int b = 0; b < kButtons; ++b) if (d.buttons[b] && !old->buttons[b]) {
                            for (auto& binding : config.buttons) if (binding.device == d.id && binding.button == b) binding = {};
                            config.buttons[capture] = {d.id, b}; learned = true; break;
                        }
                    }
                    if (learned) break;
                }
                if (learned) { capture = -1; save(); }
                else if (capture != ButtonCount) baseline = devices;
            }
            Panel(window, "GT2 / ASSIGN CONTROL", {capture == ButtonCount ? "Move ONLY the selected axis" : "Press the desired device button",
                "For steering: start centred, then turn LEFT", "For pedals: start released, then press fully", "Escape cancels the assignment"}, -1,
                capture < 0 ? "Assigned. Calibrate endpoints on the next page." : "Keep other controls still. Waiting for input...", "Keyboard Escape: cancel");
            continue;
        }
        std::string title = "GT2 / RACING WHEEL";
        std::vector<std::string> rows;
        if (page == 0) {
            const char* modes[] = {"Race default", "Paddles", "H-pattern"};
            rows = {std::string("Wheel input: ") + (config.enabled ? "ON" : "OFF"), "Axes and calibration...", "Buttons and shifter...",
                std::string("MT controls: ") + modes[config.gearbox], std::string("Force feedback: ") + (config.feedback ? "ON" : "OFF"),
                "FFB strength: " + std::to_string(config.gain) + "%", "FFB damping: " + std::to_string(config.damping) + "%",
                std::string("Invert aligning force: ") + (config.invertForce ? "YES" : "NO"), "Connected devices...", "Back"};
        } else if (page == 1) {
            title = "GT2 / WHEEL AXES";
            for (int i = 0; i < AxisCount; ++i) rows.push_back(std::string(axisLabels[i]) + ": " + (config.axes[i].Valid(i == Steering) ? "Assigned" : "Not calibrated"));
            rows.push_back("Back");
        } else if (page == 2) {
            const auto& a = config.axes[axis]; const auto* d = find(a.device);
            title = std::string("GT2 / ") + axisLabels[axis];
            rows = {"Assign axis by movement...", "Record centre / released: " + std::to_string(a.rest),
                std::string(axis == Steering ? "Record full LEFT: " : "Record fully pressed: ") + std::to_string(a.end),
                std::string(axis == Steering ? "Record full RIGHT: " : "Opposite endpoint (unused): ") + std::to_string(a.right),
                "Dead zone: " + std::to_string(a.deadzone) + "%", "Saturation: " + std::to_string(a.saturation) + "%",
                "Response curve: " + std::to_string(a.curve) + "% (100 = linear)", "Invert axis", "Clear assignment", "Back"};
            if (d && a.axis >= 0) message = DeviceName(*d) + " / " + rawAxisLabels[a.axis] + ": " + std::to_string(d->axes[a.axis]) + " -> " + std::to_string(int(a.Map(d->axes[a.axis], axis == Steering) * 100)) + "%";
            else message = "No assigned device connected";
        } else if (page == 3) {
            title = "GT2 / WHEEL BUTTONS / " + std::to_string(buttonPage + 1);
            for (int i = buttonPage * 7; i < std::min((buttonPage + 1) * 7, int(ButtonCount)); ++i) {
                const auto& b = config.buttons[i];
                rows.push_back(std::string(buttonLabels[i]) + ": " + (b.button < 0 ? "Unassigned" : "Button " + std::to_string(b.button + 1)));
            }
            rows.push_back(buttonPage ? "Previous page" : "Next page"); rows.push_back("Back");
            message = "Enter: assign. Delete: clear selected binding.";
        } else if (page == 4) {
            title = "GT2 / CONNECTED DEVICES";
            const int pages = std::max(1, (int(devices.size()) + 5) / 6);
            devicePage = std::min(devicePage, pages - 1);
            for (int i = devicePage * 6; i < std::min((devicePage + 1) * 6, int(devices.size())); ++i) {
                const auto& d = devices[i]; rows.push_back(DeviceName(d) + (FitsDeviceRole(d, DeviceRole::Any) ? "" : " [extended]")
                    + (d.online ? " [OK]" : " [unavailable]") + (d.forceCapable ? " FFB" : ""));
            }
            rows.push_back("Rescan devices"); rows.push_back("Next page"); rows.push_back("Back");
            message = "Use the manufacturer's Windows PC driver / mode.";
        }
        selected = std::clamp(selected, 0, int(rows.size()) - 1);
        const bool back = window.Pressed(gt2::keys::kEscape) || window.Pressed(gt2::keys::kBack);
        if (back) { if (page == 0) break; page = page == 2 ? 1 : 0; selected = 0; message.clear(); continue; }
        if (window.Pressed(gt2::keys::kUp) || (VrMode() && window.PadPressed(gt2::input::ps1::kSquare))) selected = (selected + int(rows.size()) - 1) % int(rows.size());
        if (window.Pressed(gt2::keys::kDown) || (VrMode() && window.PadPressed(gt2::input::ps1::kR1))) selected = (selected + 1) % int(rows.size());
        const bool accept = window.Pressed(gt2::keys::kReturn);
        int direction = window.Pressed(gt2::keys::kLeft) ? -1 : window.Pressed(gt2::keys::kRight) || accept ? 1 : 0;
        if (VrMode()) {
            const int trigger = wheelTriggers.Update(window.Pad().pressureL2 / 255.f, window.Pad().pressureR2 / 255.f);
            if (trigger) direction = trigger;
        }
        if (page == 3 && window.KeyPressed(0x2e) && selected < 7) { config.buttons[buttonPage * 7 + selected] = {}; save(); }
        if (direction) {
            if (page == 0) {
                if (selected == 0) config.enabled = !config.enabled;
                if (selected == 1 && accept) { page = 1; selected = 0; continue; }
                if (selected == 2 && accept) { page = 3; selected = 0; continue; }
                if (selected == 3) { config.gearbox = (config.gearbox + direction + 3) % 3; config.autoGearbox = false; }
                if (selected == 4) { config.feedback = !config.feedback; config.autoFeedback = false; }
                if (selected == 5) config.gain = std::clamp(config.gain + 5 * direction, 0, 100);
                if (selected == 6) config.damping = std::clamp(config.damping + 5 * direction, 0, 100);
                if (selected == 7) config.invertForce = !config.invertForce;
                if (selected == 8 && accept) { page = 4; selected = 0; continue; }
                if (selected == 9 && accept) break;
                save();
            } else if (page == 1 && accept) {
                if (selected == AxisCount) { page = 0; selected = 1; }
                else { axis = selected; page = 2; selected = 0; }
                continue;
            } else if (page == 2) {
                auto& a = config.axes[axis]; const auto* d = find(a.device);
                if (selected == 0 && accept) { capture = ButtonCount; baseline = devices; continue; }
                if (d && a.axis >= 0 && accept) {
                    if (selected == 1) a.rest = d->axes[a.axis];
                    if (selected == 2) a.end = d->axes[a.axis];
                    if (selected == 3 && axis == Steering) a.right = d->axes[a.axis];
                }
                if (selected == 4) a.deadzone = std::clamp(a.deadzone + direction, 0, 25);
                if (selected == 5) a.saturation = std::clamp(a.saturation + direction * 5, 50, 100);
                if (selected == 6) a.curve = std::clamp(a.curve + direction * 5, 50, 200);
                if (selected == 7) { if (axis == Steering) std::swap(a.end, a.right); else std::swap(a.rest, a.end); }
                if (selected == 8 && accept) a = {};
                if (selected == 9 && accept) { page = 1; selected = axis; continue; }
                save();
            } else if (page == 3 && accept) {
                if (selected < 7) { capture = buttonPage * 7 + selected; baseline = devices; continue; }
                if (selected == 7) buttonPage = 1 - buttonPage;
                else { page = 0; selected = 2; }
                continue;
            } else if (page == 4 && accept) {
                if (selected == int(rows.size()) - 3) { rig.Rescan(); message = "Rescanning..."; }
                if (selected == int(rows.size()) - 2) devicePage = (devicePage + 1) % std::max(1, (int(devices.size()) + 5) / 6);
                if (selected == int(rows.size()) - 1) { page = 0; selected = 8; continue; }
            }
        }
        Panel(window, title, rows, selected, !saveError.empty() ? saveError : page == 0 ? rig.Status() : message,
            VrMode() ? "Stick: row  Triggers: value  A: assign  B: back" : "Arrows: select/change  Enter: assign  Esc: back");
    }
    rig.Stop();
}

bool WheelBack(GameWindow& w) { return w.Pressed(gt2::keys::kEscape) || w.Pressed(gt2::keys::kBack); }
int WheelValue(GameWindow& w, gt2::vr::MenuTriggers& triggers) {
    if (w.KeyPressed(gt2::keys::kLeft)) return -1;
    if (w.KeyPressed(gt2::keys::kRight)) return 1;
    if (!VrMode()) return w.Pressed(gt2::keys::kLeft) ? -1 : w.Pressed(gt2::keys::kRight) ? 1 : 0;
    const int trigger = triggers.Update(w.Pad().pressureL2 / 255.f, w.Pad().pressureR2 / 255.f);
    if (trigger) return trigger;
    const auto buttons = w.Input().Wheel().MenuButtons();
    if ((buttons & gt2::input::ps1::kLeft) && w.PadPressed(gt2::input::ps1::kLeft)) return -1;
    if ((buttons & gt2::input::ps1::kRight) && w.PadPressed(gt2::input::ps1::kRight)) return 1;
    return 0;
}
struct WheelCapture {
    gt2::input::wheel::Rig& rig;
    WheelCapture(GameWindow& w, const std::string& id) : rig(w.Input().Wheel()) { rig.CaptureDevice(id); }
    ~WheelCapture() { rig.CaptureDevice(""); }
};
void WheelSave(std::string& message) {
    try { gt2::input::wheel::SaveSettings(); message = "Saved"; }
    catch (const std::exception& e) { message = std::string("Not saved: ") + e.what(); }
}
bool CommitWheelSetup(const gt2::input::wheel::Settings& candidate, std::string& message) {
    auto& current = gt2::input::wheel::Config();
    const auto previous = current; current = candidate;
    WheelSave(message);
    if (message == "Saved") return true;
    current = previous; return false;
}
const gt2::input::wheel::DeviceState* WheelDevice(GameWindow& w, const std::string& id) {
    for (const auto& d : w.Input().Wheel().Devices()) if (d.id == id && d.online) return &d;
    return nullptr;
}
bool PickWheelDevice(GameWindow& w, const std::string& title, std::string& result, const std::string& automatic = "") {
    using namespace gt2::input::wheel;
    const auto role = automatic == "auto" ? DeviceRole::Wheel : automatic == "base" ? DeviceRole::Pedals
        : automatic == "none" ? DeviceRole::Shifter : DeviceRole::Any;
    int selected = 0, page = 0;
    while (!w.Closed() && w.BeginFrame()) {
        w.Input().Wheel().Stop();
        std::vector<std::string> ids, names;
        if (!automatic.empty()) { ids.push_back(automatic); names.push_back(automatic == "base" ? "Pedals connected to wheel base" : automatic == "none" ? "Wheel paddles / no USB shifter" : "Automatic selection"); }
        const auto& devices = w.Input().Wheel().Devices();
        for (const auto& d : devices) if (d.online && FitsDeviceRole(d, role)) {
            auto name = DeviceName(d);
            const auto duplicates = std::count_if(devices.begin(), devices.end(), [&](const auto& other) {
                return other.online && FitsDeviceRole(other, role) && DeviceName(other) == name;
            });
            if (duplicates > 1) name += " [" + d.id.substr(0, 8) + "]";
            ids.push_back(d.id); names.push_back(name);
        }
        const int pages = std::max(1, (int(names.size()) + 5) / 6); page = std::min(page, pages - 1);
        std::vector<std::string> rows;
        for (int i = page * 6; i < std::min(int(names.size()), page * 6 + 6); ++i) rows.push_back(names[i]);
        const int count = int(rows.size());
        if (pages > 1) rows.push_back("Next page");
        rows.push_back("Back");
        selected = std::clamp(selected, 0, int(rows.size()) - 1);
        if (WheelBack(w)) return false;
        if (w.Pressed(gt2::keys::kUp)) selected = (selected + int(rows.size()) - 1) % int(rows.size());
        if (w.Pressed(gt2::keys::kDown)) selected = (selected + 1) % int(rows.size());
        if (w.Pressed(gt2::keys::kReturn)) {
            if (selected < count) { result = ids[page * 6 + selected]; if (result == "auto") result.clear(); return true; }
            if (selected == count && pages > 1) { page = (page + 1) % pages; selected = 0; }
            else return false;
        }
        Panel(w, title, rows, selected, names.empty() ? "Connect the device to continue" : "Unlisted devices: use Guided setup", "D-pad: choose   Confirm: select   Back: cancel");
    }
    return false;
}
void WheelAxisGuide(GameWindow& w, int target) {
    using namespace gt2::input::wheel;
    std::string id;
    if (!PickWheelDevice(w, std::string("SET UP / ") + axisLabels[target], id)) return;
    WheelCapture capture(w, id);
    AxisBinding pending;
    DeviceState rest;
    int step = 0;
    std::string error;
    while (!w.Closed() && w.BeginFrame()) {
        w.Input().Wheel().Stop();
        if (WheelBack(w)) return;
        const auto* d = WheelDevice(w, id);
        const char* instructions[] = {target == Steering ? "Centre the wheel" : "Release the pedal completely",
            target == Steering ? "Turn fully LEFT and hold" : "Press the pedal fully and hold",
            "Turn fully RIGHT and hold", "Check the live movement, then confirm to save"};
        if (d && w.Pressed(gt2::keys::kReturn)) {
            if (step == 0) { rest = *d; step = 1; error.clear(); }
            else if (step == 1) {
                int best = -1, distance = 8192;
                for (int i = 0; i < kAxes; ++i) if (d->available[i] && std::abs(d->axes[i] - rest.axes[i]) > distance) {
                    best = i; distance = std::abs(d->axes[i] - rest.axes[i]);
                }
                if (best < 0) error = "No clear movement yet. Move only this control fully";
                else {
                    pending = {id, best, rest.axes[best], d->axes[best], 32767, target == Steering ? 0 : 2, 100, 100};
                    step = target == Steering ? 2 : 3; error.clear();
                }
            } else if (step == 2) {
                pending.right = d->axes[pending.axis];
                if (!pending.Valid(true)) error = "Left and right must be on opposite sides of centre";
                else { step = 3; error.clear(); }
            } else {
                auto next = Config(); next.axes[target] = pending; next.automatic = false; next.enabled = true;
                if (target == Clutch) next.useClutch = true;
                if (CommitWheelSetup(next, error)) return;
            }
        }
        std::vector<std::string> rows = {instructions[step], "Press Confirm when ready", "Back cancels without changing your setup"};
        if (step == 3 && d) rows.push_back("Position: " + std::to_string(int(pending.Map(d->axes[pending.axis], target == Steering) * 100)) + "%");
        Panel(w, std::string("GUIDED SETUP / ") + axisLabels[target], rows, -1,
            !d ? "Reconnect the selected device" : error.empty() ? "Move only the control shown above" : error, "Confirm: next / save   Back: cancel");
    }
}
void WheelShifterGuide(GameWindow& w) {
    using namespace gt2::input::wheel;
    std::string id;
    if (!PickWheelDevice(w, "SET UP / H-PATTERN SHIFTER", id)) return;
    WheelCapture capture(w, id);
    ShifterSetup setup;
    int gears = 6;
    bool started = false;
    std::string message;
    gt2::vr::MenuTriggers triggers;
    triggers.Begin(w.Pad().pressureL2 / 255.f, w.Pad().pressureR2 / 255.f);
    while (!w.Closed() && w.BeginFrame()) {
        w.Input().Wheel().Stop();
        if (WheelBack(w)) return;
        const auto* d = WheelDevice(w, id);
        if (!started) {
            gears = std::clamp(gears + WheelValue(w, triggers), 1, 7);
            if (d && w.Pressed(gt2::keys::kReturn)) { setup.Start(*d, gears); started = true; }
        } else if (!setup.Complete()) setup.Sample(d);
        else if (d && w.Pressed(gt2::keys::kReturn)) {
            auto next = Config(); setup.ApplyTo(next); if (CommitWheelSetup(next, message)) return;
        }
        NativeCanvas c;
        c.Quad(28, 24, 744, 552, 0x101c2a); c.Quad(28, 24, 744, 5, 0x42b6f5);
        c.Text(54, 48, "SET UP YOUR SHIFTER", 0xffffff);
        c.Text(54, 93, "Forward gears: " + std::to_string(gears) + (!started ? "  [Left / Right]" : ""), 0x8ed8f8);
        const std::string instruction = !started ? "Put the lever in NEUTRAL, then Confirm" : setup.Complete() ? "All gates learned. Confirm to save" : setup.WaitingForNeutral() ? "Return the lever to NEUTRAL" : "Select gear " + (setup.Gear() ? std::to_string(setup.Gear()) : "R");
        c.Text(54, 141, instruction, 0xffffff);
        c.Quad(220, 300, 360, 7, 0x637b90);
        for (int column = 0; column < 4; ++column) {
            const float x = 220.f + float(column) * 120.f;
            c.Quad(x, 225, 7, 150, 0x637b90);
            for (int row = 0; row < 2; ++row) {
                const int gear = column * 2 + row + 1;
                if (gear > gears && gear != 8) continue;
                const bool active = started && !setup.Complete() && !setup.WaitingForNeutral() && (gear == 8 ? setup.Gear() == 0 : setup.Gear() == gear);
                const float y = row == 0 ? 206.f : 366.f;
                c.Quad(x - 16, y - 5, 40, 38, active ? 0x2688b6 : 0x234050);
                c.Text(x - 7, y, gear == 8 ? "R" : std::to_string(gear), active ? 0xffffff : 0xb8c9d8);
            }
        }
        c.Text(383, 296, "N", 0xffffff);
        c.Text(54, 430, "Follow the gear labels on your shifter", 0xa8b7c8, .9f);
        const std::string status = !d ? "Reconnect the shifter" : !message.empty() ? message : setup.Error();
        c.Text(54, 479, status.substr(0, 57), 0xa8b7c8);
        c.Text(54, 520, "Confirm: start / save   Back: cancel", 0xffffff);
        std::vector<gt2view::DrawItem> items; c.Append(w.Renderer(), items); w.EndFrame(items);
    }
}
void WheelMenuButtons(GameWindow& w) {
    using namespace gt2::input::wheel;
    const int bits[] = {14,12,4,6,7,5,3,10,11,13,15,0,1,2,8,9};
    const char* labels[] = {"Confirm", "Back", "Up", "Down", "Left", "Right", "Open settings", "Previous tab", "Next tab", "Circle", "Square", "Select", "Left stick click", "Right stick click", "L2", "R2"};
    int selected = 0, page = 0, capture = -1;
    std::vector<DeviceState> baseline;
    std::string message;
    while (!w.Closed() && w.BeginFrame()) {
        w.Input().Wheel().Stop();
        if (capture >= 0) {
            if (w.KeyPressed(gt2::keys::kEscape)) { capture = -1; continue; }
            for (const auto& d : w.Input().Wheel().Devices()) if (d.online) {
                const auto old = std::find_if(baseline.begin(), baseline.end(), [&](const auto& b) { return b.id == d.id; });
                if (old == baseline.end()) continue;
                for (int b = 0; b < kButtons; ++b) if (d.buttons[b] && !old->buttons[b]) {
                    const int bit = bits[capture];
                    for (auto& binding : Config().navigation) if (binding.device == d.id && binding.button == b) binding = {};
                    if (bit != 3 && Config().buttons[Menu].device == d.id && Config().buttons[Menu].button == b) Config().buttons[Menu] = {};
                    Config().navigation[bit] = {d.id, b};
                    if (bit == 3) Config().buttons[Menu] = {d.id, b};
                    Config().automatic = false; Config().enabled = true;
                    capture = -1; WheelSave(message); break;
                }
                if (capture < 0) break;
            }
            if (capture >= 0) baseline = w.Input().Wheel().Devices();
            Panel(w, "WHEEL / LEARN A BUTTON", {capture < 0 ? "Button learned" : std::string("Press the button for: ") + labels[capture],
                "Release buttons held when you opened this page"}, -1, message, "Escape: cancel");
            continue;
        }
        if (WheelBack(w)) return;
        std::vector<std::string> rows;
        for (int i = page * 8; i < page * 8 + 8; ++i) rows.push_back(std::string(labels[i]) + (Config().navigation[bits[i]].button >= 0 ? "   [set]" : "   [not set]"));
        rows.push_back(page ? "Previous page" : "More buttons"); rows.push_back("Back");
        if (w.Pressed(gt2::keys::kUp)) selected = (selected + 9) % 10;
        if (w.Pressed(gt2::keys::kDown)) selected = (selected + 1) % 10;
        if (w.Pressed(gt2::keys::kReturn)) {
            if (selected < 8) { capture = page * 8 + selected; baseline = w.Input().Wheel().Devices(); }
            else if (selected == 8) { page = 1 - page; selected = 0; }
            else return;
        }
        Panel(w, "WHEEL / MENU BUTTONS", rows, selected, message, "D-pad: choose   Confirm: learn   Back: return");
    }
}
void ShowWheelSettings(GameWindow& w) {
    using namespace gt2::input::wheel;
    auto& s = Config(); auto& rig = w.Input().Wheel();
    int selected = 0, page = 0;
    std::string message;
    gt2::vr::MenuTriggers triggers;
    triggers.Begin(w.Pad().pressureL2 / 255.f, w.Pad().pressureR2 / 255.f);
    while (!w.Closed() && w.BeginFrame()) {
        rig.Stop();
        if (WheelBack(w)) { if (!page) break; page = 0; selected = 0; continue; }
        const char* gearNames[] = {"Race default", "Paddles", "H-pattern"};
        std::vector<std::string> rows;
        if (!page) rows = {std::string("Input: ") + (s.automatic ? "Automatic" : s.enabled ? "Custom" : "Off"),
            std::string("MT controls: ") + gearNames[s.gearbox], std::string("Force feedback: ") + (s.feedback ? "On" : "Off"),
            "Force strength: " + std::to_string(s.gain) + "%", "Devices and clutch", "Guided setup / buttons", "Advanced settings", "Steering model...", "Driving assists...", "Back"};
        if (page == 1) {
            auto label = [&](const std::string& id, const char* fallback) { const auto* d = WheelDevice(w, id); return d ? DeviceName(*d) : std::string(fallback); };
            const auto& base = s.axes[Steering].device;
            const auto& pedal = s.axes[Throttle].device;
            const auto& gear = s.buttons[s.gearbox == 2 ? Gear1 : ShiftUp].device;
            const auto pedalName = !base.empty() && pedal == base ? std::string("Connected through wheel base") : label(pedal, "Auto / choose USB pedals");
            const auto gearName = !base.empty() && gear == base ? std::string(s.gearbox == 2 ? "Base H-pattern connection" : "Wheel paddles")
                : label(gear, "No USB shifter assigned");
            rows = {"Wheel: " + label(base, "Auto / choose"), "Pedals: " + pedalName,
                "Shifter: " + gearName, std::string("Clutch pedal fitted: ") + (s.useClutch ? "Yes" : "No"),
                "Wheel rim buttons...", "Find connected devices again", "Back"};
        }
        if (page == 2) rows = {"Steering", "Accelerator", "Brake", "Clutch", "H-pattern shifter", "Menu buttons", "Back"};
        if (page == 3) rows = {"Wheel rotation: " + std::to_string(s.steeringGeometry.wheelDegrees) + " deg",
            "Mechanical trail: " + std::to_string(s.steeringGeometry.mechanicalTrailMm) + " mm",
            "Contact half-length: " + std::to_string(s.steeringGeometry.patchHalfLengthMm) + " mm",
            "Torque reference: " + std::to_string(s.steeringGeometry.referenceTorqueNm) + " Nm", "Back"};
        if (page == 4) {
            const char* help[] = {"Off", "Weak", "Strong"};
            rows = {"Traction control: " + std::to_string(s.tractionControl) + " / 5",
                std::string("Countersteering assistance: ") + help[s.countersteer],
                std::string("Ignore gear-change speed: ") + (s.ignoreShiftSpeed ? "On" : "Off"), "Back"};
        }
        selected = std::clamp(selected, 0, int(rows.size()) - 1);
        if (w.Pressed(gt2::keys::kUp)) selected = (selected + int(rows.size()) - 1) % int(rows.size());
        if (w.Pressed(gt2::keys::kDown)) selected = (selected + 1) % int(rows.size());
        const bool accept = w.Pressed(gt2::keys::kReturn);
        const int valueChange = WheelValue(w, triggers);
        const int change = valueChange ? valueChange : accept ? 1 : 0;
        if (change) {
            if (!page) {
                if (selected == 0) {
                    const int mode = ((s.automatic ? 1 : s.enabled ? 2 : 0) + change + 3) % 3;
                    if (mode == 1) ResetAutomatic(s);
                    else { s.automatic = false; s.enabled = mode == 2; }
                }
                if (selected == 1) { s.gearbox = (s.gearbox + change + 3) % 3; s.autoGearbox = false; }
                if (selected == 2) { s.feedback = !s.feedback; s.autoFeedback = false; }
                if (selected == 3) s.gain = std::clamp(s.gain + change * 5, 0, 100);
                if (selected == 4 && accept) { page = 1; selected = 0; continue; }
                if (selected == 5 && accept) { page = 2; selected = 0; continue; }
                if (selected == 6 && accept) { ShowWheelAdvanced(w); continue; }
                if (selected == 7 && accept) { page = 3; selected = 0; continue; }
                if (selected == 8 && accept) { page = 4; selected = 0; continue; }
                if (selected == 9 && accept) break;
                WheelSave(message);
            } else if (page == 1) {
                if (selected < 3 && accept) {
                    std::string choice;
                    if (PickWheelDevice(w, selected == 0 ? "SELECT WHEEL" : selected == 1 ? "SELECT PEDALS" : "SELECT SHIFTER", choice, selected == 0 ? "auto" : selected == 1 ? "base" : "none")) {
                        (selected == 0 ? s.baseChoice : selected == 1 ? s.pedalChoice : s.shifterChoice) = choice;
                        s.automatic = true; s.profile.clear(); s.signature.clear(); s.axes = {}; s.buttons = {}; s.navigation = {};
                    }
                }
                if (selected == 3) {
                    if (!s.useClutch && !s.automatic) WheelAxisGuide(w, Clutch);
                    else { s.useClutch = !s.useClutch; if (!s.useClutch) s.axes[Clutch] = {}; }
                }
                if (selected == 4 && accept) {
                    const auto* d = WheelDevice(w, s.axes[Steering].device);
                    const auto* profile = d ? MatchProfile(*d) : nullptr;
                    if (!profile || !profile->rimSpecific) WheelMenuButtons(w);
                    else {
                        int item = 0;
                        const bool moza = d->vendor == 0x346e;
                        while (!w.Closed() && w.BeginFrame()) {
                            rig.Stop(); if (WheelBack(w)) break;
                            if (w.Pressed(gt2::keys::kUp) || w.Pressed(gt2::keys::kDown)) item = 1 - item;
                            if (w.Pressed(gt2::keys::kReturn)) {
                                if (!item) { s.rimButtons = true; s.signature.clear(); }
                                else WheelMenuButtons(w);
                                break;
                            }
                            Panel(w, "WHEEL RIM BUTTONS", {moza ? "Use MOZA CS V2 button layout" : "Use wireless rim / paddle button layout", "Learn buttons on my wheel"}, item,
                                "The button layout depends on the attached rim", "D-pad: choose   Confirm: select   Back: return");
                        }
                    }
                }
                if (selected == 5 && accept) rig.Rescan();
                if (selected == 6 && accept) { page = 0; selected = 4; continue; }
                WheelSave(message);
            } else if (page == 4) {
                if (selected == 0) s.tractionControl = std::clamp(s.tractionControl + change, 0, 5);
                if (selected == 1) s.countersteer = std::clamp(s.countersteer + change, 0, 2);
                if (selected == 2) s.ignoreShiftSpeed = !s.ignoreShiftSpeed;
                if (selected == 3 && accept) { page = 0; selected = 8; continue; }
                WheelSave(message);
            } else if (page == 3) {
                auto& g = s.steeringGeometry;
                if (selected == 0) g.wheelDegrees = std::clamp(g.wheelDegrees + change * 90, 180, 2520);
                if (selected == 1) g.mechanicalTrailMm = std::clamp(g.mechanicalTrailMm + change * 5, 0, 100);
                if (selected == 2) g.patchHalfLengthMm = std::clamp(g.patchHalfLengthMm + change * 5, 20, 150);
                if (selected == 3) g.referenceTorqueNm = std::clamp(g.referenceTorqueNm + change, 1, 100);
                if (selected == 4 && accept) { page = 0; selected = 7; continue; }
                WheelSave(message);
            } else if (page == 2 && accept) {
                if (selected < 4) WheelAxisGuide(w, selected);
                if (selected == 4) WheelShifterGuide(w);
                if (selected == 5) WheelMenuButtons(w);
                if (selected == 6) { page = 0; selected = 5; }
                continue;
            }
        }
        if (page == 4) {
            const char* hints[] = {"Reduces throttle during wheelspin; 0 disables help",
                "Adds steering correction when the rear slides out",
                "Allow forward/reverse changes while moving; default Off", "Return to wheel settings"};
            Panel(w, "WHEEL / DRIVING ASSISTS", rows, selected, hints[selected], "Left/right: adjust   Settings save automatically"); continue;
        }
        if (page == 3) {
            const char* hints[] = {"Match the total rotation set in your wheel driver",
                "Generic geometry: lever arm of the tyre force",
                "Generic tyre size: controls self-aligning torque",
                "Model torque at full FFB output; lower is stronger", "Return to wheel settings"};
            Panel(w, "WHEEL / STEERING MODEL", rows, selected, hints[selected], "Left/right: adjust   Settings save automatically"); continue;
        }
        if (page) { Panel(w, page == 1 ? "WHEEL / YOUR DEVICES" : "WHEEL / GUIDED SETUP", rows, selected, rig.Status(), "D-pad: choose   Confirm: select   Back: return"); continue; }
        NativeCanvas c;
        c.Quad(28, 24, 744, 552, 0x101c2a); c.Quad(28, 24, 744, 5, 0x42b6f5);
        c.Text(54, 45, "YOUR RACING WHEEL", 0xffffff);
        c.Text(54, 79, s.profile.empty() ? "Connect your wheel; known devices set up automatically" : s.profile.substr(0, 70), 0x8ed8f8, .8f);
        for (int i = 0; i < int(rows.size()); ++i) {
            const float y = 116.f + float(i) * 28.f;
            if (i == selected) c.Quad(44, y - 4, 711, 26, 0x234b68);
            c.Text(56, y, rows[i], i == selected ? 0x8ed8f8 : 0xe2e8f0);
        }
        for (int i = 0; i < 3; ++i) {
            const auto& a = s.axes[i]; const auto* d = WheelDevice(w, a.device);
            const float value = d && a.Valid(i == Steering) && d->available[a.axis] ? a.Map(d->axes[a.axis], i == Steering) : 0;
            const float x = 56.f + float(i) * 234.f;
            c.Text(x, 410, std::string(axisLabels[i]) + " " + std::to_string(int(value * 100)) + "%", 0xb8c9d8, .7f);
            c.Quad(x, 437, 204, 13, 0x283d50);
            if (i == Steering) { c.Quad(x + 102, 433, 2, 21, 0x728ca0); c.Quad(x + 100 + value * 100, 436, 5, 15, 0x42b6f5); }
            else c.Quad(x, 437, value * 204, 13, 0x42b6f5);
        }
        c.Text(54, 479, (message.starts_with("Not saved") ? message : rig.Status()).substr(0, 57), 0xa8b7c8);
        c.Text(54, 520, VrMode() ? "Up/down: row  Triggers: value  Confirm  Back" : "Up/down: row  Left/right: value  Confirm  Back", 0xffffff);
        std::vector<gt2view::DrawItem> items; c.Append(w.Renderer(), items); w.EndFrame(items);
    }
    rig.Stop();
}


#endif

void ShowSettingsMenu(GameWindow& window) {
    const gt2::audio::ScopedMixPause pause;
    PrepareNativeUi(window.Renderer()); window.Input().StopFeedback();
    if (vrScale < 0) vrScale = int(VrOptionsInUse().renderScale * 100 + 0.5f);
    const bool vr = VrMode();
    int page = 0, selected = 0, carPage = 0, hudPage = 0, controlPage = 0;
    gt2::vr::MenuTriggers triggers;
    triggers.Begin(window.Pad().pressureL2 / 255.f, window.Pad().pressureR2 / 255.f);
    bool first = true, done = false;
    std::printf("overlay: opened (%s) at field %d\n", vr ? "VR" : "desktop", window.Field());
    std::string status = "GAME PAUSED - settings save automatically.";
    while (!done && !window.Closed()) {
        auto graphics = CurrentGraphics();
        std::vector<std::string> rows;
        std::string title = "GT2 VR / MENU";
        if (page == 0) rows = {"Graphics and performance", "Cheats", "HUD elements", "Controls", "Change game (Arcade / Simulation)", "Original game pause / exit", "Resume game"};
        if (page == 9) {
            title = "GT2 VR / CHANGE GAME";
            rows = {"Return to disc selection", "Back to current game"};
        }
        if (page == 1) {
            title = "GT2 VR / GRAPHICS";
            rows = {"Eye resolution: " + std::to_string(vrScale) + "% (restart)",
                "Refresh rate: " + std::to_string(vrRefresh) + " Hz",
                "MSAA: " + std::to_string(graphics.msaa) + "x",
                "Draw distance: " + (graphics.drawDistance < 0 ? std::string("Entire course") : graphics.drawDistance == 0 ? std::string("Original") : std::to_string(graphics.drawDistance) + " m"),
                "Vibration: " + std::to_string(rumble) + "%", "Texture filtering: " + std::string(graphics.smoothTextures ? "Smooth + mipmaps" : "Original"),
                "FPS profiler: " + std::string(profiler ? "ON" : "OFF"),
                "Foveation: " + std::string(foveation == 0 ? "Off" : foveation == 1 ? "Low" : foveation == 2 ? "Balanced" : "High"),
                "HD textures and media: " + std::string(gt2::hd::Enabled() ? "ON" : "OFF"),
                "PlayStation intro: " + std::string(playStationIntro ? "ON" : "OFF"), "Back"};
        }
        if (page == 1 && !vr) {
            rows[0] = "Resolution: " + (graphics.renderHeight ? std::to_string(graphics.renderHeight) + "p" : std::to_string(graphics.renderScale) + "%");
            rows[1] = "Frame limit: " + (graphics.frameCap ? std::to_string(graphics.frameCap) + " FPS" : std::string("Display"));
            rows[7] = "Display timing...";
        }
        if (page == 8) {
            title = "GT2 / DISPLAY TIMING";
            rows = {std::string("VSync: ") + (graphics.vsync ? "ON" : "OFF"),
                    std::string("Frame presentation: ") + (graphics.frameRate ? "Display rate" : "Original"), "Back"};
        }
        if (page == 2) {
            title = "GT2 VR / CHEATS";
            rows = {"Arcade - all tracks: " + std::string(gt2::pc::unlockCourses ? "ON" : "OFF"),
                "Arcade - all cars: " + std::string(gt2::pc::unlockCars ? "ON" : "OFF"),
                "Simulation - all licences GOLD", "Simulation - 99,999,999 credits", "Simulation - all cars catalogue",
                "Simulation - event unlock: " + std::string(gt2::pc::unlockSimulationEvents ? "ON" : "OFF"), "Back"};
        }
        if (page == 3) {
            title = "ALL CARS / PAGE " + std::to_string(carPage + 1);
            const size_t start = size_t(carPage) * 8;
            for (size_t i = start; i < std::min(start + 8, catalogue.size()); ++i)
                rows.push_back(cheatData->cars.At(catalogue[i]).name);
        }
        if (page == 6) {
            title = "GT2 VR / CONTROLS";
            rows = {"Steering and wheel", "Button bindings", std::string("Brake to reverse (AT): ")+(controlBindings.brakeReverse ? "ON" : "OFF"),
                std::string("Steering stick: ")+(controlBindings.steeringStick ? "Right" : "Left"), "Reset controls to defaults", "Back"};
        }
        if (page == 6 && !vr) {
            rows = {std::string("Gamepad bindings: ") + (desktopCustom ? "Custom" : "Original game"), "Button bindings",
                std::string("Brake to reverse (AT): ") + (desktopBindings.brakeReverse ? "ON" : "OFF"),
                std::string("Custom steering stick: ") + (desktopBindings.steeringStick ? "Right" : "Left"),
                "Reset controls to defaults", "DualSense pedal resistance: " + std::to_string(adaptive) + "%", "Back"};
        }
        if (page == 7) {
            title = "GT2 VR / BINDINGS / PAGE " + std::to_string(controlPage+1);
            for (int i=controlPage*4;i<controlPage*4+4;++i) rows.push_back(std::string(gt2::vr::controlNames[i])+": "+(vr ? gt2::vr::controlSources[controlBindings.source[i]] : gt2::vr::desktopControlSources[desktopBindings.source[i]]));
            rows.push_back(controlPage ? "Previous bindings" : "More bindings"); rows.push_back("Back");
        }
#ifdef _WIN32
        if (page == 6) rows.insert(rows.end() - 1, "Racing wheel / pedals / shifter...");
#endif
        if (page == 5) {
            title = "GT2 VR / DRIVING";
            const char* modes[] = {"Stick", "Virtual wheel", "Motion"};
            rows = {std::string("Steering: ") + modes[drivingSettings.mode],
                std::string("Motion hand: ") + (drivingSettings.motionHand ? "Right" : "Left"),
                "Wheel height: " + std::to_string(drivingSettings.wheelHeightCm) + " cm",
                "Wheel distance: " + std::to_string(drivingSettings.wheelDistanceCm) + " cm",
                "Wheel radius: " + std::to_string(drivingSettings.wheelRadiusCm) + " cm",
                "Intro camera lower: " + std::to_string(introLowerCm) + " cm", "Back"};
        }
        if (page == 4) {
            title = "GT2 VR / HUD / PAGE " + std::to_string(hudPage + 1);
            rows.push_back(std::string("Speed units: ") + (OverlayMetricUnits(baseMetricUnits) ? "km/h" : "mph"));
            const size_t start = size_t(hudPage) * 6;
            for (size_t i = start; i < std::min(start + 6, std::size(hudSettings)); ++i) {
                const auto& setting = hudSettings[i];
                rows.push_back(std::string(setting.label) + ": " + (hudVisibility.*setting.member ? "ON" : "OFF"));
            }
            rows.push_back(hudPage == 0 ? "More HUD settings" : "Previous HUD settings");
            rows.push_back("Back");
        }
        if (!vr && title.starts_with("GT2 VR")) title.replace(0, 6, "GT2");
        if (!first) {
            if (!window.BeginFrame()) break;
            if (window.PadPressed(gt2::input::ps1::kStart) || window.Pressed(gt2::keys::kF10)) break;
            if (window.Pressed(gt2::keys::kBack) || window.Pressed(gt2::keys::kEscape)) {
                if (page == 0) break;
                page = page == 3 ? 2 : page == 8 ? 1 : (page == 5 || page == 7) ? 6 : 0; selected = 0; continue;
            }
            const int count = int(rows.size());
            if ((vr && window.PadPressed(gt2::input::ps1::kSquare)) || window.Pressed(gt2::keys::kUp)) selected = (selected + count - 1) % count;
            if ((vr && window.PadPressed(gt2::input::ps1::kR1)) || window.Pressed(gt2::keys::kDown)) selected = (selected + 1) % count;
            const bool accept = window.Pressed(gt2::keys::kReturn);
            int direction = triggers.Update(window.Pad().pressureL2 / 255.f, window.Pad().pressureR2 / 255.f);
            if (!vr) {
                if (window.Pressed(gt2::keys::kLeft)) direction = -1;
                if (window.Pressed(gt2::keys::kRight) || accept) direction = 1;
            } else {
                const auto wheelButtons = window.Input().Wheel().MenuButtons();
                if ((wheelButtons & gt2::input::ps1::kLeft) && window.PadPressed(gt2::input::ps1::kLeft)) direction = -1;
                if ((wheelButtons & gt2::input::ps1::kRight) && window.PadPressed(gt2::input::ps1::kRight)) direction = 1;
            }
            if (direction || accept) {
                if (page == 0 && accept) {
                    if (selected == 4) {
                        if (gameChangeAvailable) { page = 9; selected = 1; }
                        else status = "Launch from the disc picker with both discs installed.";
                    }
                    else if (selected >= 5) { if (selected == 5) window.RequestGamePause(); done = true; }
                    else { page = selected == 3 ? 6 : selected == 2 ? 4 : selected + 1; selected = 0; }
                }
                else if (page == 9 && accept) {
                    if (selected == 0) {
                        std::puts("player: change game requested");
                        gameChangeRequested = true;
                        window.Close(); done = true;
                    } else { page = 0; selected = 4; }
                }
                else if (page == 1 && (direction || selected == 10)) {
                    const int samples[] = {1,2,4,8}, distances[] = {0,250,500,1000,2000,-1};
                    status = "Saved.";
                    if (vr && selected == 0) { vrScale = std::clamp(vrScale + direction * 5, 50, 200); status = "Eye resolution saved. Restart to apply."; }
                    if (vr && selected == 1) {
                        const auto rates = window.RefreshRates();
                        if (!rates.empty()) {
                            auto it = std::find(rates.begin(), rates.end(), vrRefresh);
                            const int index = it == rates.end() ? 0 : int(it - rates.begin());
                            const int rate = rates[size_t((index + direction + int(rates.size())) % int(rates.size()))];
                            if (window.SetRefreshRate(rate)) vrRefresh = rate; else status = "Headset refused refresh rate; unchanged.";
                        } else status = "Headset does not offer refresh-rate selection.";
                    }
                    if (!vr && selected == 0) {
                        const int resolutions[] = {50,75,100,125,150,175,200,-720,-1080,-1440,-2160};
                        const int choice = Cycle(graphics.renderHeight ? -graphics.renderHeight : graphics.renderScale, resolutions, direction);
                        graphics.renderHeight = choice < 0 ? -choice : 0;
                        if (choice > 0) graphics.renderScale = choice;
                    }
                    if (!vr && selected == 1) {
                        const int caps[] = {0,30,60,72,80,90,120,144,165,240};
                        graphics.frameCap = Cycle(graphics.frameCap, caps, direction);
                    }
                    if (selected == 2) graphics.msaa = Cycle(graphics.msaa, samples, direction);
                    if (selected == 3) graphics.drawDistance = Cycle(graphics.drawDistance, distances, direction);
                    if (selected == 4) rumble = std::clamp(rumble + direction * 5, 0, 100);
                    if (selected == 5) graphics.smoothTextures = !graphics.smoothTextures;
                    if (selected == 6) profiler = !profiler;
                    if (vr && selected == 7) { foveation = (foveation + direction + 4) % 4; window.Renderer().SetFoveation(foveation); status = "Saved. Centre and HUD stay full resolution."; }
                    if (selected == 8 && direction) { gt2::hd::SetEnabled(!gt2::hd::Enabled()); status = "Saved. Media changes apply on resume / next movie."; }
                    if (selected == 9 && direction) { playStationIntro = !playStationIntro; status = "Saved. Applies on next launch."; }
                    if (!vr && selected == 7 && accept) { page = 8; selected = 0; }
                    else if (selected == 10) { page = 0; selected = 0; }
                    SetGraphicsFromOverlay(graphics); window.Renderer().SetOptions(RenderOptionsOf(graphics));
                    window.Input().SetRumbleScale(rumble);
                } else if (page == 8) {
                    if (selected == 0) graphics.vsync = !graphics.vsync;
                    if (selected == 1) graphics.frameRate = !graphics.frameRate;
                    if (selected == 2) { page = 1; selected = 7; }
                    SetGraphicsFromOverlay(graphics); window.Renderer().SetOptions(RenderOptionsOf(CurrentGraphics()));
                    status = "Saved.";
                } else if (page == 2) {
                    if (selected == 0 && direction) gt2::pc::SetUnlocks(!gt2::pc::unlockCourses, gt2::pc::unlockCars);
                    if (selected == 1 && direction) gt2::pc::SetUnlocks(gt2::pc::unlockCourses, !gt2::pc::unlockCars);
                    if (selected == 2 && accept) status = ApplySimulationCheat(0);
                    if (selected == 3 && accept) status = ApplySimulationCheat(1);
                    if (selected == 4 && accept) {
                        if (!cheatData) status = "Enter GT Mode first. Catalogue unavailable in races.";
                        else {
                            if (catalogueData != cheatData) {
                                catalogue = gt2::career::CheatCatalogue(*cheatData); catalogueData = cheatData;
                            }
                            if (!catalogue.empty()) { page = 3; selected = carPage = 0; status = "A: add free car. Triggers: pages. X/Y: row. B: back."; }
                        }
                    }
                    if (selected == 5 && page == 2 && direction) gt2::pc::unlockSimulationEvents = !gt2::pc::unlockSimulationEvents;
                    if (selected == 6 && page == 2) { page = 0; selected = 0; }
                } else if (page == 4) {
                    if (selected == count - 1) { page = 0; selected = 0; }
                    else if (selected == count - 2) { hudPage = 1 - hudPage; selected = 0; }
                    else if (selected == 0 && direction) { metricUnits = !OverlayMetricUnits(baseMetricUnits); status = "Saved. Speed units apply on resume."; }
                    else if (direction) { hudVisibility.*hudSettings[size_t(hudPage) * 6 + size_t(selected - 1)].member ^= true; status = "Saved. HUD changes apply on resume."; }
                }
#ifdef _WIN32
                else if (page == 6 && accept && selected == (vr ? 5 : 6)) {
                    ShowWheelSettings(window);
                } else if (page == 6 && accept && selected == (vr ? 6 : 7)) { page = 0; selected = 0; }
#endif
                else if (page == 6 && !vr) {
                    if (selected == 0 && direction) desktopCustom = !desktopCustom;
                    else if (selected == 1 && accept) { page = 7; selected = controlPage = 0; }
                    else if (selected == 2 && direction) desktopBindings.brakeReverse = !desktopBindings.brakeReverse;
                    else if (selected == 3 && direction) desktopBindings.steeringStick = 1 - desktopBindings.steeringStick;
                    else if (selected == 4 && accept) { desktopCustom = false; desktopBindings = gt2::vr::DesktopBindings(); }
                    else if (selected == 5 && direction) adaptive = std::clamp(adaptive + direction * 10, 0, 100);
                    else if (selected == 6 && accept) { page = 0; selected = 0; }
                    status = "Saved. Original game bindings remain available.";
                } else if (page == 6) {
                    if (selected==0 && accept) {page=5;selected=0;}
                    else if (selected==1 && accept) {page=7;selected=controlPage=0;}
                    else if (selected==2 && direction) controlBindings.brakeReverse = !controlBindings.brakeReverse;
                    else if (selected==3 && direction) controlBindings.steeringStick = 1-controlBindings.steeringStick;
                    else if (selected==4 && accept) {controlBindings={};drivingSettings={};status="Default controls restored.";}
                    else if (selected==5 && accept) {page=0;selected=0;}
                } else if (page == 7) {
                    if (selected<4 && direction) {
                        auto& binding=(vr ? controlBindings : desktopBindings).source[size_t(controlPage*4+selected)];
                        const int sources = vr ? int(std::size(gt2::vr::controlSources)) : int(std::size(gt2::vr::desktopControlSources));
                        binding=(binding+direction+sources)%sources;
                        if (!vr) desktopCustom = true;
                        status="Saved. Bindings affect driving; menu controls stay fixed.";
                    } else if (selected==4) {controlPage=1-controlPage;selected=0;}
                    else if (selected==5) {page=6;selected=0;}
                } else if (page == 5) {
                    if (selected == 0 && direction) drivingSettings.mode = (drivingSettings.mode + direction + 3) % 3;
                    if (selected == 1 && direction) drivingSettings.motionHand = 1-drivingSettings.motionHand;
                    if (selected == 2 && direction) drivingSettings.wheelHeightCm = std::clamp(drivingSettings.wheelHeightCm+direction*2,-60,0);
                    if (selected == 3 && direction) drivingSettings.wheelDistanceCm = std::clamp(drivingSettings.wheelDistanceCm+direction*2,20,75);
                    if (selected == 4 && direction) drivingSettings.wheelRadiusCm = std::clamp(drivingSettings.wheelRadiusCm+direction,8,40);
                    if (selected == 5 && direction) introLowerCm = std::clamp(introLowerCm+direction*50,0,2000);
                    if (selected == 6 && accept) { page = 6; selected = 0; }
                    status = "Saved. Wheel: grip rim. Motion: hold grip to steer.";
                } else if (page == 3) {
                    if (accept) status = ApplySimulationCheat(2, catalogue[size_t(carPage) * 8 + size_t(selected)]);
                    else { const int pages = int((catalogue.size() + 7) / 8); carPage = (carPage + direction + pages) % pages; selected = 0; }
                }
                try { Save(); } catch (const std::exception& e) { status = std::string("Settings not saved: ") + e.what(); }
                continue;
            }
        }
        first = false;
        window.Input().StopFeedback();
        Panel(window, title, rows, selected, page == 9 ? "Unsaved progress will be lost. Save in the game first." : status,
              vr ? "Stick up/down: row   Triggers: value   A: open   B: back" : "Up/down: row  Left/right: value  Enter: open Esc: back");
    }
    window.Input().StopFeedback(); window.ResetPacing();
    std::printf("overlay: closed at field %d\n", window.Field());
}
}

void SetSimulationCheatContext(gt2::career::CareerSave* save, const gt2::career::CareerData* data, const std::string& path) {
    cheatSave = save; cheatData = data; cheatPath = path;
    if (!data) catalogueData = nullptr;
}
void PrepareNativeUi(gt2view::VkSceneRenderer& renderer) { UploadFont(renderer); }
bool FrameProfilerEnabled() { return profiler; }
void AppendFrameProfiler(gt2view::VkSceneRenderer& renderer, std::vector<gt2view::DrawItem>& items, const FrameProfiler& stats) {
    NativeCanvas canvas;
    canvas.Quad(492, 4, 284, 101, 0x101c2a);
    char fps[48], timing[64];
    if (stats.fps > 0) {
        std::snprintf(fps, sizeof(fps), "APP %.1f FPS", stats.fps);
        std::snprintf(timing, sizeof(timing), "%.1f MS / MAX %.1f MS", stats.frameMs, stats.maxMs);
    } else {
        std::snprintf(fps, sizeof(fps), "APP -- FPS");
        std::snprintf(timing, sizeof(timing), "-- MS / MAX -- MS");
    }
    canvas.Text(534, 7, fps, 0x8ed8f8, 0.7f);
    canvas.Text(534, 26, timing, 0xe2e8f0, 0.65f);
    const auto& cull = renderer.LastCulling();
    char geometry[64];
    std::snprintf(geometry, sizeof(geometry), "CULL %u / %u DRAWS", cull.culled, cull.tested);
    canvas.Text(534, 45, geometry, 0xe2e8f0, 0.65f);
    char gpu[64];
    const uint32_t cached = renderer.CachedMaterials(), total = cached + renderer.UncachedMaterials();
    if (renderer.GpuMilliseconds() >= 0) std::snprintf(gpu, sizeof(gpu), "GPU %.2f MS / TEX %u%%", renderer.GpuMilliseconds(), total ? cached * 100 / total : 0);
    else std::snprintf(gpu, sizeof(gpu), "GPU -- MS");
    canvas.Text(534, 64, gpu, 0x8ed8f8, 0.65f);
    char low[64];
    std::snprintf(low, sizeof(low), "1%% LOW %.0f / PEAK %.0f MS", stats.lowFps, stats.recentMaxMs);
    canvas.Text(504, 83, low, stats.recentMaxMs > 22.3 ? 0xff9866 : 0xe2e8f0, 0.65f);
    canvas.Append(renderer, items, gt2view::VkSceneRenderer::kProfilerVertexBase);
}
void AppendSkipHint(gt2view::VkSceneRenderer& renderer, std::vector<gt2view::DrawItem>& items) {
    if (!hudVisibility.movieHint) return;
    NativeCanvas canvas; canvas.Quad(244, 550, 312, 34, 0x101c2a);
    canvas.Text(256, 555, VrMode() ? "A / B : SKIP MOVIE" : "START / ENTER : SKIP"); canvas.Append(renderer, items);
}
bool ConsoleStartupHandled() { return consoleStartupHandled; }
bool PlayStationIntroEnabled() { return playStationIntro; }
bool TakeGameChangeRequest() {
    const bool requested = gameChangeRequested;
    gameChangeRequested = false;
    return requested;
}
std::string SelectGameDisc(const std::string& root, const std::string& preferred, bool vrMode,
                          bool deterministic, const std::string& script, const std::string& shot, bool sound, bool playStartup) {
    std::vector<std::string> modes, rows;
    for (const std::string mode : {"arcade", "simulation"}) {
        if (std::filesystem::exists(std::filesystem::path(root) / mode / "disc.raw2352")) {
            modes.push_back(mode); rows.push_back(mode == "arcade" ? "ARCADE DISC - Quick races and time trials" : "SIMULATION DISC - GT Mode career");
        }
    }
    if (modes.empty()) throw std::runtime_error("No installed GT2 disc found");
    gameChangeAvailable = modes.size() > 1;
    gameChangeRequested = false;
    SetVrMode(vrMode, deterministic); VrOptions vr; vr.stereo = false; SetVrOptions(vr);
    GameWindow window("Gran Turismo 2 - Choose disc", 1280, 960); window.EnableNativeMenu(false); PrepareNativeUi(window.Renderer());
    if (!script.empty()) window.AddScript(script);
    if (!shot.empty()) window.AddShot(2, shot);
    consoleStartupHandled = true;
    auto startup = std::filesystem::path(root) / "startup-hd.gtm";
    if(!gt2::hd::Enabled() || !std::filesystem::is_regular_file(startup)) startup=std::filesystem::path(root)/"startup.gtm";
    if (playStartup && playStationIntro && std::filesystem::is_regular_file(startup)) {
        MovieSpec spec; spec.skippable = false; spec.displayWidth = 640; spec.displayHeight = 480; spec.x = spec.y = 0;
        std::printf("player: PlayStation intro %s\n", startup.string().c_str());
        try { if (PlayPreparedMovie(window, startup.string(), spec, sound) == MovieResult::kClosed) return {}; }
        catch (const std::exception& e) { std::printf("BIOS intro unavailable: %s\n", e.what()); }
    }
    int selected = modes.size() == 2 && preferred == modes[1] ? 1 : 0;
    std::printf("player: disc picker (%s), %zu installed disc(s)\n", vrMode ? "VR" : "desktop", modes.size());
    while (window.BeginFrame()) {
        if (window.Pressed(gt2::keys::kEscape) || window.Pressed(gt2::keys::kBack)) return {};
        if (window.Pressed(gt2::keys::kUp) || window.Pressed(gt2::keys::kDown)) selected = (selected + 1) % int(modes.size());
        if (window.Pressed(gt2::keys::kReturn)) {
            std::printf("player: selected %s\n", modes[size_t(selected)].c_str());
            window.RetainBackendForNextWindow(); return modes[size_t(selected)];
        }
        Panel(window, "GRAN TURISMO 2 / SELECT DISC", rows, selected, "Choose your disc for this session.",
              vrMode ? "Left stick: select   A: start   B: exit" : "Up/down: select   Enter / A: start   Esc: exit");
    }
    return {};
}

void ShowPcOverlay(GameWindow& window, const std::vector<gt2view::DrawItem>&, size_t) {
    ShowSettingsMenu(window);
}
}
