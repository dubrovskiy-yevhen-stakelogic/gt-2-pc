#pragma once
// The title's attract demo in gt2game, both discs (game/shell/title_attract.h: which demo / the intro, the demo file): the demo
// file's replay plays like a Replay Theater replay - the title's case 6 runs 0x80010EDC and the race overlay with argument 1,
// the same launch as the theater's. The overlay keeps the argument at 0x800A9500 (ovl0 0x800121AC, Simulation v1.2 SHA-1
// 3030aa27...); with it set, Start (generic pad bit 0x10000) makes the race frame (0x80015C44 .. 0x80015C98) return 0 instead of
// opening the pause: the demo ends and member 1 starts again with the title (observed: Start at field 3000 of the first demo ->
// member 1 at 3146; Cross does nothing). Otherwise the demo ends when its stream runs out (0x800A8D68).
// The exit fade (after Start, and likewise when the stream runs out): no scene is drawn any more; every field one RECT 320 x
// 240 of colour word 0x080810 (R 16, G 8, B 8) with blend mode 2 (B - F) goes into the buffer that is displayed next,
// alternately, for 69 fields (gt2play captures: RECTs in fields 3008 .. 3076 after Start at 3000, then 0x8005DA3C(1); at the
// end of demo 01 in fields 18012 .. 18080); the displayed buffer j fields into it has had (j + 1) / 2 of them.
#include <vector>
#include <memory>
#include <optional>
#include <string>

#include "game/shell/title_attract.h"
#include "gt2formats/replay_card.h"

namespace gt2 {
class DiscImage;
class GtfsVolume;
} // namespace gt2

namespace gt2view {
struct DrawItem;
}

namespace gt2game {

class GameWindow;
struct RaceViewConfig;
struct RaceOptions;

// The demo file of the disc's language (US: arcade/demofile_us.gmr on both discs) and the attract cycle's state.
class TitleAttractDemos {
public:
    TitleAttractDemos(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, bool arcade, uint8_t language = 1);

    // One attract: the intro (arcade, returns kMovie) or the next demo's payload (in `payload`).
    enum class Kind { kMovie, kDemo };
    Kind Next(gt2::ReplayPayload& payload, std::string& title);

    const std::string& Path() const { return path_; }
    gt2::shell::TitleAttract cycle;

private:
    std::string path_;
    std::unique_ptr<gt2::ReplayCardFile> file_;
};

// Plays a title-launched replay (Replay Theater / attract) in `window`: a race replay (LoadReplayRace), a game mode 6 record
// (ghost_replay.h) or a two-player record (split_race.h, both streams); Start / Esc ends it at once (0x800A9500 != 0). Returns
// false when the window was closed.
// The exit fade of a title-launched replay left with Start or at its stream's end (argument 1 of the race overlay: the attract demo and the Replay
// Theater): the last presented frame `shown` (items[0, sceneCount) = the scene) stays and darkens through kExitFadeFields fields.
constexpr int kExitFadeFields = 69;
constexpr uint32_t kExitFadeColour = 0x080810u;          // the RECT's colour word (0xBBGGRR)
constexpr int ExitFadeSteps(int field) { return (field + 1) / 2; } // subtractions in the displayed buffer at fade field 0..68
void PlayTitleExitFade(GameWindow& window, const std::vector<gt2view::DrawItem>& shown, size_t sceneCount);

bool PlayTitleReplay(GameWindow& window, const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const gt2::ReplayPayload& payload, const std::string& title,
                     const RaceOptions& raceOptions, const RaceViewConfig& raceView, bool hudModeGiven);

} // namespace gt2game
