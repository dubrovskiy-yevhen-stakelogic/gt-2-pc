#include "game/shell/title_transfer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "game/career/garage.h"
#include "game/career/results.h"
#include "game/shell/title_draw.h"
#include "gt2formats/save_data.h"

namespace gt2::shell {

namespace {
constexpr int kMixedCourseRecords = 0x80; // 0x8001E014 stops after 128 records
uint32_t U32(const void* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
} // namespace

void MixCourseRecords(career::CareerState& ours, const career::CareerState& other) { // 0x8001E014
    for (int i = 0; i < kMixedCourseRecords; i++) {
        const uint32_t theirs = U32(&other.courses[i]), mine = U32(&ours.courses[i]);
        if (theirs == 0xFFFFFFFFu) continue;
        if (mine != 0xFFFFFFFFu && mine <= theirs) continue;
        std::memcpy(&ours.courses[i], &other.courses[i], sizeof(career::CourseRecord));
    }
}

int32_t MixLicenceRank(const career::TimeRecord& time, const char* name, const career::LicenceTestRecord& record) { // 0x8001E11C
    const uint32_t t = uint32_t(time.time[0]);
    if (t == 0xFFFFFFFFu) return -1;
    for (int k = 0; k < 5; k++) {
        const uint32_t slot = uint32_t(record.times[k].time[0]);
        if (slot == 0xFFFFFFFFu || t < slot) return k;
        if (slot != t) continue;
        if (std::memcmp(&record.times[k], &time, sizeof(career::TimeRecord)) != 0) continue;
        const char* mine = reinterpret_cast<const char*>(record.entries[k]);
        const size_t n = std::strlen(mine);
        if (n != std::strlen(name)) continue;
        if (std::memcmp(mine, name, n + 1) == 0) return -1; // the same time by the same driver: already there
    }
    return -1;
}

void MixLicenceRecords(career::CareerState& ours, const career::CareerState& other) { // 0x8001E284
    for (size_t l = 0; l < career::kLicenceCount; l++)
        for (size_t t = 0; t < career::kLicenceTests; t++) {
            const career::LicenceTestRecord& theirs = other.licences[l][t];
            career::LicenceTestRecord& mine = ours.licences[l][t];
            for (int k = 0; k < 5; k++) {
                const char* name = reinterpret_cast<const char*>(theirs.entries[k]);
                if (MixLicenceRank(theirs.times[k], name, mine) >= 0) career::StoreLicenceRecord(mine, theirs.times[k], std::string(name));
            }
        }
}

int32_t InsertMachineTestEntry(career::MachineTestRecord& record, const uint8_t* entry, bool higher) { // EXE 0x8005E0D0
    uint8_t* r = record.bytes;
    auto at = [&](int k) { return r + 4 + k * 0x14; };
    auto better = [&](uint32_t value, uint32_t than) { return higher ? than < value : value < than; };
    if (r[0] > 8) r[0] = 0;
    const uint32_t car = U32(entry), value = U32(entry + 4);
    for (int k = 0; k < int(int8_t(r[0])); k++) { // an entry of the same car: replaced only by a better value
        if (U32(at(k)) != car) continue;
        if (!better(value, U32(at(k) + 4))) return -1;
        for (int j = k + 1; j < int(int8_t(r[0])); j++) std::memcpy(at(j - 1), at(j), 0x14);
        const uint32_t none = 0xFFFFFFFFu;
        std::memcpy(at(int(int8_t(r[0])) - 1) + 4, &none, 4);
        r[0] = uint8_t(r[0] - 1);
        break;
    }
    int k = 0;
    const int count = int(int8_t(r[0]));
    while (k < count && !better(value, U32(at(k) + 4))) k++;
    if (k == count && k >= 8) return -1;
    for (int j = 6; j >= k; j--) std::memcpy(at(j + 1), at(j), 0x14);
    std::memcpy(at(k), entry, 0x14);
    if (int8_t(r[0]) < 8) r[0] = uint8_t(r[0] + 1);
    return k;
}

void MixMachineTests(career::CareerState& ours, const career::CareerState& other) { // 0x8001E38C / 0x8001E408 / 0x8001E484
    for (int test = 0; test < 3; test++) {
        const uint8_t* theirs = other.machineTests[test].bytes;
        for (int k = 0; k < int(int8_t(theirs[0])); k++) InsertMachineTestEntry(ours.machineTests[test], theirs + 4 + k * 0x14, test == 2);
    }
}

void MixRecords(career::CareerState& ours, const career::CareerState& other) { // 0x8001E550
    MixCourseRecords(ours, other);
    MixLicenceRecords(ours, other);
    MixMachineTests(ours, other);
}

uint32_t Gt1Checksum(std::span<const uint8_t> data) { // 0x8001FBFC
    uint32_t sum = 0xAAAA, crc = 0x3770;
    for (const uint8_t byte : data) {
        uint32_t b = byte;
        sum = (sum + b) ^ (b << 8);
        for (int i = 0; i < 8; i++) {
            crc <<= 1;
            if (crc & 0x10000) crc ^= 0x11021;
            crc |= (b >> 7) & 1;
            b <<= 1;
        }
    }
    return (crc & 0xFFFF) | sum << 16;
}

bool Gt1DataOk(std::span<const uint8_t> data) { // 0x8001FC7C
    if (data.size() < kGt1CheckedSize + 4) return false;
    return U32(data.data() + kGt1CheckedSize) == Gt1Checksum(data.first(kGt1CheckedSize));
}

void ConvertGt1Licences(career::CareerState& ours, std::span<const uint8_t> data) { // 0x8001E61C
    auto allPassed = [&](size_t offset) {
        for (size_t i = 0; i < 8; i++)
            if (offset + i >= data.size() || data[offset + i] == 0) return false;
        return true;
    };
    if (!allPassed(0x2B5C)) return;
    for (career::LicenceTestRecord& r : ours.licences[5])
        if (r.passed == 0) r.passed = 1;
    if (!allPassed(0x2B64)) return;
    for (career::LicenceTestRecord& r : ours.licences[4])
        if (r.passed == 0) r.passed = 1;
}

int32_t TradeAvailability(const career::CareerState& ours, const career::GarageBlock& other, int row) { // 0x8001EAD8
    if (ours.garage.count >= career::kGarageCapacity) return 1;
    return ours.garage.money < other.cars[row].value ? 2 : 0;
}

void TradeBuy(career::CareerState& ours, const career::GarageBlock& other, int row) { // 0x8001EB60
    career::AddPreparedCar(ours.garage, other.cars[row]);
    ours.garage.money -= other.cars[row].value;
}


// ---------------------------------------------------------------- member 1's card manager (DATA TRANSFER)

namespace {
// Strings: data-global.txd (0x801EF6B0 block) and data-title.txd (0x801B9630 block).
constexpr uint32_t kSelectSlot = 0x801EF709u, kChecking = 0x801EF726u, kDoNotRemove = 0x801EF744u, kNoCard = 0x801EF76Fu, kReadFailed = 0x801EF78Cu,
                   kStartLoading = 0x801EF880u, kLoadingNow = 0x801EF895u, kLoadDataFailed = 0x801EF8BFu, kNoGameFile = 0x801EF997u,
                   kLoadingFailed = 0x801EF9BFu;
constexpr uint32_t kTradeLine = 0x801B9D08u, kMixLine = 0x801B9D2Cu, kConvertLine = 0x801B9D96u, kNoGt1 = 0x801B9DC9u, kNoCars = 0x801B9DF6u,
                   kMixDone = 0x801B9E33u, kConvertDone = 0x801B9E51u, kGt1Failed = 0x801BA3E1u;
constexpr uint32_t kBarTemplates[6] = {0x8004C718u, 0x8004C748u, 0x8004C778u, 0x8004C7A8u, 0x8004C7D8u, 0x8004C808u};
constexpr uint32_t kViewColours[3] = {0xD68C9Au, 0x90B4D6u, 0x7878D6u}; // views 0x8004C59C / 0x8004C644 / 0x8004C698
constexpr uint32_t kViewTitles[3] = {0x801B9CE0u, 0x801B9CEAu, 0x801B9CFBu};
constexpr const char* kGameFile = "BASCUS-94455GAME";
} // namespace

TransferManager::TransferManager(const TitleAssets& assets, Mode mode, std::array<CardSlot, 2> slots)
    : assets_(assets), mode_(mode), slots_(std::move(slots)) {
    const GuestImage& o = assets.ovl1;
    for (size_t k = 0; k < bars_.size(); k++) bars_[k] = CardManager::ReadBar(o, o.Sim(kBarTemplates[k]));
    bars_[kSlot].cursor = 1;                     // 0x8002062C: mgr + 0x165 = 1 (Slot2)
    headerBand_ = Band::Read(o, o.Sim(0x8004C6FCu)); // the "Memory Card N" band, + 0x4C = -1
    headerBand_.anim = -1;
    Switch(0);
}

int TransferManager::Status() const { return CardStatus(slots_[size_t(slot_)]); }

void TransferManager::ProgressReset(uint32_t total) { // 0x8006C04C
    progressTotal_ = total;
    progressVisible_ = 0;
    progress_.fill(-1);
}

void TransferManager::ProgressUpdate(uint32_t remaining) { // 0x8006C074
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

int TransferManager::Enter(int id) {
    switch (id) {
    case 0: delay_ = 0x18; return -1; // 0x8001FF1C
    case 1: // 0x8001FF74: the error text (+0x24) with ERROR! Change Slot / Exit
        sounds.push_back(0);
        CardManager::OpenBar(bars_[kError]);
        bars_[kError].cursor = 0;
        line1_ = line2_ = 0;
        return -1;
    case 2: // 0x800200E8
        if (headerBand_.anim >= 0) headerBand_.anim = int16_t(~headerBand_.steps);
        CardManager::OpenBar(bars_[kSlot]);
        line1_ = mode_ == kTrade ? kTradeLine : mode_ == kMix ? kMixLine : kConvertLine;
        line2_ = kSelectSlot;
        return -1;
    case 3: // 0x80020278
        line1_ = kChecking, line2_ = 0;
        headerBand_.anim = 0;
        return Step();
    case 4: // 0x800203B0
        CardManager::OpenBar(bars_[kNoCard]);
        line1_ = kNoCard, line2_ = 0;
        return -1;
    case 5: // 0x8002049C
        sounds.push_back(0);
        CardManager::OpenBar(bars_[kError]);
        bars_[kError].cursor = 0;
        line1_ = mode_ == kConvert ? kNoGt1 : kNoGameFile, line2_ = 0;
        return -1;
    case 6:  // 0x8001F780
    case 10: // 0x8001FCB0
        CardManager::OpenBar(bars_[kStart]);
        bars_[kStart].cursor = 1;
        line1_ = kStartLoading, line2_ = 0;
        return -1;
    case 7: // 0x8001F898: 0x8006A1B4 reads the save (0x8006A12C: 0x7F00 bytes)
        progressColour_ = 0x0090500Cu;
        ProgressReset(0x7F00);
        transferLeft_ = 0x7F00;
        line1_ = kLoadingNow, line2_ = kDoNotRemove;
        return -1;
    case 11: // 0x8001FDC8: 0x8007D658(6, slot, "BASCUS-94194GT", mgr + 0x41C, 0, 0xA000, 0)
        progressColour_ = 0x0090500Cu;
        ProgressReset(uint32_t(kGt1ReadSize));
        transferLeft_ = int(kGt1ReadSize);
        line1_ = kLoadingNow, line2_ = kDoNotRemove;
        return -1;
    case 8: // 0x8001FA24
        CardManager::OpenBar(bars_[kComplete]);
        bars_[kComplete].cursor = 1;
        line1_ = mode_ == kMix ? kMixDone : kConvertDone, line2_ = 0;
        return -1;
    case 9: // 0x8001FB20
        sounds.push_back(0);
        CardManager::OpenBar(bars_[kNoCars]);
        bars_[kNoCars].cursor = 1;
        line1_ = kNoCars, line2_ = 0;
        return -1;
    default: return -1;
    }
}

int TransferManager::Step() {
    auto bar = [&](int index, int yes, int no) { // the Yes / No and Change Slot / Exit bars: 0 -> sound 1, `yes`; 1 / -1 -> `no`
        const int r = barResult_[size_t(index)];
        if (r == 0) { sounds.push_back(1); return yes; }
        if (r == 1 || r == -1) return no;
        return -1;
    };
    switch (state_) {
    case 0: return --delay_ < 0 ? 2 : -1;
    case 1: return bar(kError, 2, kExit);
    case 2: {
        const int r = barResult_[kSlot];
        if (r == -1) return kExit;
        if (r == 0 || r == 1) {
            sounds.push_back(1);
            slot_ = r;
            return 3;
        }
        return -1;
    }
    case 3: {
        std::vector<uint8_t> card;
        switch (CardStatus(slots_[size_t(slot_)], &card)) {
        case 1: return -1;
        case 0:
            if (mode_ == kConvert) return CardFindFile(card, kGt1SaveFileName) >= 0 ? 10 : 5;
            return CardFindFile(card, kGameFile) >= 0 ? 6 : 5;
        case 2: return 4;
        case 4: error_ = kReadFailed; return 1;
        default: return 5;
        }
    }
    case 4:
        if (Status() == 2) return bar(kNoCard, 2, kExit);
        CardManager::CloseBar(bars_[kNoCard]);
        return 3;
    case 5: {
        const int status = Status();
        if (status == 2 || status == 4) {
            CardManager::CloseBar(bars_[kError]);
            return 3;
        }
        return bar(kError, 2, kExit);
    }
    case 6:
    case 10: {
        if (Status() != 0) {
            CardManager::CloseBar(bars_[kStart]);
            return 3;
        }
        const int r = barResult_[kStart];
        if (r == 0) { sounds.push_back(1); return state_ == 6 ? 7 : 11; }
        if (r == 1) { sounds.push_back(1); return 2; }
        if (r == -1) { sounds.push_back(2); return 2; }
        return -1;
    }
    case 7:
    case 11: {
        transferLeft_ = std::max(0, transferLeft_ - 0x80 * sectorsPerField);
        ProgressUpdate(uint32_t(transferLeft_));
        if (transferLeft_ > 0) return -1;
        progressVisible_ = -1;
        std::vector<uint8_t> card;
        try {
            card = career::ReadFileBytes(slots_[size_t(slot_)].path);
        } catch (const std::exception& e) {
            log.push_back(std::string("read failed: ") + e.what());
            error_ = kLoadDataFailed;
            return 1;
        }
        if (state_ == 7) { // CRC 0x8006A314 -> mode 0: a garage without cars 9, else done
            try {
                const career::CareerSave save = career::LoadCareerFromCard(card);
                if (!save.CrcOk()) {
                    error_ = kLoadingFailed;
                    return 1;
                }
                loaded_ = save.state;
            } catch (const std::exception& e) {
                log.push_back(std::string("load failed: ") + e.what());
                error_ = kLoadDataFailed;
                return 1;
            }
            log.push_back("loaded the save of " + slots_[size_t(slot_)].path + " (" + std::to_string(loaded_->garage.count) + " car(s))");
            if (mode_ == kTrade && loaded_->garage.count < 1) return 9;
            return 8;
        }
        std::vector<uint8_t> file;
        for (const MemoryCardFile& f : ReadMemoryCardFiles(card))
            if (f.name == kGt1SaveFileName) file = f.bytes;
        file.resize(std::max(file.size(), kGt1ReadSize), 0);
        gt1_.assign(file.begin() + std::ptrdiff_t(kGt1DataOffset), file.begin() + std::ptrdiff_t(kGt1ReadSize));
        if (!Gt1DataOk(gt1_)) { // 0x8001FC7C
            error_ = kGt1Failed;
            return 1;
        }
        log.push_back("loaded the Gran Turismo save of " + slots_[size_t(slot_)].path);
        return 8;
    }
    case 8: return bar(kComplete, 2, kExit);
    case 9: return bar(kNoCars, 2, kExit);
    default: return -1;
    }
}

void TransferManager::Switch(int id) { // 0x800205C8
    for (int guard = 0; guard < 16 && id >= 0; guard++) {
        state_ = id;
        id = Enter(id);
    }
}

int TransferManager::Update(const MenuListPad* pad) { // 0x80020868
    sounds.clear();
    if (idle_) return 0;
    headerBand_.Tick();
    static constexpr int kOrder[6] = {kError, kSlot, kNoCard, kComplete, kNoCars, kStart}; // +0x50, +0xE8, +0x180, +0x218, +0x348, +0x2B0
    for (int k : kOrder) barResult_[size_t(k)] = CardManager::UpdateBar(bars_[size_t(k)], pad, sounds);
    int id = Step();
    for (int guard = 0; guard < 16; guard++) {
        if (id == -1) return 0;
        if (id == kExit) {
            if (headerBand_.anim >= 0) headerBand_.anim = int16_t(~headerBand_.steps);
            line1_ = line2_ = 0;
            idle_ = true;
            state_ = -1;
            return 1;
        }
        if (id == 8) {
            if (mode_ == kTrade) { // the TRADE list follows; the manager idles
                if (headerBand_.anim >= 0) headerBand_.anim = int16_t(~headerBand_.steps);
                line1_ = line2_ = 0;
                idle_ = true;
                state_ = -1;
                return 2;
            }
            Switch(8);
            return 2;
        }
        state_ = id;
        id = Enter(id);
    }
    return 0;
}

std::vector<MenuPrim> TransferManager::Frame() const { // 0x80020A3C over the view header
    std::vector<MenuPrim> prims = TitleFrameStart();
    MenuOtSlot gradients, fills, base, header;
    const HudFont& medium = assets_.fonts[TitleAssets::kMediumFont];
    AddText(base, medium, assets_.Text(line1_), 0xB0, 0xD6, 1, 0x025C5248u, 1, TextAlign::kCentre);
    AddText(base, medium, assets_.Text(line2_), 0xB0, 0xFA, 1, 0x025C5248u, 1, TextAlign::kCentre);
    CardManager::DrawSlotBand(assets_, base, headerBand_, slot_);
    static constexpr int kOrder[6] = {kError, kSlot, kNoCard, kComplete, kNoCars, kStart};
    for (int k : kOrder) CardManager::DrawBar(assets_, gradients, fills, base, bars_[size_t(k)]);
    if (state_ == 1 && error_) CardManager::DrawErrorText(assets_, base, error_);
    if (progressVisible_ >= 0) CardManager::DrawProgress(base, progress_, progressColour_);
    AddViewHeader(header, assets_, assets_.Text(kViewTitles[mode_]), kViewColours[mode_], 0x80);
    gradients.Emit(prims, 0x200);
    fills.Emit(prims, 0x200);
    base.Emit(prims, 0x200);
    header.Emit(prims, 0x200);
    return prims;
}

// ---------------------------------------------------------------- TRADE list

namespace {
constexpr uint32_t kTradeList = 0x8004C43Cu, kMoneyBand = 0x8004C404u, kCountBand = 0x8004C420u, kBuyBar = 0x8004C470u, kRowSprite = 0x8004B9FCu;
constexpr uint32_t kAvailable = 0x801B9E70u, kNoMoney = 0x801B9E84u, kGarageFull = 0x801B9EA0u, kCredits = 0x801B9ED5u, kCarsFormat = 0x801B9ED9u;

// 0x8001E808: an amount with thousands separators ("0" for 0).
std::string Credits(uint32_t v) {
    if (v == 0) return "0";
    std::string digits = std::to_string(v % 100000000u);
    std::string out;
    int first = int(digits.size()) % 3;
    if (first == 0) first = 3;
    for (size_t i = 0; i < digits.size(); i++) {
        if (i != 0 && (int(i) - first) % 3 == 0) out.push_back(',');
        out.push_back(digits[i]);
    }
    return out;
}

// "%d/% 3d cars" (printf: the count, then 100 in a 3-wide field).
std::string CarsText(const std::string& format, int count) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%d/%3d cars", count, 100);
    (void)format;
    return buffer;
}
} // namespace

TradeScreen::TradeScreen(const TitleAssets& assets, const CarInfoDirectory& cars, career::CareerState& career, const career::CareerState& other)
    : assets_(assets), cars_(cars), career_(career), other_(other) { // 0x8001F084
    const GuestImage& o = assets.ovl1;
    list_ = MenuListWidget::Read(o, o.Sim(kTradeList));
    list_.count = other.garage.count;
    money_ = Band::Read(o, o.Sim(kMoneyBand));
    count_ = Band::Read(o, o.Sim(kCountBand));
    money_.anim = count_.anim = -1;
    MenuListReset(list_, [this](int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) { return RowCallback(command, w, row, draw); });
    bar_ = CardManager::ReadBar(o, o.Sim(kBuyBar));
    state_ = 0;
    delay_ = 12;
}

int32_t TradeScreen::RowCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) { // 0x8001EE24
    if (command == kMenuListEnabled) return row >= 0 && TradeAvailability(career_, other_.garage, row) == 0 ? 1 : 0;
    if (command != kMenuListDraw || !draw || !draw->ot || row < 0) return 0;
    int level = (int(w.fade) << 7) / int(w.fadeMax);
    if (row == w.selection) { // 0x8006CF64: the selection's own fade (closing: 0x3B frames before the end)
        int v = w.fade;
        if (w.state < -1) {
            v = w.fadeMax - 0x3B - w.state;
            if (v < 0) v = 0;
            if (w.fadeMax < v) v = w.fadeMax;
        }
        level = -((v << 7) / int(w.fadeMax));
    }
    if (level == 0) return 0;
    if (w.state != -1) DrawRow(rowSlot_ ? *rowSlot_ : *draw->ot, row, draw->x, draw->y, draw->alpha, level);
    if (row != w.selection) return 0;
    const int available = TradeAvailability(career_, other_.garage, row);
    const uint32_t text = available == 1 ? kGarageFull : available == 2 ? kNoMoney : kAvailable;
    const uint32_t colour = available == 0 ? 0x606060u : 0x142864u;
    if (state_ == 0 && w.state >= 0) AddText(*draw->ot, assets_.fonts[TitleAssets::kSmallFont], assets_.Text(text), 0xB0, 0x19A, 1, colour, 1, TextAlign::kCentre);
    return 0;
}

void TradeScreen::DrawRow(MenuOtSlot& ot, int row, int x, int y, int alpha, int a) const { // 0x8001EBD4
    const career::GarageCar& car = other_.garage.cars[row];
    int level = a < 0 ? alpha + 0x80 + a : (alpha * a) >> 7;
    if (level > 0xFF) level = 0xFF;
    const uint32_t colour = uint32_t(level / 2) | uint32_t(level / 2) << 8 | uint32_t(level / 3) << 16 | 0x02000000u;
    const HudFont& small = assets_.fonts[TitleAssets::kSmallFont];
    const CarInfoRecord* info = cars_.Find(car.modelId);
    AddText(ot, small, info ? info->rawName : std::string(), x - 0x6E, y + 8, 1, colour, 1); // 0x80060AE8 + 0x8006AC90
    uint16_t chip = 0; // 0x80060D28(model, paint): the paint's chip colour (paint characters compared case-insensitively)
    if (info && !info->chipColors.empty()) {
        auto lower = [](int32_t c) { return uint32_t(c - 0x41) < 0x1Au ? c + 0x20 : c; };
        size_t index = 0;
        for (size_t i = 0; i < info->PaintCount() && i < info->paintIds.size(); i++)
            if (lower(int32_t(int8_t(info->paintIds[i]))) == lower(int32_t(car.paint))) {
                index = i;
                break;
            }
        chip = index < info->chipColors.size() ? info->chipColors[index] : 0;
    }
    MenuListPaintChip(ot, x - 0x7C, y - 8, 9, 16, chip, level);
    AddNumberText(ot, small, Credits(uint32_t(car.value)), x + 0x78, y + 8, 1, -2, 0, colour, 1, true); // 0x8006B184
    const GuestImage& o = assets_.ovl1;
    MenuPrim s; // 0x80081478: SPRT 0x64 of the panel 0x8004B9FC {uv, CLUT 0x3E30, 256 x 24}, then E1 *(0x8004BA04)
    s.kind = MenuPrim::kSprite;
    s.w = int16_t(o.Get<uint16_t>(o.Sim(kRowSprite) + 4)), s.h = int16_t(o.Get<uint16_t>(o.Sim(kRowSprite) + 6));
    s.x[0] = int16_t(x - (s.w >> 1)), s.y[0] = int16_t(y - (s.h >> 1));
    s.u = o.Get<uint8_t>(o.Sim(kRowSprite)), s.v = o.Get<uint8_t>(o.Sim(kRowSprite) + 1);
    s.clut = o.Get<uint16_t>(o.Sim(kRowSprite) + 2);
    s.colour[0] = uint32_t(level) * 0x010101u;
    const uint16_t e1 = o.Get<uint16_t>(o.Sim(kRowSprite) + 8);
    s.tpage = e1;
    ot.Add(s);
    ot.DrawMode(e1);
}

int TradeScreen::Update(const MenuListPad* pad) { // 0x8001F11C
    sounds.clear();
    money_.Tick();
    count_.Tick();
    if (delay_ > 0 && --delay_ == 0) {
        MenuListOpen(list_);
        money_.anim = count_.anim = 0;
    }
    if (state_ == 0) {
        CardManager::UpdateBar(bar_, nullptr, sounds);
        const int32_t r = MenuListUpdate(list_, pad);
        if (r == -3) { sounds.push_back(6); return 0; }
        if (r == -4) { sounds.push_back(0); return 0; }
        if (r == -2) return 0;
        if (r == -1) {
            MenuListClose(list_);
            money_.anim = int16_t(~money_.steps);
            count_.anim = int16_t(~count_.steps);
            sounds.push_back(4);
            return 2;
        }
        sounds.push_back(1);
        CardManager::OpenBar(bar_);
        bar_.cursor = 1; // 0x800B156D = 1: "No"
        state_ = 1;
        return 0;
    }
    MenuListUpdate(list_, nullptr);
    const int r = CardManager::UpdateBar(bar_, pad, sounds);
    if (r == 0) {
        TradeBuy(career_, other_.garage, list_.selection);
        bought++;
    }
    if (r == 0 || r == 1) sounds.push_back(1);
    else if (r == -1) sounds.push_back(2);
    else return 0;
    CardManager::CloseBar(bar_);
    state_ = 0;
    return 0;
}

std::vector<MenuPrim> TradeScreen::Frame() const { // 0x8001F358 over the view header
    std::vector<MenuPrim> prims = TitleFrameStart();
    // OT + 0x1C the rows, + 0x18 the list, the texts and the bar's gradients (its third slot), + 0x14 the bar's fills, + 0x10 the bar
    MenuOtSlot rows, list, fills, bar, header;
    const HudFont& small = assets_.fonts[TitleAssets::kSmallFont];
    CardManager::DrawBar(assets_, list, fills, bar, bar_);
    rowSlot_ = &rows;
    MenuListDraw(list_, list);
    rowSlot_ = nullptr;
    if (state_ != 1) {
        AddText(list, small, assets_.Text(kCredits), 0x40, 0x1B8, 1, 0x02604214u, 1, TextAlign::kRight);
        AddNumberText(list, small, Credits(uint32_t(career_.garage.money)), 0x90, 0x1B8, 1, -2, 0, 0x02604214u, 1, true);
        money_.Draw(list, 0x20, 0x1A4);
        AddNumberText(list, small, CarsText(assets_.Text(kCarsFormat), career_.garage.count), 0x136, 0x1B8, 1, -2, 0, 0x02604214u, 1, true);
        count_.Draw(list, 0x146, 0x1A4);
    }
    AddViewHeader(header, assets_, assets_.Text(kViewTitle), kViewColour, 0x80);
    uint16_t mode = 0x200; // the draw mode carries over from one OT entry to the next
    for (const MenuOtSlot* slot : {&rows, &list, &fills, &bar, &header}) {
        slot->Emit(prims, mode);
        mode = slot->FinalMode(mode);
    }
    return prims;
}

} // namespace gt2::shell
