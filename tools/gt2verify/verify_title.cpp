// The title overlay (GT2.OVL member 1, src/game/shell) and the save-file class of the executable against the original
// in a RAM image taken with member 1 loaded (work/re/title/ram.bin: gt2run calltrace at the OPTIONS screen). The rows
// run only when the dump holds member 1 (checked by a hash of the option routines' code); elsewhere they skip.
//   OptGet 0x80017D74, OptSet 0x80017E68, OptStep 0x80017F2C: the option rows' byte access and stepping on random
//     career bytes, row ids / kinds / ranges (the real rows of 0x8004BD3C / 0x8004BE88 and random ones);
//   OptInput 0x80018574 (command 3): the selected row's pad handling (left / right / L1 / R1) through the real list
//     widgets 0x8004BDDC / 0x8004BF14;
//   TitleSeq 0x8001792C + 0x80017984: the title menu from its init over random pad sequences (the list widget, row
//     animation, fade, idle counter, choice) - state compared after every field;
//   SavePack 0x8006A214, SaveCheck 0x8006A314, SaveUnpack 0x8006A278 (+ 0x8006A348), SaveHead 0x8006A038: the save
//     file image of career/career_state.h.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/career/career_state.h"
#include "game/shell/title_menu.h"
#include "game/shell/title_options.h"
#include "gt2formats/overlay_data.h"
#include "guest.h"

namespace gt2::verify {
namespace {

constexpr uint32_t kOptGet = 0x80017D74u, kOptSet = 0x80017E68u, kOptStep = 0x80017F2Cu, kOptRow = 0x80018574u;
constexpr uint32_t kTitleInit = 0x8001792Cu, kTitleUpdate = 0x80017984u;
constexpr uint32_t kSavePack = 0x8006A214u, kSaveCheck = 0x8006A314u, kSaveUnpack = 0x8006A278u, kSaveHeader = 0x8006A038u;
constexpr uint32_t kCareer = career::kStateAddress;
constexpr uint32_t kRowObject = kStack + 0x200, kBlock = kStack + 0x240, kPadBlock = kStack + 0x280;
constexpr uint32_t kImage = 0x80180000u; // a free 0x8000-byte buffer for the save image (above the options' data, below 0x801C98E0)
constexpr uint32_t kOptionCodeHash = 0x02415340u; // FNV-1a of 0x80017D74..0x8001805C of member 1 (the overlay is loaded)

uint32_t Fnv(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
uint8_t* At(uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }
void SetWord(uint8_t* ram, uint32_t address, uint32_t v) { std::memcpy(At(ram, address), &v, 4); }
uint32_t Word(uint8_t* ram, uint32_t address) {
    uint32_t v;
    std::memcpy(&v, At(ram, address), 4);
    return v;
}
int16_t Half(uint8_t* ram, uint32_t address) {
    int16_t v;
    std::memcpy(&v, At(ram, address), 2);
    return v;
}
career::CareerState& Career(uint8_t* ram) { return *reinterpret_cast<career::CareerState*>(At(ram, kCareer)); }

// The option bytes the rows touch (+0x02..+0x08, +0x36, +0x88, +0xAE..+0xB4): random, with realistic values mostly.
void RandomiseOptions(uint8_t* ram, std::mt19937& rng) {
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

bool SameOptions(uint8_t* a, const uint8_t* b) { return std::memcmp(At(a, kCareer), b + (kCareer & 0x1FFFFF), 0x100) == 0; }

} // namespace

int VerifyTitle(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng) {
    if (Fnv(pristine.data() + (kOptGet & 0x1FFFFF), 0x2E8) != kOptionCodeHash) {
        std::printf("%-10s skipped (the title overlay, GT2.OVL member 1, is not loaded in this dump; hash %08X)\n", "Title",
                    Fnv(pristine.data() + (kOptGet & 0x1FFFFF), 0x2E8));
        return 0;
    }
    int failures = 0;
    auto r = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    GuestImage ram;
    ram.base = 0x80000000u;
    ram.bytes = pristine;
    const std::vector<shell::OptionRow> realRows = [&] {
        std::vector<shell::OptionRow> rows = shell::ReadOptionRows(ram, shell::kGlobalOptionRows, shell::kGlobalOptionCount);
        const std::vector<shell::OptionRow> race = shell::ReadOptionRows(ram, shell::kRaceOptionRows, shell::kRaceOptionCount);
        rows.insert(rows.end(), race.begin(), race.end());
        return rows;
    }();
    std::vector<uint8_t> ours(Bus::kRamSize);
    auto reset = [&] {
        std::memcpy(guest.Ram(), pristine.data(), pristine.size());
        RandomiseOptions(guest.Ram(), rng);
        std::memcpy(ours.data(), guest.Ram(), ours.size());
    };

    { // 0x80017D74
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 4000; n++) {
            reset();
            const shell::OptionRow row = RandomRow(rng, realRows);
            WriteRow(guest.Ram(), kRowObject, row);
            const int32_t original = int32_t(guest.Call(kOptGet, kRowObject));
            const int ours32 = shell::OptionValue(Career(ours.data()), row.id);
            cases++;
            if (original != ours32 && bad++ < 3) std::printf("    MISMATCH OptGet id %u: original %d ours %d\n", row.id, original, ours32);
        }
        Report("OptGet", kOptGet, cases, bad, failures);
    }
    { // 0x80017E68
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 4000; n++) {
            reset();
            const shell::OptionRow row = RandomRow(rng, realRows);
            WriteRow(guest.Ram(), kRowObject, row);
            const int value = r(0, 3) ? r(-3, 260) : r(-0x10000, 0x10000);
            guest.Call(kOptSet, kRowObject, uint32_t(value));
            shell::SetOptionValue(Career(ours.data()), row.id, value);
            cases++;
            if (!SameOptions(guest.Ram(), ours.data()) && bad++ < 3) std::printf("    MISMATCH OptSet id %u value %d\n", row.id, value);
        }
        Report("OptSet", kOptSet, cases, bad, failures);
    }
    { // 0x80017F2C
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 6000; n++) {
            reset();
            const shell::OptionRow row = RandomRow(rng, realRows);
            WriteRow(guest.Ram(), kRowObject, row);
            const int step = r(0, 4) ? r(-1, 1) : r(-300, 300), fast = r(0, 4) ? r(-1, 1) : r(-300, 300);
            const bool original = guest.Call(kOptStep, kRowObject, uint32_t(step), uint32_t(fast)) != 0;
            const bool changed = shell::StepOption(Career(ours.data()), row, step, fast);
            cases++;
            if ((original != changed || !SameOptions(guest.Ram(), ours.data())) && bad++ < 3)
                std::printf("    MISMATCH OptStep id %u kind %u [%d,%d) step %d fast %d: original %d ours %d\n", row.id, row.kind, row.min, row.max, step, fast,
                            int(original), int(changed));
        }
        Report("OptStep", kOptStep, cases, bad, failures);
    }
    { // 0x80018574 command 3 on the real widgets and rows
        size_t cases = 0, bad = 0;
        struct List { uint32_t widget; int count; size_t first; };
        const List lists[2] = {{0x8004BDDCu, shell::kGlobalOptionCount, 0}, {0x8004BF14u, shell::kRaceOptionCount, size_t(shell::kGlobalOptionCount)}};
        for (int n = 0; n < 4000; n++) {
            reset();
            const List& l = lists[r(0, 1)];
            const int row = r(0, l.count - 1);
            const int16_t selection = int16_t(r(0, 5) ? row : r(-1, l.count - 1));
            std::memcpy(At(guest.Ram(), l.widget + 6), &selection, 2);
            MenuListPad pad;
            const uint32_t bits[] = {menu_list_pad::kLeft, menu_list_pad::kRight, menu_list_pad::kL1, menu_list_pad::kR1, menu_list_pad::kUp, 0};
            pad.held = r(0, 1) ? bits[r(0, 5)] | bits[r(0, 5)] : uint32_t(r(0, 0xFFFF));
            pad.pressed = r(0, 1) ? bits[r(0, 5)] : 0;
            pad.repeat = r(0, 2) == 0 ? bits[r(0, 5)] : 0;
            SetWord(guest.Ram(), kPadBlock, pad.held);
            SetWord(guest.Ram(), kPadBlock + 4, pad.pressed);
            SetWord(guest.Ram(), kPadBlock + 8, 0);
            SetWord(guest.Ram(), kPadBlock + 0xC, pad.repeat);
            const bool withPad = r(0, 7) != 0;
            SetWord(guest.Ram(), kBlock, l.widget);
            SetWord(guest.Ram(), kBlock + 4, 0);
            SetWord(guest.Ram(), kBlock + 8, withPad ? kPadBlock : 0);
            std::memcpy(ours.data(), guest.Ram(), ours.size());
            guest.Call(kOptRow, 3, kBlock, uint32_t(row));
            if (withPad && selection == row) shell::OptionRowInput(Career(ours.data()), realRows[l.first + size_t(row)], pad);
            cases++;
            if (!SameOptions(guest.Ram(), ours.data()) && bad++ < 3)
                std::printf("    MISMATCH OptInput row %d sel %d held %X pressed %X repeat %X\n", row, selection, pad.held, pad.pressed, pad.repeat);
        }
        Report("OptInput", kOptRow, cases, bad, failures);
    }
    { // 0x8001792C + 0x80017984: title menu sequences
        size_t cases = 0, bad = 0;
        const uint32_t manager = Word(guest.Ram(), 0x800A8D68u);
        const uint32_t padBlock = manager + 0x1A4;
        for (int seq = 0; seq < 60; seq++) {
            std::memcpy(guest.Ram(), pristine.data(), pristine.size());
            guest.Call(kTitleInit);
            shell::TitleMenu menu(ram, guest.Ram()[kCareer & 0x1FFFFF]);
            const int steps = r(0, 3) == 0 ? 1100 : 200;
            bool seqBad = false;
            for (int s = 0; s < steps && !seqBad; s++) {
                MenuListPad pad;
                const uint32_t bits[] = {menu_list_pad::kUp, menu_list_pad::kDown, menu_list_pad::kLeft, menu_list_pad::kRight, menu_list_pad::kCross,
                                         menu_list_pad::kCircle, menu_list_pad::kTriangle, menu_list_pad::kSquare, menu_list_pad::kStart, 0, 0, 0};
                const bool idle = steps > 1000;
                if (!idle && r(0, 2) == 0) {
                    pad.pressed = bits[r(0, 11)];
                    if (seq % 3 != 0 && (pad.pressed & menu_list_pad::kChoose)) pad.pressed = menu_list_pad::kDown; // most sequences keep scrolling
                    pad.held = pad.pressed | (r(0, 3) == 0 ? menu_list_pad::kStart : 0);
                    pad.repeat = r(0, 4) == 0 ? bits[r(0, 3)] : 0;
                }
                SetWord(guest.Ram(), padBlock, pad.held);
                SetWord(guest.Ram(), padBlock + 4, pad.pressed);
                SetWord(guest.Ram(), padBlock + 8, 0);
                SetWord(guest.Ram(), padBlock + 0xC, pad.repeat);
                const bool input = r(0, 15) != 0;
                const int32_t original = int32_t(guest.Call(kTitleUpdate, input ? 1u : 0u));
                const int native = menu.Update(input ? &pad : nullptr, pad.held);
                cases++;
                uint8_t* g = guest.Ram();
                bool same = original == native && Half(g, 0x8004BC28u + 6) == menu.Selection() && Half(g, 0x8004BC00u) == menu.Fade() &&
                            Half(g, 0x800B122Cu) == menu.State();
                if (menu.result >= 0) same = same && int(g[0x801EF5F3u & 0x1FFFFF]) == menu.result;
                for (int row = 1; row <= 6; row++) same = same && Half(g, 0x800B1230u + uint32_t(row) * 0x14 + 0x10) == menu.Row(row).anim;
                if (!same) {
                    seqBad = true;
                    if (bad++ < 3)
                        std::printf("    MISMATCH TitleSeq %d step %d: ret %d/%d sel %d/%d fade %d/%d state %d/%d result %d/%d\n", seq, s, original, native,
                                    Half(g, 0x8004BC2Eu), menu.Selection(), Half(g, 0x8004BC00u), menu.Fade(), Half(g, 0x800B122Cu), menu.State(),
                                    int(g[0x801EF5F3u & 0x1FFFFF]), menu.result);
                }
                if (original == 4) break;
            }
        }
        Report("TitleSeq", kTitleUpdate, cases, bad, failures);
    }
    { // the save file class (EXE)
        size_t cases = 0, badPack = 0, badCheck = 0, badUnpack = 0;
        for (int n = 0; n < 60; n++) {
            std::memcpy(guest.Ram(), pristine.data(), pristine.size());
            uint8_t* block = At(guest.Ram(), kCareer);
            for (size_t k = 0; k < career::kSavedStateSize; k++)
                if (r(0, 7) == 0) block[k] = uint8_t(r(0, 255));
            std::vector<uint8_t> header(0x200);
            for (uint8_t& b : header) b = uint8_t(r(0, 255));
            header[0] = 'S', header[1] = 'C'; // the loader of career_state.h checks the magic
            std::memcpy(At(guest.Ram(), kImage + 0x8000), header.data(), header.size());
            guest.Call(kSavePack, kImage + 0x8000, kImage);
            career::CareerSave save;
            std::memcpy(&save.state, At(guest.Ram(), kCareer), career::kSavedStateSize);
            save.header = header;
            const std::vector<uint8_t> file = career::BuildCareerSaveFile(save);
            cases++;
            if (std::memcmp(file.data(), At(guest.Ram(), kImage), 0x7EA0) != 0 && badPack++ < 3) std::printf("    MISMATCH SavePack case %d\n", n);
            // CRC check: intact, or one byte / the CRC damaged
            const int damage = r(0, 2);
            if (damage == 1) At(guest.Ram(), kImage)[size_t(r(2, 0x7E9B))] ^= uint8_t(r(1, 255));
            if (damage == 2) At(guest.Ram(), kImage)[size_t(0x7E9C + r(0, 3))] ^= uint8_t(r(1, 255));
            std::vector<uint8_t> image(At(guest.Ram(), kImage), At(guest.Ram(), kImage) + 0x7EA0);
            const bool originalOk = guest.Call(kSaveCheck, 0, kImage) != 0;
            const bool oursOk = career::LoadCareerSaveFile(image).CrcOk();
            if (originalOk != oursOk && badCheck++ < 3) std::printf("    MISMATCH SaveCheck case %d damage %d: original %d ours %d\n", n, damage, int(originalOk), int(oursOk));
            // Unpack into the live block, then the pad configurations out (0x8006A348)
            std::memset(At(guest.Ram(), kCareer), 0x5A, career::kSavedStateSize);
            guest.Call(kSaveUnpack, 0, kImage);
            const career::CareerSave loaded = career::LoadCareerSaveFile(image);
            const bool blockSame = std::memcmp(&loaded.state, At(guest.Ram(), kCareer), career::kSavedStateSize) == 0;
            const uint8_t* s = reinterpret_cast<const uint8_t*>(&loaded.state);
            const bool padsSame = std::memcmp(At(guest.Ram(), 0x800A6EECu), s + 0x48, 20) == 0 && std::memcmp(At(guest.Ram(), 0x800A6F00u), s + 0x9A, 20) == 0;
            if (!(blockSame && padsSame) && badUnpack++ < 3) std::printf("    MISMATCH SaveUnpack case %d (block %d pads %d)\n", n, int(blockSame), int(padsSame));
        }
        Report("SavePack", kSavePack, cases, badPack, failures);
        Report("SaveCheck", kSaveCheck, cases, badCheck, failures);
        Report("SaveUnpack", kSaveUnpack, cases, badUnpack, failures);
    }
    { // 0x8006A038: the header of a new save file
        std::memcpy(guest.Ram(), pristine.data(), pristine.size());
        std::memset(At(guest.Ram(), kImage), 0xCC, 0x200);
        guest.Call(kSaveHeader, kImage);
        GuestImage exe;
        exe.base = 0x80000000u;
        exe.bytes = pristine;
        const std::vector<uint8_t> ours0 = career::BuildSaveHeader(exe);
        const bool same = ours0.size() == 0x200 && std::memcmp(ours0.data(), At(guest.Ram(), kImage), 0x200) == 0;
        Report("SaveHead", kSaveHeader, 1, same ? 0 : 1, failures);
    }
    return failures;
}

} // namespace gt2::verify
