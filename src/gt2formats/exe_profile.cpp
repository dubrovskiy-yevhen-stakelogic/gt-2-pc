#include "gt2formats/exe_profile.h"

#include <array>
#include <cstdio>
#include <stdexcept>

#include "gt2formats/overlay_data.h"

namespace gt2 {
namespace {

// kArcadeUs11Ranges: GENERATED from db/arcade_us11_symbols.yaml by `gt2tool gen-profile` (facts only).
#include "gt2formats/exe_profiles.inc"
#include "gt2formats/arcade_eu_ui.inc"

const ExeProfile kProfiles[] = {
    {
        "US Simulation v1.2", "SCUS_944.88", "3030aa271c0a4022fc69ce09d76a6bc75e69a32a", ExeBuild::kSimUs12, true, {},
        true, true, false,
    },
    {
        // race_frame: Arcade 0x80015B64 (same address, 178 words differ: pad 1 only); UpdateWheelEffects: Arcade 0x8004269C
        // without the Simulation tail (gt2tool exe-map, arcade_disc.md section 5.3).
        "US Arcade v1.1", "SCUS_944.55", "231f9dba7191b9ef915621662afdc40a7c66df95", ExeBuild::kArcadeUs11, false,
        std::span<const ProfileRange>(kArcadeUs11Ranges.data(), kArcadeUs11Ranges.size()), false, false, true,
    },
    { "Europe Simulation", "SCES_123.80", "5172a19c1d0fe07a2a61966653b755afe26d9e69", ExeBuild::kSimEu, false,
      std::span<const ProfileRange>(kSimEuRanges.data(), kSimEuRanges.size()), true, true, false },
    { "Europe Arcade", "SCES_023.80", "2b59ad844a4dbb934fc1d8ef955c63038f54e932", ExeBuild::kArcadeEu, false,
      std::span<const ProfileRange>(kArcadeEuRanges.data(), kArcadeEuRanges.size()), true, true, true },
};

const ExeProfile* g_active = &kProfiles[0];

std::string Hex(uint32_t v) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", v);
    return text;
}

} // namespace

std::optional<uint32_t> ExeProfile::TryData(uint32_t simAddress, int scope) const {
    if (reference) return simAddress;
    for (int kind = ProfileRange::kFact; kind <= ProfileRange::kAligned; kind++)
        for (const ProfileRange& r : ranges)
            if (r.kind == kind && r.scope == scope && simAddress >= r.simStart && simAddress < r.simEnd) return uint32_t(int64_t(simAddress) + r.delta);
    return std::nullopt;
}

uint32_t ExeProfile::Data(uint32_t simAddress, int scope) const {
    if (const auto a = TryData(simAddress, scope)) return *a;
    throw std::runtime_error(std::string("address profile ") + name + ": no fact covers Simulation address " + Hex(simAddress) +
                             (scope < 0 ? std::string(" (executable / RAM)") : " (GT2.OVL member " + std::to_string(scope) + ")") +
                             " - add it to db/*.yaml and regenerate (gt2tool gen-profile)");
}

std::optional<uint32_t> ExeProfile::TryCode(uint32_t simAddress) const {
    if (reference) return simAddress;
    const int scope = RaceScope(simAddress);
    for (const ProfileRange& r : ranges)
        if (r.kind == ProfileRange::kAligned && r.scope == scope && simAddress >= r.simStart && simAddress < r.simEnd) return uint32_t(int64_t(simAddress) + r.delta);
    return std::nullopt;
}

uint32_t ExeProfile::Code(uint32_t simAddress) const {
    if (const auto a = TryCode(simAddress)) return *a;
    throw std::runtime_error(std::string("address profile ") + name + ": code address " + Hex(simAddress) + " is outside the aligned runs");
}

std::span<const ExeProfile> KnownProfiles() { return kProfiles; }
const ExeProfile& SimProfile() { return kProfiles[0]; }
std::span<const ProfileRange> ArcadeEuUiRanges() { return kArcadeEuUiRanges; }

const ExeProfile* FindProfile(std::string_view exeSha1) {
    for (const ExeProfile& p : kProfiles)
        if (exeSha1 == p.exeSha1) return &p;
    return nullptr;
}

const ExeProfile& ProfileOf(const GuestImage& exe) {
    if (exe.profile) return *exe.profile;
    throw std::runtime_error("executable " + exe.fileName + " (SHA-1 " + exe.fileSha1 +
                             ") is not a known build: supported are US Simulation v1.2 (SCUS_944.88), US Arcade v1.1 (SCUS_944.55), Europe Simulation (SCES_123.80) and Europe Arcade (SCES_023.80)");
}

const ExeProfile& ProfileOf(const DiscImage& disc) { return ProfileOf(LoadExeImage(disc)); }

const ExeProfile& ActiveProfile() { return *g_active; }
void SetActiveProfile(const ExeProfile& profile) { g_active = &profile; }

} // namespace gt2
