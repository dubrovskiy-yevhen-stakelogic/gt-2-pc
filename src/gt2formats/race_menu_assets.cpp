#include "gt2formats/race_menu_assets.h"

#include <cstring>
#include <stdexcept>

#include "gt2formats/title_assets.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"

namespace gt2 {

namespace {

uint16_t U16(const std::vector<uint8_t>& b, size_t o) {
    if (o + 2 > b.size()) throw std::runtime_error("race menu assets: read past the end");
    return uint16_t(b[o] | (b[o + 1] << 8));
}
uint32_t U32(const std::vector<uint8_t>& b, size_t o) { return uint32_t(U16(b, o)) | (uint32_t(U16(b, o + 2)) << 16); }

std::vector<uint8_t> Block(const std::vector<uint8_t>& txd, uint32_t stride, uint8_t language, const char* what) {
    const size_t at = size_t(language) * stride;
    if (at + stride > txd.size()) throw std::runtime_error(std::string("race menu assets: no language block in ") + what);
    std::vector<uint8_t> block(txd.begin() + std::ptrdiff_t(at), txd.begin() + std::ptrdiff_t(at + stride));
    block.push_back(0);
    return block;
}

std::string CString(const std::vector<uint8_t>& b, size_t at) {
    std::string s;
    while (at < b.size() && b[at] != 0) s.push_back(char(b[at++]));
    return s;
}

} // namespace

void UploadTimImage(MenuVram& vram, const std::vector<uint8_t>& tim, int x, int y) {
    if (U32(tim, 0) != 0x10) throw std::runtime_error("race menu assets: not a TIM");
    size_t image = 8;
    if (U32(tim, 4) & 8) image += U32(tim, 8);
    const int w = U16(tim, image + 8), h = U16(tim, image + 10);
    if (image + 12 + size_t(w) * size_t(h) * 2 > tim.size()) throw std::runtime_error("race menu assets: TIM image block truncated");
    vram.Upload(x, y, w, h, std::span<const uint8_t>(tim).subspan(image + 12));
}

RaceMenuAssets RaceMenuAssets::Load(const DiscImage& disc, const GtfsVolume& vol, Pictures pictures, uint8_t language) {
    if (language != 1) throw std::runtime_error("race menu assets: only the US language block (1) is mapped to files");
    RaceMenuAssets a;
    a.language = language;
    a.pictures = pictures;
    a.ovl0 = UiLayout(LoadOverlayImage(disc, 0));
    a.exe = UiLayout(LoadExeImage(disc));
    a.raceText = Block(vol.Read(".text/data-race.txd"), kRaceTextStride, language, "data-race.txd");
    const GuestImage ovl1 = LoadOverlayImage(disc, 1);
    a.globalText = Block(InflateEmbeddedGzipNamed(ovl1, "data-global.txd"), kGlobalTextStride, language, "data-global.txd");
    a.licenceInfo = vol.Read("arcade/license_info_us");
    if (a.licenceInfo.size() < 0x4B4 || U16(a.licenceInfo, 0) != 6 || U16(a.licenceInfo, 2) != 10 || U32(a.licenceInfo, 4) != 0x4B4)
        throw std::runtime_error("arcade/license_info_us: unexpected header");

    // VRAM (header comment): 0x80047CAC.
    UploadTimImage(a.vram, vol.Read("arcade/arc_font.tim"), 0x180, 0);
    UploadTimImage(a.vram, vol.Read(pictures == Pictures::kLicence ? "arcade/license_tim.tim" : "arcade/setting.tim"), 0x180, 0x100);

    // Fonts: arc_fontinfo at 0x801A8C00 plus the four descriptors appended behind it (in the original they are at
    // 0x801C9110.. and 0x80047EA0 fills them).
    GuestImage info;
    info.base = kFontInfoAddress;
    info.bytes = vol.Read("arcade/arc_fontinfo");
    if (info.bytes.size() < 0x24 || U32(info.bytes, 0) != 8) throw std::runtime_error("arc_fontinfo: unexpected header");
    info.bytes.resize((info.bytes.size() + 3) & ~size_t(3));
    const uint32_t descriptors = info.End();
    for (int f = 0; f < 4; f++) {
        const uint32_t glyphs = kFontInfoAddress + U32(info.bytes, 4 + size_t(f) * 8), kerning = kFontInfoAddress + U32(info.bytes, 8 + size_t(f) * 8);
        uint8_t d[16] = {};
        std::memcpy(d, &glyphs, 4);
        std::memcpy(d + 4, &kerning, 4);
        d[8] = kFontCells[f];
        info.bytes.insert(info.bytes.end(), d, d + 16);
    }
    for (int f = 0; f < 4; f++) {
        HudFont& font = a.fonts[size_t(f)];
        font = LoadHudFont(info, descriptors + uint32_t(f) * 16);
        font.tpage = kFontPage;
        font.clutBase = uint16_t((kFontPage & 0xF) * 4 + (kFontPage & 0x10) * 0x400);
    }
    return a;
}

const HudFont& RaceMenuAssets::FontAt(uint32_t descriptor) const {
    for (size_t i = 0; i < 4; i++)
        if (kFontDescriptors[i] == descriptor) return fonts[i];
    throw std::runtime_error("race menu assets: unknown font descriptor");
}

bool RaceMenuAssets::HasText(uint32_t address) const {
    return (address >= kRaceTextBase && address - kRaceTextBase < raceText.size()) ||
           (address >= kGlobalTextBase && address - kGlobalTextBase < globalText.size()) || ovl0.Contains(address, 1);
}

std::string RaceMenuAssets::Text(uint32_t address) const {
    if (address >= kRaceTextBase && address - kRaceTextBase < raceText.size()) return CString(raceText, address - kRaceTextBase);
    if (address >= kGlobalTextBase && address - kGlobalTextBase < globalText.size()) return CString(globalText, address - kGlobalTextBase);
    if (ovl0.Contains(address, 1)) return CString(ovl0.bytes, address - ovl0.base);
    return {};
}

RaceMenuAssets::LicenceInfo RaceMenuAssets::Licence(int licence, int test) const {
    if (licence < 0 || licence >= 6 || test < 0 || test >= 10) throw std::runtime_error("race menu assets: licence / test out of range");
    const size_t record = 8 + size_t(licence) * 200 + size_t(test) * 0x14; // 0x8004CCF8: 0x8017389C + licence * 200 + test * 0x14
    LicenceInfo info;
    info.title = CString(licenceInfo, U16(licenceInfo, record));
    const int count = U16(licenceInfo, record + 2);
    if (count > 8) throw std::runtime_error("arcade/license_info_us: more than 8 lines");
    for (int i = 0; i < count; i++) info.lines.push_back(CString(licenceInfo, U16(licenceInfo, record + 4 + size_t(i) * 2)));
    return info;
}

} // namespace gt2
