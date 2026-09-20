// Dev check of CHANGE PARTS against captures of the original (change_parts_check.h).
#include "change_parts_check.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "arcade_post_race.h"
#include "game/career/career_state.h"
#include "game/career/garage.h"
#include "game/career/tuning.h"
#include "game/shell/title_draw.h"
#include "gt2export/png_writer.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/overlay_data.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "gt2view/change_parts.h"
#include "gt2view/race_menus.h"
#include "gt2view/race_result_screens.h"
#include "gt2view/race_session_screens.h"

using namespace gt2;

namespace gt2game {

namespace {

std::vector<uint8_t> ReadAll(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot read " + path);
    std::vector<uint8_t> b;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    return b;
}

// gt2play's script ("field:key[:hold],..."): the page's pad bits pressed in each field.
std::map<int, uint32_t> ParsePresses(const std::string& script) {
    namespace pb = menu_list_pad;
    static const std::map<std::string, uint32_t> kKeys = {
        {"up", pb::kUp}, {"down", pb::kDown}, {"left", pb::kLeft}, {"right", pb::kRight}, {"cross", pb::kCross}, {"circle", pb::kCircle},
        {"triangle", pb::kTriangle}, {"square", pb::kSquare}, {"l1", pb::kL1}, {"r1", pb::kR1}, {"l2", pb::kL2}, {"r2", pb::kR2},
        {"start", pb::kStart}, {"select", pb::kSelect},
    };
    std::map<int, uint32_t> out;
    size_t at = 0;
    while (at < script.size()) {
        size_t end = script.find(',', at);
        if (end == std::string::npos) end = script.size();
        const std::string item = script.substr(at, end - at);
        at = end + 1;
        const size_t c1 = item.find(':');
        if (c1 == std::string::npos) continue;
        const size_t c2 = item.find(':', c1 + 1);
        const std::string key = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        const auto k = kKeys.find(key);
        if (k == kKeys.end()) throw std::runtime_error("change-parts-check: unknown key " + key);
        out[std::atoi(item.c_str())] |= k->second;
    }
    return out;
}

size_t Compare(const MenuCanvas& ours, const std::vector<uint16_t>& vram, const std::string& sidePath) {
    const int W = RaceMenuAssets::kScreenWidth, H = RaceMenuAssets::kScreenHeight;
    std::vector<uint8_t> side(size_t(W) * 3 * H * 4, 255);
    size_t diff = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint16_t o = ours.At(x, y), c = vram[size_t(y) * 1024 + size_t(x)];
            const bool d = (o & 0x7FFF) != (c & 0x7FFF);
            diff += d ? 1 : 0;
            auto put = [&](int column, uint16_t v) {
                uint8_t* p = &side[(size_t(y) * W * 3 + size_t(column * W + x)) * 4];
                p[0] = uint8_t((v & 31) << 3), p[1] = uint8_t(((v >> 5) & 31) << 3), p[2] = uint8_t(((v >> 10) & 31) << 3);
            };
            put(0, o);
            put(1, c);
            uint8_t* e = &side[(size_t(y) * W * 3 + size_t(2 * W + x)) * 4];
            e[0] = d ? 255 : 0, e[1] = e[2] = 0;
        }
    if (!sidePath.empty()) WritePngRgba(sidePath, W * 3, H, side);
    return diff;
}

} // namespace
} // namespace gt2game

int RunChangePartsCheck(const DiscImage& disc, const GtfsVolume& vol, int argc, char** argv) {
    int at = 0;
    for (int i = 0; i < argc; i++)
        if (std::strcmp(argv[i], "--change-parts-check") == 0) at = i;
    if (at == 0 || at + 5 >= argc) {
        std::puts("usage: gt2game <disc> --change-parts-check <ram.bin> <open field> <script> <out dir> <cap.vram.bin>@<field> ...");
        return 2;
    }
    using namespace gt2game;
    const std::vector<uint8_t> ram = ReadAll(argv[at + 1]);
    if (ram.size() != 0x200000) throw std::runtime_error("change-parts-check: the RAM image is not 2 MB");
    const int openField = std::atoi(argv[at + 2]);
    const std::map<int, uint32_t> presses = ParsePresses(argv[at + 3]);
    const std::string outDir = argv[at + 4];
    std::filesystem::create_directories(outDir);
    struct Capture { std::string path; int field = 0; };
    std::vector<Capture> caps;
    std::string afterPath; // after=<ram.bin>: a dump after the page was left (the commit 0x80056FF0 compared)
    for (int i = at + 5; i < argc; i++) {
        const std::string a = argv[i];
        if (a.rfind("after=", 0) == 0) {
            afterPath = a.substr(6);
            continue;
        }
        const size_t sep = a.rfind('@');
        if (sep == std::string::npos) throw std::runtime_error("change-parts-check: <cap.vram.bin>@<field> expected, got " + a);
        caps.push_back({a.substr(0, sep), std::atoi(a.c_str() + sep + 1)});
    }

    const ExeProfile& profile = ProfileOf(disc);
    auto address = [&](uint32_t sim) -> uint32_t { // a Simulation RAM address in the dump's build
        if (!profile.arcade) return sim;
        const std::optional<uint32_t> a = profile.TryData(sim, -1);
        if (!a) throw std::runtime_error("change-parts-check: no address of the build for 0x" + std::to_string(sim));
        return *a;
    };
    auto at32 = [&](uint32_t a) { return ram.data() + (a & 0x1FFFFF); };
    const RaceMenuAssets assets = profile.arcade ? gt2game::LoadArcadeRaceMenuAssets(disc, vol) : RaceMenuAssets::Load(disc, vol, RaceMenuAssets::Pictures::kSettings);
    std::unique_ptr<career::CareerData> data;
    if (profile.arcade) {
        data = std::make_unique<career::CareerData>(career::CareerData{CarParamTables::Load(vol), CarInfoDirectory::Load(vol), GtModeRaceData::Load(vol), {},
                                                                       LoadExeImage(disc), GuestImage{}});
    } else {
        data = std::make_unique<career::CareerData>(career::CareerData::Load(disc, vol));
    }
    data->strict = false;

    // The dump's state: the sheet 0x8016E894, the race block's garage car and restrictions (0x801D585C + 0x582 ..).
    career::TuneSheet sheet;
    std::memcpy(&sheet, at32(address(career::kSettingsSheetAddress)), sizeof sheet);
    const uint32_t raceBlock = address(0x801D585Cu);
    int16_t garage = 0, index = 0;
    std::memcpy(&garage, at32(raceBlock + 0x582), 2);
    std::memcpy(&index, at32(raceBlock + 0x584), 2);
    screens::ChangePartsContext c;
    c.sheet = &sheet;
    c.data = data.get();
    std::memcpy(&c.powerLimit, at32(raceBlock + 0x586), 2);
    std::memcpy(&c.restrictions, at32(raceBlock + 0x588), 2);
    career::GarageBlock block{};
    if (garage == 0 || garage == 1) { // the home garage (career + 0x3C74) / the guest's (behind the career)
        const uint32_t careerAt = address(career::kStateAddress);
        std::memcpy(&block, at32(careerAt + 0x3C74 + uint32_t(garage) * 0x4028u), sizeof block);
        if (index >= 0 && index < block.count) c.garageCar = &block.cars[index];
    }
    std::printf("change-parts-check: %s, garage %d car %d, power limit %d, flags %u, sheet stages", profile.arcade ? "arcade" : "simulation", int(garage), int(index),
                int(c.powerLimit), unsigned(c.restrictions));
    for (int16_t s : sheet.stage) std::printf(" %d", int(s));
    std::printf("\n");

    // The page from the push field on, in the view manager's stack (0x800474F4) as the session runs it: CHANGE PARTS' L1
    // replaces it with PARTS SETTING (slide 3), PARTS SETTING's R1 with CHANGE PARTS (slide 2); each exit commits (0x80056FF0)
    // into the race slot's configuration, the race record and the garage car; leaving either page ends the run.
    sim::CarParams record{};
    CarConfig slot;
    std::memcpy(&slot, at32(raceBlock + 0x64), sizeof slot);
    std::vector<uint8_t> scratch(0x400);
    auto commit = [&]() {
        career::SettingsCommit out;
        out.raceRecord = &record;
        out.raceSlotConfig = &slot;
        out.garageCar = c.garageCar ? &block.cars[index] : nullptr;
        career::CommitSettings(sheet, data->tables, career::BuildScratch{scratch.data()}, out);
    };
    screens::SessionViewStack stack;
    stack.Start(std::make_unique<screens::ChangePartsView>(assets, c), false);
    int lastField = openField + 1;
    for (const Capture& cap : caps) lastField = std::max(lastField, cap.field + 8);
    if (!afterPath.empty()) lastField = std::max(lastField, openField + 3000); // until the page is left
    std::map<int, std::vector<MenuPrim>> frames;
    int commitFailures = 0;
    for (int field = openField + 1; field <= lastField; field++) {
        MenuListPad pad;
        const auto p = presses.find(field);
        if (p != presses.end()) pad.pressed = pad.held = p->second;
        const int r = stack.Update(&pad);
        bool left = false;
        if (auto* parts = dynamic_cast<screens::ChangePartsView*>(stack.Top()); parts && r != 0) {
            commit();
            std::printf("change-parts-check: f%d CHANGE PARTS left (%s, %d stage(s) selected)\n", field,
                        parts->exit == screens::ChangePartsView::kLeave ? "back" : "L1: PARTS SETTING", parts->changes);
            if (parts->exit == screens::ChangePartsView::kPartsSetting)
                stack.Replace(std::make_unique<screens::PartsSettingView>(assets, c), screens::SessionViewStack::kFromLeft);
            else left = true;
        } else if (auto* settings = dynamic_cast<screens::PartsSettingView*>(stack.Top()); settings && r != 0) {
            commit();
            std::printf("change-parts-check: f%d PARTS SETTING left (%s, %s)\n", field,
                        settings->exit == screens::PartsSettingView::kLeave ? "back" : "R1: CHANGE PARTS", settings->changed ? "settings changed" : "no change");
            if (settings->exit == screens::PartsSettingView::kChangeParts)
                stack.Replace(std::make_unique<screens::ChangePartsView>(assets, c), screens::SessionViewStack::kFromRight);
            else left = true;
        }
        if (left) {
            if (!afterPath.empty()) { // the commits against the later dump
                const std::vector<uint8_t> after = ReadAll(afterPath);
                auto afterAt = [&](uint32_t a) { return after.data() + (a & 0x1FFFFF); };
                const uint32_t careerAt = address(career::kStateAddress) + 0x3C74 + uint32_t(garage) * 0x4028u;
                const bool slotSame = std::memcmp(&slot, afterAt(raceBlock + 0x64), sizeof slot) == 0;
                const bool recordSame = std::memcmp(&record, afterAt(address(career::kSettingsRecordAddress)), sizeof record) == 0;
                const bool garageSame = !c.garageCar || std::memcmp(&block, afterAt(careerAt), sizeof block) == 0;
                const bool sheetSame = std::memcmp(&sheet, afterAt(address(career::kSettingsSheetAddress)), sizeof sheet) == 0;
                std::printf("change-parts-check: the commit vs %s: race slot configuration %s, race record %s, garage block %s, sheet %s\n", afterPath.c_str(),
                            slotSame ? "equal" : "DIFFERS", recordSame ? "equal" : "DIFFERS", garageSame ? "equal" : "DIFFERS", sheetSame ? "equal" : "DIFFERS");
                if (!slotSame || !recordSame || !garageSame || !sheetSame) commitFailures++;
            }
            break;
        }
        size_t modelAt = 0;
        std::optional<screens::PostRaceModel> model;
        frames[field] = stack.Frame(assets, modelAt, model);
    }
    int failures = 0;
    for (const Capture& cap : caps) {
        const std::vector<uint8_t> bytes = ReadAll(cap.path);
        if (bytes.size() != 1024 * 512 * 2) throw std::runtime_error(cap.path + ": not a 1024 x 512 VRAM dump");
        std::vector<uint16_t> vram(1024 * 512);
        std::memcpy(vram.data(), bytes.data(), bytes.size());
        size_t best = std::numeric_limits<size_t>::max();
        int bestField = -1;
        for (int f = cap.field + 4; f <= cap.field + 8; f++) {
            const auto it = frames.find(f);
            if (it == frames.end()) continue;
            const size_t d = Compare(screens::RenderRaceMenuFrame(assets, it->second, true), vram, std::string());
            if (d < best) best = d, bestField = f;
        }
        if (bestField < 0) {
            std::printf("change-parts-check: %s: no frame of ours near field %d\n", cap.path.c_str(), cap.field + 6);
            failures++;
            continue;
        }
        const std::string side = (std::filesystem::path(outDir) / ("side_" + std::to_string(cap.field) + "_" + std::to_string(bestField) + ".png")).string();
        Compare(screens::RenderRaceMenuFrame(assets, frames[bestField], true), vram, side);
        std::printf("change-parts-check: capture f%d (VRAM of f%d) vs ours f%d: %zu differing pixels -> %s\n", cap.field, cap.field + 6, bestField, best, side.c_str());
        if (best) failures++;
    }
    std::printf("change-parts-check: %zu capture(s), %d differ\n", caps.size(), failures);
    return failures || commitFailures ? 1 : 0;
}
