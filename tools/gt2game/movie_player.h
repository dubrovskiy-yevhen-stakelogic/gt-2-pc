#pragma once
// The movies of the Arcade disc played natively (docs/formats/str_video.md): STREAM.DAT through gt2formats/str_video.h
// (the ported frame decoder of GT2.OVL member 5 and the MDEC model) and gt2view/movie_view.h, the XA audio of the stream
// through the game's mixer. What the original plays and how (member 5, US Arcade v1.1):
//   - the intro, movie 24: at boot before the title (EXE main 0x8005D670 -> member 5 0x800114B8 -> 0x80011328) and every
//     fourth attract cycle of the title (member 1 0x8001156C: 0x801EF030 counts 1, 2, 3, 0 -> member 5); 320 x 192 in a
//     320 x 240 24-bit display at y 24, a frame every 2 fields, Start skips it (0x8001102C: pad + 0xD4 & 0x10000 while
//     + 0xE0 == 0); then the title (member 1);
//   - the endings, movies 25 / 26: ENDING CREDITS row 0 (Arcade Mode) / row 1 (Simulation Mode) -> view 0x80052670 -> 24
//     fields -> result 5 / 6 -> member 5 0x800114E0(1 / 0) -> 0x80011430 / 0x800113A8: 640 x 216 at y 12 / 640 x 224 at
//     y 8 of a 640 x 240 24-bit display, a frame every 4 fields, not skippable (+ 0xE0 = 1); then the title;
//   - the course previews, movies 0..23: the COURSE SELECTION page (member 2), see CoursePreview.
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "game/arcade/arcade_course_page.h"
#include "gt2formats/str_video.h"
#include "gt2vfs/disc_image.h"

namespace gt2game {

class GameWindow;

// The movie data of a disc: null when the disc has no STREAM.DAT (the Simulation disc).
struct MovieLibrary {
    std::string discPath;
    std::vector<gt2::StreamMovie> movies;
    uint32_t streamLba = 0;
    gt2::GtVlcTable vlc;
    gt2::MdecCore mdec;
    std::vector<uint16_t> courseMovies; // member 2 0x80053334: .crsinfo course record -> movie
    static std::unique_ptr<MovieLibrary> Load(const gt2::DiscImage& disc, const std::string& discPath);
};

// How member 5 shows a movie (the numbers of 0x80011328 / 0x800113A8 / 0x80011430).
struct MovieSpec {
    int movie = gt2::kMovieIntro;
    int fieldsPerFrame = 2;                    // + 0x18
    int displayWidth = 320, displayHeight = 240;
    int x = 0, y = 24;                         // + 0x150 / + 0x154
    bool skippable = true;                     // + 0xE0 == 0: Start ends it
    int prebuffer = 2;                         // + 0x13C: frames waited for before the first is shown
};
MovieSpec MovieSpecOf(int movie);

enum class MovieResult { kFinished, kSkipped, kClosed };
MovieResult PlayPreparedMovie(GameWindow& window, const std::string& path, const MovieSpec& spec, bool sound);
// Plays one movie full screen in `window` (its own sound device unless `sound` is false). Returns when it ends, is
// skipped (Start / S / Esc, when the original allows it) or the window closes.
MovieResult PlayMovie(GameWindow& window, const gt2::DiscImage& disc, const MovieLibrary& library, int movie, bool sound);

// The COURSE SELECTION preview (member 2: the preview object 0x80129510 / 0x800EFA58, 0x8001805C / 0x800265B8 / 0x80026660): the
// movie 0x80053334[index] decoded to 15-bit words (DecDCTin(buf, 0), loaded at VRAM (640, 256)), started over at its end; the
// course page (game/arcade/arcade_course_page.h) drives it and draws the sprite. The stream's audio is silent (every sample of the
// 24 previews' XA is 0) and is not played. Checked: gt2run movie-check, the VRAM rectangle = DecodeMovieFrame15 at every change.
class CoursePreview final : public gt2::arcade::CourseMovieSource {
public:
    CoursePreview(const gt2::DiscImage& disc, const MovieLibrary& library) : disc_(disc), library_(library) {}
    void Start(int index) override;
    void Stop() override;
    // One field: the stream advances 150 * 1001 / 60000 sectors; the latest completed frame becomes the picture.
    void Tick() override;
    const std::vector<uint16_t>* Picture(uint32_t& version) const override {
        version = version_;
        return picture_.empty() ? nullptr : &picture_;
    }

private:
    void Open();
    const gt2::DiscImage& disc_;
    const MovieLibrary& library_;
    int movie_ = -1;
    std::unique_ptr<gt2::GtMovieReader> reader_;
    double clock_ = 0;
    std::vector<uint16_t> picture_;
    uint32_t version_ = 0;
};

} // namespace gt2game
