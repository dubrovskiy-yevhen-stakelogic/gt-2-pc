#include "gt2formats/title_assets.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <stdexcept>

#include "gt2formats/exe_profile.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "gt2vfs/inflate.h"

namespace gt2 {

namespace {

uint16_t U16(const std::vector<uint8_t>& b, size_t o) {
    if (o + 2 > b.size()) throw std::runtime_error("title assets: read past the end");
    return uint16_t(b[o] | (b[o + 1] << 8));
}
uint32_t U32(const std::vector<uint8_t>& b, size_t o) { return uint32_t(U16(b, o)) | (uint32_t(U16(b, o + 2)) << 16); }

std::vector<uint8_t> LanguageBlock(const std::vector<uint8_t>& txd, uint32_t stride, uint8_t language) {
    const size_t at = size_t(language) * stride;
    if (at >= txd.size()) throw std::runtime_error("title assets: no text block for the language");
    std::vector<uint8_t> block(txd.begin() + std::ptrdiff_t(at), txd.begin() + std::ptrdiff_t(std::min(txd.size(), at + stride)));
    block.push_back(0);
    return block;
}

// 0x800111DC: the background TIM: its CLUT block to (384, 511) and its image block to (384, 0), with the blocks'
// own sizes (the TIM's destinations are not used).
void UploadTitleBackground(MenuVram& vram, const std::vector<uint8_t>& tim) {
    if (U32(tim, 0) != 0x10 || (U32(tim, 4) & 8) == 0) throw std::runtime_error("title background: not a TIM with a CLUT");
    const size_t clutBlock = 8, clutLength = U32(tim, clutBlock);
    vram.Upload(384, 511, U16(tim, clutBlock + 8), U16(tim, clutBlock + 10), std::span<const uint8_t>(tim).subspan(clutBlock + 12));
    const size_t image = clutBlock + clutLength;
    vram.Upload(384, 0, U16(tim, image + 8), U16(tim, image + 10), std::span<const uint8_t>(tim).subspan(image + 12));
}

// US Arcade v1.1 member 1 (SHA-1 20bb63ff66247bafc19cf1e9d939c90aa0871e2d; our disassembly, db/arcade_us11_symbols.yaml):
// 0x800161B4 inflates the gzip at 0x80020D88 and copies block language * 0xDAE (0xDAE bytes) to 0x801B9330; 0x8001D674 the
// gzip at 0x8002247C, block language * 0x76F (0x76F bytes) to 0x801EF0E0.
constexpr uint32_t kArcadeTitleTxdGzip = 0x80020D88u, kArcadeTitleText = 0x801B9330u, kArcadeTitleStride = 0xDAE;
constexpr uint32_t kArcadeGlobalTxdGzip = 0x8002247Cu, kArcadeGlobalText = 0x801EF0E0u, kArcadeGlobalStride = 0x76F;

// A build image copied to the Simulation addresses [simBase, simEnd) of `scope` (the ranges applied from the lowest priority to
// the highest, as ExeProfile::TryData orders them). `source[s - simBase]` = the build address of each copied byte (0 = none).
GuestImage CopyToSimLayout(const GuestImage& build, const ExeProfile& p, int scope, uint32_t simBase, uint32_t simEnd, std::vector<uint32_t>& source) {
    GuestImage out;
    out.base = simBase;
    out.bytes.assign(simEnd - simBase, 0);
    out.module = build.module;
    out.fileName = build.fileName;
    source.assign(out.bytes.size(), 0);
    for (const ProfileRange::Kind kind : {ProfileRange::kAligned, ProfileRange::kRefRun, ProfileRange::kFact})
        for (const ProfileRange& r : p.ranges) {
            if (r.kind != kind || r.scope != scope) continue;
            for (uint32_t s = std::max(r.simStart, simBase); s < std::min(r.simEnd, simEnd); s++) {
                const uint32_t a = uint32_t(int64_t(s) + r.delta);
                if (!build.Contains(a, 1)) continue;
                out.bytes[s - simBase] = build.bytes[a - build.base];
                source[s - simBase] = a;
            }
        }
    return out;
}

// The inverse of a copy: build address -> Simulation address (0 = not copied).
std::vector<uint32_t> InverseOf(const GuestImage& build, const GuestImage& copy, const std::vector<uint32_t>& source) {
    std::vector<uint32_t> inverse(build.bytes.size(), 0);
    for (size_t i = 0; i < source.size(); i++)
        if (source[i]) inverse[source[i] - build.base] = copy.base + uint32_t(i);
    return inverse;
}

// The Simulation end of a module's ranges in the profile (the copy's size).
uint32_t SimEnd(const ExeProfile& p, int scope, uint32_t simBase) {
    uint32_t end = simBase;
    for (const ProfileRange& r : p.ranges)
        if (r.scope == scope && r.kind != ProfileRange::kFact && r.simStart >= simBase && r.simStart < 0x80200000u && (scope >= 0 || r.kind == ProfileRange::kAligned))
            end = std::max(end, r.simEnd);
    return (end + 3) & ~3u;
}

} // namespace

std::vector<uint8_t> InflateEmbeddedGzip(const GuestImage& image, uint32_t address) {
    const uint8_t* p = image.At(address, 10);
    if (p[0] != 0x1F || p[1] != 0x8B || p[2] != 8) throw std::runtime_error("embedded gzip: bad header");
    const uint8_t flags = p[3];
    uint32_t at = address + 10;
    if (flags & 4) at += 2 + image.Get<uint16_t>(at);
    if (flags & 8)
        while (image.Get<uint8_t>(at++) != 0) {}
    if (flags & 16)
        while (image.Get<uint8_t>(at++) != 0) {}
    if (flags & 2) at += 2;
    return Inflate(std::span<const uint8_t>(image.At(at, image.End() - at), image.End() - at));
}

std::vector<uint8_t> EuropeanArcadeText(const std::vector<uint8_t>& data, bool global) {
#include "gt2formats/arcade_eu_text.inc"
    const size_t stride = global ? 0x7A9 : 0x314;
    const size_t referenceStride = global ? 0x76F : 0x2E0;
    if (data.size() < stride * 2) throw std::runtime_error("European text block is truncated");
    std::vector<uint8_t> out(referenceStride + 1);
    const auto ranges = global ? std::span<const RegionalTextRange>(kGlobalEuText) : std::span<const RegionalTextRange>(kArcadeEuText);
    for (const auto& r : ranges) {
        if (r.european + r.size > stride || r.reference + r.size > referenceStride) throw std::runtime_error("invalid regional text range");
        std::copy_n(data.begin() + ptrdiff_t(stride + r.european), r.size, out.begin() + r.reference);
    }
    return out;
}
std::vector<uint8_t> InflateEmbeddedGzipNamed(const GuestImage& image, const char* name) {
    const size_t length=std::strlen(name)+1;
    for(size_t i=0;i+10+length<=image.bytes.size();++i) {
        const auto* p=image.bytes.data()+i;
        if(p[0]==0x1f && p[1]==0x8b && p[2]==8 && p[3]==8 && std::memcmp(p+10,name,length)==0)
            return InflateEmbeddedGzip(image,image.base+uint32_t(i));
    }
    throw std::runtime_error(std::string("embedded asset not found: ")+name);
}
TitleAssets TitleAssets::Load(const DiscImage& disc, const GtfsVolume& vol, uint8_t language) {
    TitleAssets a;
    a.language = language;
    a.ovl1 = LoadOverlayImage(disc, 1);
    a.exe = LoadExeImage(disc);
    if (a.exe.profile && !a.exe.profile->reference) {
        a.textProfile=a.exe.profile;
        a.titleTextAt=a.exe.Sim(kTitleTextBase);
        a.globalTextAt=a.exe.Sim(kGlobalTextBase);
    }
    a.titleText = LanguageBlock(InflateEmbeddedGzipNamed(a.ovl1, "data-title.txd"), kTitleTextStride, language);
    a.globalText = LanguageBlock(InflateEmbeddedGzipNamed(a.ovl1, "data-global.txd"), kGlobalTextStride, language);

    // VRAM (header comment). The language-dependent files of the US disc: tables 0x80023E7C / 0x80023EA8, entry 1.
    if (language != 1) throw std::runtime_error("title assets: only the US language block (1) is mapped to files");
    a.vram.UploadTimToPage(vol.Read("arcade/arc_key_config.tim"), 0x1D);
    a.vram.UploadTimToPage(vol.Read("arcade/arc_font.tim"), 0x1E);
    a.vram.UploadTimToPage(vol.Read("arcade/topmenu_panels_us.tim"), 0x0E);
    a.vram.UploadTimToPage(vol.Read("arcade/title_item.tim"), 0x0C);
    UploadTitleBackground(a.vram, vol.Read("arcade/title_gtmode_us.tim"));

    LoadTitleFonts(a, vol);
    return a;
}

TitleAssets TitleAssets::LoadArcade(const DiscImage& disc, const GtfsVolume& vol, uint8_t language) {
    if(ProfileOf(disc).build==ExeBuild::kArcadeEu) {
        auto a=Load(disc,vol,language);
        UploadTitleBackground(a.vram,vol.Read("arcade/title_arcade_us.tim"));
        return a;
    }
    // US Arcade v1.1 member 1 (docs/research/arcade_disc.md section 17): the same uploads with the arcade background
    // arcade/title_arcade_us.tim (file id 0x28 of the arcade file table 0x801E2950, as in the capture of the title). Its
    // data-title.txd / data-global.txd are other builds of the texts (other block sizes, strings at other offsets): the
    // blocks at the arcade addresses, Text() goes through the build profile (section 17.8 of arcade_disc.md).
    TitleAssets a;
    a.language = language;
    a.ovl1 = LoadOverlayImage(disc, 1);
    a.exe = LoadExeImage(disc);
    if (!a.ovl1.profile || !a.ovl1.profile->arcade) throw std::runtime_error("title assets: LoadArcade needs the US Arcade v1.1 disc");
    a.textProfile = a.ovl1.profile;
    a.titleText = LanguageBlock(InflateEmbeddedGzipNamed(a.ovl1, "data-title.txd"), kArcadeTitleStride, language);
    a.globalText = LanguageBlock(InflateEmbeddedGzipNamed(a.ovl1, "data-global.txd"), kArcadeGlobalStride, language);
    a.titleTextAt = kArcadeTitleText;
    a.globalTextAt = kArcadeGlobalText;
    // The profile places the Simulation blocks' starts at these addresses (reference runs -0x300 / -0x5D0).
    if (a.textProfile->TryData(kTitleTextBase, -1) != kArcadeTitleText || a.textProfile->TryData(kGlobalTextBase, -1) != kArcadeGlobalText)
        throw std::runtime_error("title assets: the build profile does not place the text blocks where member 1 copies them");
    if (language != 1) throw std::runtime_error("title assets: only the US language block (1) is mapped to files");
    a.vram.UploadTimToPage(vol.Read("arcade/arc_key_config.tim"), 0x1D);
    a.vram.UploadTimToPage(vol.Read("arcade/arc_font.tim"), 0x1E);
    a.vram.UploadTimToPage(vol.Read("arcade/topmenu_panels_us.tim"), 0x0E);
    a.vram.UploadTimToPage(vol.Read("arcade/title_item.tim"), 0x0C);
    UploadTitleBackground(a.vram, vol.Read("arcade/title_arcade_us.tim"));
    LoadTitleFonts(a, vol);
    return a;
}

void TitleAssets::LoadTitleFonts(TitleAssets& a, const GtfsVolume& vol) {
    // Fonts: arc_fontinfo at 0x800E15C0 + four descriptors {glyph, kerning, cell, gap} appended behind it (the
    // descriptors themselves live at 0x801B95C0.. in the original; 0x80011BC4 fills them).
    GuestImage info;
    info.base = kFontInfoAddress;
    info.bytes = vol.Read("arcade/arc_fontinfo");
    if (info.bytes.size() < 0x24 || U32(info.bytes, 0) != 8) throw std::runtime_error("arc_fontinfo: unexpected header");
    info.bytes.resize((info.bytes.size() + 3) & ~size_t(3));
    const uint32_t descriptors = info.End();
    static constexpr uint8_t kCells[4] = {12, 7, 5, 3};
    for (int f = 0; f < 4; f++) {
        const uint32_t glyphs = kFontInfoAddress + U32(info.bytes, 4 + size_t(f) * 8), kerning = kFontInfoAddress + U32(info.bytes, 8 + size_t(f) * 8);
        uint8_t d[16] = {};
        std::memcpy(d, &glyphs, 4);
        std::memcpy(d + 4, &kerning, 4);
        d[8] = kCells[f];
        info.bytes.insert(info.bytes.end(), d, d + 16);
    }
    for (int f = 0; f < 4; f++) {
        a.fonts[size_t(f)] = LoadHudFont(info, descriptors + uint32_t(f) * 16);
        a.fonts[size_t(f)].tpage = kFontPage;
        a.fonts[size_t(f)].clutBase = uint16_t((kFontPage & 0xF) * 4 + (kFontPage & 0x10) * 0x400);
    }
}

const std::vector<uint8_t>* TitleAssets::Locate(uint32_t address, size_t& at) const {
    uint32_t a = address;
    if (textProfile) { // another build: a build text token as it is, a Simulation string address through the profile
        if (a >= kBuildTextTag + 0x80000000u && a - kBuildTextTag < 0x80200000u) {
            a -= kBuildTextTag;
        } else if ((a >= kTitleTextBase && a - kTitleTextBase < kTitleTextStride) || (a >= kGlobalTextBase && a - kGlobalTextBase < kGlobalTextStride)) {
            const std::optional<uint32_t> built = textProfile->TryData(a, -1);
            if (!built) return nullptr;
            a = *built;
        } else {
            return nullptr;
        }
    }
    if (a >= titleTextAt && a - titleTextAt < titleText.size()) {
        at = a - titleTextAt;
        return &titleText;
    }
    if (a >= globalTextAt && a - globalTextAt < globalText.size()) {
        at = a - globalTextAt;
        return &globalText;
    }
    return nullptr;
}

bool TitleAssets::HasText(uint32_t address) const {
    size_t at = 0;
    return Locate(address, at) != nullptr;
}

TitleAssets TitleAssets::SimLayoutScreens() const {
    TitleAssets a = *this;
    const ExeProfile* p = ovl1.profile;
    if (!p || p->reference) return a;
    std::vector<uint32_t> ovl1Source, exeSource;
    a.ovl1 = CopyToSimLayout(ovl1, *p, 1, kOverlayLoadAddress, SimEnd(*p, 1, kOverlayLoadAddress), ovl1Source);
    a.exe = CopyToSimLayout(exe, *p, -1, exe.base, SimEnd(*p, -1, exe.base), exeSource);
    const std::vector<uint32_t> ovl1Inverse = InverseOf(ovl1, a.ovl1, ovl1Source);
    // Only the pointers the screens follow are translated: member 1's pointers into member 1 (the option rows' label tables,
    // ...) and both images' string pointers. Words of the executable that look like its own addresses are data as often as
    // not (the save icon's CLUT 0x80091CC4 holds 0x80078003): they stay as they are.
    auto translate = [&](GuestImage& copy, const std::vector<uint32_t>& source, bool overlay) {
        for (size_t o = 0; o + 4 <= copy.bytes.size(); o += 4) {
            const uint32_t from = source[o];
            if (from == 0 || (from & 3) != 0 || source[o + 3] != from + 3) continue; // a word copied as a word
            uint32_t w;
            std::memcpy(&w, copy.bytes.data() + o, 4);
            uint32_t t = 0;
            if (overlay && ovl1.Contains(w, 1)) t = ovl1Inverse[w - ovl1.base]; // member 1 (below its end: the overlay)
            else if ((w >= titleTextAt && w - titleTextAt < titleText.size()) || (w >= globalTextAt && w - globalTextAt < globalText.size())) t = w + kBuildTextTag;
            if (t) std::memcpy(copy.bytes.data() + o, &t, 4);
        }
    };
    translate(a.ovl1, ovl1Source, true);
    translate(a.exe, exeSource, false);
    a.ovl1.profile = a.exe.profile = &SimProfile();
    return a;
}

std::string TitleAssets::Text(uint32_t address) const {
    size_t at = 0;
    const std::vector<uint8_t>* block = Locate(address, at);
    if (!block) return {};
    std::string s;
    while (at < block->size() && (*block)[at] != 0) s.push_back(char((*block)[at++]));
    return s;
}

} // namespace gt2
