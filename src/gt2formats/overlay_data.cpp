#include "gt2formats/overlay_data.h"

#include <cstdio>
#include <algorithm>
#include <span>
#include <stdexcept>

#include "gt2formats/exe_profile.h"
#include "gt2formats/sha1.h"
#include "gt2vfs/inflate.h"

namespace gt2 {
namespace {

uint32_t ReadU32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }

std::vector<uint8_t> ReadRootFile(const DiscImage& disc, const IsoFile& file) {
    std::vector<uint8_t> data(file.size);
    disc.ReadForm1(file.lba, 0, data.data(), data.size());
    return data;
}

} // namespace

const uint8_t* GuestImage::At(uint32_t address, size_t size) const {
    if (!Contains(address, size)) {
        char text[96];
        std::snprintf(text, sizeof(text), "guest image: 0x%08X + %zu is outside 0x%08X..0x%08X", address, size, base, End());
        throw std::out_of_range(text);
    }
    return bytes.data() + (address - base);
}

uint32_t GuestImage::Sim(uint32_t simAddress) const {
    if (unknownBuild) (void)ProfileOf(*this); // throws: unknown executable
    if (!profile) return simAddress;
    return module == kRaceRam ? profile->Race(simAddress) : profile->Data(simAddress, module);
}

uint32_t UiAddress(const GuestImage& image, uint32_t address, bool arcade) {
    if (!image.profile || (image.profile->build != ExeBuild::kSimEu && image.profile->build != ExeBuild::kArcadeEu))
        return address;
    auto profile = *image.profile;
    if (arcade)
        profile.ranges = ArcadeEuUiRanges();
    return profile.Data(address, image.module);
}
GuestImage UiLayout(GuestImage image, bool arcade) {
    if (!image.profile || (image.profile->build != ExeBuild::kSimEu && image.profile->build != ExeBuild::kArcadeEu))
        return image;
    const auto ranges = arcade ? ArcadeEuUiRanges() : image.profile->ranges;
    uint32_t end = image.base;
    for (const auto &r : ranges)
        if (r.scope == image.module && r.kind == ProfileRange::kAligned)
            end = std::max(end, r.simEnd);
    if (end <= image.base || end - image.base > 0x100000)
        throw std::runtime_error("invalid UI layout bounds");
    GuestImage out;
    out.base = image.base;
    out.module = image.module;
    out.fileName = image.fileName;
    out.fileSha1 = image.fileSha1;
    out.profile = arcade ? &KnownProfiles()[1] : &SimProfile();
    out.bytes.resize(end - image.base);
    std::vector<uint32_t> source(out.bytes.size()), inverse(image.bytes.size());
    for (auto kind : {ProfileRange::kAligned, ProfileRange::kRefRun, ProfileRange::kFact})
        for (const auto &r : ranges) {
            if (r.kind != kind || r.scope != image.module)
                continue;
            for (uint32_t a = std::max(out.base, r.simStart); a < std::min(end, r.simEnd); ++a) {
                const uint32_t b = uint32_t(int64_t(a) + r.delta);
                if (image.Contains(b, 1)) {
                    out.bytes[a - out.base] = image.bytes[b - image.base];
                    source[a - out.base] = b;
                    inverse[b - image.base] = a;
                }
            }
        }
    for (size_t at = 0; at + 4 <= out.bytes.size(); at += 4) {
        if (!source[at] || (source[at] & 3) || source[at + 3] != source[at] + 3)
            continue;
        uint32_t word;
        std::memcpy(&word, out.bytes.data() + at, 4);
        uint32_t translated = 0;
        // Only overlay-local pointers and RAM text/state pointers are relocated.
        // EXE words resembling resident pointers can be packed CLUT colours.
        if (image.module >= 0 && image.Contains(word, 1))
            translated = inverse[word - image.base];
        else if (word >= 0x800B0000 && word < 0x80200000) {
            for (int kind = ProfileRange::kFact; kind <= ProfileRange::kRefRun && !translated; ++kind)
                for (const auto &r : ranges) {
                    if (r.scope != -1 || r.kind != kind)
                        continue;
                    const int64_t a = int64_t(word) - r.delta;
                    if (a >= r.simStart && a < r.simEnd) {
                        translated = uint32_t(a);
                        break;
                    }
                }
        }
        if (translated)
            std::memcpy(out.bytes.data() + at, &translated, 4);
    }
    return out;
}
GuestImage LoadExeImage(const DiscImage &disc) {
    std::string name;
    for (const IsoFile &f : disc.RootFiles())
        if (f.name.rfind("SCUS_", 0) == 0 || f.name.rfind("SCES_", 0) == 0 || f.name.rfind("SCPS_", 0) == 0)
            name = f.name;
    auto file = disc.FindRootFile(name);
    if (!file)
        throw std::runtime_error("executable not found in the disc root");
    const std::vector<uint8_t> raw = ReadRootFile(disc, *file);
    if (raw.size() < 0x800 || std::memcmp(raw.data(), "PS-X EXE", 8) != 0)
        throw std::runtime_error(name + ": not a PS-X EXE");
    const uint32_t textAddress = ReadU32(raw.data() + 0x18), textSize = ReadU32(raw.data() + 0x1C);
    if (textSize == 0 || textSize > raw.size() - 0x800)
        throw std::runtime_error(name + ": bad t_size");
    GuestImage image;
    image.base = textAddress;
    image.bytes.assign(raw.begin() + 0x800, raw.begin() + 0x800 + textSize);
    image.fileName = name;
    image.fileSha1 = Sha1Hex(raw);
    image.profile = FindProfile(image.fileSha1);
    image.unknownBuild = image.profile == nullptr;
    image.module = -1;
    return image;
}

GuestImage LoadOverlayImage(const DiscImage& disc, uint32_t index) {
    const GuestImage exe = LoadExeImage(disc);
    auto file = disc.FindRootFile("GT2.OVL");
    if (!file) throw std::runtime_error("GT2.OVL not found in the disc root");
    const std::vector<uint8_t> ovl = ReadRootFile(disc, *file);
    if (ovl.size() < 8) throw std::runtime_error("GT2.OVL: too small");
    const uint32_t count = ReadU32(ovl.data()) / 8;
    if (count == 0 || count > 64 || count * 8 > ovl.size()) throw std::runtime_error("GT2.OVL: bad member table");
    if (index >= count) throw std::runtime_error("GT2.OVL: member index out of range");
    const uint32_t offset = ReadU32(ovl.data() + index * 8), packed = ReadU32(ovl.data() + index * 8 + 4);
    if (offset > ovl.size() || packed > ovl.size() - offset) throw std::runtime_error("GT2.OVL: member outside the file");
    GuestImage image;
    image.base = kOverlayLoadAddress;
    image.bytes = Gunzip(std::span<const uint8_t>(ovl.data() + offset, packed));
    image.fileName = exe.fileName;
    image.fileSha1 = exe.fileSha1;
    image.profile = exe.profile;
    image.unknownBuild = exe.unknownBuild;
    image.module = int(index);
    return image;
}

} // namespace gt2
