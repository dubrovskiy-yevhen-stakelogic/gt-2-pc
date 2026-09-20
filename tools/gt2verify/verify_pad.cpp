// The controller path (src/platform/input/ps1_pad.h) against the original in a RAM image of US Simulation v1.2:
//   resident EXE (every dump):
//     PadRemap 0x80083A4C (raw buttons -> generic bits, pairs 0x800A6F3C), NegBtn 0x80083A88 (neGcon analogue as buttons),
//     PadAxis 0x80085890 (DualShock stick filter), NegTwist 0x800831BC / NegPedal 0x80083250 (neGcon calibration),
//     PadTrack 0x800838B4 (the button tracker), PadDigi 0x80083818 / PadAnlg 0x800858CC / PadNegc 0x800832C0 (the race's
//     handlers on a pad object), PadAct 0x8008371C (the actuator bytes, up to its PadSetAct call);
//   title overlay (work/re/title/ram.bin; GT2.OVL member 1 loaded): the KEY CONFIGURATION page (game/shell/key_config.h)
//     KeyLoad 0x80019388, KeyStore 0x80019498, KeyFind 0x80019598, KeyAssign 0x800196E8, KeySteer 0x80019934, KeyPreset
//     0x80019B24, KeyDefault 0x80019C4C, KeyEdit 0x80019D5C on random list objects / pads, KeyPage 0x8001A860 (the page
//     update with the title's pad objects: page object, both lists and both pad blocks compared; the sound requests
//     0x80060840 run in the guest and are not compared);
//     and the ANALOG pages (game/shell/analog_config.h): AnaItems 0x8001C2A0, AnaReset 0x8001C48C, AnaEnter 0x8001C690,
//     AnaUpdate 0x8001C7B8 (page objects 0x800B12D8 / 0x800B1358, the globals 0x800B1408..0x800B14AF and both pad blocks
//     compared; the live bytes in libpad's receive buffer 0x801F0C98 + port * 0x22 + 4);
//   race overlay (race dumps only):
//     PadLogic 0x80014BB4 (the race's logical pad through the key tables of the pad block), VibFeed 0x800133F0 (its tail:
//     the vibration words of the car's pad object; the whole function runs, only the pad object is compared).
// Other builds (the Arcade disc) skip these rows: the functions are compared on the Simulation build only.
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "game/shell/analog_config.h"
#include "game/shell/key_config.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/overlay_data.h"
#include "guest.h"
#include "platform/input/ps1_pad.h"

namespace gt2::verify {
namespace {

template <typename T> T Get(const uint8_t* ram, uint32_t address) { T v; std::memcpy(&v, ram + (address & 0x1FFFFF), sizeof(T)); return v; }
template <typename T> void Put(uint8_t* ram, uint32_t address, T v) { std::memcpy(ram + (address & 0x1FFFFF), &v, sizeof(T)); }
uint8_t* At(uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }

constexpr uint32_t kRemapFn = 0x80083A4Cu, kNegBtnFn = 0x80083A88u, kAxisFn = 0x80085890u, kTwistFn = 0x800831BCu, kPedalFn = 0x80083250u;
constexpr uint32_t kTrackFn = 0x800838B4u, kDigitalFn = 0x80083818u, kAnalogFn = 0x800858CCu, kNegconFn = 0x800832C0u, kActFn = 0x8008371Cu;
constexpr uint32_t kSetActFn = 0x800874B8u, kLogicFn = 0x80014BB4u, kRenderCarFn = 0x800133F0u;
constexpr uint32_t kPad1 = 0x800A9528u, kPadStride = 0xB0u, kPadManager = 0x801F0C70u, kInterruptFlag = 0x800A7B7Eu;
constexpr uint32_t kCareerPadBlock = 0x801C98EAu, kDemoFlag = 0x800A951Cu;
constexpr uint32_t kObject = kStack + 0x400, kWords = kStack + 0x300; // free harness memory above the guest stack

input::PadTables TablesOf(const uint8_t* ram) {
    input::PadTables t;
    for (uint32_t i = 0; i < 16; i++) {
        t.remap[i][0] = Get<uint8_t>(ram, input::PadTables::kRemap + i * 2);
        t.remap[i][1] = Get<uint8_t>(ram, input::PadTables::kRemap + i * 2 + 1);
    }
    for (uint32_t i = 0; i < 3; i++) {
        const uint32_t a = input::PadTables::kNegconButtons + i * 4;
        t.negconButtons[i].threshold = Get<uint16_t>(ram, a);
        t.negconButtons[i].axis = Get<uint8_t>(ram, a + 2);
        t.negconButtons[i].bit = Get<uint8_t>(ram, a + 3);
    }
    t.repeat[0] = Get<uint8_t>(ram, input::PadTables::kRepeat);
    t.repeat[1] = Get<uint8_t>(ram, input::PadTables::kRepeat + 1);
    return t;
}

struct Rng {
    std::mt19937& g;
    int operator()(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(g); }
    uint32_t Word() { return uint32_t(g()); }
    bool Chance(int n) { return (*this)(0, n - 1) == 0; }
};

// A realistic stick / pedal byte: centre, extremes, dead-zone edges, anything.
uint16_t AxisValue(Rng& r) {
    switch (r(0, 5)) {
    case 0: return 0x80;
    case 1: return uint16_t(r(0, 1) ? 0 : 255);
    case 2: return uint16_t(r(85, 170));
    case 3: return uint16_t(r(0, 0xFFFF));
    default: return uint16_t(r(0, 255));
    }
}

// The neGcon calibration of both ports (0x800A6EEC + port * 20): ordered realistic values mostly, anything sometimes.
void RandomCalibration(uint8_t* ram, Rng& r) {
    for (uint32_t port = 0; port < 2; port++) {
        const uint32_t base = input::PadTables::kCalibration + port * 20;
        if (r.Chance(4)) {
            for (uint32_t k = 0; k < 20; k++) Put<uint8_t>(ram, base + k, uint8_t(r(0, 255)));
            continue;
        }
        const int lo = r(0, 0x70), deadLo = r(lo, 0x80), deadHi = r(0x80, 0x90), hi = r(deadHi, 255);
        const int v[10] = {lo, deadLo, deadHi, hi, r(0, 0x40), r(0xA0, 255), r(0, 0x40), r(0xA0, 255), r(0, 0x40), r(0xA0, 255)};
        for (uint32_t k = 0; k < 10; k++) Put<uint16_t>(ram, base + k * 2, uint16_t(v[k]));
    }
}

void RandomTracker(uint8_t* tr, Rng& r) {
    for (uint32_t k = 0; k < input::tracker::kSize; k++) tr[k] = uint8_t(r(0, 255));
    if (!r.Chance(4)) { // the table's repeat bytes, counters near them
        tr[input::tracker::kRepeatReload] = 0x1E;
        tr[input::tracker::kRepeatFirst] = 0x25;
        for (uint32_t k = 0; k < 32; k++) tr[input::tracker::kCounters + k] = uint8_t(r(0, 0x26));
    }
}

// A key-table entry: a generic button, one of the four axis forms, or any byte.
uint8_t TableEntry(Rng& r) {
    switch (r(0, 9)) {
    case 0: case 1: case 2: case 3: return uint8_t(r(0, 17));
    case 4: return uint8_t(0x80 | r(0, 7));
    case 5: return uint8_t(0xA0 | r(0, 7));
    case 6: return uint8_t(0xC0 | r(0, 7));
    case 7: return uint8_t(0xE0 | r(0, 7));
    case 8: return uint8_t(0x80 | r(0, 31));
    default: return uint8_t(r(0, 255));
    }
}

uint8_t RandomType(Rng& r) {
    static const uint8_t kTypes[] = {0, 2, 4, 5, 7, 14, 7, 4};
    return r.Chance(8) ? uint8_t(r(0, 15)) : kTypes[r(0, 7)];
}

uint32_t Fnv(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

// A generic pad of the title ({held, pressed, released, repeat}): one or two buttons, Start held sometimes, noise.
MenuListPad RandomMenuPad(Rng& r) {
    MenuListPad p;
    static const uint32_t kBits[] = {0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x10000, 0x20000};
    const int n = r(0, 2);
    for (int k = 0; k < n; k++) p.pressed |= kBits[r(0, 13)];
    if (r.Chance(4)) p.pressed = r.Word() & 0x3FFFF;
    p.held = p.pressed | (r.Chance(4) ? 0x10000u : 0u) | (r.Chance(8) ? r.Word() & 0x3FFFF : 0u);
    if (r.Chance(4)) p.repeat = r.Word() & 0xF;
    return p;
}

void PutPad(uint8_t* ram, uint32_t address, const MenuListPad& p) {
    Put<uint32_t>(ram, address, p.held);
    Put<uint32_t>(ram, address + 4, p.pressed);
    Put<uint32_t>(ram, address + 8, p.released);
    Put<uint32_t>(ram, address + 12, p.repeat);
}

// A list object: a real table of the pad block or random entries; state bytes realistic mostly.
shell::KeyConfigList RandomList(Rng& r, const uint8_t* ram) {
    shell::KeyConfigList l;
    l.type = r.Chance(8) ? r(-2, 6) : r(0, 3);
    const uint32_t block = 0x801C98EAu + uint32_t(r(0, 1)) * 0x52;
    for (size_t k = 0; k < 11; k++) l.table[k] = r.Chance(2) ? Get<uint8_t>(ram, block + uint32_t(l.type >= 0 && l.type < 4 ? l.type : 0) * 11 + uint32_t(k)) : TableEntry(r);
    l.steering = uint8_t(r.Chance(6) ? r(0, 255) : r(0, 2));
    l.preset = uint8_t(r(0, 8)); // (the page keeps 0..8: a larger preset makes the original read past its preset table)
    l.savedAccel = uint8_t(r.Chance(3) ? TableEntry(r) : r(0, 13));
    l.savedBrake = uint8_t(r.Chance(3) ? TableEntry(r) : r(0, 13));
    return l;
}

void PutList(uint8_t* ram, uint32_t address, const shell::KeyConfigList& l) { std::memcpy(At(ram, address), &l, sizeof l); }
shell::KeyConfigList GetList(uint8_t* ram, uint32_t address) {
    shell::KeyConfigList l;
    std::memcpy(&l, At(ram, address), sizeof l);
    return l;
}
bool SameList(const shell::KeyConfigList& a, const shell::KeyConfigList& b) { return std::memcmp(&a, &b, 0x13) == 0; } // (+0x13 is padding)

// The KEY CONFIGURATION rows on the title dump.
int VerifyKeyConfig(Guest& guest, const std::vector<uint8_t>& pristine, Rng& r) {
    constexpr uint32_t kLoad = 0x80019388u, kStore = 0x80019498u, kFind = 0x80019598u, kAssign = 0x800196E8u, kSteer = 0x80019934u, kPreset = 0x80019B24u;
    constexpr uint32_t kDefault = 0x80019C4Cu, kEdit = 0x80019D5Cu, kPageUpdate = 0x8001A860u;
    constexpr uint32_t kList = kStack + 0x400, kPad = kStack + 0x300, kPadType = kStack + 0x340, kBlock = 0x801C98EAu;
    constexpr uint32_t kPageObject = 0x800B12D0u, kList1 = 0x800B13D8u, kList2 = 0x800B13F0u, kPadBase = 0x800A8D68u;
    int failures = 0;
    GuestImage image;
    image.base = 0x80000000u;
    image.bytes = pristine;
    const shell::KeyConfigData data = shell::KeyConfigData::Read(image, image);
    auto fresh = [&] { std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize); };
    std::vector<int> sounds;

    { // 0x80019388 / 0x80019498
        size_t cases = 0, bad = 0, storeCases = 0, storeBad = 0;
        for (int n = 0; n < 2000; n++) {
            fresh();
            uint8_t* ram = guest.Ram();
            for (uint32_t k = 0; k < 0x52 * 2; k++)
                if (r.Chance(3)) Put<uint8_t>(ram, kBlock + k, uint8_t(r(0, 255)));
            const uint32_t port = uint32_t(r(0, 1));
            const uint8_t type = RandomType(r);
            Put<uint8_t>(ram, kPadType + 2, type);
            const shell::KeyConfigList before = RandomList(r, ram);
            PutList(ram, kList, before);
            std::vector<uint8_t> block(At(ram, kBlock + port * 0x52), At(ram, kBlock + port * 0x52) + 0x52);
            guest.Call(kLoad, kList, kPadType, port);
            shell::KeyConfigList ours = before;
            shell::LoadKeyList(ours, type, block.data());
            cases++;
            if (!SameList(ours, GetList(guest.Ram(), kList)) && bad++ < 3) std::printf("    MISMATCH KeyLoad type %u\n", type);
            // store: a random list into the block
            fresh();
            ram = guest.Ram();
            const shell::KeyConfigList list = RandomList(r, ram);
            PutList(ram, kList, list);
            std::vector<uint8_t> ourBlock(At(ram, kBlock + port * 0x52), At(ram, kBlock + port * 0x52) + 0x52);
            guest.Call(kStore, kList, port);
            shell::StoreKeyList(list, ourBlock.data());
            storeCases++;
            if (std::memcmp(ourBlock.data(), At(guest.Ram(), kBlock + port * 0x52), 0x52) != 0 && storeBad++ < 3) std::printf("    MISMATCH KeyStore type %d\n", list.type);
        }
        Report("KeyLoad", kLoad, cases, bad, failures);
        Report("KeyStore", kStore, storeCases, storeBad, failures);
    }
    auto listRow = [&](const char* name, uint32_t function, int kind, int count) {
        size_t cases = 0, bad = 0;
        for (int n = 0; n < count; n++) {
            fresh();
            uint8_t* ram = guest.Ram();
            shell::KeyConfigList ours = RandomList(r, ram);
            PutList(ram, kList, ours);
            const MenuListPad pad = RandomMenuPad(r);
            PutPad(ram, kPad, pad);
            const uint8_t button = uint8_t(r.Chance(4) ? r(0, 255) : r(0, 13));
            const int function2 = r(2, 9), row = r(0, 10);
            int32_t original = 0, result = 0;
            sounds.clear();
            switch (kind) {
            case 0: original = int32_t(guest.Call(function, kList, button)); result = shell::FindKeyUse(ours, button); break;
            case 1: guest.Call(function, kList, uint32_t(function2), button); shell::AssignKey(ours, function2, button); break;
            case 2: original = int32_t(guest.Call(function, kList, kPad)); result = shell::SteeringInput(ours, pad, sounds); break;
            case 3: original = int32_t(guest.Call(function, kList, kPad)); result = shell::PedalPresetInput(ours, pad, data, sounds); break;
            case 4: guest.Call(function, kList); shell::DefaultKeys(ours, data); break;
            default: original = int32_t(guest.Call(function, kList, kPad, uint32_t(row))); result = shell::EditKeyRow(ours, pad, row, data, sounds); break;
            }
            cases++;
            if ((original != result || !SameList(ours, GetList(guest.Ram(), kList))) && bad++ < 3)
                std::printf("    MISMATCH %s type %d: original %d ours %d, pressed %X held %X\n", name, ours.type, original, result, pad.pressed, pad.held);
        }
        Report(name, function, cases, bad, failures);
    };
    listRow("KeyFind", kFind, 0, 4000);
    listRow("KeyAssign", kAssign, 1, 4000);
    listRow("KeySteer", kSteer, 2, 4000);
    listRow("KeyPreset", kPreset, 3, 4000);
    listRow("KeyDefault", kDefault, 4, 1000);
    listRow("KeyEdit", kEdit, 5, 6000);
    { // 0x8001A860: the page update on random page states, pad types and pads (with and without input)
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 3000; n++) {
            fresh();
            uint8_t* ram = guest.Ram();
            const uint32_t pads = Get<uint32_t>(ram, kPadBase);
            const bool input = !r.Chance(4);
            // Rows are -1 only while the page is not edited (no pad): the original indexes its row table with them.
            std::array<int16_t, 4> page = {int16_t(r(input ? 0 : -1, 10)), int16_t(r(input ? 0 : -1, 10)), int16_t(r(0, 0x2C)), int16_t(r(-1, 5))};
            for (uint32_t k = 0; k < 4; k++) Put<int16_t>(ram, kPageObject + k * 2, page[k]);
            const std::array<shell::KeyConfigList, 2> lists = {RandomList(r, ram), RandomList(r, ram)};
            PutList(ram, kList1, lists[0]);
            PutList(ram, kList2, lists[1]);
            const std::array<uint8_t, 2> types = {RandomType(r), r.Chance(2) ? uint8_t(0) : RandomType(r)};
            Put<uint8_t>(ram, pads + 0xDE, types[0]);
            Put<uint8_t>(ram, pads + 0x142, types[1]);
            const MenuListPad pad1 = RandomMenuPad(r), pad2 = r.Chance(2) ? MenuListPad{} : RandomMenuPad(r);
            PutPad(ram, pads + 0x1A4, pad1);
            PutPad(ram, pads + 0x1B4, pad2);
            for (uint32_t b = 0; b < 2; b++) {
                for (uint32_t k = 0; k < 44; k++)
                    if (r.Chance(6)) Put<uint8_t>(ram, kBlock + b * 0x52 + k, TableEntry(r));
                for (uint32_t t = 0; t < 3; t++) { // the page state of each table: steering 0..2, preset 0..8, saved buttons
                    Put<uint8_t>(ram, kBlock + b * 0x52 + 0x2D + t * 4, uint8_t(r(0, 2)));
                    Put<uint8_t>(ram, kBlock + b * 0x52 + 0x2E + t * 4, uint8_t(r(0, 8)));
                    Put<uint8_t>(ram, kBlock + b * 0x52 + 0x2F + t * 4, uint8_t(r(0, 13)));
                    Put<uint8_t>(ram, kBlock + b * 0x52 + 0x30 + t * 4, uint8_t(r(0, 13)));
                }
            }
            std::vector<uint8_t> blocks(At(ram, kBlock), At(ram, kBlock) + 0x52 * 2);
            const int32_t original = int32_t(guest.Call(kPageUpdate, kPageObject, input ? pads + 0x1A4 : 0));
            shell::KeyConfigPage ours;
            ours.SetState(page, lists);
            uint8_t* ourBlocks[2] = {blocks.data(), blocks.data() + 0x52};
            const MenuListPad* ourPads[2] = {input ? &pad1 : nullptr, &pad2};
            sounds.clear();
            const int result = ours.Update(ourBlocks, types, ourPads, data, sounds);
            bool same = original == result && std::memcmp(blocks.data(), At(guest.Ram(), kBlock), blocks.size()) == 0;
            const std::array<int16_t, 4> ourPage = ours.PageState();
            for (uint32_t k = 0; k < 4; k++) same = same && ourPage[k] == Get<int16_t>(guest.Ram(), kPageObject + k * 2);
            same = same && SameList(ours.Lists()[0], GetList(guest.Ram(), kList1)) && SameList(ours.Lists()[1], GetList(guest.Ram(), kList2));
            cases++;
            if (!same && bad++ < 3) std::printf("    MISMATCH KeyPage: original %d ours %d, types %u / %u\n", original, result, types[0], types[1]);
        }
        Report("KeyPage", kPageUpdate, cases, bad, failures);
    }
    return failures;
}

// The ANALOG pages on the title dump.
int VerifyAnalogConfig(Guest& guest, const std::vector<uint8_t>& pristine, Rng& r) {
    constexpr uint32_t kItems = 0x8001C2A0u, kReset = 0x8001C48Cu, kEnter = 0x8001C690u, kUpdate = 0x8001C7B8u;
    constexpr uint32_t kPages[2] = {0x800B12D8u, 0x800B1358u}, kGlobals = 0x800B1408u, kBlock = 0x801C98EAu, kPadBase = 0x800A8D68u;
    constexpr uint32_t kReceive = 0x801F0C98u;
    int failures = 0;
    GuestImage image;
    image.base = 0x80000000u;
    image.bytes = pristine;
    const shell::AnalogConfigData data = shell::AnalogConfigData::Read(image, image);
    auto fresh = [&] { std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize); };
    std::vector<int> sounds;
    // A neGcon table of a port: steering / accelerate / brake on axes (or buttons), the calibration realistic mostly.
    auto randomBlock = [&](uint8_t* ram, uint32_t port) {
        const uint32_t b = kBlock + port * 0x52;
        static const uint8_t kSteer[] = {0x80, 0x80, 0x80, 0x81, 0x82, 0x02, 0x83};
        static const uint8_t kPedal[] = {0x81, 0x82, 0x83, 0x81, 0x09, 0x0A, 0x80};
        Put<uint8_t>(ram, b + 0x16, kSteer[r(0, 6)]);
        Put<uint8_t>(ram, b + 0x17, Get<uint8_t>(ram, b + 0x16));
        Put<uint8_t>(ram, b + 0x18, kPedal[r(0, 6)]);
        Put<uint8_t>(ram, b + 0x19, kPedal[r(0, 6)]);
        if (r.Chance(3)) {
            const int lo = r(0, 0x70), deadLo = r(lo, 0x80), deadHi = r(0x80, 0x90), hi = r(deadHi, 255);
            const int v[10] = {lo, deadLo, deadHi, hi, r(0, 0x40), r(0xA0, 255), r(0, 0x40), r(0xA0, 255), r(0, 0x40), r(0xA0, 255)};
            for (uint32_t k = 0; k < 10; k++) Put<uint16_t>(ram, b + 0x3E + k * 2, uint16_t(v[k]));
        } else if (r.Chance(4)) {
            for (uint32_t k = 0; k < 20; k++) Put<uint8_t>(ram, b + 0x3E + k, uint8_t(r(0, 255)));
        }
    };
    auto setTypes = [&](uint8_t* ram, const std::array<uint8_t, 2>& types) {
        const uint32_t pads = Get<uint32_t>(ram, kPadBase);
        Put<uint8_t>(ram, pads + 0xDE, types[0]);
        Put<uint8_t>(ram, pads + 0xDE + 100, types[1]);
    };
    auto sameState = [&](const shell::AnalogPageObject& o, uint32_t page, const shell::AnalogGlobals& g, const std::vector<uint8_t>& blocks) {
        return std::memcmp(&o, At(guest.Ram(), page), sizeof o) == 0 && std::memcmp(&g, At(guest.Ram(), kGlobals), sizeof g) == 0 &&
               std::memcmp(blocks.data(), At(guest.Ram(), kBlock), blocks.size()) == 0;
    };
    auto loadState = [&](uint8_t* ram, uint32_t page, shell::AnalogPageObject& o, shell::AnalogGlobals& g, std::vector<uint8_t>& blocks) {
        std::memcpy(&o, At(ram, page), sizeof o);
        std::memcpy(&g, At(ram, kGlobals), sizeof g);
        blocks.assign(At(ram, kBlock), At(ram, kBlock) + 0x52 * 2);
    };
    { // 0x8001C2A0 / 0x8001C48C / 0x8001C690
        size_t items = 0, itemsBad = 0, resets = 0, resetsBad = 0, enters = 0, entersBad = 0;
        for (int n = 0; n < 2000; n++) {
            const uint32_t port = uint32_t(r(0, 1));
            const std::array<uint8_t, 2> types = {r.Chance(3) ? RandomType(r) : uint8_t(2), r.Chance(3) ? RandomType(r) : uint8_t(2)};
            for (int kind = 0; kind < 3; kind++) {
                fresh();
                uint8_t* ram = guest.Ram();
                randomBlock(ram, 0), randomBlock(ram, 1), setTypes(ram, types);
                shell::AnalogPageObject o;
                shell::AnalogGlobals g;
                std::vector<uint8_t> blocks;
                const uint32_t page = kPages[port];
                if (kind == 2) Put<int16_t>(ram, page + 2, int16_t(port));
                loadState(ram, page, o, g, blocks);
                uint8_t* block = blocks.data() + port * 0x52;
                int32_t original = 0, ours = 0;
                if (kind == 0) {
                    original = int32_t(guest.Call(kItems, port));
                    ours = shell::AnalogItems(g, types[port], block) ? 1 : 0;
                } else if (kind == 1) {
                    guest.Call(kReset, page, port);
                    shell::AnalogReset(o, int(port), block, data);
                } else {
                    original = int32_t(guest.Call(kEnter, page));
                    ours = shell::AnalogEnter(o, g, types[port], block);
                }
                const bool same = original == ours && sameState(o, page, g, blocks);
                size_t& cases = kind == 0 ? items : kind == 1 ? resets : enters;
                size_t& bad = kind == 0 ? itemsBad : kind == 1 ? resetsBad : entersBad;
                cases++;
                if (!same && bad++ < 3) std::printf("    MISMATCH %s port %u type %u: original %d ours %d\n", kind == 0 ? "AnaItems" : kind == 1 ? "AnaReset" : "AnaEnter", port, types[port], original, ours);
            }
        }
        Report("AnaItems", kItems, items, itemsBad, failures);
        Report("AnaReset", kReset, resets, resetsBad, failures);
        Report("AnaEnter", kEnter, enters, entersBad, failures);
    }
    { // 0x8001C7B8 after a real enter, over random pads and live bytes, several fields in a row
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 1500; n++) {
            fresh();
            uint8_t* ram = guest.Ram();
            const uint32_t port = uint32_t(r(0, 1)), page = kPages[port];
            std::array<uint8_t, 2> types = {uint8_t(2), uint8_t(2)};
            if (r.Chance(4)) types[1 - port] = RandomType(r);
            randomBlock(ram, 0), randomBlock(ram, 1), setTypes(ram, types);
            Put<int16_t>(ram, page + 2, int16_t(port));
            guest.Call(kEnter, page); // the same start for both sides
            if (r.Chance(3)) Put<int16_t>(ram, page + 4, int16_t(r(0, 3)));
            shell::AnalogPageObject o;
            shell::AnalogGlobals g;
            std::vector<uint8_t> blocks;
            loadState(guest.Ram(), page, o, g, blocks);
            const uint32_t pads = Get<uint32_t>(guest.Ram(), kPadBase);
            for (int field = 0; field < 8; field++) {
                if (r.Chance(12)) types[port] = RandomType(r), setTypes(guest.Ram(), types);
                std::array<MenuListPad, 2> pad{};
                for (uint32_t k = 0; k < 2; k++) {
                    static const uint32_t kBits[] = {0x1, 0x2, 0x4, 0x8, 0x1000, 0x10000, 0x200, 0x800, 0x100, 0x400, 0x10};
                    if (!r.Chance(3)) pad[k].pressed = kBits[r(0, 10)] | (r.Chance(6) ? kBits[r(0, 10)] : 0u);
                    if (r.Chance(4)) pad[k].repeat = kBits[r(0, 3)];
                    pad[k].held = pad[k].pressed;
                    PutPad(guest.Ram(), pads + 0x1A4 + k * 0x10, pad[k]);
                }
                std::array<uint8_t, 4> raw{};
                for (uint32_t k = 0; k < 4; k++) raw[k] = uint8_t(AxisValue(r));
                for (uint32_t k = 0; k < 4; k++) Put<uint8_t>(guest.Ram(), kReceive + port * 0x22 + 4 + k, raw[k]);
                const int32_t original = int32_t(guest.Call(kUpdate, page));
                sounds.clear();
                const int ours = shell::AnalogUpdate(o, g, types, pad, raw, blocks.data() + port * 0x52, sounds);
                cases++;
                if ((original != ours || !sameState(o, page, g, blocks)) && bad++ < 3)
                    std::printf("    MISMATCH AnaUpdate field %d state %d item %d: original %d ours %d\n", field, int(o.state), int(o.item), original, ours);
                if (original != ours || !sameState(o, page, g, blocks)) break;
            }
        }
        Report("AnaUpdate", kUpdate, cases, bad, failures);
    }
    return failures;
}

} // namespace

int VerifyPad(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, bool raceOverlay) {
    if (!ActiveProfile().reference) {
        std::printf("%-10s skipped (the controller rows run on the US Simulation v1.2 build only)\n", "Pad");
        return 0;
    }
    int failures = 0;
    Rng r{rng};
    const input::PadTables tables = TablesOf(pristine.data());
    auto fresh = [&] { std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize); };

    { // 0x80083A4C
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 3000; n++) {
            fresh();
            const uint32_t raw = r.Chance(3) ? r.Word() : uint32_t(r(0, 0xFFFF));
            const uint32_t original = guest.Call(kRemapFn, raw, input::PadTables::kRemap, 16);
            const uint32_t ours = input::RemapButtons(raw, tables);
            cases++;
            if (original != ours && bad++ < 3) std::printf("    MISMATCH PadRemap %08X: original %08X ours %08X\n", raw, original, ours);
        }
        Report("PadRemap", kRemapFn, cases, bad, failures);
    }
    { // 0x80083A88
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 3000; n++) {
            fresh();
            std::array<uint16_t, 4> analog{};
            for (uint16_t& v : analog) v = AxisValue(r);
            for (uint32_t k = 0; k < 4; k++) Put<uint16_t>(guest.Ram(), kWords + k * 2, analog[k]);
            const uint32_t original = guest.Call(kNegBtnFn, kWords, input::PadTables::kNegconButtons, 3);
            const uint32_t ours = input::NegconButtons(analog, tables);
            cases++;
            if (original != ours && bad++ < 3) std::printf("    MISMATCH NegBtn: original %08X ours %08X\n", original, ours);
        }
        Report("NegBtn", kNegBtnFn, cases, bad, failures);
    }
    { // 0x80085890
        size_t cases = 0, bad = 0;
        for (uint32_t raw = 0; raw < 0x10000; raw += raw < 0x200 ? 1 : 97) {
            const uint32_t original = guest.Call(kAxisFn, raw);
            const uint16_t ours = input::DualShockAxis(uint16_t(raw));
            cases++;
            if (uint16_t(original) != ours && bad++ < 3) std::printf("    MISMATCH PadAxis %u: original %u ours %u\n", raw, original, ours);
        }
        Report("PadAxis", kAxisFn, cases, bad, failures);
    }
    { // 0x800831BC / 0x80083250
        size_t cases = 0, bad = 0, pedalCases = 0, pedalBad = 0;
        for (int n = 0; n < 4000; n++) {
            fresh();
            RandomCalibration(guest.Ram(), r);
            const uint32_t port = uint32_t(r(0, 1));
            const uint16_t raw = AxisValue(r);
            const uint8_t* calibration = At(guest.Ram(), input::PadTables::kCalibration + port * 20);
            std::array<uint8_t, 20> copy{};
            std::memcpy(copy.data(), calibration, 20);
            const uint32_t original = guest.Call(kTwistFn, port, 0, raw);
            const uint16_t ours = input::NegconTwist(copy.data(), raw);
            cases++;
            if (uint16_t(original) != ours && bad++ < 3) std::printf("    MISMATCH NegTwist raw %u: original %u ours %u\n", raw, original, ours);
            const int axis = r(1, 3);
            const uint32_t pedal = guest.Call(kPedalFn, port, uint32_t(axis), raw);
            const uint16_t ourPedal = input::NegconPedal(copy.data(), axis, raw);
            pedalCases++;
            if (uint16_t(pedal) != ourPedal && pedalBad++ < 3) std::printf("    MISMATCH NegPedal axis %d raw %u: original %u ours %u\n", axis, raw, pedal, ourPedal);
        }
        Report("NegTwist", kTwistFn, cases, bad, failures);
        Report("NegPedal", kPedalFn, pedalCases, pedalBad, failures);
    }
    { // 0x800838B4 on a random tracker
        StatefulResult total;
        for (int n = 0; n < 600; n++) {
            const uint32_t bits = r.Chance(3) ? r.Word() : uint32_t(r(0, 0x3FFFF));
            const StatefulResult one = VerifyStateful(
                guest, pristine, kTrackFn, {kObject}, 1, [&](uint8_t* ram, uint8_t*, uint32_t object, size_t) { RandomTracker(At(ram, object), r); },
                [&](uint8_t* ram, uint8_t*, uint32_t object) { input::TrackButtons(At(ram, object), bits); }, bits);
            total.cases += one.cases, total.mismatches += one.mismatches;
        }
        Report("PadTrack", kTrackFn, total.cases, total.mismatches, failures);
    }
    // The race's handlers on a random pad object (+ the analogue words at kWords).
    auto handlerRow = [&](const char* name, uint32_t function, int kind) {
        StatefulResult total;
        for (int n = 0; n < 500; n++) {
            const uint32_t raw = r.Chance(4) ? r.Word() : uint32_t(r(0, 0xFFFF));
            std::array<uint16_t, 4> analog{};
            for (uint16_t& v : analog) v = AxisValue(r);
            const StatefulResult one = VerifyStateful(
                guest, pristine, function, {kObject}, 1,
                [&](uint8_t* ram, uint8_t*, uint32_t object, size_t) {
                    uint8_t* o = At(ram, object);
                    for (uint32_t k = 0; k < input::pad_object::kSize; k++) o[k] = uint8_t(r(0, 255));
                    RandomTracker(o + input::pad_object::kTracker, r);
                    Put<int16_t>(ram, object, int16_t(r(0, 1))); // the port (the neGcon handler picks its calibration by it)
                    for (uint32_t k = 0; k < 4; k++) Put<uint16_t>(ram, kWords + k * 2, analog[k]);
                    RandomCalibration(ram, r);
                },
                [&](uint8_t* ram, uint8_t*, uint32_t object) {
                    uint8_t* o = At(ram, object);
                    if (kind == 0) input::DigitalHandler(o, raw, tables);
                    else if (kind == 1) input::AnalogHandler(o, raw, analog, tables);
                    else input::NegconHandler(o, raw, analog, tables, At(ram, input::PadTables::kCalibration + uint32_t(Get<int16_t>(ram, object)) * 20));
                },
                raw, kWords, 1);
            total.cases += one.cases, total.mismatches += one.mismatches;
        }
        Report(name, function, total.cases, total.mismatches, failures);
    };
    handlerRow("PadDigi", kDigitalFn, 0);
    handlerRow("PadAnlg", kAnalogFn, 1);
    handlerRow("PadNegc", kNegconFn, 2);
    { // 0x8008371C up to its PadSetAct (0x800874B8): the two actuator bytes of the port's block (0x801F0C88 + port * 8)
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 3000; n++) {
            fresh();
            uint8_t* ram = guest.Ram();
            const uint32_t port = uint32_t(r(0, 1));
            const uint32_t object = kPad1 + port * kPadStride;
            Put<int16_t>(ram, object, int16_t(port));
            Put<int16_t>(ram, object + input::pad_object::kVibration + 2, int16_t(r.Chance(3) ? r(-0x8000, 0x7FFF) : r(0, 255) << 4));
            Put<int16_t>(ram, object + input::pad_object::kVibration + 4, int16_t(r.Chance(3) ? r(-0x8000, 0x7FFF) : r(0, 1) << 4));
            const uint8_t mode = uint8_t(r.Chance(4) ? r(0, 255) : r(0, 2));
            Put<uint8_t>(ram, kPadManager + 0x19 + port * 8, mode);
            std::array<uint8_t, input::pad_object::kSize> copy{};
            std::memcpy(copy.data(), At(ram, object), copy.size());
            if (!guest.CallUntil(kActFn, kSetActFn, object)) throw std::runtime_error("PadAct: 0x8008371C did not reach PadSetAct");
            const input::Actuators ours = input::ActuatorsOf(copy.data(), mode);
            const uint8_t smallMotor = Get<uint8_t>(guest.Ram(), kPadManager + 0x1A + port * 8), largeMotor = Get<uint8_t>(guest.Ram(), kPadManager + 0x1B + port * 8);
            cases++;
            if ((smallMotor != ours.smallMotor || largeMotor != ours.largeMotor || guest.Reg(6) != 2) && bad++ < 3)
                std::printf("    MISMATCH PadAct mode %u: original %u / %u ours %u / %u\n", mode, smallMotor, largeMotor, ours.smallMotor, ours.largeMotor);
        }
        Report("PadAct", kActFn, cases, bad, failures);
    }
    // The title overlay (member 1) is loaded when the options' code is (the hash of verify_title.cpp).
    if (!raceOverlay) {
        if (Fnv(pristine.data() + (0x80017D74u & 0x1FFFFF), 0x2E8) == 0x02415340u) {
            failures += VerifyKeyConfig(guest, pristine, r);
            failures += VerifyAnalogConfig(guest, pristine, r);
        }
        else std::printf("%-10s skipped (the title overlay, GT2.OVL member 1, is not loaded in this dump)\n", "KeyConfig");
        return failures;
    }

    { // 0x80014BB4 on player 1's / player 2's pad object with random key tables in the career's pad block
        const StatefulResult result = VerifyStateful(
            guest, pristine, kLogicFn, {kPad1, kPad1 + kPadStride}, 800,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                uint8_t* o = At(ram, object);
                Put<uint16_t>(ram, kInterruptFlag, 1); // the critical-section helper 0x80081CF8 leaves at once (no BIOS calls)
                o[input::pad_object::kType] = RandomType(r);
                RandomTracker(o + input::pad_object::kTracker, r);
                Put<uint16_t>(ram, object + input::pad_object::kAxisMask, uint16_t(r.Chance(3) ? r(0, 0xFFFF) : r(0, 1) ? 0xF : 0x3F));
                for (uint32_t k = 0; k < 9; k++) Put<uint16_t>(ram, object + input::pad_object::kAxes + k * 2, variant % 4 == 0 ? Get<uint16_t>(pristine.data(), object + input::pad_object::kAxes + k * 2) : AxisValue(r));
                for (uint32_t k = input::pad_object::kSnapshot; k < input::pad_object::kSize; k++) o[k] = uint8_t(r(0, 255));
                const uint32_t block = kCareerPadBlock + (object == kPad1 ? 0 : 0x52);
                Put<uint32_t>(ram, object + input::pad_object::kPadBlock, block);
                if (variant % 4 != 0)
                    for (uint32_t k = 0; k < 44; k++) Put<uint8_t>(ram, block + k, TableEntry(r));
            },
            [&](uint8_t* ram, uint8_t*, uint32_t object) {
                input::BuildRaceLogical(At(ram, object), At(ram, Get<uint32_t>(ram, object + input::pad_object::kPadBlock)));
            });
        Report("PadLogic", kLogicFn, result.cases, result.mismatches, failures);
    }
    { // 0x800133F0 (the whole function) on car 0: its vibration tail against FeedVibration on the car's pad object
        size_t cases = 0, bad = 0;
        const uint32_t car = kCarBase;
        for (int n = 0; n < 600; n++) {
            fresh();
            uint8_t* ram = guest.Ram();
            const int16_t padIndex = int16_t(r(0, 1));
            Put<int16_t>(ram, car + 0x0C, padIndex);
            Put<int16_t>(ram, car + 0x18, int16_t(r.Chance(5) ? r(-2, 6) : r(1, 4)));
            Put<int32_t>(ram, car + 0x24, r(0, 2) ? 0 : int32_t(r.Word() & 0x7FFFFF));
            Put<int16_t>(ram, car + kBodyOffset + 0x760, int16_t(r.Chance(2) ? 0 : r(-0x8000, 0x7FFF)));
            Put<uint8_t>(ram, car + kBodyOffset + 0x762, uint8_t(r(0, 255)));
            Put<uint8_t>(ram, car + kBodyOffset + 0x763, uint8_t(r.Chance(3) ? r(0, 255) : r(0, 1)));
            Put<uint8_t>(ram, kCareerPadBlock + uint32_t(padIndex) * 0x52 + 0x2C, uint8_t(r.Chance(4) ? r(0, 255) : r(0, 1)));
            Put<uint8_t>(ram, kDemoFlag, uint8_t(r.Chance(4) ? 1 : 0));
            const uint32_t object = kPad1 + uint32_t(padIndex) * kPadStride;
            for (uint32_t k = 0; k < 8; k++) Put<uint8_t>(ram, object + input::pad_object::kVibration + k, uint8_t(r(0, 255)));
            std::array<uint8_t, input::pad_object::kSize> copy{};
            std::memcpy(copy.data(), At(ram, object), copy.size());
            input::VibrationSource v;
            v.demo = Get<uint8_t>(ram, kDemoFlag) != 0;
            v.padSlot = Get<int16_t>(ram, car + 0x18);
            v.vibrationOff = Get<uint8_t>(ram, kCareerPadBlock + uint32_t(padIndex) * 0x52 + 0x2C);
            v.finishTime = Get<int32_t>(ram, car + 0x24);
            v.word760 = Get<int16_t>(ram, car + kBodyOffset + 0x760);
            v.loadLevel = Get<uint8_t>(ram, car + kBodyOffset + 0x762);
            v.impactFlag = Get<uint8_t>(ram, car + kBodyOffset + 0x763);
            try {
                guest.Call(kRenderCarFn, car);
            } catch (const std::runtime_error& e) {
                std::printf("    skipped (original trapped): %s\n", e.what());
                continue;
            }
            input::FeedVibration(copy.data(), v);
            cases++;
            if (std::memcmp(copy.data() + input::pad_object::kVibration, At(guest.Ram(), object + input::pad_object::kVibration), 8) != 0 && bad++ < 3)
                std::printf("    MISMATCH VibFeed slot %d finish %d: the pad object's +0x5A..+0x61 differ\n", int(v.padSlot), int(v.finishTime));
        }
        Report("VibFeed", kRenderCarFn, cases, bad, failures);
    }
    return failures;
}

} // namespace gt2::verify
