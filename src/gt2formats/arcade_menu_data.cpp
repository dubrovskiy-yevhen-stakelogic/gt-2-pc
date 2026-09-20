#include "gt2formats/arcade_menu_data.h"

#include <stdexcept>

#include "gt2formats/exe_profile.h"
#include "gt2formats/title_assets.h"
#include "gt2vfs/disc_image.h"

namespace gt2 {

namespace {

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
    if (!ProfileOf(disc).arcade) throw std::runtime_error("arcade menu data: the disc is not US Arcade v1.1 (its member 2 layout is not mapped)");
    const auto original=LoadOverlayImage(disc,2);
    m.ovl2 = UiLayout(original,true);
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
    const std::vector<uint8_t> txd = InflateEmbeddedGzipNamed(original, "data-arcade.txd");
    const size_t at = size_t(language) * kTextStride;
    if (at + kTextStride > txd.size()) throw std::runtime_error("arcade menu data: no data-arcade.txd block for the language");
    m.text.assign(txd.begin() + std::ptrdiff_t(at), txd.begin() + std::ptrdiff_t(at + kTextStride));
    if (ProfileOf(disc).build == ExeBuild::kArcadeEu) m.text = EuropeanArcadeText(txd, false);
    m.text.push_back(0);
    return m;
}

} // namespace gt2
