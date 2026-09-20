#include "game/shell/arcade_title.h"

#include <algorithm>
#include <exception>

#include "game/shell/title_draw.h"

namespace gt2::shell {
namespace {

// data-title.txd / data-global.txd strings by their Simulation addresses (menus_gtmode.md 10.1).
constexpr uint32_t kLoadingSaveData = 0x801B9934u, kAutoComplete = 0x801B9956u, kAutoFailed = 0x801B9971u, kCorrupt = 0x801B998Du;
constexpr uint32_t kMemoryCard1 = 0x801EFB39u;
constexpr uint32_t kTotal = 0x7F00 + 0x80; // the file's sectors in bytes (0x7EA0 rounded to the card's blocks' 0x80-byte sectors)

} // namespace

TitleBootLoad::TitleBootLoad(const TitleAssets& assets, const CardSlot& slot1) : assets_(assets), slot_(slot1) {
    std::vector<uint8_t> image;
    if (CardStatus(slot_, &image) == 0 && CardFindFile(image, "BASCUS-94455GAME") >= 0) {
        state_ = 3;
        remaining_ = int(kTotal);
        segments_.fill(-1);
        progress_ = true;
    }
}

bool TitleBootLoad::Update(const MenuListPad& pad, career::CareerState& state) {
    if (state_ == 0) return false;
    if (state_ == 3) { // the read, the progress bar (0x8006C074: done^2 / total against i * total / 32)
        remaining_ = std::max(0, remaining_ - 0x80 * sectorsPerField);
        const uint32_t done = kTotal - uint32_t(remaining_), p = (done * done) / kTotal;
        for (int i = 0; i < 32; i++) {
            int8_t& s = segments_[size_t(i)];
            if (s < 0) {
                if (((uint32_t(i) * kTotal) >> 5) <= p) s = 0;
            } else if (++s > 0x18) {
                s = 0x18;
            }
        }
        if (remaining_ == 0) {
            progress_ = false;
            try {
                const career::CareerSave loaded = career::LoadCareerFromCard(career::ReadFileBytes(slot_.path));
                if (loaded.CrcOk()) {
                    state = loaded.state; // 0x8006A278
                    loaded_ = true;
                    state_ = 5, timer_ = 0xB4, line_ = assets_.Text(kAutoComplete), colour_ = 0x604610;
                    log.push_back("auto-loaded " + slot_.path);
                } else {
                    state_ = 6, timer_ = 0xF0, line_ = assets_.Text(kCorrupt), colour_ = 0x020A50F0;
                    log.push_back("auto-load: CRC mismatch in " + slot_.path);
                }
            } catch (const std::exception& e) {
                state_ = 8, timer_ = 0xF0, line_ = assets_.Text(kAutoFailed), colour_ = 0x020A50F0;
                log.push_back(std::string("auto-load failed: ") + e.what());
            }
        }
    } else if (state_ == 5 || state_ == 6 || state_ == 8) {
        timer_--;
        if (pad.pressed & 0xF00) timer_ = 0;
        if (timer_ <= 0) state_ = 9;
    } else if (state_ == 9) {
        timer_ = 6, state_ = 10;
    } else if (--timer_ == 0) {
        state_ = 0;
        return false;
    }
    return true;
}

std::vector<MenuPrim> TitleBootLoad::Frame() const {
    std::vector<MenuPrim> prims = TitleFrameStart();
    MenuOtSlot ot;
    const HudFont& medium = assets_.fonts[TitleAssets::kMediumFont];
    if (state_ == 3) AddText(ot, medium, assets_.Text(kLoadingSaveData), 0xB0, 0xF0, 1, colour_, 1, TextAlign::kCentre);
    else if (state_ == 5 || state_ == 6 || state_ == 8) AddText(ot, medium, line_, 0xB0, 0xF0, 1, colour_, 1, TextAlign::kCentre);
    if (state_ >= 3 && state_ <= 8) AddText(ot, medium, assets_.Text(kMemoryCard1), 0xB0, 0xB4, 1, colour_, 1, TextAlign::kCentre);
    if (progress_) // 0x8006C174 with the object 0x8004B82C
        for (int i = 0; i < 32; i++) {
            const int k = segments_[size_t(i)];
            const uint32_t c = k < 0 ? MenuListLerp(0x90500C, 0, 0x60, 0x80) : MenuListLerp(0x90500C, 0xD4D4D4, 0x18 - k, 0x18);
            MenuPrim t;
            t.kind = MenuPrim::kTile;
            t.x[0] = int16_t(0x60 + 5 * i), t.y[0] = 0x140, t.w = 4, t.h = 0x18;
            t.colour[0] = c & 0xFFFFFF;
            ot.Add(t);
        }
    ot.Emit(prims, 0x200);
    return prims;
}

} // namespace gt2::shell
