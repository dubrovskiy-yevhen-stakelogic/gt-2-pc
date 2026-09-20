#include "game/arcade/arcade_battle.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "gt2formats/car_info.h"

namespace gt2::arcade {

namespace {

namespace mp = menu_list_pad;

// Arcade v1.1 member 2 addresses (0x80020700 .. 0x80022658).
constexpr uint32_t kClassListP[2] = {0x8004FDDCu, 0x8004FDFCu};
constexpr uint32_t kItemsNoS[2] = {0x8004FC88u, 0x8004FD3Cu}, kItemsWithS[2] = {0x8004FC74u, 0x8004FD28u}, kItemsRally[2] = {0x8004FCECu, 0x8004FDA0u};
constexpr uint32_t kGarageLists = 0x8004FEC0u; // + garage * 0x34 + player * 0x68
constexpr uint32_t kCarouselTemplates[2] = {0x8004FE38u, 0x8004FE48u};
constexpr uint32_t kPageLayouts[2] = {0x8004FE58u, 0x8004FE6Cu}; // 10 s16: widgets 0 / 2 / (unused) / 3 at x + dx, y; the bars' x, y
constexpr uint32_t kTransmissionDefs[2] = {0x8004FBA0u, 0x8004FE80u}; // 0x8004FE98[player]
constexpr uint32_t kSettingsDefs[2] = {0x8004FBB8u, 0x8004FEA0u};     // 0x8004FEB8[player]
constexpr uint32_t kLabelTemplate = 0x8004FC4Cu, kLabelTexts[2] = {0x800F83D8u, 0x800F83E4u}; // "PLAYER 1" / "PLAYER 2"
constexpr uint32_t kBandTemplates[2] = {0x8004FC24u, 0x8004FC38u};
constexpr uint32_t kClassLabelTemplate = 0x8004FE1Cu, kClassLabelInitial = 0x800F82FAu; // "class A"
constexpr uint32_t kClassColours = 0x8004FAC0u, kClassNames = 0x8004FAF8u;               // [class row]
constexpr uint32_t kGrowLine = 0x8004FC10u;
constexpr uint32_t kMakerSprites = 0x8004F5F0u;                                          // 12-byte sprites by the class's second number
constexpr uint32_t kCentredLogoCar0 = 0x80027050u, kCentredLogoCar1 = 0x80027068u;       // 0x80020208: no maker logo, the name logo centred
constexpr uint32_t kGarageBars = 0x80051FBCu;                                            // 3 x 0xFF: no bar values
constexpr int16_t kLabelX[2] = {0x14, 0x14C}, kLabelY[2] = {0x6C, 0x1C8};
constexpr int16_t kClassLabelX[2] = {0x9C, 0xC4}, kClassLabelY[2] = {0x6C, 0x1C8};
constexpr int16_t kBandX[2] = {0, 0x160}, kBandY[2] = {0x58, 0x1B4};
constexpr int16_t kAreaX[2] = {0x8A, 4}, kAreaY[2] = {0x5E, 0x12E}, kAreaW = 0xD2, kAreaH = 0x90; // 0x80022658 -> 0x8008025C
constexpr int32_t kFrameLength = 0x117; // view + 0x218 (as ArcadeCarPage: <= 0xFB90, no zoom step)

uint16_t U16(std::span<const uint8_t> b, size_t o) { return uint16_t(b[o] | b[o + 1] << 8); }
int16_t S16(std::span<const uint8_t> b, size_t o) { return int16_t(U16(b, o)); }
uint32_t U32(std::span<const uint8_t> b, size_t o) { return uint32_t(U16(b, o)) | uint32_t(U16(b, o + 2)) << 16; }
template <typename T> void Put(uint8_t* p, T v) { std::memcpy(p, &v, sizeof v); }

// 0x80020170: the model camera of a player.
menu::OverlayModelCamera BattleCamera() {
    menu::OverlayModelCamera c;
    c.position = {0, 0, 0xB0000};
    c.pitch = 0x5E;
    c.yaw = 0x1500;
    c.roll = 0;
    c.x = 0, c.y = 0, c.w = 0xD2, c.h = 0x90;
    c.left = -0x69, c.right = 0x69, c.top = 0x43, c.bottom = -0x11, c.distance = 400, c.farZ = 0x7FFF;
    c.floor = true;
    c.floorSemi = false;
    c.floorColour = 0x404040;
    return c;
}

} // namespace

// ---------------------------------------------------------------- page

ArcadeBattlePage::ArcadeBattlePage(const ArcadeMenuAssets& a, const ArcadeData& data, const CarInfoDirectory& cars) : a_(a), data_(data), cars_(cars) {
    const GuestImage& o = a.data.ovl2;
    for (int p = 0; p < 2; p++) {
        const uint32_t t = kCarouselTemplates[p];
        carousel_[size_t(p)].x = o.Get<int16_t>(t);
        carousel_[size_t(p)].y = o.Get<int16_t>(t + 2);
        carousel_[size_t(p)].w = o.Get<int16_t>(t + 4);
        carousel_[size_t(p)].h = o.Get<int16_t>(t + 6);
        logos_[size_t(p)].tpage = uint16_t(0x16 + p); // 0x80016E54 at 0x80013F48: pages 22 / 23
        specs_[size_t(p)].Init(a);
        transmission_[size_t(p)].Init(a, kTransmissionDefs[p]);
        settings_[size_t(p)].Init(a, kSettingsDefs[p]);
        cameras_[size_t(p)] = BattleCamera();
    }
    line_.Read(o, kGrowLine);
}

const ArcadeClassCars& ArcadeBattlePage::ClassOf(int cls) const { return a_.data.classes.at(size_t(std::clamp(cls, 0, 6))); }

void ArcadeBattlePage::SetClassModel(int p, int cls, int index, int paintIndex) { // 0x80016530
    Model& m = models_[size_t(p)];
    m = Model{};
    m.car = PackCarId(ClassOf(cls).cars.at(size_t(index)));
    const int32_t info = cars_.IndexOf(m.car);
    m.paints = std::max<int>(1, int(cars_.At(info < 0 ? 0 : size_t(info)).PaintCount()));
    m.paint = paintIndex % m.paints;
}

void ArcadeBattlePage::SetGarageModel(int p, int garage, int index) { // 0x8001634C(model, view, garage, index)
    Model& m = models_[size_t(p)];
    m = Model{};
    const std::span<const uint8_t> car = std::span<const uint8_t>(garageBlocks_[size_t(garage)]).subspan(4 + size_t(index) * kGarageCarSize, kGarageCarSize);
    m.car = U32(car, 0x8C);
    m.garage = true;
    const int32_t info = cars_.IndexOf(m.car);
    const CarInfoRecord& r = cars_.At(info < 0 ? 0 : size_t(info));
    m.paints = std::max<int>(1, int(r.PaintCount()));
    const int32_t paint = int32_t(U32(car, 4));
    for (size_t i = 0; i < r.PaintCount() && i < r.paintIds.size(); i++)
        if (int32_t(int8_t(r.paintIds[i])) == paint) m.paint = int(i);
    const uint16_t wd = U16(car, 0x94);
    m.drive = wd >> 13;
    m.figures = {int16_t(U16(car, 0x98) & 0x3FFF), 0, S16(car, 0x96), 0, int16_t(wd & 0x1FFF)};
}

void ArcadeBattlePage::ClearModel(int p) { // 0x800165CC
    models_[size_t(p)].data = false;
    models_[size_t(p)].loaded = false;
}

void ArcadeBattlePage::SetLogo(int p, int cls, int index, MenuVram& vram) { // 0x80016ECC(logo, 0x8001D2E0, PackCarId(0x8001D308))
    const ArcadeClassCars& cc = ClassOf(cls);
    logos_[size_t(p)].Set(a_, vram, cc.model.at(size_t(index)), PackCarId(cc.cars.at(size_t(index))));
}

void ArcadeBattlePage::ModelTick() {
    for (Model& m : models_)
        if (m.car != 0) m.data = m.loaded = true;
}

bool ArcadeBattlePage::CarShown(int p) const { return timerModels_ == 0 && models_[size_t(p)].loaded; }

menu::MenuCarProjection ArcadeBattlePage::Projection(int p, bool withYaw) const {
    menu::MenuCarProjection r = menu::OverlayModelProject(cameras_[size_t(p)], kAreaX[p], kAreaY[p], withYaw);
    r.w = kAreaW;
    r.h = kAreaH;
    return r;
}

std::vector<MenuPrim> ArcadeBattlePage::Floor(int p) const { return menu::OverlayModelFloor(Projection(p, false), cameras_[size_t(p)]); }

void ArcadeBattlePage::Enter(const Context& ctx, bool reenter, const uint8_t* sel, MenuVram& vram) { // 0x80020700
    const GuestImage& o = a_.data.ovl2;
    if (!reenter) {
        ctx_ = ctx;
        for (int g = 0; g < 2; g++) garageBlocks_[size_t(g)].assign(ctx.garages[size_t(g)].begin(), ctx.garages[size_t(g)].end());
        timerReopen_ = -1;
        timerOpen_ = 0x18;
        timerModels_ = 0x18;
        for (PlayerState& s : state_) s = PlayerState{};
        line_.Open();
        for (int p = 0; p < 2; p++) {
            const size_t k = size_t(p);
            ClearModel(p);
            labels_[k].Init(a_, kLabelTemplate);
            labels_[k].text = a_.data.Text(kLabelTexts[p]);
            if (p == 1) labels_[k].flags = uint16_t(labels_[k].flags | 0x100);
            bands_[k].Read(o, kBandTemplates[p]);
            classLabels_[k].Init(a_, kClassLabelTemplate);
            classLabels_[k].text = a_.data.Text(kClassLabelInitial);
            // the class lists: Rally 3 rows, else 5 / 6 (with S); the garage rows disabled while the garage is empty
            const bool rally = ctx.listKind == 4;
            const uint32_t items = rally ? kItemsRally[p] : ctx.classSOpen ? kItemsWithS[p] : kItemsNoS[p];
            classList_[k] = PanelList::Read(o, kClassListP[p], items, rally ? 3 : ctx.classSOpen ? 6 : 5);
            for (PanelItem& it : classList_[k].items)
                if ((it.result == 4 || it.result == 5) && GarageCount(garageBlocks_[size_t(it.result - 4)]) == 0) it.kind = uint8_t(it.kind | 0x80);
            classList_[k].Init();
            ArcCarousel& car = carousel_[k]; // 0x8001C790(object, template, 0)
            car.group = 0, car.index = 0, car.anim = -1, car.arrows = false, car.available = nullptr;
            car.prevGroup = car.prevIndex = -1;
            car.slide = 0, car.vertical = 0;
            logos_[k].Clear();
            specs_[k].Init(a_);
            transmission_[k].Init(a_, kTransmissionDefs[p]);
            settings_[k].Init(a_, kSettingsDefs[p]);
            cameras_[k] = BattleCamera();
            for (int g = 0; g < 2; g++) { // FUN_8006CCDC(widget, row callback, view)
                MenuListWidget& w = garageList_[k][size_t(g)];
                w = MenuListWidget::Read(o, kGarageLists + uint32_t(g) * 0x34 + uint32_t(p) * 0x68);
                w.count = int16_t(GarageCount(garageBlocks_[size_t(g)]));
                garageInfo_[k][size_t(g)] = GarageSummary(garageBlocks_[size_t(g)]);
                MenuListReset(w, [this, p, g](int command, const MenuListWidget& wd, int row, const MenuListRowDraw* d) {
                    GarageRowSource src;
                    src.a = &a_, src.cars = &cars_, src.block = garageBlocks_[size_t(g)], src.info = &garageInfo_[size_t(p)][size_t(g)];
                    src.order = order_[size_t(p)].data(), src.rally = ctx_.rally, src.ctx = rowCtx_;
                    return GarageListRow(src, command, wd, row, d);
                });
            }
            for (int i = 0; i < 100; i++) order_[k][size_t(i)] = int16_t(i);
        }
        return;
    }
    // back from COURSE SELECTION: the cars again after 24 fields, the bars and carousels after 32
    timerModels_ = 0x18;
    timerReopen_ = 0x20;
    for (int p = 0; p < 2; p++) {
        const size_t k = size_t(p);
        logos_[k].Clear();
        int16_t g = 0;
        std::memcpy(&g, sel + Sel2P::kGarage + 2 * k, 2);
        if (g < 0) {
            state_[k].state = 7;
            SetLogo(p, state_[k].row, carousel_[k].index, vram);
        } else {
            state_[k].state = 3;
            int16_t slot = 0;
            std::memcpy(&slot, sel + Sel2P::kGarageSlot + 2 * k, 2);
            const std::span<const uint8_t> car = std::span<const uint8_t>(garageBlocks_[size_t(g)]).subspan(4 + size_t(slot) * kGarageCarSize, kGarageCarSize);
            logos_[k].Set(a_, vram, -1, U32(car, 0x8C));
        }
        labels_[k].Open(-1);
        bands_[k].Open();
        classLabels_[k].Open(-1);
    }
    line_.Open();
}

ArcadeBattlePage::Result ArcadeBattlePage::Update(const std::array<const MenuListPad*, 2>& pads, uint8_t* sel, std::vector<int>& sounds, MenuVram& vram, TextCtx& c) { // 0x8002229C
    rowCtx_ = &c;
    line_.Tick();
    Result result = kStay;
    if (timerModels_ > 0) timerModels_--;
    if (timerOpen_ > 0 && --timerOpen_ == 0)
        for (int p = 0; p < 2; p++) labels_[size_t(p)].Open(-1), bands_[size_t(p)].Open();
    if (timerReopen_ > 0 && --timerReopen_ == 0) {
        for (int p = 0; p < 2; p++) (state_[size_t(p)].state == 7 ? settings_ : transmission_)[size_t(p)].Open();
        for (ArcCarousel& car : carousel_) car.Open();
    }
    for (int p = 0; p < 2; p++) labels_[size_t(p)].Tick(), bands_[size_t(p)].Tick(), classLabels_[size_t(p)].Tick();
    const bool input = pads[0] != nullptr;
    const int r0 = Player(0, pads[0], input, sel, sounds, vram, c);
    const int r1 = Player(1, pads[1], input, sel, sounds, vram, c);
    if (r0 && !r1) CloseOther(0, sel);
    if (r1 && !r0) CloseOther(1, sel);
    if (!r0 || !r1) { // a player left the page: back
        sounds.push_back(4);
        result = kBack;
        for (int p = 0; p < 2; p++) {
            labels_[size_t(p)].Close();
            bands_[size_t(p)].Close();
            logos_[size_t(p)].Clear();
            ClearModel(p);
            state_[size_t(p)].state = 9;
        }
        line_.Close();
    }
    auto done = [](uint8_t s) { return s == 8 || s == 4; };
    if (done(state_[0].state) && done(state_[1].state)) {
        for (int p = 0; p < 2; p++) {
            state_[size_t(p)].state = 9;
            labels_[size_t(p)].Close();
            bands_[size_t(p)].Close();
            classLabels_[size_t(p)].Close();
            carousel_[size_t(p)].Close();
            logos_[size_t(p)].Clear();
        }
        result = kChosen;
        line_.Close();
        timerModels_ = -1;
        sounds.push_back(3);
    }
    return result;
}

int ArcadeBattlePage::Player(int p, const MenuListPad* pad, bool input, uint8_t* sel, std::vector<int>& sounds, MenuVram& vram, TextCtx& c) { // 0x80020D88
    const size_t k = size_t(p);
    PlayerState& s = state_[k];
    Model& m = models_[k];
    if (s.refresh && m.data) { // 0x800194FC
        ArcCarFigures f;
        if (s.row >= 4 && s.row <= 5) { // a garage car: 0x80016A7C, no curve, bars 0x80051FBC, no chips
            switch (m.drive) { // 0x80016A10
            case 0: f.badge = 1; break;
            case 2: f.badge = 2; break;
            case 3: f.badge = 3; break;
            case 4: f.badge = 4; break;
            default: f.badge = 0; break;
            }
            f.figures = m.figures;
            for (size_t b = 0; b < 3; b++) f.bars[b] = a_.data.ovl2.Get<uint8_t>(kGarageBars + uint32_t(b));
            f.curve = false;
        } else {
            f = ClassCarFigures(a_, data_, cars_, s.row, carousel_[k].index);
        }
        specs_[k].Fill(a_, f, ctx_.language, c);
        s.refresh = 0;
    }
    if (m.loaded) menu::TurnModelCamera(cameras_[k], 16, kFrameLength); // 0x8001E5BC
    specs_[k].Tick();
    if (!input) pad = nullptr;
    const uint32_t pressed = pad ? pad->pressed : 0;
    int16_t garage = 0;
    std::memcpy(&garage, sel + Sel2P::kGarage + 2 * k, 2);
    const MenuListPad* toList = nullptr;
    const MenuListPad* toHome = nullptr;
    const MenuListPad* toGuest = nullptr;
    const MenuListPad* toCarousel = nullptr;
    const MenuListPad* toTransmission = nullptr;
    const MenuListPad* toSettings = nullptr;
    switch (s.state) {
    case 0: toList = pad; break;
    case 1:
        if (garage == 0) toHome = pad;
        else if (garage == 1) toGuest = pad;
        break;
    case 2:
    case 5: toCarousel = pad; break;
    case 3:
    case 6: toTransmission = pad; break;
    case 7: toSettings = pad; break;
    default: break;
    }
    const int32_t rList = classList_[k].Update(toList);
    const int rCar = carousel_[k].Update(toCarousel);
    const int rTrans = transmission_[k].Update(toTransmission);
    const int rSet = settings_[k].Update(toSettings);
    const int32_t rHome = MenuListUpdate(garageList_[k][0], toHome);
    const int32_t rGuest = MenuListUpdate(garageList_[k][1], toGuest);
    auto classLabel = [&](int cls) {
        classLabels_[k].c0 = a_.data.ovl2.Get<uint32_t>(kClassColours + uint32_t(cls) * 4);
        classLabels_[k].text = a_.data.Text(a_.data.ovl2.Get<uint32_t>(kClassNames + uint32_t(cls) * 4));
        classLabels_[k].Open(-1);
    };
    switch (s.state) {
    case 0: {
        if (s.timer > 0 && --s.timer == 0) classList_[k].Open();
        if (rList == -2) return 1;
        if (rList == -3) {
            sounds.push_back(6);
            return 1;
        }
        if (rList == -4) {
            sounds.push_back(0);
            return 1;
        }
        if (rList == -1) {
            classList_[k].Close();
            return 0;
        }
        sounds.push_back(1);
        s.row = int8_t(rList);
        classList_[k].Close();
        if (rList == 4 || rList == 5) {
            s.timer = 0x18;
            s.state = 1;
            Put<int16_t>(sel + Sel2P::kGarage + 2 * k, int16_t(rList - 4));
            return 1;
        }
        const int cls = int(rList);
        int first = 0; // 0x8001D358: the first open car of lists 0 and 6
        const std::vector<uint8_t>& flags = ctx_.classFlags[size_t(std::clamp(cls, 0, 6))];
        if (cls == 0 || cls == 6)
            for (size_t i = 0; i < flags.size(); i++)
                if (flags[i]) {
                    first = int(i);
                    break;
                }
        classLabel(cls);
        ArcCarousel& car = carousel_[k];
        car.groups = 1;
        car.index = int16_t(first);
        car.counts[0] = int16_t(ClassOf(cls).cars.size()); // 0x800519A0[class]
        available_[k] = flags;
        car.available = &available_[k];
        logos_[k].Clear();
        SetLogo(p, cls, first, vram);
        SetClassModel(p, cls, first, 0);
        cameras_[k] = BattleCamera();
        s.state = 5;
        s.timer = 0x0C;
        Put<int16_t>(sel + Sel2P::kGarage + 2 * k, int16_t(-1));
        return 1;
    }
    case 1: {
        const int g = garage;
        if (g < 0 || g > 1) return 1;
        if (s.timer > 0 && --s.timer == 0) MenuListOpen(garageList_[k][size_t(g)]);
        const int32_t r = g != 0 ? rGuest : rHome;
        if (r == -3) {
            sounds.push_back(6);
            return 1;
        }
        if (r == -4) {
            sounds.push_back(0);
            return 1;
        }
        if (r == -2) return 1;
        if (r == -1) {
            sounds.push_back(2);
            MenuListClose(garageList_[k][size_t(g)]);
            s.state = 0;
            s.timer = 0x0C;
            return 1;
        }
        const int16_t index = order_[k].at(size_t(r));
        sounds.push_back(1);
        MenuListClose(garageList_[k][size_t(g)]);
        classLabel(garageInfo_[k][size_t(g)].at(size_t(index)).cls);
        ArcCarousel& car = carousel_[k];
        car.counts[0] = 1;
        car.groups = 1;
        car.index = 0;
        car.available = nullptr;
        const std::span<const uint8_t> gc = std::span<const uint8_t>(garageBlocks_[size_t(g)]).subspan(4 + size_t(index) * kGarageCarSize, kGarageCarSize);
        logos_[k].Set(a_, vram, -1, U32(gc, 0x8C));
        SetGarageModel(p, g, index);
        cameras_[k] = BattleCamera();
        s.state = 2;
        s.timer = 0x14;
        Put<int16_t>(sel + Sel2P::kGarageSlot + 2 * k, index);
        Put<uint32_t>(sel + Sel2P::kCar + 4 * k, U32(gc, 0x8C));
        Put<int16_t>(sel + Sel2P::kColour + 2 * k, int16_t(int8_t(m.paint)));
        return 1;
    }
    case 2: {
        if (s.timer > 0 && --s.timer == 0) {
            carousel_[k].Open();
            specs_[k].Swap();
            s.refresh = 1;
        }
        if (rCar == -2) return 1;
        if (rCar == -3) {
            sounds.push_back(7);
            return 1;
        }
        if (rCar == -1) {
            sounds.push_back(2);
            classLabels_[k].Close();
            carousel_[k].Close();
            specs_[k].CloseCurrent();
            ClearModel(p);
            s.state = 1;
            s.timer = 0x0C;
            return 1;
        }
        sounds.push_back(1);
        transmission_[k].Init(a_, kTransmissionDefs[p]);
        transmission_[k].Open();
        s.refresh = 0;
        specs_[k].CloseCurrent();
        s.state = 3;
        return 1;
    }
    case 3:
    case 6: {
        if (rTrans == -2) return 1;
        if (rTrans == -3) {
            sounds.push_back(5);
            return 1;
        }
        if (rTrans == -1) {
            sounds.push_back(2);
            specs_[k].Swap();
            s.refresh = 1;
            transmission_[k].Close();
            s.state = s.state == 3 ? 2 : 5;
            s.timer = 0;
            return 1;
        }
        if (s.state == 3) {
            if (!m.loaded) {
                sounds.push_back(0);
                return 1;
            }
            sounds.push_back(1);
            transmission_[k].Close();
            sel[Sel2P::kTransmission + k] = uint8_t(a_.data.transmissions[size_t(rTrans)]);
            s.state = 4;
            return 1;
        }
        sounds.push_back(1);
        transmission_[k].Close();
        settings_[k].Init(a_, kSettingsDefs[p]);
        settings_[k].Open();
        sel[Sel2P::kTransmission + k] = uint8_t(a_.data.transmissions[size_t(rTrans)]);
        s.state = 7;
        return 1;
    }
    case 4:
    case 8:
        if ((pressed & mp::kBack) == 0) return 1;
        sounds.push_back(2);
        (s.state == 4 ? transmission_ : settings_)[k].Open();
        s.state = s.state == 4 ? 3 : 7;
        return 1;
    case 5: {
        bool paintInput = s.timer == 0;
        if (s.timer > 0) {
            if (--s.timer == 0) {
                carousel_[k].Open();
                specs_[k].Swap();
                s.refresh = 1;
                paintInput = true;
            }
        }
        if (paintInput) {
            const uint32_t bits = pad ? (pad->pressed | pad->repeat) : 0;
            auto turn = [&](int d) { // 0x80015E9C
                if (m.loaded) m.paint = ((m.paint + d) % m.paints + m.paints) % m.paints;
            };
            if (bits & mp::kUp) turn(-1);
            if (bits & mp::kDown) turn(1);
            if (bits & (mp::kUp | mp::kDown)) sounds.push_back(5);
            specs_[k].SelectChip(m.paint);
        }
        const int idx = carousel_[k].index;
        if (rCar == -2) return 1;
        if (rCar == -3) {
            sounds.push_back(7);
            s.index = int8_t(idx);
            SetLogo(p, s.row, idx, vram);
            SetClassModel(p, s.row, idx, idx);
            cameras_[k] = BattleCamera();
            specs_[k].Swap();
            s.refresh = 1;
            return 1;
        }
        if (rCar == -1) {
            sounds.push_back(2);
            classLabels_[k].Close();
            carousel_[k].Close();
            specs_[k].CloseCurrent();
            ClearModel(p);
            s.state = 0;
            s.timer = 0x0C;
            return 1;
        }
        sounds.push_back(1);
        transmission_[k].Init(a_, kTransmissionDefs[p]);
        transmission_[k].Open();
        s.refresh = 0;
        specs_[k].CloseCurrent();
        Put<uint32_t>(sel + Sel2P::kCar + 4 * k, PackCarId(ClassOf(s.row).cars.at(size_t(idx))));
        Put<int16_t>(sel + Sel2P::kColour + 2 * k, int16_t(int8_t(m.paint)));
        s.state = 6;
        return 1;
    }
    case 7: {
        if (rSet == -2) return 1;
        if (rSet == -3) {
            sounds.push_back(6);
            return 1;
        }
        if (rSet == -1) {
            sounds.push_back(2);
            transmission_[k].Open();
            settings_[k].Close();
            s.state = 6;
            return 1;
        }
        if (!m.loaded) {
            sounds.push_back(0);
            return 1;
        }
        sounds.push_back(1);
        settings_[k].Close();
        sel[Sel2P::kTyres + k] = uint8_t(a_.data.transmissions[size_t(rSet)]); // 0x8004FBD0[button] (the same table)
        s.state = 8;
        return 1;
    }
    default: return 1;
    }
}

void ArcadeBattlePage::CloseOther(int p, const uint8_t* sel) { // 0x80022058
    const size_t k = size_t(p);
    const PlayerState& s = state_[k];
    switch (s.state) {
    case 0:
        if (s.timer == 0) classList_[k].Close();
        break;
    case 1: {
        int16_t g = 0;
        std::memcpy(&g, sel + Sel2P::kGarage + 2 * k, 2);
        if (s.timer == 0 && g >= 0 && g <= 1) MenuListClose(garageList_[k][size_t(g)]);
        break;
    }
    case 2:
    case 5:
        classLabels_[k].Close();
        if (s.timer == 0) {
            carousel_[k].Close();
            specs_[k].CloseCurrent();
        }
        break;
    case 3:
    case 6:
        classLabels_[k].Close();
        carousel_[k].Close();
        transmission_[k].Close();
        break;
    case 4:
    case 8:
        classLabels_[k].Close();
        carousel_[k].Close();
        break;
    case 7:
        classLabels_[k].Close();
        carousel_[k].Close();
        settings_[k].Close();
        break;
    default: break;
    }
}

void ArcadeBattlePage::PageDraw(int p, const ArcCarousel::Item& it, MenuOtSlot& ot, TextCtx& c) const { // 0x80020208
    const size_t k = size_t(p);
    const GuestImage& o = a_.data.ovl2;
    int16_t L[10];
    for (int i = 0; i < 10; i++) L[i] = o.Get<int16_t>(kPageLayouts[p] + uint32_t(i) * 2);
    const bool current = it.current < 0 || it.index == it.current;
    ArcButtonBar tb = transmission_[k], sb = settings_[k];
    tb.x = L[8], tb.y = L[9];
    tb.Draw(&ot, c);
    sb.x = L[8], sb.y = L[9];
    sb.Draw(&ot, c);
    specs_[k].Draw(0, current, &ot, it.x + L[0], L[1], c);
    specs_[k].Draw(2, current, &ot, it.x + L[2], L[3], c);
    specs_[k].Draw(3, current, &ot, it.x + L[6], L[7], c);
    if (it.current < 0) return;
    const int row = state_[k].row;
    uint32_t carId = 0;
    bool centred = false;
    if (row >= 4 && row <= 5) {
        std::memcpy(&carId, &models_[k].car, 4); // selection + 0xA8 + player * 4 (the garage car's model id)
    } else {
        const ArcadeClassCars& cc = ClassOf(row);
        const std::string& name = cc.cars.at(size_t(it.index));
        centred = name == a_.data.ImageString(kCentredLogoCar0) || name == a_.data.ImageString(kCentredLogoCar1);
        if (!centred) {
            const uint32_t e = kMakerSprites + uint32_t(cc.number2.at(size_t(it.index))) * 12;
            const int16_t h = o.Get<int16_t>(e + 6);
            const uint32_t g = uint32_t(it.brightness) | uint32_t(it.brightness) << 8 | uint32_t(it.brightness) << 16;
            AddScaledSprite(ot, it.x, (it.y - (h >> 2)) - 2, o.Get<uint8_t>(e), o.Get<uint8_t>(e + 1), o.Get<uint16_t>(e + 2), o.Get<int16_t>(e + 4), h,
                            o.Get<uint16_t>(e + 8), g, 0x40); // 0x80011C2C
        }
        carId = PackCarId(name);
    }
    logos_[k].Draw(ot, carId, it.x, it.y, it.brightness, centred, 0x40);
}

void ArcadeBattlePage::Draw(ViewOt& ot, TextCtx& c) const { // 0x80022658
    rowCtx_ = &c;
    MenuOtSlot& s2 = ot.slot[2];
    line_.Draw(s2);
    for (int p = 0; p < 2; p++) classList_[size_t(p)].Draw(s2);
    for (int p = 0; p < 2; p++)
        for (int g = 0; g < 2; g++) MenuListDraw(garageList_[size_t(p)][size_t(g)], s2);
    for (int p = 0; p < 2; p++) carousel_[size_t(p)].Draw(s2, [&](const ArcCarousel::Item& it) { PageDraw(p, it, s2, c); });
    for (int p = 0; p < 2; p++) {
        ArcText t = labels_[size_t(p)];
        t.x = kLabelX[p], t.y = kLabelY[p];
        t.Draw(&ot.slot[0], c);
    }
    for (int p = 0; p < 2; p++) bands_[size_t(p)].Draw(s2, kBandX[p], kBandY[p]);
    s2.DrawMode(0x220);
    for (int p = 0; p < 2; p++) {
        ArcText t = classLabels_[size_t(p)];
        t.x = kClassLabelX[p], t.y = kClassLabelY[p];
        t.Draw(&ot.slot[0], c);
    }
}

} // namespace gt2::arcade
