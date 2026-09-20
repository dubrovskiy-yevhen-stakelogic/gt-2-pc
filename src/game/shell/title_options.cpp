#include "game/shell/title_options.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace gt2::shell {

namespace {

uint8_t* Bytes(career::CareerState& s) { return reinterpret_cast<uint8_t*>(&s); }
const uint8_t* Bytes(const career::CareerState& s) { return reinterpret_cast<const uint8_t*>(&s); }

// The byte of ids 0..13 (0x80017D74 / 0x80017E68 jump tables); id 14 is the pair +0x36 / +0x88.
constexpr uint16_t kOptionOffset[14] = {0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4};
constexpr uint16_t kVibrationPad1 = 0x36, kVibrationPad2 = 0x88; // pad block (0x52 bytes at +0x0A / +0x5C) + 0x2C

} // namespace

std::vector<OptionRow> ReadOptionRows(const GuestImage& ovl1, uint32_t address, int count) {
    std::vector<OptionRow> rows;
    for (int i = 0; i < count; i++) {
        const uint32_t a = address + uint32_t(i) * 0x14;
        OptionRow r;
        r.id = ovl1.Get<uint8_t>(a);
        r.kind = ovl1.Get<uint8_t>(a + 1);
        r.min = ovl1.Get<int16_t>(a + 2);
        r.max = ovl1.Get<int16_t>(a + 4);
        r.labelX = ovl1.Get<int16_t>(a + 6);
        r.valueX = ovl1.Get<int16_t>(a + 8);
        r.label = ovl1.Get<uint32_t>(a + 0x0C);
        r.values = ovl1.Get<uint32_t>(a + 0x10);
        if (r.kind == kChoice && r.values != 0)
            for (int k = 0; k < r.max; k++) r.valueLabels.push_back(ovl1.Get<uint32_t>(r.values + uint32_t(k) * 4));
        rows.push_back(std::move(r));
    }
    return rows;
}

int OptionValue(const career::CareerState& state, uint8_t id) {
    const uint8_t* b = Bytes(state);
    if (id >= 15) return 0; // lb id; sltiu 15 (negative ids included): 0
    if (id == kVibration) return b[kVibrationPad1] == 0 ? 1 : 0;
    const uint8_t v = b[kOptionOffset[id]];
    if (id == kBattleHandicap || id == kBattleBoost) return int(int8_t(v)); // lb +7 / +8
    return int(v);
}

void SetOptionValue(career::CareerState& state, uint8_t id, int value) {
    uint8_t* b = Bytes(state);
    if (id >= 15) return;
    if (id == kVibration) {
        b[kVibrationPad1] = b[kVibrationPad2] = value != 1 ? 1 : 0;
        return;
    }
    if (id == kArcadeDamage || id == kBattleDamage) b[kOptionOffset[id]] = value != 0 ? 1 : 0;
    else b[kOptionOffset[id]] = uint8_t(value);
}

bool StepOption(career::CareerState& state, const OptionRow& row, int step, int fast) {
    int old = 0, v = 0;
    if (row.kind == kSlider) {
        old = OptionValue(state, row.id);
        v = old + step + fast;
    } else if (row.kind == kVolume) {
        old = OptionValue(state, row.id);
        const int g = OptionValue(state, row.id);
        int t = g + 15;
        if (t < 0) t = g + 30; // the rounding of a signed division by 16 (never negative here)
        v = ((t >> 4) << 4) + (step << 4);
    } else {
        if (row.kind == kLaps) step += fast;
        old = v = OptionValue(state, row.id);
        v += step;
    }
    if (v < row.min) v = row.min;
    if (row.max <= v) v = row.max - 1;
    SetOptionValue(state, row.id, v);
    return old != v;
}

OptionInput OptionRowInput(career::CareerState& state, const OptionRow& row, const MenuListPad& pad) {
    int step = 0, fast = 0;
    const uint32_t edges = pad.pressed | pad.repeat;
    if (edges & menu_list_pad::kLeft) step = -1;
    if (edges & menu_list_pad::kRight) step = 1;
    if (pad.held & menu_list_pad::kL1) fast = -1;
    if (pad.held & menu_list_pad::kR1) fast = 1;
    OptionInput r;
    if (step == 0 && fast == 0) return r;
    r.changed = StepOption(state, row, step, fast);
    r.sound = r.changed && step != 0;
    return r;
}

GameOptions ReadGameOptions(const career::CareerState& state) {
    GameOptions o;
    o.musicVolume = uint8_t(OptionValue(state, kMusicVolume));
    o.sfxVolume = uint8_t(OptionValue(state, kSfxVolume));
    o.courseMap = OptionValue(state, kCourseMap) != 0;
    o.viewAngle = uint8_t(OptionValue(state, kViewAngle));
    o.chaseView = uint8_t(OptionValue(state, kChaseView));
    o.cameraPosition = uint8_t(OptionValue(state, kCameraPosition));
    o.replayInfo = uint8_t(OptionValue(state, kReplayInfo));
    o.vibration = OptionValue(state, kVibration) != 0;
    return o;
}

int16_t ViewAngleProjection(const GuestImage& raceOverlay, uint8_t viewAngle) {
    return raceOverlay.Get<int16_t>(0x8002F370u + uint32_t(viewAngle) * 2);
}

bool GraphicsSettings::Parse(const std::string& key, const std::string& value) {
    const int v = std::atoi(value.c_str());
    auto clamp = [](int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; };
    if (key == "frame_rate") frameRate = value == "original" ? kFrameRateOriginal : kFrameRateDisplay;
    else if (key == "frame_cap") frameCap = clamp(v, 0, 1000);
    else if (key == "vsync") vsync = v != 0;
    else if (key == "render_scale") { renderScale = clamp(v, 50, 200); renderHeight = 0; }
    else if (key == "render_height") renderHeight = v == 720 || v == 1080 || v == 1440 || v == 2160 ? v : 0;
    else if (key == "msaa") msaa = v >= 8 ? 8 : v >= 4 ? 4 : v >= 2 ? 2 : 1;
    else if (key == "texture_filter") smoothTextures = value == "smooth";
    else if (key == "texture_mapping") affine = value == "affine";
    else if (key == "scenery_detail") maxDetail = value == "max";
    else if (key == "draw_distance") drawDistance = value == "all" ? -1 : value == "original" ? 0 : clamp(v, 0, 100000);
    else return false;
    return true;
}

std::string GraphicsSettings::Serialize() const {
    std::ostringstream o;
    o << "frame_rate=" << (frameRate == kFrameRateOriginal ? "original" : "display") << "\n";
    o << "frame_cap=" << frameCap << "\n";
    o << "vsync=" << int(vsync) << "\n";
    o << "render_scale=" << renderScale << "\n";
    o << "render_height=" << renderHeight << "\n";
    o << "msaa=" << msaa << "\n";
    o << "texture_filter=" << (smoothTextures ? "smooth" : "nearest") << "\n";
    o << "texture_mapping=" << (affine ? "affine" : "perspective") << "\n";
    o << "scenery_detail=" << (maxDetail ? "max" : "original") << "\n";
    o << "draw_distance=";
    if (drawDistance < 0) o << "all";
    else if (drawDistance == 0) o << "original";
    else o << drawDistance;
    o << "\n";
    return o.str();
}

bool VrSettings::Parse(const std::string& key, const std::string& value) {
    const int v = std::atoi(value.c_str());
    auto clamp = [](int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; };
    if (key == "vr_stereo") stereo = v != 0;
    else if (key == "vr_multiview") multiview = v != 0;
    else if (key == "vr_horizon_lock") horizonLock = clamp(v, 0, 100);
    else if (key == "vr_world_scale") worldScale = clamp(v, 10, 1000);
    else if (key == "vr_render_scale") renderScale = clamp(v, 50, 200);
    else if (key == "vr_near_mm") nearMm = clamp(v, 10, 1000);
    else if (key == "vr_seat_x") seatMm[0] = clamp(v, -2000, 2000);
    else if (key == "vr_seat_y") seatMm[1] = clamp(v, -2000, 2000);
    else if (key == "vr_seat_z") seatMm[2] = clamp(v, -2000, 2000);
    else return false;
    return true;
}

std::string VrSettings::Serialize() const {
    std::ostringstream o;
    o << "vr_stereo=" << int(stereo) << "\n";
    o << "vr_multiview=" << int(multiview) << "\n";
    o << "vr_horizon_lock=" << horizonLock << "\n";
    o << "vr_world_scale=" << worldScale << "\n";
    o << "vr_render_scale=" << renderScale << "\n";
    o << "vr_near_mm=" << nearMm << "\n";
    o << "vr_seat_x=" << seatMm[0] << "\n";
    o << "vr_seat_y=" << seatMm[1] << "\n";
    o << "vr_seat_z=" << seatMm[2] << "\n";
    return o.str();
}

PcSettings PcSettings::Load(const std::string& path) {
    PcSettings s;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line.empty() || line[0] == '#') continue;
        const std::string key = line.substr(0, eq);
        const int v = std::atoi(line.c_str() + eq + 1);
        if (key == "units") s.metric = line.substr(eq + 1) == "kmh";
        else if (key == "music_volume") s.options.musicVolume = uint8_t(v), s.haveCareerOptions = true;
        else if (key == "sfx_volume") s.options.sfxVolume = uint8_t(v), s.haveCareerOptions = true;
        else if (key == "course_map") s.options.courseMap = v != 0;
        else if (key == "view_angle") s.options.viewAngle = uint8_t(v);
        else if (key == "chase_view") s.options.chaseView = uint8_t(v);
        else if (key == "camera_position") s.options.cameraPosition = uint8_t(v);
        else if (key == "replay_info") s.options.replayInfo = uint8_t(v);
        else if (key == "vibration") s.options.vibration = v != 0;
        else if (key == "trigger_pedals") s.triggerPedals = v != 0;
        else if (key == "rumble_scale") s.rumbleScale = v < 0 ? 0 : v > 100 ? 100 : v;
        else if (s.graphics.Parse(key, line.substr(eq + 1))) {}
        else if (s.vr.Parse(key, line.substr(eq + 1))) {}
        else if ((key == "pad1" || key == "pad2") && line.size() >= eq + 1 + 2 * 0x52) { // hex bytes of the pad block
            std::array<uint8_t, 0x52>& b = s.padBlocks[key == "pad1" ? 0 : 1];
            for (size_t i = 0; i < b.size(); i++) b[i] = uint8_t(std::strtoul(line.substr(eq + 1 + 2 * i, 2).c_str(), nullptr, 16));
            s.havePadBlocks = true;
        }
    }
    return s;
}

void PcSettings::Save(const std::string& path) const {
    std::ostringstream o;
    o << "# gt2game settings (written by the title's options; the career save keeps the original options too)\n";
    o << "units=" << (metric ? "kmh" : "mph") << "\n";
    o << "music_volume=" << int(options.musicVolume) << "\n";
    o << "sfx_volume=" << int(options.sfxVolume) << "\n";
    o << "course_map=" << int(options.courseMap) << "\n";
    o << "view_angle=" << int(options.viewAngle) << "\n";
    o << "chase_view=" << int(options.chaseView) << "\n";
    o << "camera_position=" << int(options.cameraPosition) << "\n";
    o << "replay_info=" << int(options.replayInfo) << "\n";
    o << "vibration=" << int(options.vibration) << "\n";
    o << "trigger_pedals=" << int(triggerPedals) << "\n";
    o << "rumble_scale=" << rumbleScale << "\n";
    o << graphics.Serialize();
    o << vr.Serialize();
    if (havePadBlocks)
        for (size_t p = 0; p < padBlocks.size(); p++) {
            o << "pad" << (p + 1) << "=";
            char hex[3];
            for (uint8_t b : padBlocks[p]) {
                std::snprintf(hex, sizeof(hex), "%02X", b);
                o << hex;
            }
            o << "\n";
        }
    std::ofstream out(path, std::ios::trunc);
    out << o.str();
}

} // namespace gt2::shell
