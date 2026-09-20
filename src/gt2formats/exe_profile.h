#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace gt2 {

class DiscImage;
struct GuestImage;

// Address profile of a build of the program (docs/research/arcade_disc.md sections 5 and 8). The ported code names
// every table / global / function by its address in the reference build, US Simulation v1.2 (SCUS_944.88 SHA-1
// 3030aa271c0a4022fc69ce09d76a6bc75e69a32a). Another build of the same code base (US Arcade v1.1, SCUS_944.55 SHA-1
// 231f9dba7191b9ef915621662afdc40a7c66df95) holds the same data at moved addresses; its profile translates a Simulation
// address to the build's address. The profile is selected by the SHA-1 of the disc's executable; its ranges are FACTS
// generated from db/<build>_symbols.yaml by `gt2tool gen-profile` (src/gt2formats/exe_profiles.inc, never edited by hand).
//
// Scope of an address: -1 = the resident executable, its BSS, the rest of RAM and the scratchpad; 0..5 = GT2.OVL member N
// (all members load at 0x80010000, so an overlay address means nothing without its member).
enum class ExeBuild : uint8_t { kSimUs12, kArcadeUs11, kSimEu, kArcadeEu };

struct ProfileRange {
    enum Kind : uint8_t {
        kFact = 0,    // a verified fact of db/*.yaml (an address or table with its size)
        kRefRun = 1,  // a run of lui-built data references of aligned code with one delta (gt2tool exe-map)
        kAligned = 2, // a run of the word alignment of a module image (code and static data; gt2formats/exe_map.h)
    };
    int8_t scope = -1;
    Kind kind = kFact;
    uint32_t simStart = 0, simEnd = 0; // [simStart, simEnd), Simulation v1.2 addresses
    int32_t delta = 0;                 // build address = Simulation address + delta
};

struct ExeProfile {
    const char* name = "";     // "US Simulation v1.2"
    const char* exeName = "";  // "SCUS_944.88"
    const char* exeSha1 = "";  // lower-case hex
    ExeBuild build = ExeBuild::kSimUs12;
    bool reference = false;    // the Simulation v1.2 build: every address maps to itself
    std::span<const ProfileRange> ranges;
    // Logic that differs between the builds (arcade_disc.md section 5.3; everything else is instruction-identical modulo
    // relocations):
    //   race_frame 0x80015B64: the Simulation build polls a second pad slot (race object + 0x140) and a flag table
    //   (0x801D98E0 + 0xBF7C) for the pause / quit input; the Arcade build reads pad 1 only.
    bool raceFramePadSlot2 = true;
    //   UpdateWheelEffects 0x800426F0: the Simulation build ends with the clear of the effect levels of a car of contact
    //   type 2 (ground.cpp); the Arcade build (0x8004269C) returns after the levels.
    bool wheelEffectsTail = true;
    bool arcade = false;       // the disc's flow is the arcade one (title "ARCADE MODE DISC", arcade menus, usa_arcade_data)

    // The build's address of the Simulation address `simAddress` in `scope`; nullopt when no fact / run covers it.
    std::optional<uint32_t> TryData(uint32_t simAddress, int scope) const;
    // TryData or std::runtime_error naming the address and the build.
    uint32_t Data(uint32_t simAddress, int scope) const;
    // Data in the race's memory map: the race overlay (member 0) below its end, the executable / RAM above.
    uint32_t Race(uint32_t simAddress) const { return Data(simAddress, RaceScope(simAddress)); }
    // A code address of the race's memory map (aligned runs only).
    uint32_t Code(uint32_t simAddress) const;
    std::optional<uint32_t> TryCode(uint32_t simAddress) const;

    // Scope of a Simulation address while the race overlay is loaded: member 0 spans 0x80010000..0x8005D5F8 (its
    // inflated size 316,920 bytes); the resident code and data start above it.
    static int RaceScope(uint32_t simAddress) { return simAddress >= 0x80010000u && simAddress < kSimRaceOverlayEnd ? 0 : -1; }
    static constexpr uint32_t kSimRaceOverlayEnd = 0x8005D5F8u;
};

// The known builds.
std::span<const ExeProfile> KnownProfiles();
const ExeProfile& SimProfile();
std::span<const ProfileRange> ArcadeEuUiRanges();
const ExeProfile* FindProfile(std::string_view exeSha1);
// The profile of an executable image (GuestImage::fileSha1, set by LoadExeImage); throws for an unknown build.
const ExeProfile& ProfileOf(const GuestImage& exe);
// The profile of a disc (its executable's SHA-1); throws for an unknown build.
const ExeProfile& ProfileOf(const DiscImage& disc);

// The process-wide profile of the disc being played / verified (tools that read guest RAM by Simulation address:
// gt2verify, gt2game's capture comparison). Defaults to the Simulation profile.
const ExeProfile& ActiveProfile();
void SetActiveProfile(const ExeProfile& profile);
// Race-map translation with the active profile (shorthand for ActiveProfile().Race / Code).
inline uint32_t RaceAddress(uint32_t simAddress) { return ActiveProfile().Race(simAddress); }

} // namespace gt2
