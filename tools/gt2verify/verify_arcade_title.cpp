// The arcade title's options / save screens (US Arcade v1.1 member 1 = the Simulation title's code shifted; src/game/shell on
// gt2formats TitleAssets::LoadArcade + SimLayoutScreens, docs/research/arcade_disc.md section 17.8) against an arcade RAM image
// with member 1 loaded (work/play/arcade_title/cap/opt_1450.txt.ram.bin: gt2play --prims at the OPTIONS screen). ARCADE v1.1
// addresses. The rows skip themselves unless the dump holds the arcade member 1 (hash of its option routines).
//   ArcTText: the language blocks LoadArcade reads (member 1 0x80020D88 / 0x8002247C) = the original's RAM copies 0x801B9330
//     (0xDAE bytes) / 0x801EF0E0 (0x76F); every string pointer the screens read from the Simulation-layout copies (the option
//     rows' labels and value labels, the page titles, the views' titles, the card manager's bar templates) -> Text() = the
//     string at the original's own pointer in the dump; every Simulation string constant of the screens -> Text() = the string
//     at the address the profile gives in the dump (the facts / reference runs of db/arcade_us11_symbols.yaml).
//   ArcOptGet 0x800179E0, ArcOptSet 0x80017AD4, ArcOptStep 0x80017B98 (Simulation 0x80017D74 / 0x80017E68 / 0x80017F2C): random
//     career option bytes (career 0x801C9340), the real rows of the copy and random ones, vs shell::OptionValue / SetOptionValue
//     / StepOption.
//   ArcSavePack 0x8006A124, ArcSaveCheck 0x8006A224, ArcSaveUnpack 0x8006A188 (+ 0x8006A258: pads to 0x800A6BE4 / 0x800A6BF8),
//   ArcSaveHead 0x80069F48 (Simulation 0x8006A214 / 0x8006A314 / 0x8006A278 / 0x8006A348 / 0x8006A038): the save file of
//     career_state.h ("BASCUS-94455GAME", the same file as the Simulation disc's) with the header of the Simulation-layout EXE.
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "game/career/career_state.h"
#include "game/shell/title_options.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/title_assets.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "guest.h"

namespace gt2::verify {
namespace {

constexpr uint32_t kOptGet = 0x800179E0u, kOptSet = 0x80017AD4u, kOptStep = 0x80017B98u;
constexpr uint32_t kSavePack = 0x8006A124u, kSaveCheck = 0x8006A224u, kSaveUnpack = 0x8006A188u, kSaveHeader = 0x80069F48u;
constexpr uint32_t kCareer = 0x801C9340u, kPads1 = 0x800A6BE4u, kPads2 = 0x800A6BF8u;
constexpr uint32_t kTitleText = 0x801B9330u, kGlobalText = 0x801EF0E0u;
constexpr uint32_t kRowObject = kStack + 0x200;
constexpr uint32_t kImage = 0x80180000u;             // a free 0x8000-byte buffer for the save image (below the career block)
constexpr uint32_t kOptionCodeHash = 0xE92681C9u;    // FNV-1a of 0x800179E0..+0x2E8 of the arcade member 1

uint32_t Fnv(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
uint8_t* At(uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }
const uint8_t* At(const std::vector<uint8_t>& ram, uint32_t address) { return ram.data() + (address & 0x1FFFFF); }
uint32_t Word(const std::vector<uint8_t>& ram, uint32_t address) {
    uint32_t v;
    std::memcpy(&v, At(ram, address), 4);
    return v;
}
std::string RamString(const std::vector<uint8_t>& ram, uint32_t address) {
    std::string s;
    for (uint32_t a = address; s.size() < 256 && ram[a & 0x1FFFFF]; a++) s.push_back(char(ram[a & 0x1FFFFF]));
    return s;
}
career::CareerState& Career(uint8_t* ram) { return *reinterpret_cast<career::CareerState*>(At(ram, kCareer)); }
bool SameOptions(uint8_t* a, const uint8_t* b) { return std::memcmp(At(a, kCareer), b + (kCareer & 0x1FFFFF), 0x100) == 0; }

void RandomiseOptions(uint8_t* ram, std::mt19937& rng) { // as verify_title.cpp
    auto r = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    uint8_t* b = At(ram, kCareer);
    for (uint32_t o : {0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u, 0x36u, 0x88u, 0xAEu, 0xAFu, 0xB0u, 0xB1u, 0xB2u, 0xB3u, 0xB4u})
        b[o] = uint8_t(r(0, 3) == 0 ? r(0, 255) : (o >= 0xB3 ? r(0, 16) * 16 - r(0, 1) : o == 0x07 ? r(-100, 100) : r(0, 3)));
}

shell::OptionRow RandomRow(std::mt19937& rng, const std::vector<shell::OptionRow>& real) {
    auto r = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    if (r(0, 1)) return real[size_t(r(0, int(real.size()) - 1))];
    shell::OptionRow row;
    row.id = uint8_t(r(0, 9) == 0 ? r(0, 255) : r(0, 16));
    row.kind = uint8_t(r(0, 9) == 0 ? r(0, 255) : r(0, 4));
    row.min = int16_t(r(0, 3) ? r(-100, 2) : r(-0x8000, 0x7FFF));
    row.max = int16_t(r(0, 3) ? r(1, 256) : r(-0x8000, 0x7FFF));
    return row;
}

void WriteRow(uint8_t* ram, uint32_t address, const shell::OptionRow& row) {
    uint8_t* p = At(ram, address);
    std::memset(p, 0, 0x14);
    p[0] = row.id, p[1] = row.kind;
    std::memcpy(p + 2, &row.min, 2);
    std::memcpy(p + 4, &row.max, 2);
}

} // namespace

int VerifyArcadeTitle(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const DiscImage* disc, const GtfsVolume* vol) {
    if (!disc || !vol || !ProfileOf(*disc).arcade || Fnv(pristine.data() + (kOptGet & 0x1FFFFF), 0x2E8) != kOptionCodeHash) {
        std::printf("%-10s skipped (the arcade title, US Arcade v1.1 GT2.OVL member 1, is not loaded in this dump)\n", "ArcTitle");
        return 0;
    }
    const auto savedMap = gCodeAddressMap; // the dump's own addresses
    gCodeAddressMap = nullptr;
    int failures = 0;
    const TitleAssets native = TitleAssets::LoadArcade(*disc, *vol);
    const TitleAssets screens = native.SimLayoutScreens();
    const ExeProfile& profile = ProfileOf(*disc);

    { // ArcTText
        size_t cases = 0, bad = 0;
        auto check = [&](const std::string& what, const std::string& ours, const std::string& original) {
            cases++;
            if (ours != original && bad++ < 6) std::printf("    MISMATCH ArcTText %s: ours \"%s\" original \"%s\"\n", what.c_str(), ours.c_str(), original.c_str());
        };
        cases++;
        if ((native.titleText.size() < 0xDAE || std::memcmp(native.titleText.data(), At(pristine, kTitleText), 0xDAE) != 0) && bad++ < 6)
            std::puts("    MISMATCH ArcTText: the data-title block differs from RAM 0x801B9330");
        cases++;
        if ((native.globalText.size() < 0x76F || std::memcmp(native.globalText.data(), At(pristine, kGlobalText), 0x76F) != 0) && bad++ < 6)
            std::puts("    MISMATCH ArcTText: the data-global block differs from RAM 0x801EF0E0");
        // String pointers of the tables the screens read: {Simulation address in the copy, image, count of words}.
        struct Table { uint32_t sim; bool exe; uint32_t words; const char* name; };
        const Table tables[] = {
            {shell::kGlobalOptionRows, false, 8 * 5, "GLOBAL OPTIONS rows"}, {shell::kRaceOptionRows, false, 7 * 5, "RACE OPTIONS rows"},
            {0x8004BCF0u, false, 19, "option value labels"}, {0x8004BFA4u, false, 5, "page titles"},
            {0x8004B528u, false, 1, "SAVE GAME view title"}, {0x8004B5D0u, false, 1, "LOAD GAME view title"}, {0x8004C08Cu, false, 1, "OPTIONS view title"},
            {0x8004C0F4u, false, 11, "key configuration labels"}, {0x80091F04u, true, 9 * 12, "card manager bar templates"},
        };
        for (const Table& t : tables) {
            const GuestImage& copy = t.exe ? screens.exe : screens.ovl1;
            const uint32_t build = profile.Data(t.sim, t.exe ? -1 : 1); // the original's table in the dump
            for (uint32_t k = 0; k < t.words; k++) {
                const uint32_t original = Word(pristine, build + k * 4), translated = copy.Get<uint32_t>(t.sim + k * 4);
                const bool text = (original >= kTitleText && original < kTitleText + 0xDAE) || (original >= kGlobalText && original < kGlobalText + 0x76F);
                if (!text) continue;
                char what[96];
                std::snprintf(what, sizeof(what), "%s word %u (0x%08X)", t.name, k, t.sim + k * 4);
                check(what, screens.Text(translated), RamString(pristine, original));
            }
        }
        // The Simulation string constants of the screens (title_screens.cpp, key_config / analog_config, arcade_title.cpp, title_replay /
        // title_copy / title_transfer).
        const uint32_t constants[] = {
            0x801B9934u, 0x801B9956u, 0x801B9971u, 0x801B998Du, 0x801B99B4u, 0x801B9AB6u, 0x801B9ABFu, 0x801B9AD1u, 0x801B9AD8u, 0x801B9B06u,
            0x801B9B15u, 0x801B9B29u, 0x801B9B32u, 0x801B9CADu, 0x801B9CB2u, 0x801B9CB7u, 0x801B9CCFu, 0x801BA0EEu, 0x801BA0F6u, 0x801BA107u,
            0x801BA111u, 0x801BA120u, 0x801BA196u, 0x801BA1FEu, 0x801BA205u, 0x801BA20Au, 0x801BA212u, 0x801BA222u, 0x801BA259u, 0x801BA292u,
            0x801BA29Fu, 0x801BA2C0u, 0x801BA2EFu, 0x801BA30Fu, 0x801BA340u, 0x801BA364u, 0x801BA38Fu, 0x801BA3B0u, 0x801BA44Du, 0x801BA45Du,
            0x801EF6EAu, 0x801EF709u, 0x801EF726u, 0x801EF744u, 0x801EF76Fu, 0x801EF78Cu, 0x801EF7B5u, 0x801EF7D6u, 0x801EF7EEu, 0x801EF807u,
            0x801EF880u, 0x801EF895u, 0x801EF8A8u, 0x801EF8BFu, 0x801EF8DCu, 0x801EF8F1u, 0x801EF908u, 0x801EF91Bu, 0x801EF951u, 0x801EF96Au,
            0x801EF997u, 0x801EF9BFu, 0x801EF9D6u, 0x801EFB39u, 0x801EFB4Au, 0x801EFBE4u, 0x801EFBE7u,
            // the Replay Theater / Copy Replay / Data Transfer screens (title_replay / title_copy / title_transfer, the card manager's
            // replay modes): docs/formats/title.md section 12
            0x801B9655u, 0x801B96A9u, 0x801B96D8u, 0x801B9703u, 0x801B972Eu, 0x801B9752u, 0x801B9776u, 0x801B97A1u, 0x801B97CCu, 0x801B97F7u,
            0x801B9822u, 0x801B983Bu, 0x801B9866u, 0x801B9897u, 0x801B98C0u, 0x801B98EFu, 0x801B991Cu, 0x801B99C7u, 0x801B99DAu, 0x801B99F0u,
            0x801B9A34u, 0x801B9A6Cu, 0x801B9A87u, 0x801B9CE0u, 0x801B9CEAu, 0x801B9CFBu, 0x801B9D08u, 0x801B9D2Cu, 0x801B9D96u, 0x801B9DC9u,
            0x801B9DF6u, 0x801B9E33u, 0x801B9E51u, 0x801B9E70u, 0x801B9E84u, 0x801B9EA0u, 0x801B9ED5u, 0x801B9ED9u, 0x801B9F84u, 0x801B9FABu,
            0x801B9FD1u, 0x801B9FFCu, 0x801BA02Bu, 0x801BA05Cu, 0x801BA091u, 0x801BA3E1u, 0x801BA408u, 0x801BA417u, 0x801BA42Cu, 0x801BA469u,
            0x801EF6CBu, 0x801EF824u, 0x801EF933u, 0x801EFA00u, 0x801EFA22u, 0x801EFA4Du, 0x801EFA7Cu, 0x801EFA93u, 0x801EFB5Bu, 0x801EFB74u,
            0x801EFB8Bu, 0x801EFBA2u, 0x801EFBC2u, 0x801EFCCDu, 0x801EFCDCu, 0x801EFCF0u, 0x801EFCFCu, 0x801EFD16u, 0x801EFD26u, 0x801EFD2Eu,
            0x801EFD3Fu, 0x801EFD4Cu, 0x801EFD50u, 0x801EFD61u, 0x801EFD7Eu, 0x801EFDA2u, 0x801EFDEEu, 0x801EFE0Eu, 0x801EFE18u, 0x801EFE23u,
        };
        for (uint32_t c : constants) {
            const std::optional<uint32_t> build = profile.TryData(c, -1);
            char what[48];
            std::snprintf(what, sizeof(what), "constant 0x%08X", c);
            const std::string ours = screens.Text(c);
            if (!build || ours.empty()) {
                cases++;
                if (bad++ < 6) std::printf("    MISMATCH ArcTText %s: not mapped\n", what);
                continue;
            }
            check(what, ours, RamString(pristine, *build));
        }
        Report("ArcTText", kTitleText, cases, bad, failures);
    }

    // The option rows of the copy (the same rows the screens use).
    std::vector<shell::OptionRow> realRows = shell::ReadOptionRows(screens.ovl1, shell::kGlobalOptionRows, shell::kGlobalOptionCount);
    {
        const std::vector<shell::OptionRow> race = shell::ReadOptionRows(screens.ovl1, shell::kRaceOptionRows, shell::kRaceOptionCount);
        realRows.insert(realRows.end(), race.begin(), race.end());
    }
    std::vector<uint8_t> ours(Bus::kRamSize);
    auto reset = [&] {
        std::memcpy(guest.Ram(), pristine.data(), pristine.size());
        RandomiseOptions(guest.Ram(), rng);
        std::memcpy(ours.data(), guest.Ram(), ours.size());
    };
    auto r = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    { // ArcOptGet
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 2000; n++) {
            reset();
            const shell::OptionRow row = RandomRow(rng, realRows);
            WriteRow(guest.Ram(), kRowObject, row);
            const int32_t original = int32_t(guest.Call(kOptGet, kRowObject));
            const int native32 = shell::OptionValue(Career(ours.data()), row.id);
            cases++;
            if (original != native32 && bad++ < 3) std::printf("    MISMATCH ArcOptGet id %u: original %d ours %d\n", row.id, original, native32);
        }
        Report("ArcOptGet", kOptGet, cases, bad, failures);
    }
    { // ArcOptSet
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 2000; n++) {
            reset();
            const shell::OptionRow row = RandomRow(rng, realRows);
            WriteRow(guest.Ram(), kRowObject, row);
            const int value = r(0, 3) ? r(-3, 260) : r(-0x10000, 0x10000);
            guest.Call(kOptSet, kRowObject, uint32_t(value));
            shell::SetOptionValue(Career(ours.data()), row.id, value);
            cases++;
            if (!SameOptions(guest.Ram(), ours.data()) && bad++ < 3) std::printf("    MISMATCH ArcOptSet id %u value %d\n", row.id, value);
        }
        Report("ArcOptSet", kOptSet, cases, bad, failures);
    }
    { // ArcOptStep
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 3000; n++) {
            reset();
            const shell::OptionRow row = RandomRow(rng, realRows);
            WriteRow(guest.Ram(), kRowObject, row);
            const int step = r(0, 4) ? r(-1, 1) : r(-300, 300), fast = r(0, 4) ? r(-1, 1) : r(-300, 300);
            const bool original = guest.Call(kOptStep, kRowObject, uint32_t(step), uint32_t(fast)) != 0;
            const bool changed = shell::StepOption(Career(ours.data()), row, step, fast);
            cases++;
            if ((original != changed || !SameOptions(guest.Ram(), ours.data())) && bad++ < 3)
                std::printf("    MISMATCH ArcOptStep id %u kind %u step %d fast %d: original %d ours %d\n", row.id, row.kind, step, fast, int(original), int(changed));
        }
        Report("ArcOptStep", kOptStep, cases, bad, failures);
    }
    { // the save file class of the arcade EXE
        size_t cases = 0, badPack = 0, badCheck = 0, badUnpack = 0;
        for (int n = 0; n < 60; n++) {
            std::memcpy(guest.Ram(), pristine.data(), pristine.size());
            uint8_t* block = At(guest.Ram(), kCareer);
            for (size_t k = 0; k < career::kSavedStateSize; k++)
                if (r(0, 7) == 0) block[k] = uint8_t(r(0, 255));
            std::vector<uint8_t> header(0x200);
            for (uint8_t& b : header) b = uint8_t(r(0, 255));
            header[0] = 'S', header[1] = 'C';
            std::memcpy(At(guest.Ram(), kImage + 0x8000), header.data(), header.size());
            guest.Call(kSavePack, kImage + 0x8000, kImage);
            career::CareerSave save;
            std::memcpy(&save.state, At(guest.Ram(), kCareer), career::kSavedStateSize);
            save.header = header;
            const std::vector<uint8_t> file = career::BuildCareerSaveFile(save);
            cases++;
            if (std::memcmp(file.data(), At(guest.Ram(), kImage), 0x7EA0) != 0 && badPack++ < 3) std::printf("    MISMATCH ArcSavePack case %d\n", n);
            const int damage = r(0, 2);
            if (damage == 1) At(guest.Ram(), kImage)[size_t(r(2, 0x7E9B))] ^= uint8_t(r(1, 255));
            if (damage == 2) At(guest.Ram(), kImage)[size_t(0x7E9C + r(0, 3))] ^= uint8_t(r(1, 255));
            const std::vector<uint8_t> image(At(guest.Ram(), kImage), At(guest.Ram(), kImage) + 0x7EA0);
            const bool originalOk = guest.Call(kSaveCheck, 0, kImage) != 0;
            const bool oursOk = career::LoadCareerSaveFile(image).CrcOk();
            if (originalOk != oursOk && badCheck++ < 3) std::printf("    MISMATCH ArcSaveCheck case %d damage %d\n", n, damage);
            std::memset(At(guest.Ram(), kCareer), 0x5A, career::kSavedStateSize);
            guest.Call(kSaveUnpack, 0, kImage);
            const career::CareerSave loaded = career::LoadCareerSaveFile(image);
            const uint8_t* s = reinterpret_cast<const uint8_t*>(&loaded.state);
            const bool same = std::memcmp(&loaded.state, At(guest.Ram(), kCareer), career::kSavedStateSize) == 0 &&
                              std::memcmp(At(guest.Ram(), kPads1), s + 0x48, 20) == 0 && std::memcmp(At(guest.Ram(), kPads2), s + 0x9A, 20) == 0;
            if (!same && badUnpack++ < 3) std::printf("    MISMATCH ArcSaveUnpack case %d\n", n);
        }
        Report("ArcSavePack", kSavePack, cases, badPack, failures);
        Report("ArcSaveCheck", kSaveCheck, cases, badCheck, failures);
        Report("ArcSaveUnpack", kSaveUnpack, cases, badUnpack, failures);
    }
    { // ArcSaveHead: the original's header of a new file against ours from the Simulation-layout EXE (what CardManager uses)
        std::memcpy(guest.Ram(), pristine.data(), pristine.size());
        std::memset(At(guest.Ram(), kImage), 0xCC, 0x200);
        guest.Call(kSaveHeader, kImage);
        const std::vector<uint8_t> header = career::BuildSaveHeader(screens.exe);
        const bool same = header.size() == 0x200 && std::memcmp(header.data(), At(guest.Ram(), kImage), 0x200) == 0;
        Report("ArcSaveHead", kSaveHeader, 1, same ? 0 : 1, failures);
    }
    gCodeAddressMap = savedMap;
    return failures;
}

} // namespace gt2::verify
