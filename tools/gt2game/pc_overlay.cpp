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
int foveation = 2;
int metricUnits = -1;
bool baseMetricUnits = true;
gt2view::HudVisibility hudVisibility;
gt2::vr::DrivingSettings drivingSettings;
gt2::vr::ControlBindings controlBindings;
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
    adaptive = 60; rumble = 50; vrScale = -1; vrRefresh = 72; profiler = true; foveation = 2;
    hudVisibility = {}; drivingSettings = {}; controlBindings = {}; introLowerCm = 200;
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

void ShowVrMenu(GameWindow& window) {
    const gt2::audio::ScopedMixPause pause;
    PrepareNativeUi(window.Renderer()); window.Input().StopFeedback();
    if (vrScale < 0) vrScale = int(VrOptionsInUse().renderScale * 100 + 0.5f);
    int page = 0, selected = 0, carPage = 0, hudPage = 0, controlPage = 0;
    gt2::vr::MenuTriggers triggers;
    triggers.Begin(window.Pad().pressureL2 / 255.f, window.Pad().pressureR2 / 255.f);
    bool first = true, done = false;
    std::string status = "GAME PAUSED - settings save automatically.";
    while (!done && !window.Closed()) {
        auto graphics = CurrentGraphics();
        std::vector<std::string> rows;
        std::string title = "GT2 VR / MENU";
        if (page == 0) rows = {"Graphics and performance", "Cheats", "HUD elements", "Controls", "Original game pause / exit", "Resume game"};
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
        if (page == 7) {
            title = "GT2 VR / BINDINGS / PAGE " + std::to_string(controlPage+1);
            for (int i=controlPage*4;i<controlPage*4+4;++i) rows.push_back(std::string(gt2::vr::controlNames[i])+": "+gt2::vr::controlSources[controlBindings.source[i]]);
            rows.push_back(controlPage ? "Previous bindings" : "More bindings"); rows.push_back("Back");
        }
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
        if (!first) {
            if (!window.BeginFrame()) break;
            if (window.PadPressed(gt2::input::ps1::kStart) || window.Pressed(gt2::keys::kF10)) break;
            if (window.Pressed(gt2::keys::kBack) || window.Pressed(gt2::keys::kEscape)) {
                if (page == 0) break;
                page = page == 3 ? 2 : (page == 5 || page == 7) ? 6 : 0; selected = 0; continue;
            }
            const int count = int(rows.size());
            if (window.PadPressed(gt2::input::ps1::kSquare) || window.Pressed(gt2::keys::kUp)) selected = (selected + count - 1) % count;
            if (window.PadPressed(gt2::input::ps1::kR1) || window.Pressed(gt2::keys::kDown)) selected = (selected + 1) % count;
            const bool accept = window.Pressed(gt2::keys::kReturn);
            const int direction = triggers.Update(window.Pad().pressureL2 / 255.f, window.Pad().pressureR2 / 255.f);
            if (direction || accept) {
                if (page == 0 && accept) {
                    if (selected >= 4) { if (selected == 4) window.RequestGamePause(); done = true; }
                    else { page = selected == 3 ? 6 : selected == 2 ? 4 : selected + 1; selected = 0; }
                }
                else if (page == 1 && (direction || selected == 10)) {
                    const int samples[] = {1,2,4,8}, distances[] = {0,250,500,1000,2000,-1};
                    status = "Saved.";
                    if (selected == 0) { vrScale = std::clamp(vrScale + direction * 5, 50, 200); status = "Eye resolution saved. Restart to apply."; }
                    if (selected == 1) {
                        const auto rates = window.RefreshRates();
                        if (!rates.empty()) {
                            auto it = std::find(rates.begin(), rates.end(), vrRefresh);
                            const int index = it == rates.end() ? 0 : int(it - rates.begin());
                            const int rate = rates[size_t((index + direction + int(rates.size())) % int(rates.size()))];
                            if (window.SetRefreshRate(rate)) vrRefresh = rate; else status = "Headset refused refresh rate; unchanged.";
                        } else status = "Headset does not offer refresh-rate selection.";
                    }
                    if (selected == 2) graphics.msaa = Cycle(graphics.msaa, samples, direction);
                    if (selected == 3) graphics.drawDistance = Cycle(graphics.drawDistance, distances, direction);
                    if (selected == 4) rumble = std::clamp(rumble + direction * 5, 0, 100);
                    if (selected == 5) graphics.smoothTextures = !graphics.smoothTextures;
                    if (selected == 6) profiler = !profiler;
                    if (selected == 7) { foveation = (foveation + direction + 4) % 4; window.Renderer().SetFoveation(foveation); status = "Saved. Centre and HUD stay full resolution."; }
                    if (selected == 8 && direction) { gt2::hd::SetEnabled(!gt2::hd::Enabled()); status = "Saved. Media changes apply on resume / next movie."; }
                    if (selected == 9 && direction) { playStationIntro = !playStationIntro; status = "Saved. Applies on next launch."; }
                    if (selected == 10) { page = 0; selected = 0; }
                    SetGraphicsFromOverlay(graphics); window.Renderer().SetOptions(RenderOptionsOf(graphics));
                    window.Input().SetRumbleScale(rumble);
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
                } else if (page == 6) {
                    if (selected==0 && accept) {page=5;selected=0;}
                    else if (selected==1 && accept) {page=7;selected=controlPage=0;}
                    else if (selected==2 && direction) controlBindings.brakeReverse = !controlBindings.brakeReverse;
                    else if (selected==3 && direction) controlBindings.steeringStick = 1-controlBindings.steeringStick;
                    else if (selected==4 && accept) {controlBindings={};drivingSettings={};status="Default controls restored.";}
                    else if (selected==5 && accept) {page=0;selected=0;}
                } else if (page == 7) {
                    if (selected<4 && direction) {
                        auto& binding=controlBindings.source[size_t(controlPage*4+selected)];
                        binding=(binding+direction+int(std::size(gt2::vr::controlSources)))%int(std::size(gt2::vr::controlSources));
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
        Panel(window, title, rows, selected, status, "Stick up/down: row   Triggers: value   A: open   B: back");
    }
    window.Input().StopFeedback(); window.ResetPacing();
}
}

void SetSimulationCheatContext(gt2::career::CareerSave* save, const gt2::career::CareerData* data, const std::string& path) {
    cheatSave = save; cheatData = data; cheatPath = path;
    if (!data) catalogueData = nullptr;
}
void PrepareNativeUi(gt2view::VkSceneRenderer& renderer) { UploadFont(renderer); }
bool FrameProfilerEnabled() { return profiler && VrMode(); }
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
std::string SelectQuestDisc(const std::string& root, const std::string& preferred) {
    std::vector<std::string> modes, rows;
    for (const std::string mode : {"arcade", "simulation"}) {
        if (std::filesystem::exists(std::filesystem::path(root) / mode / "disc.raw2352")) {
            modes.push_back(mode); rows.push_back(mode == "arcade" ? "ARCADE DISC - Quick races and time trials" : "SIMULATION DISC - GT Mode career");
        }
    }
    if (modes.empty()) throw std::runtime_error("No installed GT2 disc found");
    SetVrMode(true, false); VrOptions vr; vr.stereo = false; SetVrOptions(vr);
    GameWindow window("GT2 VR - Choose disc", 1280, 960); window.EnableNativeMenu(false); PrepareNativeUi(window.Renderer());
    auto startup = std::filesystem::path(root) / "startup-hd.gtm";
    if(!gt2::hd::Enabled() || !std::filesystem::is_regular_file(startup)) startup=std::filesystem::path(root)/"startup.gtm";
    if (playStationIntro && std::filesystem::is_regular_file(startup)) {
        MovieSpec spec; spec.skippable = false; spec.displayWidth = 640; spec.displayHeight = 480; spec.x = spec.y = 0;
        try { if (PlayPreparedMovie(window, startup.string(), spec, true) == MovieResult::kClosed) return {}; }
        catch (const std::exception& e) { std::printf("BIOS intro unavailable: %s\n", e.what()); }
    }
    int selected = modes.size() == 2 && preferred == modes[1] ? 1 : 0;
    while (window.BeginFrame()) {
        if (window.Pressed(gt2::keys::kUp) || window.Pressed(gt2::keys::kDown)) selected = (selected + 1) % int(modes.size());
        if (window.PadPressed(gt2::input::ps1::kCross)) { window.RetainBackendForNextWindow(); return modes[size_t(selected)]; }
        Panel(window, "GRAN TURISMO 2 / SELECT DISC", rows, selected, "Choose your disc for this session.", "Left stick: select   A: start");
    }
    return {};
}

void ShowPcOverlay(GameWindow& window, const std::vector<gt2view::DrawItem>& background, size_t sceneCount) {
    if (VrMode()) { ShowVrMenu(window); return; }
    using namespace gt2view;
    const gt2::audio::ScopedMixPause audioPause;
    auto& renderer = window.Renderer();
    UploadFont(renderer);
    auto graphics = CurrentGraphics();
    if (VrMode() && vrScale < 0) vrScale = int(VrOptionsInUse().renderScale * 100.0f + 0.5f);
    int selected = 0;
    std::string status = "Changes apply immediately and are saved.";
    window.Input().StopFeedback();
    std::printf("overlay: opened at field %d\n", window.Field());
    auto change = [&](int direction) {
        const int resolutions[] = {50, 75, 100, 125, 150, 200, -720, -1080, -1440, -2160}, samples[] = {1, 2, 4, 8}, caps[] = {0, 30, 60, 90, 120, 144, 165, 240};
        const int distances[] = {0, 250, 500, 1000, 2000, -1};
        if (selected <= 6 && GraphicsPinned()) { status = "Graphics locked by command-line flags."; return; }
        if (VrMode() && (selected == 2 || selected == 4 || selected == 8)) {
            status = selected == 8 ? "Touch has no adaptive triggers." : "Display timing is controlled by the headset."; return;
        }
        switch (selected) {
        case 0: {
            if (VrMode()) { vrScale = std::clamp(vrScale + direction * 5, 50, 200); break; }
            const int choice = Cycle(graphics.renderHeight ? -graphics.renderHeight : graphics.renderScale, resolutions, direction);
            graphics.renderHeight = choice < 0 ? -choice : 0;
            if (choice > 0) graphics.renderScale = choice;
            break;
        }
        case 1: graphics.frameRate = graphics.frameRate == 0 ? 1 : 0; break;
        case 2: graphics.frameCap = Cycle(graphics.frameCap, caps, direction); break;
        case 3: graphics.msaa = Cycle(graphics.msaa, samples, direction); break;
        case 4: graphics.vsync = !graphics.vsync; break;
        case 5: graphics.smoothTextures = !graphics.smoothTextures; break;
        case 6: graphics.drawDistance = Cycle(graphics.drawDistance, distances, direction); break;
        case 7: rumble = std::clamp(rumble + direction * 5, 0, 100); break;
        case 8: adaptive = std::clamp(adaptive + direction * 10, 0, 100); break;
        case 9: gt2::pc::SetUnlocks(!gt2::pc::unlockCourses, gt2::pc::unlockCars); break;
        case 10: gt2::pc::SetUnlocks(gt2::pc::unlockCourses, !gt2::pc::unlockCars); break;
        }
        SetGraphicsFromOverlay(graphics);
        renderer.SetOptions(RenderOptionsOf(CurrentGraphics()));
        if (rumble >= 0) window.Input().SetRumbleScale(rumble);
        try { Save(); status = VrMode() && selected == 0 ? "Eye resolution saved. Restart the game to apply." : "Saved. Arcade unlocks refresh the selection menu."; }
        catch (const std::exception& e) { status = std::string("Save failed: ") + e.what(); }
        std::printf("overlay: %s; %s\n", DescribeGraphics(CurrentGraphics()).c_str(), status.c_str());
    };
    // The caller is suspended here. The scene, physics, race timer and menus keep their current state.
    bool first = true;
    while (!window.Closed()) {
        if (!first) {
            if (!window.BeginFrame()) break;
            if (window.Pressed(gt2::keys::kF10) || window.Pressed(gt2::keys::kEscape) || window.Pressed(gt2::keys::kBack) ||
                (window.PadHeld(gt2::input::ps1::kSelect) && window.PadPressed(gt2::input::ps1::kStart))) break;
            if (window.Pressed(gt2::keys::kUp)) selected = (selected + 10) % 11;
            if (window.Pressed(gt2::keys::kDown)) selected = (selected + 1) % 11;
            if (window.Pressed(gt2::keys::kLeft)) change(-1);
            if (window.Pressed(gt2::keys::kRight) || window.Pressed(gt2::keys::kReturn)) change(1);
        }
        first = false;
        window.Input().StopFeedback();
        std::vector<SceneVertex> vertices;
        auto quad = [&](float x, float y, float w, float h, uint32_t color, int glyph) {
            const int cornerX[] = {0, 1, 1, 0, 1, 0}, cornerY[] = {0, 0, 1, 0, 1, 1};
            for (int k = 0; k < 6; ++k) {
                SceneVertex v{};
                v.pos[0] = (x + float(cornerX[k]) * w) / 800.0f * 2 - 1;
                v.pos[1] = (y + float(cornerY[k]) * h) / 600.0f * 2 - 1;
                v.pos[2] = 1;
                v.color[0] = float((color >> 16) & 255) / 255; v.color[1] = float((color >> 8) & 255) / 255; v.color[2] = float(color & 255) / 255;
                v.flags = 0;
                if (glyph >= 0) {
                    v.flags |= kTextured | (2u << 8);
                    v.page = fontRow << 16;
                    v.texel[0] = float((glyph % 16) * 16 + cornerX[k] * 16);
                    v.texel[1] = float((glyph / 16) * 24 + cornerY[k] * 24);
                }
                vertices.push_back(v);
            }
        };
        auto text = [&](float x, float y, const std::string& s, uint32_t color = 0xE2E8F0) {
            for (unsigned char c : s) { if (c >= 32 && c < 128) quad(x, y, 16, 24, color, int(c) - 32); x += 12; }
        };
        quad(38, 28, 724, 548, 0x101C2A, -1);
        quad(38, 28, 724, 5, 0x42B6F5, -1);
        text(64, 45, VrMode() ? "GT2 / VR SETTINGS" : "GT2 / PC SETTINGS", 0xFFFFFF);
        text(64, 74, "GAME PAUSED", 0x69CCF5);
        const auto extent = renderer.Extent();
        const auto scale = float(graphics.renderScale) / 100;
        const std::string resolution = graphics.renderHeight ?
            std::to_string(graphics.renderHeight * 16 / 9) + "x" + std::to_string(graphics.renderHeight) + (graphics.renderHeight == 2160 ? " / 4K" : " / fixed") :
            std::to_string(int(float(extent.width) * scale)) + "x" + std::to_string(int(float(extent.height) * scale)) + " / " + std::to_string(graphics.renderScale) + "%";
        const std::string on = "ON", off = "OFF";
        std::vector<std::pair<std::string, std::string>> rows = {
            {VrMode() ? "Eye scale (restart)" : "Internal resolution", VrMode() ? std::to_string(vrScale) + "%" : resolution},
            {"Frame presentation", graphics.frameRate == 0 ? "Original 30" : "Interpolated"},
            {"FPS limit", VrMode() ? "Headset timing" : graphics.frameCap ? std::to_string(graphics.frameCap) : "Display / uncapped"},
            {"MSAA", std::to_string(graphics.msaa) + "x (GPU " + std::to_string(renderer.EffectiveMsaa()) + "x)"},
            {"VSync", VrMode() ? "Headset timing" : graphics.vsync ? on : off}, {"Texture filtering", graphics.smoothTextures ? "Smooth + mipmaps" : "Original"},
            {"Draw distance", graphics.drawDistance < 0 ? "Entire course" : graphics.drawDistance == 0 ? "Original" : std::to_string(graphics.drawDistance) + " m"},
            {"Vibration strength", std::to_string(rumble) + "%"},
            {"Adaptive pedals", VrMode() ? "Not available on Touch" : std::to_string(adaptive) + "%"},
            {"Cheat: all arcade tracks", gt2::pc::unlockCourses ? on : off}, {"Cheat: all arcade cars", gt2::pc::unlockCars ? on : off}
        };
        for (size_t i = 0; i < rows.size(); ++i) {
            const float y = 115 + float(i) * 29;
            if (int(i) == selected) quad(55, y - 2, 690, 29, 0x234B68, -1);
            text(65, y, rows[i].first); text(438, y, rows[i].second, 0x8ED8F8);
        }
        text(64, 449, VrMode() ? "Touch: left stick steering, triggers gas / brake" : window.Input().Port1HasTriggers() ? "DualSense HID connected" : "Adaptive pedals need a native DualSense", 0xA8B7C8);
        text(64, 479, status.substr(0, 56), 0xA8B7C8);
        text(64, 514, "Arrows / D-pad: change   F10 / Back: close", 0xFFFFFF);
        text(64, 543, VrMode() ? "Hold left stick click + press Menu for settings" : "Pad shortcut: hold Create + press Options", 0xA8B7C8);
        renderer.SetVertices(vertexBase, vertices);
        DrawItem layer; layer.firstVertex = vertexBase; layer.vertexCount = uint32_t(vertices.size());
        layer.mvp[0] = layer.mvp[5] = layer.mvp[10] = layer.mvp[15] = 1;
        auto items = background; items.push_back(layer);
        window.EndFrame(items, {}, std::chrono::nanoseconds(16'666'667), sceneCount);
    }
    window.Input().StopFeedback();
    window.ResetPacing();
    std::printf("overlay: closed at field %d\n", window.Field());
}
}
