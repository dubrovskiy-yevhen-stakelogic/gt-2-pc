#include "gt2formats/arcade_menu_data.h"

#include <cstring>
#include <stdexcept>
#include <utility>

#include "gt2formats/exe_profile.h"
#include "gt2formats/title_assets.h"
#include "gt2vfs/disc_image.h"

namespace gt2 {

namespace {

// The Simulation disc's arcade overlay copies its own regional text blocks to lower Simulation RAM addresses. Pointers
// in its member-2 data still name those addresses: move them to the reference block offsets the menus draw from.
constexpr uint32_t kSimArcadeTextBase = 0x800F84E0u, kSimArcadeTextStride = 0x314;
constexpr uint32_t kSimGlobalTextBase = 0x801EF6B0u, kSimGlobalTextStride = 0x7A9;
constexpr uint32_t kArcadeGlobalTextBase = 0x801EF0E0u, kArcadeGlobalTextStride = 0x76F;

void TranslateSimulationTextPointers(GuestImage& image) {
    for (size_t at = 0; at + 4 <= image.bytes.size(); at += 4) {
        uint32_t word;
        std::memcpy(&word, image.bytes.data() + at, 4);
        uint32_t translated = 0;
        auto translate = [&](uint32_t base, uint32_t stride, uint32_t outBase, uint32_t outStride, auto offsetOf) {
            if (word < base || word - base >= stride) return false;
            uint32_t offset = word - base;
            if (const auto mapped = offsetOf(offset)) offset = *mapped;
            if (offset >= outStride) return false;
            translated = outBase + offset;
            return true;
        };
        if (!translate(kSimArcadeTextBase, kSimArcadeTextStride, ArcadeMenuData::kTextBase, ArcadeMenuData::kTextStride, EuropeanArcadeTextOffset) &&
            !translate(kSimGlobalTextBase, kSimGlobalTextStride, kArcadeGlobalTextBase, kArcadeGlobalTextStride, EuropeanGlobalTextOffset))
            continue;
        std::memcpy(image.bytes.data() + at, &translated, 4);
    }
}

constexpr uint32_t kEventNames = 0x8002729Cu, kLevelBytes = 0x800272DCu, kGameModes = 0x8004F8ECu, kTransmissions = 0x8004FBD0u;
constexpr uint32_t kCourseLists[ArcadeMenuData::kCourseListCount] = {0x80050730u, 0x800509F0u, 0x80050CB0u, 0x80050FB0u,
                                                                      0x800512B0u, 0x800513F0u, 0x800516B0u};
constexpr uint32_t kClassCounts = 0x800519A0u, kClassNames = 0x80051F14u, kClassModel = 0x80051F4Cu, kClassNumber2 = 0x80051F68u,
                   kClassBars = 0x80051F84u, kClassFigures = 0x80051FA0u, kClassFlags = 0x80051F30u;
constexpr uint32_t kClassSUnlock = 0x800519E4u, kBonusUnlock = 0x80051D64u;

} // namespace

std::string ArcadeMenuData::ImageString(uint32_t address) const {
    std::string s;
    for (uint32_t a = address;; a++) {
        const char c = char(ovl2.Get<uint8_t>(a));
        if (c == 0) break;
        s.push_back(c);
        if (s.size() > 256) throw std::runtime_error("arcade menu data: unterminated string");
    }
    return s;
}

std::string ArcadeMenuData::Text(uint32_t address) const {
    if (address < kTextBase || address - kTextBase >= text.size()) return {};
    std::string s;
    for (size_t at = address - kTextBase; at < text.size() && text[at] != 0; at++) s.push_back(char(text[at]));
    return s;
}

ArcadeMenuData ArcadeMenuData::Load(const DiscImage& disc, uint8_t language) {
    ArcadeMenuData m;
    if (!ArcadeFlow()) throw std::runtime_error("arcade menu data: the disc is not playing the Arcade flow");
    const auto original=LoadOverlayImage(disc,2);
    m.ovl2 = UiLayout(original,true);
    if (!ProfileOf(disc).arcade) TranslateSimulationTextPointers(m.ovl2);
    const GuestImage& o = m.ovl2;
    for (uint32_t level = 0; level < 4; level++)
        for (uint32_t cls = 0; cls < 4; cls++) m.eventNames[level][cls] = m.ImageString(o.Get<uint32_t>(kEventNames + (level * 4 + cls) * 4));
    for (uint32_t k = 0; k < 4; k++) {
        m.levelBlockByte[k] = o.Get<int8_t>(kLevelBytes + k);
        m.gameModes[k] = o.Get<int8_t>(kGameModes + k);
    }
    for (uint32_t k = 0; k < 2; k++) m.transmissions[k] = o.Get<int8_t>(kTransmissions + k);
    for (size_t l = 0; l < kCourseListCount; l++) {
        for (uint32_t a = kCourseLists[l];; a += 0x20) {
            const uint32_t name = o.Get<uint32_t>(a);
            if (name == 0) break;
            ArcadeCourse c;
            c.file = m.ImageString(name);
            c.display = m.ImageString(o.Get<uint32_t>(a + 4));
            c.mapInfo = o.Get<uint32_t>(a + 8);
            c.flags = o.Get<uint32_t>(a + 0x0C);
            c.id = o.Get<uint32_t>(a + 0x10);
            c.tier = o.Get<int32_t>(a + 0x14);
            c.record = o.Get<int32_t>(a + 0x18);
            c.available = o.Get<uint32_t>(a + 0x1C);
            m.courses[l].push_back(c);
            if (m.courses[l].size() > 64) throw std::runtime_error("arcade menu data: course list without an end");
        }
    }
    for (uint32_t c = 0; c < kClassListCount; c++) {
        ArcadeClassCars& cc = m.classes[c];
        const int16_t count = o.Get<int16_t>(kClassCounts + c * 2);
        const uint32_t names = o.Get<uint32_t>(kClassNames + c * 4), model = o.Get<uint32_t>(kClassModel + c * 4),
                       number2 = o.Get<uint32_t>(kClassNumber2 + c * 4), bars = o.Get<uint32_t>(kClassBars + c * 4),
                       figures = o.Get<uint32_t>(kClassFigures + c * 4);
        cc.flagsAddress = o.Get<uint32_t>(kClassFlags + c * 4);
        for (int16_t i = 0; i < count; i++) {
            cc.cars.push_back(m.ImageString(o.Get<uint32_t>(names + uint32_t(i) * 4)));
            cc.model.push_back(o.Get<int16_t>(model + uint32_t(i) * 2));
            cc.number2.push_back(o.Get<int16_t>(number2 + uint32_t(i) * 2));
            std::array<uint8_t, 3> b{};
            for (uint32_t k = 0; k < 3; k++) b[k] = o.Get<uint8_t>(bars + uint32_t(i) * 3 + k);
            cc.bars.push_back(b);
            std::array<uint8_t, 10> f{};
            for (uint32_t k = 0; k < 10; k++) f[k] = o.Get<uint8_t>(figures + uint32_t(i) * 10 + k);
            cc.figures.push_back(f);
        }
    }
    for (uint32_t k = 0; k < m.classSUnlock.size(); k++) m.classSUnlock[k] = o.Get<int8_t>(kClassSUnlock + k);
    for (uint32_t k = 0; k < m.bonusUnlock.size(); k++) m.bonusUnlock[k] = o.Get<int8_t>(kBonusUnlock + k);
    // The Arcade disc names the gzip; the Simulation disc's copy carries no file name header, and it sits just past the
    // last mapped range, so find the member's (only) gzip header when the name lookup fails.
    std::vector<uint8_t> txd;
    try {
        txd = InflateEmbeddedGzipNamed(original, "data-arcade.txd");
    } catch (const std::exception&) {
        for (uint32_t at = original.base; at + 10 < original.End() && txd.empty(); at++)
            if (original.Get<uint8_t>(at) == 0x1F && original.Get<uint8_t>(at + 1) == 0x8B && original.Get<uint8_t>(at + 2) == 8) {
                try { // the text is the multi-block member; the overlay holds no other such gzip
                    std::vector<uint8_t> candidate = InflateEmbeddedGzip(original, at);
                    if (candidate.size() >= 2 * kTextStride) txd = std::move(candidate);
                } catch (const std::exception&) {}
            }
        if (txd.empty()) throw std::runtime_error("arcade menu data: data-arcade.txd not found");
    }
    // The Simulation disc's arcade overlay stores the seven regional blocks (the European layout); the Arcade disc's
    // copy keeps only the reference layout. Convert either to the reference block the menus address.
    if (txd.size() >= 7 * 0x314) {
        m.text = EuropeanArcadeText(txd, false); // the reference block of language 1, grown for the patched strings
    } else {
        const size_t at = size_t(language) * kTextStride;
        if (at + kTextStride > txd.size()) throw std::runtime_error("arcade menu data: no data-arcade.txd block for the language");
        m.text.assign(txd.begin() + std::ptrdiff_t(at), txd.begin() + std::ptrdiff_t(at + kTextStride));
    }
    m.text.push_back(0);
    return m;
}

} // namespace gt2
