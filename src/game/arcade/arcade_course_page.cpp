#include "game/arcade/arcade_course_page.h"

#include <cstring>
#include <stdexcept>

#include "game/career/championship.h"
#include "gt2formats/course_data.h"
#include "gt2formats/course_map.h"
#include "gt2formats/hud_assets.h"

namespace gt2::arcade {

namespace {

namespace mp = menu_list_pad;

// Arcade v1.1 member 2 addresses.
constexpr uint32_t kCarouselTemplate = 0x8004FF90u;                  // {x 0xB0, y 0x78, w 0xE0, h 0x18, callback 0x800229E0}
constexpr uint32_t kBoxes[3] = {0x8004FFA0u, 0x8004FFBCu, 0x8004FFD8u}; // TOTAL LENGTH, STRAIGHT LENGTH, FASTEST LAP
constexpr uint32_t kBandBottom = 0x8004FFF4u, kBandTop = 0x80050008u;
constexpr uint32_t kBoxText0 = 0x80051FC0u, kBoxText1 = 0x80051FCCu;   // the label colour 0 -> 0x606060 (| semi) by the box fade
constexpr uint32_t kFadeFrom = 0x8004F430u, kLengthTo = 0x8004F440u, kSectorTo = 0x8004F434u, kLapTo = 0x8004F438u, kNameTo = 0x8004F43Cu;
constexpr uint32_t kNumberFormat = 0x80027264u; // "%d. "
// data-arcade.txd strings
constexpr uint32_t kTotalLength = 0x800F842Eu, kStraightLength = 0x800F8445u, kFastestLap = 0x800F8323u, kFeet = 0x800F845Fu, kNoRecords = 0x800F841Fu;
constexpr uint32_t kCellFive = 0x801234D0u, kCellSeven = 0x801294E0u;
constexpr uint8_t kReverseMark = 0x7F; // 0x800229E0: strcat(name, {0x7F, 0})

[[maybe_unused]] int Shr4(int x) { return (x < 0 ? x + 15 : x) >> 4; }

MenuPrim GradientQuad(int left, int right, int y, int h, uint32_t cl, uint32_t cr) {
    MenuPrim p;
    p.kind = MenuPrim::kPolyG4;
    p.gouraud = true;
    const int xs[4] = {left, right, left, right}, ys[4] = {y, y, y + h, y + h};
    const uint32_t cs[4] = {cl, cr, cl, cr};
    for (int i = 0; i < 4; i++) {
        p.x[i] = int16_t(xs[i]);
        p.y[i] = int16_t(ys[i]);
        p.colour[i] = cs[i] & 0xFFFFFF;
    }
    p.semi = (cl & 0x2000000u) != 0; // the packet's code byte = (c0's top byte | 0x38)
    return p;
}

// "%dft": the EXE's sprintf with one %d.
std::string FormatD(const std::string& fmt, int v) {
    const size_t at = fmt.find("%d");
    if (at == std::string::npos) throw std::logic_error("arcade course page: format without %d");
    return fmt.substr(0, at) + std::to_string(v) + fmt.substr(at + 2);
}

int32_t Rd32(std::span<const uint8_t> b, size_t o) {
    if (o + 4 > b.size()) throw std::out_of_range("arcade course page: career read");
    int32_t v;
    std::memcpy(&v, b.data() + o, 4);
    return v;
}

// 0x8005DD04(record, i): sector i of a lap record (i < 3: split[i] - split[i - 1]; 3: time - split[2]), -1 when unknown.
int32_t SectorTime(std::span<const uint8_t> r, int i) {
    int32_t a = i < 3 ? Rd32(r, 4 + size_t(i) * 4) : Rd32(r, 0);
    const int32_t b = i - 1 >= 0 ? Rd32(r, 4 + size_t(i - 1) * 4) : 0;
    if (a == -1 && b != -1) a = Rd32(r, 0);
    if (a == b) a = -1;
    if (a == -1 || b == -1) return -1;
    return a - b;
}

} // namespace

// ---------------------------------------------------------------- the box and the band

void ArcGrowBox::Read(const GuestImage& o, uint32_t a) {
    w = o.Get<int16_t>(a);
    h = o.Get<int16_t>(a + 2);
    steps = o.Get<int16_t>(a + 4);
    flags = o.Get<int16_t>(a + 6);
    c0 = o.Get<uint32_t>(a + 8);
    c1 = o.Get<uint32_t>(a + 12);
    from = o.Get<uint32_t>(a + 16);
    to = o.Get<uint32_t>(a + 20);
    anim = o.Get<int16_t>(a + 24);
}

void ArcGrowBox::Tick() { // 0x8006BD74
    if (anim < 0) {
        if (anim < -1) anim++;
        return;
    }
    anim++;
    if (steps < anim) anim = steps;
}

int ArcGrowBox::Fade() const { // 0x8006BDC4
    int v = anim;
    if (v < 0) v = v < -1 ? ~v : 0;
    return (v << 7) / steps;
}

void ArcGrowBox::Draw(MenuOtSlot& ot, int x, int y) const { // 0x8006BE04
    if (anim == -1) return;
    uint32_t target = from;
    int t = steps - anim;
    if (anim < 0) {
        target = to;
        t = steps + 1 + anim;
    }
    int left = x, right = x + w;
    if (flags & 1) left = x - w, right = x;
    const int d = (w * t) / steps;
    left -= d;
    right += d;
    ot.Add(GradientQuad(left, right, y, h, LerpColour(c0, target, t, steps), LerpColour(c1, target, t, steps)));
}

void ArcBand::Read(const GuestImage& o, uint32_t a) { // 0x80011EA0
    w = o.Get<int16_t>(a);
    h = o.Get<int16_t>(a + 2);
    c0 = o.Get<uint32_t>(a + 4);
    c1 = o.Get<uint32_t>(a + 8);
    from = o.Get<uint32_t>(a + 12);
    steps = o.Get<int16_t>(a + 16);
    flags = o.Get<int16_t>(a + 18);
    anim = -1;
}

void ArcBand::Tick() { // 0x80011F18
    if (anim < 0) {
        if (anim < -1) anim++;
        return;
    }
    anim++;
    if (steps <= anim) anim = steps;
}

void ArcBand::Draw(MenuOtSlot& ot, int x, int y) const { // 0x80011F68
    if (anim == -1) return;
    int t = steps - anim;
    if (anim < 0) t = steps + 1 + anim;
    int left = x - w, right = x;
    if ((flags & 1) == 0) left = x, right = x + w;
    const int d = (w * t) / steps;
    left -= d;
    right += d;
    auto mix = [&](uint32_t c, int k) {
        const int a = int((c >> (8 * k)) & 0xFF), b = int((from >> (8 * k)) & 0xFF);
        return uint32_t(a + ((b - a) * t) / steps) << (8 * k);
    };
    const uint32_t cl = mix(c0, 0) | mix(c0, 1) | mix(c0, 2) | (c0 & 0x2000000u);
    const uint32_t cr = mix(c1, 0) | mix(c1, 1) | mix(c1, 2);
    ot.Add(GradientQuad(left, right, y, h, cl, cr));
}

// ---------------------------------------------------------------- the page

ArcadeCoursePage::ArcadeCoursePage(const ArcadeMenuAssets& a, const CarInfoDirectory& cars) : a_(a), cars_(cars) {
    const GuestImage& o = a.data.ovl2;
    carousel_.x = o.Get<int16_t>(kCarouselTemplate);
    carousel_.y = o.Get<int16_t>(kCarouselTemplate + 2);
    carousel_.w = o.Get<int16_t>(kCarouselTemplate + 4);
    carousel_.h = o.Get<int16_t>(kCarouselTemplate + 6);
    for (size_t k = 0; k < 3; k++) boxes_[k].Read(o, kBoxes[k]);
    bands_[0].Read(o, kBandBottom);
    bands_[1].Read(o, kBandTop);
}

const ArcadeCourse& ArcadeCoursePage::Row(int group, int index) const { // 0x800229B0
    const std::vector<ArcadeCourse>& n = a_.data.courses[normal_];
    const std::vector<ArcadeCourse>& r = a_.data.courses[reverse_];
    if (group != 0 && size_t(index) < r.size() && size_t(index) < open_[reverse_].size() && open_[reverse_][size_t(index)] != 0) return r[size_t(index)];
    return n.at(size_t(index));
}

void ArcadeCoursePage::LoadPicture(const ArcadeCourse& row, MenuVram& vram) { // 0x80017C44 .. 0x80017CFC
    pictureLoaded_ = false;
    const auto found = a_.coursePictures.find(row.file);
    if (found == a_.coursePictures.end()) throw std::runtime_error("arcade course page: no picture of " + row.file + " in arcade/course_map");
    const CoursePicture& pic = found->second;
    const int px = (pictureTpage_ & 0xF) * 64, py = (pictureTpage_ & 0x10) * 16;
    std::vector<uint8_t> clut(pic.clut.size() * 2), image(pic.image.size() * 2);
    std::memcpy(clut.data(), pic.clut.data(), clut.size());
    std::memcpy(image.data(), pic.image.data(), image.size());
    vram.Upload(px, py, int(pic.clut.size()), 1, clut);
    vram.Upload(px, py + 1, pic.words, pic.rows, image);
    pictureOf_ = row.file;
    pictureLoaded_ = true;
    uploads_++;
}

void ArcadeCoursePage::RequestMovie(const ArcadeCourse& row) { // 0x800228A4(s16 of the map picture record + 6)
    movieIndex_ = a_.data.ovl2.Get<int16_t>(row.mapInfo + 6);
    movieCountdown_ = 12;
}

void ArcadeCoursePage::StopMovie() { // 0x80013B0C
    if (movie_ && moviePlaying_) movie_->Stop();
    moviePlaying_ = false;
}

void ArcadeCoursePage::SetInfo(const ArcadeCourse& row) { // 0x80022BD8
    const uint32_t index = career::CourseIndexOfId(a_.courses, CourseFileId(row.file)); // 0x80060DC4 of the row's id
    recordAt_ = 0x218 + size_t(index) * 0x24;
    recordState_ = 1, recordAnim_ = 1; // 0x800175F8
    mapInfo_ = row.mapInfo;
    lengthsState_ = 1, lengthsAnim_ = 1; // 0x800173C4
}

void ArcadeCoursePage::Enter(int listKind, const std::array<std::vector<uint8_t>, ArcadeMenuData::kCourseListCount>& open, std::span<const uint8_t> career,
                             MenuVram& vram) { // 0x80022C34
    open_ = open;
    career_.assign(career.begin(), career.end());
    switch (listKind) { // 0x800228BC
    case 0: normal_ = 0, reverse_ = 1; break;
    case 1: normal_ = 2, reverse_ = 3; break;
    case 2: normal_ = reverse_ = 4; break;
    case 3: normal_ = reverse_ = 5; break;
    case 4: normal_ = reverse_ = 6; break;
    default: throw std::logic_error("arcade course page: list kind " + std::to_string(listKind));
    }
    auto count = [&](size_t l) {
        int n = 0;
        for (uint8_t f : open_[l]) n += f ? 1 : 0;
        return n;
    };
    const int normalCount = count(normal_), reverseCount = reverse_ != normal_ ? count(reverse_) : normalCount;
    timer_ = 0x18;
    leaving_ = 0;
    StopMovie();
    movieCountdown_ = -1;
    carousel_.groups = int16_t(((listKind > 4 || listKind < 2) && reverseCount > 0) ? 2 : 1);
    carousel_.counts[0] = carousel_.counts[1] = int16_t(normalCount); // 0x8001C7F0 with {normal, normal}
    carousel_.group = 0;
    carousel_.index = 0;
    carousel_.prevGroup = carousel_.prevIndex = -1;
    carousel_.anim = -1;
    carousel_.slide = 0;
    carousel_.vertical = 0;
    carousel_.arrows = false;
    carousel_.available = nullptr;
    pictureLoaded_ = false; // 0x80017C64
    LoadPicture(a_.data.courses[normal_].at(0), vram);
    for (ArcGrowBox& b : boxes_) b.anim = -1;
    lengthsState_ = 0, lengthsAnim_ = 0;
    recordState_ = 0, recordAnim_ = 0;
    for (size_t k = 0; k < 2; k++) bands_[k].Read(a_.data.ovl2, k == 0 ? kBandBottom : kBandTop);
}

ArcadeCoursePage::Result ArcadeCoursePage::Update(const MenuListPad* pad, uint8_t* sel, std::vector<int>& sounds, MenuVram& vram) { // 0x80022D98
    if (timer_ > 0 && --timer_ == 0) {
        carousel_.Open();
        SetInfo(a_.data.courses[normal_].at(0)); // 0x80022BD8 of the normal list's first row
        RequestMovie(a_.data.courses[normal_].at(0));
        for (ArcGrowBox& b : boxes_) b.anim = 0;
        for (ArcBand& b : bands_) b.Open();
    }
    if (!leaving_ && movieCountdown_ >= 0 && --movieCountdown_ == 0 && movie_) { // 0x80022EBC -> 0x80013ADC
        movie_->Start(movieIndex_);
        moviePlaying_ = true;
    }
    if (movie_ && moviePlaying_) { // the preview object's field (0x8001805C) and the frame it loads at VRAM (640, 256)
        movie_->Tick();
        uint32_t version = 0;
        const std::vector<uint16_t>* picture = movie_->Picture(version);
        if (picture && version != movieVersion_ && picture->size() == 112 * 96) {
            movieVersion_ = version;
            vram.Upload(640, 256, 112, 96, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(picture->data()), picture->size() * 2));
            uploads_++;
        }
    }
    auto tickFade = [](int state, int& anim) { // 0x800173E8 / 0x80017618
        if (state != 1) return;
        if (anim >= 1 && anim <= 11) anim++;
        if (anim < 0 && anim >= -11) anim--;
    };
    tickFade(lengthsState_, lengthsAnim_);
    for (ArcGrowBox& b : boxes_) b.Tick();
    tickFade(recordState_, recordAnim_);
    for (ArcBand& b : bands_) b.Tick();
    const int r = carousel_.Update(pad);
    if (r == -2) return kStay;
    if (r == -3) {
        sounds.push_back(7);
        StopMovie();
        const ArcadeCourse& row = Row(carousel_.group, carousel_.index);
        LoadPicture(row, vram);
        RequestMovie(row);
        SetInfo(row);
        return kStay;
    }
    auto closeAll = [&] {
        StopMovie();
        leaving_ = 1;
        pictureLoaded_ = false; // 0x80017C64
        carousel_.Close();
        lengthsAnim_ = -1;      // 0x800173DC
        for (ArcGrowBox& b : boxes_) b.anim = int16_t(~b.steps);
        recordAnim_ = -1;       // 0x8001760C
        for (ArcBand& b : bands_) b.Close();
    };
    if (r == -1) {
        sounds.push_back(4);
        closeAll();
        return kBack;
    }
    if (!pictureLoaded_) { // view + 0x2AC: the picture is still loading
        sounds.push_back(0);
        return kStay;
    }
    const ArcadeCourse row = Row(carousel_.group, carousel_.index);
    closeAll();
    sounds.push_back(3);
    std::memcpy(sel + Sel::kCourseName, row.display.c_str(), row.display.size() + 1); // strcpy(selection + 0xB8, row + 4)
    const int16_t record = int16_t(row.record);
    std::memcpy(sel + Sel::kCourseRecord, &record, 2);
    const uint32_t id = CourseFileId(row.file); // the row's +0x10 as 0x8001D120 recomputed it
    std::memcpy(sel + Sel::kCourseId, &id, 4);
    return kChosen;
}

void ArcadeCoursePage::PageDraw(const ArcCarousel::Item& it, MenuOtSlot& ot, TextCtx& c) const { // 0x800229E0
    if (it.current < 0) return;
    const ArcadeCourse& row = Row(it.group, it.index);
    c.font = &a_.FontAt(kCellSeven);
    c.ot = &ot;
    c.mode = 1;
    std::string text;
    {
        const std::string fmt = a_.data.ImageString(kNumberFormat);
        text = FormatD(fmt, it.index + 1);
    }
    if (row.flags & 0x10) {
        text.push_back(char(kReverseMark));
        c.mode = 2;
    }
    text += row.display;
    c.colour = uint32_t(it.brightness) | uint32_t(it.brightness) << 8 | uint32_t(it.brightness) << 16 | 0x2000000u;
    DrawText(c, text, it.x - (TextWidth(c, text, 1) >> 1), it.y + 12, 1); // 0x8006ACC4
    if (it.current == it.index && pictureLoaded_) { // 0x80017EF0
        const uint32_t g = uint32_t(it.brightness) | uint32_t(it.brightness) << 8 | uint32_t(it.brightness) << 16;
        const uint16_t clut = uint16_t(((pictureTpage_ & 0x10) << 10) | ((pictureTpage_ & 0xF) << 2));
        AddSprite(ot, it.x - 0x1C, it.y + 0x82 - 100, 0, 1, clut, 0xBC, 200, pictureTpage_, g);
    }
}

void ArcadeCoursePage::Draw(ViewOt& ot, TextCtx& c) const { // 0x800231AC
    MenuOtSlot& s0 = ot.slot[0];
    carousel_.Draw(ot.slot[2], [&](const ArcCarousel::Item& it) { PageDraw(it, ot.slot[2], c); });
    if (movie_ && moviePlaying_ && movieVersion_ != 0) { // 0x80013B34 -> 0x8001819C: the picture, once one is decoded
        uint32_t version = 0;
        if (movie_->Picture(version)) AddSprite(s0, 24, 254, 0, 0, 0, 112, 96, 0x11A, 0x808080);
    }
    const auto labels = [&](int fade, uint32_t text, int x, int y) {
        c.font = &a_.FontAt(kCellFive);
        c.ot = &s0;
        c.mode = 1;
        c.colour = LerpColour(a_.data.ovl2.Get<uint32_t>(kBoxText0), a_.data.ovl2.Get<uint32_t>(kBoxText1), fade, 0x80);
        DrawText(c, a_.data.Text(text), x, y, 1);
    };
    if (const int f = boxes_[0].Fade(); f > 0) {
        labels(f, kTotalLength, 0x1C, 0xAA);
        labels(f, kStraightLength, 0x1C, 0xDA);
    }
    boxes_[0].Draw(s0, 0x18, 0x96);
    boxes_[1].Draw(s0, 0x18, 0xC6);
    if (lengthsState_ == 1) { // 0x80017488(object, ot, ctx, 0x82, 0xBE, 0x30)
        int t = lengthsAnim_;
        if (t < 0) t += 12;
        c.ot = &s0;
        c.mode = 1;
        c.font = &a_.FontAt(kCellFive);
        c.colour = LerpColour(a_.data.ovl2.Get<uint32_t>(kFadeFrom), a_.data.ovl2.Get<uint32_t>(kLengthTo), (t << 7) / 12, 0x80);
        const std::string fmt = a_.data.Text(kFeet);
        for (int k = 0; k < 2; k++) {
            const std::string s = FormatD(fmt, a_.data.ovl2.Get<int16_t>(mapInfo_ + uint32_t(k) * 4));
            DrawNumber(c, s, 0x82 - NumberWidth(c, s, 1, 0), 0xBE + k * 0x30, 1, -2, 0); // 0x8006B094
        }
    }
    if (const int f = boxes_[2].Fade(); f > 0) labels(f, kFastestLap, 0x2A, 0x186);
    boxes_[2].Draw(s0, 0x26, 0x172);
    if (recordState_ == 1) { // 0x800176B8(object, ot, ctx, 0x50, 0x17C)
        int t = recordAnim_;
        if (t < 0) t += 12;
        t = (t << 7) / 12;
        c.ot = &s0;
        c.mode = 1;
        c.font = &a_.FontAt(kCellFive);
        const GuestImage& o = a_.data.ovl2;
        const uint32_t from = o.Get<uint32_t>(kFadeFrom);
        c.colour = LerpColour(from, o.Get<uint32_t>(kSectorTo), t, 0x80);
        const std::span<const uint8_t> rec = std::span<const uint8_t>(career_).subspan(recordAt_, 0x24);
        int x = 0x50;
        const int y = 0x17C;
        for (int i = 0; i < 5; i++) {
            int32_t v = Rd32(rec, 0);
            if (i < 4) v = SectorTime(rec, i);
            if (i == 4) c.colour = LerpColour(from, o.Get<uint32_t>(kLapTo), t, 0x80);
            DrawTimeRight(c, FormatRaceTime(uint32_t(v)), x, y + 0x30, 6, 5, 0, 0);
            x += 0x3C;
        }
        x -= 0x39;
        const uint32_t nameColour = LerpColour(from, o.Get<uint32_t>(kNameTo), t, 0x80);
        c.colour = nameColour;
        if (Rd32(rec, 0) == -1) {
            const std::string s = a_.data.Text(kNoRecords);
            DrawText(c, s, x - TextWidth(c, s, 1), y + 0x18, 1); // 0x8006AD38
        } else { // the record's car (0x800609F8 of +0x14) and the career's name string (+0x18), each with a mark
            const int32_t car = Rd32(rec, 0x14);
            const int32_t ci = cars_.IndexOf(uint32_t(car));
            const std::string carName = cars_.At(ci < 0 ? 0 : size_t(ci)).rawName;
            int w = TextWidth(c, carName, 1);
            DrawText(c, carName, x - w, y + 0x18, 1);
            s0.Add(Tile((x - w) - 8, y + 0x0C, 4, 6, nameColour));
            s0.DrawMode(0x220);
            std::string name;
            for (size_t k = 0x18; k < rec.size() && rec[k]; k++) name.push_back(char(rec[k]));
            c.colour = LerpColour(from, o.Get<uint32_t>(kNameTo), t, 0x80);
            w = TextWidth(c, name, 1);
            DrawText(c, name, x - w, y, 1);
            s0.Add(Tile((x - w) - 8, y - 0x0C, 4, 6, c.colour));
            s0.DrawMode(0x220);
        }
    }
    bands_[0].Draw(ot.slot[1], 0x160, 0x174);
    ot.slot[1].DrawMode(0x200);
    bands_[1].Draw(ot.slot[4], 0, 0x6A);
    ot.slot[4].DrawMode(0x200);
}

} // namespace gt2::arcade
