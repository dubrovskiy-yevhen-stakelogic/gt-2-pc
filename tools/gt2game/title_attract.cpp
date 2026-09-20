// The title's attract demo in gt2game. See title_attract.h.
#include "title_attract.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>

#include "game/sim/race_shell.h"
#include "game_window.h"
#include "ghost_replay.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/replay.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "gt2view/vk_scene_renderer.h"
#include "race_common.h"
#include "race_view.h"
#include "split_race.h"

using namespace gt2;

namespace gt2game {

TitleAttractDemos::TitleAttractDemos(const DiscImage& disc, const GtfsVolume& vol, bool arcade, uint8_t language) : cycle(arcade) {
    const GuestImage ovl1 = LoadOverlayImage(disc, 1);
    path_ = shell::AttractDemoFilePath(ovl1, vol, language);
    file_ = std::make_unique<ReplayCardFile>(ReplayCardFile::FromBytes(vol.Read(path_)));
    std::printf("attract: demo file %s (%d replays)\n", path_.c_str(), file_->Count());
}

TitleAttractDemos::Kind TitleAttractDemos::Next(ReplayPayload& payload, std::string& title) {
    const shell::TitleAttract::Step step = cycle.Next(file_->Count());
    if (step.movie) return Kind::kMovie;
    std::vector<uint8_t> data = file_->EntryData(step.demo); // 0x80020E14 / 0x80020A98: the entry's chain
    data.resize(size_t(file_->Entry(step.demo).size));
    payload = UnpackReplayPayload(data);                      // 0x80010EDC reads the gathered payload (0x80069AC4)
    title = file_->Entry(step.demo).Title();
    return Kind::kDemo;
}

void PlayTitleExitFade(GameWindow& window, const std::vector<gt2view::DrawItem>& shown, size_t sceneCount) {
    gt2view::VkSceneRenderer& renderer = window.Renderer();
    constexpr uint32_t kFadeVertexBase = 1'048'536; // between PanelView's range and MovieView's
    for (int j = 0; j < kExitFadeFields; j++) {
        if (!window.BeginFrame()) return;
        std::vector<gt2view::DrawItem> items = shown;
        const int m = ExitFadeSteps(j);
        if (m > 0) { // m RECTs of B - F with clamping at 0 = one of m times the colour (clamped to 255)
            float colour[3];
            for (int c = 0; c < 3; c++) colour[c] = float(std::min(255u, uint32_t(m) * ((kExitFadeColour >> (8 * c)) & 0xFF))) / 255.0f;
            std::vector<gt2view::SceneVertex> quad;
            static constexpr float kCorners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
            for (const auto& p : kCorners) {
                gt2view::SceneVertex v{};
                v.pos[0] = p[0], v.pos[1] = p[1], v.pos[2] = 1.0f;
                for (int c = 0; c < 3; c++) v.color[c] = colour[c];
                quad.push_back(v);
            }
            renderer.SetVertices(kFadeVertexBase, quad);
            gt2view::DrawItem item;
            item.firstVertex = kFadeVertexBase;
            item.vertexCount = 6;
            item.blend = 2; // B - F
            const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::copy(identity, identity + 16, item.mvp);
            items.push_back(item);
        }
        window.EndFrame(items, std::string(), std::chrono::nanoseconds(16'683'333), sceneCount);
    }
}

bool PlayTitleReplay(GameWindow& window, const DiscImage& disc, const GtfsVolume& vol, const ReplayPayload& payload, const std::string& title,
                     const RaceOptions& raceOptions, const RaceViewConfig& raceView, bool hudModeGiven) {
    RaceData data;
    RaceOptions ro = raceOptions;
    RaceViewConfig v = raceView;
    v.replayEndLeaves = true; // 0x80015DCC: the stream ran out -> member 1
    v.exitFade = true;        // Start: the exit fade, then member 1
    v.replayOut.clear();
    sim::GhostSession session;
    ReplayFile file;
    size_t cars = 1;
    if (payload.GameMode() == 6) { // a Time Trial / Rally record: player 1 on its lap ring (ghost_replay.h)
        LoadGhostRecordRace(disc, vol, payload, data, ro, session);
        v.replay = nullptr;
        v.ghostReplay = true;
        v.ghostOption = 0;
        std::printf("attract: playing '%s' (game mode 6 record, %zu lap(s))\n", title.c_str(), payload.ghosts.size());
    } else {
        file = payload.ToReplayFile();
        for (const ReplayEntry& e : file.cars)
            if (e.kind() == 4) { // a player-2 slot (0x801D5946 = 4): the 2 player Battle's record, both streams in the split screen
                LoadReplayRace(disc, vol, file, data, ro);
                ApplyIntroHold(disc, data, ro);
                SplitRaceConfig split;
                split.view = v;
                split.view.sponsorSeed = file.SponsorSeed(); // race block + 0x54
                split.view.sponsorSeedGiven = true;
                split.replay = &file;
                split.startLeaves = true; // argument 1 (0x800A9500): Start leaves, no pause menu
                std::printf("attract: playing '%s' (%s, 2 player Battle, %zu car(s))\n", title.c_str(), file.CourseName().c_str(), file.cars.size());
                const SplitRaceResult r = RunSplitRace(window, nullptr, disc, vol, data, ro, split);
                std::printf("attract: replay left after %d steps\n", r.steps);
                return r.exit != RaceExit::kClosed && !window.Closed();
            }
        LoadReplayRace(disc, vol, file, data, ro);
        cars = data.params.size();
        v.replay = &file;
        v.sponsorSeed = file.SponsorSeed(); // race block + 0x54
        v.sponsorSeedGiven = true;
        if (!hudModeGiven && file.GameMode() == 3) v.hudMode = 3;
        std::printf("attract: playing '%s' (%s, game mode %u, %zu car(s))\n", title.c_str(), file.CourseName().c_str(), unsigned(payload.GameMode()), cars);
    }
    ApplyIntroHold(disc, data, ro);
    const RaceViewResult r = RunRaceView(window, nullptr, disc, vol, data, cars, ro, v, nullptr);
    std::printf("attract: replay left after %d steps (%s)\n", r.steps, r.exit == RaceExit::kQuit || r.exit == RaceExit::kExited ? "Start" : r.exit == RaceExit::kFinished ? "end of the stream" : "closed");
    return r.exit != RaceExit::kClosed && !window.Closed();
}

} // namespace gt2game
