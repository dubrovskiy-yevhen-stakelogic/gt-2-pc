#pragma once
// COURSE SELECTION of the arcade menus (view 0x80052334 of GT2.OVL member 2, US Arcade v1.1 SCUS_944.55 SHA-1
// 231f9dba7191b9ef915621662afdc40a7c66df95; ARCADE v1.1 addresses): init 0x80022C34, update 0x80022D98, draw 0x800231AC.
// docs/research/arcade_disc.md section 18.
//
// A carousel of the list kind's courses (0x800F35D0, template 0x8004FF90, page callback 0x800229E0: "<n>. " (0x80027264) + the
// course's name, a reverse course marked with character 0x7F and drawn subtractive, centred in the cell-7 font; the course picture
// of arcade/course_map (0x80017EF0, tpage 0x16) under the current page), two groups (normal / reverse) for the road and time trial
// lists when a reverse course is open; the length box (0x80017488: "%dft" 0x800F845F of the picture record +0 / +4), the record
// table (0x800176B8: the career's course record career + 0x218 + course x 0x24, its three sectors and the lap, "- No Records -"
// 0x800F841F or the car / name), three label boxes (EXE 0x8006BD74 / 0x8006BDC4 / 0x8006BE04 at 0x8004FFA0 / BC / D8: TOTAL LENGTH,
// STRAIGHT LENGTH, FASTEST LAP), two bands (0x80011EA0 .. 0x80011F68, templates 0x8004FFF4 / 0x80050008).
// The course movie of STREAM.DAT: 0x800228A4 (on the first course and every course change: index = s16 of the row's map picture
// record + 6, countdown 12), 0x80022EBC (the countdown runs while the view is not leaving; at 0 -> 0x80013ADC: the preview object
// 0x80129510 starts movie 0x80053334[index]), 0x80013B0C (stop: course change / leaving), 0x80013B34 (a 112 x 96 SPRT of the
// 15-bit picture at VRAM (640, 256), tpage 0x11A, colour 0x808080, at (24, 254), into OT slot 0 before the labels). The frames
// come from the caller's decoder (CourseMovieSource: gt2game's CoursePreview on gt2formats/str_video.h, docs/formats/str_video.md).
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "game/arcade/arcade_menus.h"
#include "game/arcade/arcade_widgets.h"

namespace gt2::arcade {

// EXE 0x8006BD74 / 0x8006BDC4 / 0x8006BE04: a growing gradient box (0x1C bytes: w, h, steps, flags, c0, c1, the colour it grows
// from, the colour it shrinks to, anim).
struct ArcGrowBox {
    int16_t w = 0, h = 0, steps = 1, flags = 0;
    uint32_t c0 = 0, c1 = 0, from = 0, to = 0;
    int16_t anim = -1;
    void Read(const GuestImage& ovl2, uint32_t address);
    void Tick();
    int Fade() const;                                // 0..128
    void Draw(MenuOtSlot& ot, int x, int y) const;
};

// 0x80011EA0 .. 0x80011F68: a band (w, h, c0 left, c1 right, the colour both grow from, steps, flags bit 0 = anchored right).
struct ArcBand {
    int16_t w = 0, h = 0, steps = 1, flags = 0;
    uint32_t c0 = 0, c1 = 0, from = 0;
    int16_t anim = -1;
    void Read(const GuestImage& ovl2, uint32_t address);
    void Open() { anim = 0; }
    void Close() { anim = int16_t(~steps); }
    void Tick();
    void Draw(MenuOtSlot& ot, int x, int y) const;
};

// The decoder of the course movies (implemented by the program: it reads the disc's STREAM.DAT).
class CourseMovieSource {
public:
    virtual ~CourseMovieSource() = default;
    virtual void Start(int index) = 0; // index into member 2's table 0x80053334 (course -> movie)
    virtual void Stop() = 0;
    virtual void Tick() = 0;           // one field of the stream
    // The latest 112 x 96 15-bit picture (null before the first); `version` changes with every new picture.
    virtual const std::vector<uint16_t>* Picture(uint32_t& version) const = 0;
};

class ArcadeCoursePage {
public:
    ArcadeCoursePage(const ArcadeMenuAssets& a, const CarInfoDirectory& cars);

    // 0x80022C34: `listKind` = 0x800F364C (0 road, 1 time trial, 2 rally, 3 / 4 2P); `open` = the rows 0x8001D120 marks open, per
    // course list of ArcadeMenuData (0 road, 1 road reverse, 2 time trial, 3 reverse, 4 rally, 5 2P, 6 2P dirt).
    void Enter(int listKind, const std::array<std::vector<uint8_t>, ArcadeMenuData::kCourseListCount>& open, std::span<const uint8_t> career, MenuVram& vram);
    enum Result { kStay = 0, kChosen = 1, kBack = 2 };
    // 0x80022D98: fills the selection (course name, id, record number) when a course is chosen.
    Result Update(const MenuListPad* pad, uint8_t* sel, std::vector<int>& sounds, MenuVram& vram);
    void Draw(ViewOt& ot, TextCtx& c) const; // 0x800231AC
    uint32_t Uploads() const { return uploads_; }
    void SetMovieSource(CourseMovieSource* source) { movie_ = source; }

private:
    const ArcadeCourse& Row(int group, int index) const; // 0x800229B0
    void LoadPicture(const ArcadeCourse& row, MenuVram& vram); // 0x80017C44 (the original reads it from the CD over a few fields)
    void SetInfo(const ArcadeCourse& row);                     // 0x80022BD8: the record and the lengths of the row
    void PageDraw(const ArcCarousel::Item& it, MenuOtSlot& ot, TextCtx& c) const; // 0x800229E0

    const ArcadeMenuAssets& a_;
    const CarInfoDirectory& cars_;
    size_t normal_ = 0, reverse_ = 0; // the course lists of the kind (0x800228BC)
    std::array<std::vector<uint8_t>, ArcadeMenuData::kCourseListCount> open_;
    std::vector<uint8_t> career_;
    ArcCarousel carousel_;
    std::array<ArcGrowBox, 3> boxes_;
    std::array<ArcBand, 2> bands_; // 0x800F3610 (bottom, anchored right), 0x800F3630 (top)
    int timer_ = 0, leaving_ = 0;
    // the lengths object 0x800F3600 and the record object 0x800F3608: {state, anim, record}
    int lengthsState_ = 0, lengthsAnim_ = 0, recordState_ = 0, recordAnim_ = 0;
    uint32_t mapInfo_ = 0;
    size_t recordAt_ = 0;
    // the picture object (view + 0x294): loaded (+0x18) and its course
    bool pictureLoaded_ = false;
    std::string pictureOf_;
    uint16_t pictureTpage_ = 0x16;
    uint32_t uploads_ = 0;
    // the course movie (0x800F366C countdown, 0x800F366E index)
    void RequestMovie(const ArcadeCourse& row); // 0x800228A4
    void StopMovie();                           // 0x80013B0C
    CourseMovieSource* movie_ = nullptr;
    int16_t movieCountdown_ = -1, movieIndex_ = 0;
    bool moviePlaying_ = false;
    uint32_t movieVersion_ = 0;
};

} // namespace gt2::arcade
