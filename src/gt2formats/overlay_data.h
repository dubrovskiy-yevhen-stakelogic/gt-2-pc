#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gt2vfs/disc_image.h"

namespace gt2 {

// A module of the original program as it sits in guest RAM: the bytes of the file on the disc and the address
// they are loaded at. Used to read DATA TABLES of the executable / overlays natively (no code is executed).
// Facts (US Simulation v1.2, SCUS_944.88 SHA-1 3030aa27..., docs/research/scout_exe.md sections 2-4):
//   - SCUS_944.88 is a PS-X EXE: 0x800-byte header with t_addr / t_size at +0x18 / +0x1C (0x80010000 / 0x99000);
//     the image follows the header, so address A is file offset 0x800 + (A - t_addr);
//   - GT2.OVL holds 6 gzip members behind a table of { u32 offset, u32 packedSize } pairs at offset 0; every
//     member is loaded at 0x80010000 (the loader 0x8005DAD8 inflates member[i] there). Member 0 is the race
//     overlay: code 0x80010000..0x8005A77C, its tuning tables at 0x80046Cxx..0x80046Fxx.
struct ExeProfile;

struct GuestImage {
    uint32_t base = 0;
    std::vector<uint8_t> bytes;
    // The build the image belongs to (gt2formats/exe_profile.h), set by LoadExeImage / LoadOverlayImage from the SHA-1 of
    // the disc's executable; null = an image in the Simulation v1.2 layout (e.g. built from a RAM dump of that build).
    const ExeProfile* profile = nullptr;
    bool unknownBuild = false;   // the executable's SHA-1 matched no profile: Sim() throws
    int module = -1;             // -1 = the executable, N = GT2.OVL member N, kRaceRam = a RAM image of a race (member 0 + executable)
    static constexpr int kRaceRam = -2;
    std::string fileName, fileSha1; // the executable's root file name and SHA-1 (of the whole file)

    // The address in this image's build of the Simulation v1.2 address `simAddress` of the same module (the profile's
    // facts; identity for the Simulation build). Throws when the build is unknown or no fact covers the address.
    uint32_t Sim(uint32_t simAddress) const;

    uint32_t End() const { return base + uint32_t(bytes.size()); }
    bool Contains(uint32_t address, size_t size) const { return address >= base && size <= bytes.size() && address - base <= bytes.size() - size; }
    // Pointer to `size` bytes at a guest address; throws std::out_of_range when the range is not in the image.
    const uint8_t* At(uint32_t address, size_t size = 1) const;
    template <typename T> T Get(uint32_t address) const {
        T v;
        std::memcpy(&v, At(address, sizeof(T)), sizeof(T));
        return v;
    }
};

constexpr uint32_t kOverlayLoadAddress = 0x80010000u;
constexpr uint32_t kRaceOverlayIndex = 0;

// The root file "SCUS_xxx.xx" of the disc (the resident executable) at its load address.
GuestImage LoadExeImage(const DiscImage& disc);
// Relocate known European UI tables to the layout consumed by the native menu code.
// Gameplay and disc identification continue to use the original images/profiles.
GuestImage UiLayout(GuestImage image, bool arcade = false);
uint32_t UiAddress(const GuestImage& image, uint32_t address, bool arcade = false);
// Member `index` of GT2.OVL, inflated, at 0x80010000.
GuestImage LoadOverlayImage(const DiscImage& disc, uint32_t index);

} // namespace gt2
