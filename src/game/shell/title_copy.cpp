#include "game/shell/title_copy.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "game/career/career_state.h"
#include "game/shell/title_draw.h"

namespace gt2::shell {

namespace {
// Member 1's templates (Simulation addresses; GuestImage::Sim maps them).
constexpr uint32_t kBandTemplate = 0x8004B614u, kRuleTemplate = 0x8004B630u, kSelector = 0x8004B704u, kList = 0x8004B724u,
                   kProgress = 0x8004B788u;
constexpr uint32_t kBarTemplates[5] = {0x8004B644u, 0x8004B674u, 0x8004B6A4u, 0x8004B6D4u, 0x8004B758u};
// Strings: data-title.txd (0x801B9630 block) and data-global.txd (0x801EF6B0 block).
constexpr uint32_t kHeaderText = 0x801B9A6Cu, kChosenMark = 0x801B9A87u, kCopyCount = 0x801B9A34u;
constexpr uint32_t kWhileCopying = 0x801B9822u, kDoNotRemoveCard = 0x801B983Bu, kDoNotRemove = 0x801EF744u;
constexpr uint32_t kNotFormatted2 = 0x801B98C0u, kWantFormat = 0x801EF7D6u, kFormatFailed2 = 0x801B9FFCu, kFormatting2 = 0x801B9FD1u;
constexpr uint32_t kSelectBlocks2 = 0x801BA02Bu, kNotEnoughBlocks2 = 0x801B98EFu, kCreateFailed2 = 0x801BA091u, kCreating2 = 0x801BA05Cu;
constexpr uint32_t kSaveFailed2 = 0x801B9897u, kSaving2 = 0x801B9FABu, kCorrupt2 = 0x801B97F7u, kLoadFailed2 = 0x801B97A1u,
                   kChecking2 = 0x801B9752u, kCorrupt1 = 0x801B97CCu, kChecking1 = 0x801B972Eu, kLoadFailed1 = 0x801B9776u;
constexpr uint32_t kTooLarge = 0x801EFD61u, kCopyWord = 0x801BA408u, kMax32 = 0x801EFD50u, kNothingToCopy = 0x801BA42Cu, kConfirm = 0x801BA417u;
constexpr uint32_t kLoading1 = 0x801B9F84u, kCopyComplete = 0x801B991Cu, kMemoryCard1 = 0x801EFB39u, kNoReplayData = 0x801EFA22u;
constexpr uint32_t kTotalReplays = 0x801EFB5Bu, kSectorsFree = 0x801EFB74u;
constexpr uint32_t kNoData1 = 0x801B9866u, kNoCard1 = 0x801B9630u, kNoCard2 = 0x801B9655u, kNotFormattedCard2 = 0x801B96A9u,
                   kCardCorrupt1 = 0x801B96D8u, kCardCorrupt2 = 0x801B9703u;

constexpr size_t kFrame = 128, kBlockBytes = 0x2000, kCardBlocks = 15;
constexpr uint32_t kRowColour = 0x5A4A3Eu, kRowColourOff = 0x08082Au; // 0x80013AA4: style + 0x10 by the row's +0x39A

std::string FormatInt(const std::string& format, int a, int b = 0) { // "%d" of 0x8008CF34 (one or two values)
    std::string out;
    int n = 0;
    for (size_t i = 0; i < format.size(); i++) {
        if (format[i] == '%' && i + 1 < format.size() && format[i + 1] == 'd') {
            out += std::to_string(n++ == 0 ? a : b);
            i++;
        } else {
            out.push_back(format[i]);
        }
    }
    return out;
}

MenuPrim Tile(int x, int y, int w, int h, uint32_t colour) {
    MenuPrim p;
    p.kind = MenuPrim::kTile;
    p.x[0] = int16_t(x), p.y[0] = int16_t(y), p.w = int16_t(w), p.h = int16_t(h);
    p.colour[0] = colour & 0xFFFFFF;
    p.semi = (colour & 0x2000000u) != 0;
    return p;
}

int BandAlpha(const Band& b) { // EXE 0x8006BEB4
    const int a = b.anim >= 0 ? b.anim : b.anim < -1 ? ~b.anim : 0;
    return b.steps ? (a << 7) / b.steps : 0;
}

uint8_t FrameChecksum(const uint8_t* f) {
    uint8_t x = 0;
    for (size_t i = 0; i + 1 < kFrame; i++) x ^= f[i];
    return x;
}

// The block chain of the replay file on a card image (directory frames 1..15), empty when the card has none.
std::vector<size_t> ReplayFileBlocks(const std::vector<uint8_t>& card) {
    std::vector<size_t> chain;
    for (size_t i = 1; i <= kCardBlocks; i++) {
        const uint8_t* f = card.data() + i * kFrame;
        if (f[0] != 0x51 || std::strncmp(reinterpret_cast<const char*>(f + 10), kReplayCardFileName, 20) != 0) continue;
        size_t b = i;
        for (size_t guard = 0; guard < kCardBlocks; guard++) {
            chain.push_back(b);
            const uint16_t next = uint16_t(card[b * kFrame + 8] | card[b * kFrame + 9] << 8);
            if (next == 0xFFFF) break;
            b = size_t(next) + 1;
            if (b < 1 || b > kCardBlocks) throw std::runtime_error("memory card: broken block chain");
        }
        break;
    }
    return chain;
}

// The BIOS create of our interpreter's card (src/machine MemoryCard::Create): the first free directory frames in order,
// their states 0x51 / 0x52 / 0x53, the size in the first, the links, the checksums; the blocks' data is not touched.
bool CreateReplayFile(std::vector<uint8_t>& card, int blocks) {
    std::vector<size_t> free;
    for (size_t i = 1; i <= kCardBlocks && int(free.size()) < blocks; i++)
        if ((card[i * kFrame] & 0xF0) == 0xA0) free.push_back(i);
    if (int(free.size()) < blocks) return false;
    for (size_t n = 0; n < free.size(); n++) {
        uint8_t* f = card.data() + free[n] * kFrame;
        std::memset(f, 0, kFrame);
        f[0] = n == 0 ? 0x51 : n + 1 == free.size() ? 0x53 : 0x52;
        if (n == 0) {
            const uint32_t size = uint32_t(blocks) * uint32_t(kBlockBytes);
            std::memcpy(f + 4, &size, 4);
            std::memcpy(f + 10, kReplayCardFileName, std::strlen(kReplayCardFileName));
        }
        const uint16_t next = n + 1 < free.size() ? uint16_t(free[n + 1] - 1) : 0xFFFF;
        f[8] = uint8_t(next), f[9] = uint8_t(next >> 8);
        f[kFrame - 1] = FrameChecksum(f);
    }
    return true;
}
} // namespace

CopyReplayScreen::CopyReplayScreen(const TitleAssets& assets, const ReplayRowText& text, std::array<CardSlot, 2> slots)
    : assets_(assets), text_(text), slots_(std::move(slots)) { // 0x80015CF8 + 0x80015D98 (0x80015B6C, state 0)
    const GuestImage& o = assets.ovl1;
    band_ = Band::Read(o, o.Sim(kBandTemplate));
    band_.anim = -1; // + 0x48
    const uint32_t r = o.Sim(kRuleTemplate);
    rule_.c0 = o.Get<uint32_t>(r), rule_.c1 = o.Get<uint32_t>(r + 4);
    rule_.x = o.Get<int16_t>(r + 8), rule_.y = o.Get<int16_t>(r + 0xA), rule_.w = o.Get<int16_t>(r + 0xC), rule_.h = o.Get<int16_t>(r + 0xE);
    rule_.steps = o.Get<int16_t>(r + 0x10);
    rule_.anim = -1; // + 0x5E
    for (size_t k = 0; k < bars_.size(); k++) bars_[k] = CardManager::ReadBar(o, o.Sim(kBarTemplates[k]));
    blocks_ = CardManager::ReadBlockSelector(o, o.Sim(kSelector), TitleAssets::kMediumFont); // + 0x10 = 0x800A8DF0 (the medium font)
    list_ = MenuListWidget::Read(o, o.Sim(kList));
    MenuListReset(list_, [this](int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) { return RowCallback(command, w, row, draw); });
    progressColour_ = o.Get<uint32_t>(o.Sim(kProgress) + 0xC);
    Poll();
    Switch(0);
}

void CopyReplayScreen::Poll() {
    for (size_t s = 0; s < 2; s++) {
        std::vector<uint8_t> image;
        status_[s] = CardStatus(slots_[s], &image);
        hasFile_[s] = status_[s] == 0 && CardFindFile(image, kReplayCardFileName) >= 0;
        freeBlocks_[s] = status_[s] == 0 ? CardFreeBlocks(image) : 0;
    }
}

bool CopyReplayScreen::CardsReady(bool strict) const { // 0x80013E14
    bool ok = status_[0] == 0;
    if (strict) ok = ok && status_[1] == 0;
    else ok = ok && (status_[1] == 0 || status_[1] == 3);
    return ok && hasFile_[0];
}

bool CopyReplayScreen::Lines(uint32_t& line1, uint32_t& line2) const { // 0x80013EA0(&l1, &l2, 1)
    line1 = line2 = 0;
    bool problem = false;
    switch (status_[0]) {
    case 0: if (!hasFile_[0]) line1 = kNoData1, problem = true; break;
    case 1: line1 = kChecking1, problem = true; break;
    case 2: line1 = kNoCard1, problem = true; break;
    case 3: line1 = kNoData1, problem = true; break;
    case 4: line1 = kCardCorrupt1, problem = true; break;
    default: break;
    }
    switch (status_[1]) {
    case 1: line2 = kChecking2, problem = true; break;
    case 2: line2 = kNoCard2, problem = true; break;
    case 3: line2 = kNotFormattedCard2, problem = true; break;
    case 4: line2 = kCardCorrupt2, problem = true; break;
    default: break;
    }
    return !problem;
}

void CopyReplayScreen::ShowHeader(bool show) {
    if (show) {
        band_.anim = 0;
        rule_.anim = 0;
    } else if (band_.anim >= 0) {
        band_.anim = int16_t(~band_.steps);
        rule_.anim = int16_t(~rule_.steps);
    }
}

int CopyReplayScreen::Fail(uint32_t text) { // + 0x20 = text, 0x80013480, state 1
    error_ = text;
    return 1;
}

void CopyReplayScreen::ProgressReset(uint32_t total, uint32_t colour) { // + 0x36C, + 0x368, 0x8006C04C
    progressTotal_ = total;
    progressColour_ = colour;
    progressVisible_ = 0;
    progress_.fill(-1);
}

void CopyReplayScreen::ProgressUpdate(uint32_t remaining) { // 0x8006C074
    if (progressVisible_ < 0) return;
    const uint32_t total = progressTotal_;
    int32_t done = int32_t(total - remaining);
    if (done < 0) done = 0;
    if (int32_t(total) < done) done = int32_t(total);
    const uint32_t p = total < 40000 ? (total ? (uint32_t(done) * uint32_t(done)) / total : 0) : (uint32_t(done) * uint32_t(done >> 8)) / (total >> 8);
    for (int i = 0; i < 32; i++) {
        if (progress_[size_t(i)] < 0) {
            if (int32_t((uint32_t(i) * total) >> 5) <= int32_t(p)) progress_[size_t(i)] = 0;
        } else if (++progress_[size_t(i)] > 0x18) {
            progress_[size_t(i)] = 0x18;
        }
    }
}

bool CopyReplayScreen::Transfer() { // one poll of 0x8007D7CC: 1 busy (the progress moves), 0 done
    transferLeft_ = std::max(0, transferLeft_ - 0x80 * sectorsPerField);
    if (transferLeft_ > 0) {
        ProgressUpdate(uint32_t(transferLeft_) + progressBase_);
        return false;
    }
    return true;
}

bool CopyReplayScreen::WriteCard(const std::vector<Piece>& pieces, bool create) {
    const std::string& path = slots_[1].path;
    try {
        std::vector<uint8_t> card = career::ReadFileBytes(path);
        if (card.size() != 0x20000 || card[0] != 'M' || card[1] != 'C') throw std::runtime_error("not a memory card image");
        if (create && !CreateReplayFile(card, file2_->Blocks())) throw std::runtime_error("no room for the replay file");
        const std::vector<size_t> chain = ReplayFileBlocks(card);
        for (const Piece& piece : pieces)
            for (size_t i = 0; i < piece.length; i++) {
                const size_t o = piece.offset + i, block = o / kBlockBytes;
                if (block >= chain.size()) throw std::runtime_error("write outside the replay file");
                card[chain[block] * kBlockBytes + o % kBlockBytes] = piece.source[i];
            }
        career::WriteFileBytes(path, card);
    } catch (const std::exception& e) {
        log.push_back(std::string("card 2 write failed: ") + e.what());
        Poll();
        return false;
    }
    Poll();
    return true;
}

int CopyReplayScreen::ChosenCount() const { // 0x80013518
    int n = 0;
    for (bool c : chosen_) n += c ? 1 : 0;
    return n;
}

int CopyReplayScreen::ChosenSectors() const { // 0x800134C4: the chosen entries' sectors (card 1's directory)
    int n = 0;
    for (int i = 0; i < 32; i++)
        if (chosen_[size_t(i)] && file1_ && i < file1_->Count()) n += file1_->Entry(i).sectors;
    return n;
}

int CopyReplayScreen::FreeAfterCopy() const { // 0x8001358C: card 2's total - used - the chosen entries' sectors
    return file2_ ? file2_->Total() - file2_->UsedSectors() - ChosenSectors() : 0;
}

int CopyReplayScreen::RowFit(int i, int& why) const { // 0x800135E0: enabled, `why` the row's message
    const int free = FreeAfterCopy();
    why = 0;
    if (chosen_[size_t(i)]) return 1;
    why = 1; // "Data is Too Large"
    if (free - file1_->Entry(i).sectors < 0) return 0;
    why = 2; // "Max 32 Files" (0x80013558: card 2's count + the chosen ones)
    if ((file2_ ? file2_->Count() : 0) + ChosenCount() == 32) return 0;
    why = 0;
    return 1;
}

void CopyReplayScreen::BuildRows() { // 0x80014D60 h(0)
    const int count = file1_ ? file1_->Count() : 0;
    rows_.assign(size_t(count) + 1, Row{});
    for (int i = 0; i < count; i++) {
        int why = 0;
        rows_[size_t(i)].kind = 0;
        rows_[size_t(i)].enabled = int16_t(RowFit(i, why));
        rows_[size_t(i)].why = int16_t(why);
    }
    rows_[size_t(count)] = {2, int16_t(ChosenCount() > 0 ? 1 : 0), 3};
    list_.count = int16_t(count + 1);
    list_.selection = keptSelection_;
}

int32_t CopyReplayScreen::RowCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) { // 0x80013AA4
    if (row < 0 || row >= int(rows_.size())) return 0;
    const Row& r = rows_[size_t(row)];
    if (command == kMenuListEnabled) return r.enabled == 1 ? 1 : 0;
    if (command != kMenuListDraw || !draw || !draw->ot) return 0;
    int x = 0, alpha = 0;
    if (!ReplayRowPlacement(w, *draw, x, alpha)) return 0;
    MenuOtSlot& ot = rowSlot_ ? *rowSlot_ : *draw->ot; // the list's OT slot + 1
    if (row < 32 && chosen_[size_t(row)]) { // "COPY" (medium font, 0x020A5A5A) and a green box behind it, drawn twice (modes 1, 2)
        const int width = AddText(ot, assets_.fonts[TitleAssets::kMediumFont], assets_.Text(kChosenMark), x - 0x98, draw->y + 0x18, 1, 0x020A5A5Au, 1);
        const uint32_t c0 = MenuListLerp(0, 0x5A8C32u, alpha, 0x80), c1 = MenuListLerp(c0, 0, 0x20, 0x80);
        AddGradientQuad(ot, x - 0x9C, draw->y - 4, width + 8, 0x22, c0, c1);
        ot.DrawMode(0x220);
        AddGradientQuad(ot, x - 0x9C, draw->y - 4, width + 8, 0x22, c0, c1);
        ot.DrawMode(0x240);
    }
    ReplayRowStyle style; // 0x8004B7C0: the medium / small fonts (0x800A8DF0 / 0x800A8DE0), spacings 1 / 1
    style.rowColour = r.enabled == 1 ? kRowColour : kRowColourOff;
    const ReplayCardEntry e = r.kind == kReplayRowEntry && file1_ ? file1_->Entry(row) : ReplayCardEntry{};
    DrawReplayRow(ot, assets_, text_, style, r.kind == kReplayRowEntry ? &e : nullptr, r.kind, x, draw->y, alpha);
    return 0;
}

// Handlers of the table 0x8004B7D8: h(0).
int CopyReplayScreen::Enter(int id) {
    switch (id) {
    case 0: delay_ = 0x18; return -1; // 0x80014024
    case 1: // 0x8001407C: the error text (+ 0x20), ERROR! bar
        sounds.push_back(0);
        CardManager::OpenBar(bars_[kError]);
        bars_[kError].cursor = 0;
        line1_ = line2_ = 0;
        return -1;
    case 2: // 0x8001420C
        ShowHeader(false);
        if (status_[1] == 3) return 3;
        CardManager::OpenBar(bars_[kStart]);
        return Step();
    case 3: // 0x80014424
        CardManager::OpenBar(bars_[kFormat]);
        bars_[kFormat].cursor = 1;
        line1_ = kNotFormatted2, line2_ = kWantFormat;
        ShowHeader(true);
        return Step();
    case 4: { // 0x80014524: 0x8007F5A8(1) formats card 2 (ours: a formatted image written at once, done at the next poll)
        try {
            career::WriteFileBytes(slots_[1].path, career::FormatMemoryCard());
            log.push_back("formatted " + slots_[1].path);
            delay_ = 0;
        } catch (const std::exception& e) {
            log.push_back(std::string("format failed: ") + e.what());
            delay_ = -1;
        }
        line1_ = kFormatting2, line2_ = kDoNotRemove;
        return -1;
    }
    case 5: // 0x800145E8
        if (freeBlocks_[1] < 3) return 6;
        blocks_.min = 3, blocks_.max = int16_t(freeBlocks_[1]), blocks_.value = 3, blocks_.anim = 0;
        line1_ = kSelectBlocks2, line2_ = 0;
        ShowHeader(true);
        return -1;
    case 6: // 0x80014704
        sounds.push_back(0);
        CardManager::OpenBar(bars_[kError]);
        bars_[kError].cursor = 0;
        line1_ = kNotEnoughBlocks2, line2_ = 0;
        return -1;
    case 7: // 0x80014800: 0x800696EC(file 2, 1) creates "BASCUS-94455REPLAY" of the file's blocks on card 2
        if (status_[1] != 0 || hasFile_[1] || freeBlocks_[1] < file2_->Blocks()) return Fail(0); // the request refused: the old + 0x20 text
        delay_ = 1;
        line1_ = kCreating2, line2_ = kDoNotRemove;
        return -1;
    case 8: // 0x800148C8: 0x800693EC, 0x8006971C(file 2, 1) writes its first 0x1580 bytes
        file2_->UpdateCrc();
        ProgressReset(0x150C, 0x0C5090u);
        progressBase_ = 0;
        transferLeft_ = 0x1580;
        line1_ = kSaving2, line2_ = kDoNotRemove;
        return -1;
    case 9: // 0x80014A00: 0x800697AC(file 2, 1) reads card 2's header and directory
        if (!hasFile_[1]) return Fail(kLoadFailed2);
        ShowHeader(true);
        ProgressReset(0x150C, 0x90500Cu);
        progressBase_ = 0;
        transferLeft_ = 0x1580;
        line1_ = kChecking2, line2_ = 0;
        usable_ = CardsReady(true);
        return -1;
    case 10: // 0x80014B98: card 1's header and directory (0x800697AC(file 1, 0)), then the poll of h(1) at once
        if (!usable_) return 2;
        if (!hasFile_[0]) return Fail(kLoadFailed1);
        barTotal_ = file2_->Total(); // 0x800136EC
        barUsed_ = int16_t(file2_->UsedSectors());
        ProgressReset(0x150C, 0x90500Cu);
        progressBase_ = 0;
        transferLeft_ = 0x1580;
        line1_ = kChecking1, line2_ = 0;
        if (!CardsReady(true)) usable_ = false;
        return Step();
    case 0x0B: // 0x80014D60
        line1_ = line2_ = 0;
        BuildRows();
        if (!CardsReady(true)) usable_ = false;
        return -1;
    case 0x0C: { // 0x80015158: the chosen entries in order: descriptions, offsets (sectors), sizes
        copy_.clear();
        uint32_t offset = 0;
        for (int i = 0; i < 32; i++) {
            if (!chosen_[size_t(i)] || i >= file1_->Count()) continue;
            Pending p;
            p.entry = i;
            p.desc = file1_->Entry(i);
            p.offset = offset;
            p.size = uint32_t(p.desc.size);
            p.rounded = (p.size + 0x7F) & ~0x7Fu;
            offset += p.rounded;
            copy_.push_back(p);
        }
        copyBytes_ = offset;
        copyIndex_ = 0;
        if (copy_.empty()) return 2;
        uint32_t done = 0;
        for (Pending& p : copy_) {
            done += p.rounded;
            p.after = copyBytes_ - done;
        }
        buffer_.assign(copyBytes_, 0);
        ProgressReset(copyBytes_, 0x50782Cu);
        line1_ = kLoading1, line2_ = kDoNotRemove;
        return 0x0D;
    }
    case 0x0D: { // 0x80015304: 0x800697E8(file 1, 0, entry, the buffer, the sector list)
        if (!usable_) {
            progressVisible_ = -1;
            return 2;
        }
        const Pending& p = copy_[size_t(copyIndex_)];
        progressBase_ = p.after;
        transferLeft_ = int(p.rounded);
        if (!CardsReady(true)) usable_ = false;
        return -1;
    }
    case 0x0E: // 0x800154D4
        if (!usable_) {
            progressVisible_ = -1;
            return 2;
        }
        return ++copyIndex_ < int(copy_.size()) ? 0x0D : 0x0F;
    case 0x0F: // 0x8001553C
        copyIndex_ = 0;
        ProgressReset(copyBytes_ + 0x150C, 0x0C5090u);
        line1_ = kSaving2, line2_ = kDoNotRemove;
        return 0x10;
    case 0x10: { // 0x800155AC: 0x80069418(file 2, -1, the description, the data, size, the sector list), 0x800691DC, 0x80069758
        const Pending& p = copy_[size_t(copyIndex_)];
        const std::span<const uint8_t> data(buffer_.data() + p.offset, p.size);
        if (!file2_->Store(-1, p.desc.desc, data)) return Fail(kCorrupt2);
        if (!file2_->Valid()) return Fail(kCorrupt2);
        progressBase_ = p.after + 0x150C;
        transferLeft_ = int(p.rounded);
        return -1;
    }
    case 0x11: // 0x80015784
        if (status_[1] != 0) return Fail(kSaveFailed2);
        return ++copyIndex_ < int(copy_.size()) ? 0x10 : 0x12;
    case 0x12: // 0x80015808: 0x8006971C(file 2, 1): header and directory
        progressBase_ = 0;
        transferLeft_ = 0x1580;
        line1_ = kSaving2, line2_ = kDoNotRemove;
        return -1;
    case 0x13: // 0x8001593C
        band_.anim = int16_t(~band_.steps);
        rule_.anim = int16_t(~rule_.steps);
        sounds.push_back(7);
        CardManager::OpenBar(bars_[kComplete]);
        bars_[kComplete].cursor = 0;
        line1_ = kCopyComplete, line2_ = 0;
        return -1;
    case 0x14: // 0x80015A30
        sounds.push_back(0);
        CardManager::OpenBar(bars_[kNoData]);
        bars_[kNoData].cursor = 0;
        line1_ = kMemoryCard1, line2_ = kNoReplayData;
        return -1;
    default: return -1;
    }
}

// h(1): one field of the state.
int CopyReplayScreen::Step() {
    auto okExit = [&](int bar) { // the ERROR! / OK bars: 0 -> sound 1, the main state; 1 / -1 -> leave
        const int r = barResult_[size_t(bar)];
        if (r == 0) {
            sounds.push_back(1);
            return 2;
        }
        return r == 1 || r == -1 ? kExit : -1;
    };
    switch (state_) {
    case 0: return --delay_ < 0 ? 2 : -1;
    case 1: return okExit(kError);
    case 2: { // 0x8001420C
        const int r = barResult_[kStart];
        uint32_t l1 = 0, l2 = 0;
        const bool ok = Lines(l1, l2);
        if (ok) line1_ = kWhileCopying, line2_ = kDoNotRemoveCard;
        else line1_ = l1, line2_ = l2;
        bars_[kStart].fill = ok || bars_[kStart].cursor != 0 ? 0x0280400Cu : 0x02080830u; // + 0x21C: red while Start cannot copy
        if (status_[1] == 0 && !hasFile_[1]) {
            CardManager::CloseBar(bars_[kStart]);
            return 5;
        }
        if (status_[1] == 3) {
            sounds.push_back(7);
            CardManager::CloseBar(bars_[kStart]);
            return 3;
        }
        if (r == 0) {
            if (!CardsReady(true)) {
                sounds.push_back(0);
                return -1;
            }
            sounds.push_back(1);
            CardManager::CloseBar(bars_[kStart]);
            chosen_.fill(false); // 0x800136C4
            return hasFile_[1] ? 9 : 5;
        }
        if (r == 1 || r == -1) {
            CardManager::CloseBar(bars_[kStart]);
            return kExit;
        }
        return -1;
    }
    case 3: { // 0x80014424
        if (status_[1] != 3) {
            CardManager::CloseBar(bars_[kFormat]);
            return 2;
        }
        const int r = barResult_[kFormat];
        if (r == 0) {
            sounds.push_back(1);
            return 4;
        }
        return r == 1 || r == -1 ? kExit : -1;
    }
    case 4: // 0x80014524: the format's poll
        if (delay_ < 0) return Fail(kFormatFailed2);
        return 5;
    case 5: { // 0x800145E8
        if (status_[1] != 0) {
            blocks_.anim = -1;
            return 2;
        }
        const int r = blockResult_;
        if (r > -4 && r < -1) return -1;
        blocks_.anim = -1;
        if (r == -1) return kExit;
        file2_ = ReplayCardFile::Create(r, BuildReplayCardHeader(assets_.exe, r)); // 0x8006911C(file 2, blocks)
        return 7;
    }
    case 6: // 0x80014704
        if (status_[1] != 0) {
            CardManager::CloseBar(bars_[kError]);
            return 2;
        }
        return okExit(kError);
    case 7: // 0x80014800: the create's poll
        if (--delay_ >= 0) return -1;
        if (!WriteCard({}, true)) return Fail(kCreateFailed2);
        log.push_back("created the replay file (" + std::to_string(file2_->Blocks()) + " blocks) on " + slots_[1].path);
        return 8;
    case 8: // 0x800148C8
        if (!Transfer()) return -1;
        progressVisible_ = -1;
        if (!WriteCard({{0, file2_->Bytes().data(), kReplayDataStart}}, false)) return Fail(kSaveFailed2);
        return 2;
    case 9: { // 0x80014A00
        if (!CardsReady(true)) usable_ = false;
        if (!Transfer()) return -1;
        progressVisible_ = -1;
        try {
            file2_ = ReplayCardFile::FromBytes(ReadReplayCardFile(career::ReadFileBytes(slots_[1].path)));
        } catch (const std::exception& e) {
            log.push_back(std::string("card 2: ") + e.what());
            return Fail(kLoadFailed2);
        }
        if (!file2_->Valid()) return Fail(kCorrupt2);
        return usable_ ? 10 : 2;
    }
    case 10: { // 0x80014B98 h(1)
        if (!CardsReady(true)) usable_ = false;
        if (!Transfer()) return -1;
        progressVisible_ = -1;
        try {
            file1_ = ReplayCardFile::FromBytes(ReadReplayCardFile(career::ReadFileBytes(slots_[0].path)));
        } catch (const std::exception& e) {
            log.push_back(std::string("card 1: ") + e.what());
            return Fail(kLoadFailed1);
        }
        if (!file1_->Valid()) return Fail(kCorrupt1);
        listDelay_ = 1;
        keptSelection_ = 0;
        log.push_back("card 1: " + std::to_string(file1_->Count()) + " replay(s); card 2: " + std::to_string(file2_->Count()) + " replay(s), " +
                      std::to_string(file2_->FreeSectors()) + " of " + std::to_string(file2_->Total()) + " sectors free");
        if (!usable_) return 2;
        return file1_->Count() == 0 ? 0x14 : 0x0B;
    }
    case 0x0B: { // 0x80014D60 h(1)
        if (!CardsReady(true)) usable_ = false;
        if (!usable_) {
            MenuListClose(list_);
            return 2;
        }
        if (listDelay_ > 0 && --listDelay_ == 0) {
            sounds.push_back(7);
            MenuListOpen(list_);
            log.push_back("the list opens");
        }
        const int32_t r = listResult_;
        if (r == -3) {
            sounds.push_back(5);
            return -1;
        }
        if (r == -4) {
            sounds.push_back(0);
            return -1;
        }
        if (r == -2 || r < -4) return -1;
        if (r == -1) {
            MenuListClose(list_);
            sounds.push_back(2);
            return 2;
        }
        sounds.push_back(1);
        if (rows_[size_t(r)].kind != 0) {
            sounds.push_back(1);
            MenuListClose(list_);
            return 0x0C;
        }
        sounds.push_back(1);
        chosen_[size_t(r)] = !chosen_[size_t(r)]; // 0x80013694
        listDelay_ = 0;
        keptSelection_ = int16_t(r);
        return 0x0B;
    }
    case 0x0D: { // 0x80015304 h(1)
        if (!CardsReady(true)) usable_ = false;
        if (!Transfer()) return -1;
        const Pending& p = copy_[size_t(copyIndex_)];
        const std::vector<uint8_t> data = file1_->EntryData(p.entry); // the entry's whole sectors
        std::copy(data.begin(), data.begin() + std::ptrdiff_t(std::min<size_t>(data.size(), p.rounded)), buffer_.begin() + std::ptrdiff_t(p.offset));
        if (!file1_->EntryCrcOk(p.entry)) { // 0x800692DC
            progressVisible_ = -1;
            return Fail(kCorrupt1);
        }
        return 0x0E;
    }
    case 0x10: { // 0x800155AC h(1): the entry's sectors to card 2
        if (!Transfer()) return -1;
        const Pending& p = copy_[size_t(copyIndex_)];
        const int index = file2_->Count() - 1;
        const std::vector<int16_t> chain = file2_->Chain(file2_->Entry(index).first);
        // 0x80069758 writes the entry's sectors from the buffer, whole sectors: past the payload the last one holds the buffer's
        // bytes, i.e. card 1's sector tail as 0x800697E8 read it.
        std::vector<Piece> pieces;
        for (size_t k = 0; k < chain.size() && (k + 1) * kReplaySectorSize <= p.rounded; k++)
            pieces.push_back({kReplayDataStart + size_t(uint16_t(chain[k])) * kReplaySectorSize, buffer_.data() + p.offset + k * kReplaySectorSize, kReplaySectorSize});
        if (!WriteCard(pieces, false)) {
            progressVisible_ = -1;
            return Fail(kSaveFailed2);
        }
        return 0x11;
    }
    case 0x12: // 0x80015808 h(1)
        if (!Transfer()) return -1;
        progressVisible_ = -1;
        if (!WriteCard({{0, file2_->Bytes().data(), kReplayDataStart}}, false)) return Fail(kSaveFailed2);
        copied_ += int(copy_.size());
        log.push_back("copied " + std::to_string(copy_.size()) + " replay(s) to " + slots_[1].path + " (" + std::to_string(file2_->Count()) + " now)");
        return 0x13;
    case 0x13: return okExit(kComplete);
    case 0x14: return okExit(kNoData);
    default: return -1;
    }
}

int CopyReplayScreen::Switch(int id) { // 0x80015B08: the state and its h(0)
    for (int guard = 0; guard < 32 && id >= 0; guard++) {
        if (id == kExit) return kExit;
        state_ = id;
        id = Enter(id);
    }
    return -1;
}

bool CopyReplayScreen::Update(const MenuListPad* pad) { // 0x80015DC0
    sounds.clear();
    if (state_ < 0) return false;
    Poll();
    band_.Tick(); // 0x8006BE64
    if (rule_.anim < 0) { // 0x8006BCB8
        if (rule_.anim < -1) rule_.anim++;
    } else if (++rule_.anim > rule_.steps) {
        rule_.anim = rule_.steps;
    }
    for (size_t k = 0; k < 4; k++) barResult_[k] = CardManager::UpdateBar(bars_[k], pad, sounds);
    blockResult_ = CardManager::UpdateBlockSelector(blocks_, pad, sounds);
    listResult_ = MenuListUpdate(list_, pad);
    barResult_[kComplete] = CardManager::UpdateBar(bars_[kComplete], pad, sounds);
    int id = Step();
    if (id >= 0 && id != kExit) id = Switch(id);
    if (id != kExit) return true;
    ShowHeader(false); // the exit: + 0x48 / + 0x5E closed, the lines cleared, state -1
    line1_ = line2_ = 0;
    state_ = -1;
    return false;
}

void CopyReplayScreen::DrawHints(MenuOtSlot& base, bool writing) const { // 0x80013728(ot, writing)
    const HudFont& small = assets_.fonts[TitleAssets::kSmallFont]; // 0x800A8DE0
    auto right = [&](const std::string& s, int y) { // 0x8006AF40 at 0x140 - 0x8006B044
        AddNumberText(base, small, s, 0x140 - small.NumberWidth(s, 1, 0), y, 1, -2, 0, 0x024A4136u, 1);
    };
    const int count = file2_ ? file2_->Count() : 0, total = file2_ ? file2_->Total() : 0, used = file2_ ? file2_->UsedSectors() : 0;
    if (!writing) {
        right(FormatInt(assets_.Text(kTotalReplays), count + ChosenCount()), 0x8A);
        right(FormatInt(assets_.Text(kSectorsFree), FreeAfterCopy(), total), 0x9E);
        CardManager::DrawSectorBar(assets_, base, barTotal_, barUsed_, 0xB0, 0xAA, 0x80, 0, ChosenSectors());
    } else {
        right(FormatInt(assets_.Text(kTotalReplays), count), 0x8A);
        right(FormatInt(assets_.Text(kSectorsFree), total - used, total), 0x9E);
        CardManager::DrawSectorBar(assets_, base, barTotal_, used, 0xB0, 0xAA, 0x80, 0, 0); // + 0x1A1A = the live count
    }
}

std::vector<MenuPrim> CopyReplayScreen::Frame() const { // 0x80015F4C over the view header
    std::vector<MenuPrim> prims = TitleFrameStart();
    // OT: base + 5 the list's rows, base + 4 the list and the band's box, base + 2 / + 1 the bars' gradients / fills, base
    MenuOtSlot rows, list, gradients, fills, base, header;
    const HudFont& medium = assets_.fonts[TitleAssets::kMediumFont]; // 0x800A8DF0
    AddText(base, medium, assets_.Text(line1_), 0xB0, 0x112, 1, 0x025C5248u, 1, TextAlign::kCentre);
    AddText(base, medium, assets_.Text(line2_), 0xB0, 0x136, 1, 0x025C5248u, 1, TextAlign::kCentre);
    if (const int alpha = BandAlpha(band_)) {
        AddText(base, medium, assets_.Text(kHeaderText), 0x14, 0x8A, 0, MenuListLerp(0x02000000u, 0x0242362Au, alpha, 0x80), 1);
        band_.Draw(base, 0x10, 0x7A);
        base.DrawMode(0x220);
        for (const int y : {0xC6, 0x6E}) { // 0x8006BD08 at (0xB0, y)
            if (rule_.anim == -1) continue;
            int t = rule_.anim, k = rule_.steps - rule_.anim;
            if (rule_.anim < 0) t = ~rule_.anim, k = rule_.steps + 1 + rule_.anim;
            const int w = rule_.steps ? (rule_.w * t) / rule_.steps : 0;
            base.Add(Tile(0xB0 - (w >> 1), y, w, rule_.h, MenuListLerp(rule_.c0, rule_.c1, k, rule_.steps)));
        }
        base.DrawMode(0x220);
        AddGradientQuad(list, 0, 0x6E, 0x160, 0x58, MenuListLerp(0x02000000u, 0x025A5A5Au, alpha, 0x80), MenuListLerp(0x02000000u, 0x02141414u, alpha, 0x80));
        list.DrawMode(0x200);
    }
    for (size_t k = 0; k < 4; k++) CardManager::DrawBar(assets_, gradients, fills, base, bars_[k]);
    CardManager::DrawBlockSelector(assets_, base, blocks_);
    base.DrawMode(0x20);
    rowSlot_ = &rows;
    MenuListDraw(list_, list);
    rowSlot_ = nullptr;
    CardManager::DrawBar(assets_, gradients, fills, base, bars_[kComplete]);
    // h(2) of the state
    const HudFont& small = assets_.fonts[TitleAssets::kSmallFont];
    switch (state_) {
    case 1: AddText(base, medium, assets_.Text(error_), 0xB0, 0x122, 1, 0x022A485Cu, 1, TextAlign::kCentre); break;
    case 10:
    case 0x0D: DrawHints(base, false); break;
    case 0x10:
    case 0x12: DrawHints(base, true); break;
    case 0x0B: {
        DrawHints(base, false);
        const std::string copyText = FormatInt(assets_.Text(kCopyCount), ChosenCount()); // 0x800139C0
        AddNumberText(base, small, copyText, 0x140 - small.NumberWidth(copyText, 1, 0), 0xE0, 1, -2, 0, 0x024A4136u, 1);
        const int selection = list_.selection;
        uint32_t message = 0;
        if (selection >= 0 && selection < int(rows_.size())) {
            switch (rows_[size_t(selection)].why) {
            case 0: message = kCopyWord; break;
            case 1: message = kTooLarge; break;
            case 2: message = kMax32; break;
            case 3: message = ChosenCount() > 0 ? kConfirm : kNothingToCopy; break;
            default: break;
            }
        }
        if (arcadeMessageContext) { // the arcade's uninitialised context (title_copy.h): the medium font on the list record's words
            HudFont residue = assets_.fonts[TitleAssets::kMediumFont];
            residue.tpage = 0x36;       // + 0xE of the record's colour word 0x363636 (bits 21..22 kept by the mask 0xFF9FFFFF | 0x200000)
            residue.clutBase = 0x3636;  // + 0xC
            AddText(base, residue, assets_.Text(message), 0xB0, 0x1B8, 1, 0x646464u, 1, TextAlign::kCentre);
        } else {
            AddText(base, small, assets_.Text(message), 0xB0, 0x1B8, 1, 0x646464u, 1, TextAlign::kCentre);
        }
        break;
    }
    default: break;
    }
    if ((state_ == 8 || state_ == 9 || state_ == 10 || state_ == 0x0D || state_ == 0x10 || state_ == 0x12) && progressVisible_ >= 0)
        CardManager::DrawProgress(base, progress_, progressColour_, assets_.ovl1.Get<int16_t>(assets_.ovl1.Sim(kProgress) + 2)); // 0x8006C174
    AddViewHeader(header, assets_, assets_.Text(kViewTitle), kViewColour, 0x80);
    uint16_t mode = 0x200; // the draw mode carries over from one OT entry to the next
    for (const MenuOtSlot* slot : {&rows, &list, &gradients, &fills, &base, &header}) {
        slot->Emit(prims, mode);
        mode = slot->FinalMode(mode);
    }
    return prims;
}

} // namespace gt2::shell
