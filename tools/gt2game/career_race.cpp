// GT-mode career races: headless (--career) and from the menus (see career_race.h).
#include "career_race.h"

#include "platform/os/keys.h"
#include "platform/os/paths.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "game/career/career_state.h"
#include "game/career/championship.h"
#include "game/career/events.h"
#include "game/career/garage.h"
#include "game/career/machine_test.h"
#include "game/career/results.h"
#include "game/career/tuning.h"
#include "arcade_race.h"
#include "game_window.h"
#include "gt2formats/car_info.h"
#include "gt2formats/car_texture.h"
#include "gt2formats/course_data.h"
#include "gt2formats/gtmode_tables.h"
#include "gt2formats/license_data.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/replay_card.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "game/shell/title_options.h"
#include "game/shell/title_screens.h"
#include "gt2formats/title_assets.h"
#include "game/shell/title_replay.h"
#include "gt2view/machine_test_views.h"
#include "gt2view/race_card_screens.h"
#include "gt2view/race_menu_views.h"
#include "gt2view/race_record_screens.h"
#include "gt2view/race_session_screens.h"
#include "gt2view/race_result_screens.h"
#include "gt2view/title_view.h"
#include "panel.h"
#include "settings_screen.h"

using namespace gt2;

namespace gt2game {

namespace {

// Everything the GT-mode menus prepare before the race overlay runs an event (ovl4 0x80013628 -> 0x80013108).
struct EventPlan {
    std::string name;
    RaceEvent event;
    career::SeriesBlock series{};
    bool championship = false;
    career::PrizeBlock prizes{};
    std::vector<career::GridCar> grid;
    std::string firstCourse;
    std::array<uint8_t, kLicenseSettingsSize> settings{}; // 0x801C98A0: the event row's block, kept for the whole series
    std::array<uint8_t, 6> pointsTable{};
    int32_t resultIndex = 0;
    int32_t races = 1;
    bool dirt = false;
    uint32_t seed = 0;
    std::vector<uint8_t> scratch = std::vector<uint8_t>(0x400, 0);
};

const char* EntryText(int32_t entry) {
    static const char* const kEntry[] = {"", "no current car", "dirt tyres needed", "licence", "drive train", "aspiration", "racing modification rule", "power limit",
                                         "not in the car list", "series not won", "series not won"};
    return entry == 1 ? "admitted" : (entry <= -1 && entry >= -10) ? kEntry[-entry] : "refused";
}

// The entry check of the menus with the current car's tune sheet (0x800173E8 + 0x8001973C).
int32_t CheckEntry(const career::CareerData& data, const career::EventMenuData& menu, const career::EventInfoTable& infos, const career::CareerState& s,
                   const std::string& name, uint32_t& message) {
    const career::GarageBlock& g = s.garage;
    if (g.currentCar < 0) return -1;
    auto sheet = std::make_unique<career::TuneSheet>();
    career::LoadCarSheet(*sheet, g.cars[g.currentCar], data.tables);
    return career::EntryCheck(s, g, menu, infos, name, career::EntryCarStateOf(*sheet), message);
}

// The menus before the race (0x80013628): day + 1, the series (0x80018A84), the prizes (0x80018C8C, of race 1 of a
// series), then the race block (0x80013108: opponents, course, the car's race tyres 0x80018004).
std::unique_ptr<EventPlan> PrepareEvent(const DiscImage& disc, const career::CareerData& data, const career::EventMenuData& menu, career::CareerState& s,
                                        const std::string& name, uint32_t seed) {
    using namespace gt2::career;
    auto plan = std::make_unique<EventPlan>();
    plan->name = name;
    plan->seed = seed;
    const int32_t row = data.race.FindEvent(name);
    if (row < 0) throw std::runtime_error("no event " + name + " in carparam/usa_gtmode_race.dat");
    if (MachineTestMode(menu, name) >= 0) throw std::runtime_error(name + " is a machine test (0x80012C6C), not an event race");
    plan->event = data.race.EventAt(size_t(row));
    const RaceEvent& event = plan->event;
    GarageBlock& g = s.garage;
    if (g.currentCar < 0) throw std::runtime_error("the career has no current car (buy one first)");
    std::printf("event %s (row %zu, menu index %d): course %s, %u lap(s), %zu opponent slot(s), licence %u, power limit %u, prizes %u/%u/%u/%u/%u/%u, prize cars %zu\n",
                event.name.c_str(), event.row, MenuEventIndex(menu, event.name), event.course.c_str(), event.Laps(), event.OpponentCount(), event.LicenceRequired(),
                event.powerLimit, event.prize[0], event.prize[1], event.prize[2], event.prize[3], event.prize[4], event.prize[5],
                size_t(std::count_if(event.prizeCars.begin(), event.prizeCars.end(), [](uint32_t id) { return id != 0; })));
    AdvanceDay(s);
    BuildSeries(plan->series, name, data, menu, seed);
    plan->championship = plan->series.count >= 2;
    const RaceEvent prizeEvent = data.race.EventAt(size_t(data.race.FindEvent(SeriesPrizeEvent(name))));
    auto purchaseSheet = std::make_unique<TuneSheet>();
    PreparePrizes(plan->prizes, prizeEvent, seed, data, *purchaseSheet, BuildScratch{plan->scratch.data()});
    if (plan->championship) {
        std::printf("championship %s: %d races, bonus %d cr, prize cars %d\n", SeriesBase(name).c_str(), plan->series.count, plan->prizes.bonus, plan->prizes.prizeCarCount);
        for (int32_t i = 0; i < plan->series.count; i++) std::printf("  race %d: %s, %u lap(s)\n", i + 1, plan->series.names[i], plan->series.laps[i]);
    }
    plan->dirt = event.DirtTyresRequired() != 0; // 0x80019538: the rules' dirt bit -> a one-car rally (0x80013108)
    uint32_t pick = seed;
    plan->grid = PickEventOpponents(data, event, s.language, pick);
    if (plan->dirt) plan->grid.resize(1);
    plan->firstCourse = EventCourse(menu, event, seed);
    auto sheet = std::make_unique<TuneSheet>();
    PrepareRaceTyres(g, g.currentCar, plan->dirt, *sheet, data, BuildScratch{plan->scratch.data()}); // 0x80018004
    const career::GarageCar& player = g.cars[g.currentCar];
    plan->grid[0].carId = player.modelId; // 0x80011000: the player's car in slot 0
    plan->grid[0].paint = char(player.paint);
    plan->grid[0].config = player.config;
    plan->grid[0].config.flags = uint8_t(plan->grid[0].config.flags | 0x40);
    plan->grid[0].opponent = 0;
    std::printf("grid (seed 0x%08X):\n", seed);
    for (size_t i = 0; i < plan->grid.size(); i++)
        std::printf("  slot %zu %-6s paint '%c'%s  %s\n", i, UnpackCarId(plan->grid[i].carId).c_str(), plan->grid[i].paint ? plan->grid[i].paint : '?', i == 0 ? "  (player)" : "",
                    i == 0 ? "" : ("opponent " + std::to_string(plan->grid[i].opponent)).c_str());
    std::copy(event.settings.begin(), event.settings.end(), plan->settings.begin());
    const GuestImage overlay = LoadOverlayImage(disc, kRaceOverlayIndex);
    for (uint32_t i = 0; i < 6; i++) plan->pointsTable[i] = overlay.Get<uint8_t>(0x8002F4CCu + i);
    plan->resultIndex = MenuEventIndex(menu, event.name);
    plan->races = plan->championship ? plan->series.count : 1;
    return plan;
}

// 0x80017A28 between the races: course 0x8005E590(series id), laps, name 0x8005E548.
std::string RaceCourse(const GtfsVolume& vol, const EventPlan& p, int32_t race) { return race == 0 ? p.firstCourse : CourseNameOfId(vol, p.series.courseId[race]); }
uint8_t RaceLaps(const EventPlan& p, int32_t race) { return race == 0 ? p.event.Laps() : uint8_t(p.series.laps[race]); }

uint32_t PaintIndexOf(const career::CareerData& data, uint32_t carId, char paint) { // 0x80018350
    const CarInfoRecord* r = data.cars.Find(carId);
    if (!r) return 0;
    const int i = r->PaintIndex(uint8_t(paint));
    return i < 0 ? 0u : uint32_t(i);
}

// Display names for the panels: the .crsinfo entry's course name, the .carinfoa name of a car.
std::string CourseDisplayName(const GtfsVolume& vol, const RaceData& rd) {
    const CourseInfoTable info = ParseCourseInfo(vol.Read(".crsinfo"));
    if (rd.courseIndex >= 0 && size_t(rd.courseIndex) < info.entries.size() && !info.entries[size_t(rd.courseIndex)].name.empty())
        return info.entries[size_t(rd.courseIndex)].name;
    return rd.trackName;
}
std::string CarName(const career::CareerData& data, uint32_t carId) {
    const CarInfoRecord* r = data.cars.Find(carId);
    std::string name = r ? r->name : UnpackCarId(carId);
    for (char& c : name)
        if (uint8_t(c) >= 0x80) c = 'e'; // the .carinfoa's Latin-1 accents (Protege) are not in the menu font's 8-bit map
    return name;
}

// The race data of race `race`: the course with the event's settings block, the grid's cars.
void BuildEventRace(const DiscImage& disc, const GtfsVolume& vol, const career::CareerData& data, const EventPlan& p, int32_t race, RaceData& out) {
    DataOptions options;
    options.eventSettings = &p.settings;
    LoadRaceTrack(disc, vol, RaceCourse(vol, p, race), options, out);
    // The race overlay's "Start Race" (0x800178B0 -> 0x8001710C(loop, 1, 2)) runs every event race in game mode 2 (race block
    // + 0xA): the AI catch-up of 0x8003EBF0 by the event row's settings (0x8003C12C -> 0x80041E4C, enabled in modes 2 / 4 / 0xC),
    // the rear-view mirror (0x800294D4: mode != 0). Captured: CBM0001 race block + 0xA = 2 (work/re/testrun/ev_race.bin.setup;
    // `--frames-compare` of that capture's race in mode 2: 1630 frames, 0 differ).
    out.constants.gameMode = 2;
    out.constants.catchUp = sim::CatchUpFromSettings(p.settings, true);
    for (const career::GridCar& car : p.grid) AddCarRecord(vol, data.tables, car.carId, car.config, PaintIndexOf(data, car.carId, car.paint), out);
    // ovl4 0x80013108, the six-car path: entry i = 0x80012F7C(block + 0x5C + i * 0xD0, grid slot 5 - i) {+0x8C 1, +0x8D 5 - i,
    // +0x8E 1, +0x8F 0}; then 0x80011000(block, 0, -1, 3, ..) makes entry 0 the player (kind 3, the grid slot kept): the player
    // starts last (captured: CBM0001 race block entries 01050300 / 00040100 .. 00000100). The dirt path (one car, a ghost file
    // 0x80019BF0) keeps its default here.
    if (!p.dirt && p.grid.size() == 6) {
        out.gridSlots.clear(); // the kinds (player 3, AI 1) are RaceData's defaults; +0x8F the transmission of --manual / the keys
        for (size_t i = 0; i < 6; i++) out.gridSlots.push_back(uint8_t(5 - i));
    }
}

struct EventRaceOutcome {
    bool finished = false;
    int32_t position = 0;       // the player's finishing position (0x801D5DE8), 0 = did not finish
    int32_t orderPosition = 0;  // the player's place in the race order at the end
    std::vector<int32_t> positions; // all cars at the player's finish
    int steps = 0;
};

// One race of an event on the ported simulation, headless (the player's entry driven by the AI).
EventRaceOutcome RunHeadlessRace(const RaceData& race, size_t carCount, RaceOptions raceOptions, double seconds) {
    raceOptions.aiPlayer = true; // headless: the player's entry is driven by the AI (class 2)
    sim::RaceSim raceSim;
    SetupRace(raceSim, race, carCount, raceOptions);
    ShellLog log;
    log.quiet = true;
    log.Attach(raceSim);
    std::vector<sim::PadRecord> pads(carCount);
    EventRaceOutcome r;
    for (; r.steps < int(seconds * 30); r.steps++) {
        raceSim.Step(pads.data());
        bool allFinished = true;
        for (size_t car = 0; car < carCount; car++) allFinished &= raceSim.CarAt(car).body.finishFlag != 0;
        if (allFinished || raceSim.RaceTaskOver()) break;
    }
    PrintStandings(raceSim, log, race.carIds);
    r.position = raceSim.Shell().State().finishPosition;
    r.finished = r.position > 0;
    for (size_t i = 0; i < carCount; i++)
        if (raceSim.RaceOrder()[i] == 0) r.orderPosition = int32_t(i + 1);
    r.positions = log.positionsAtPlayerFinish;
    return r;
}

// The results of race `race` (0x80059A7C for a single event; 0x80013824 + 0x8005E67C points and 0x80059704 for a
// championship race); returns the lines of the result panel (also printed). `shown` (optional) receives the outcome
// for the post-race views (unset for a championship race the player did not finish).
std::vector<std::string> ApplyEventRace(EventPlan& p, career::CareerState& s, int32_t race, const EventRaceOutcome& r, uint32_t clock,
                                        std::optional<career::RaceOutcome>* shown = nullptr) {
    using namespace gt2::career;
    std::vector<std::string> lines;
    char text[200];
    GarageBlock& g = s.garage;
    if (!p.championship) {
        int32_t position = r.position;
        if (position == 0) {
            position = r.orderPosition;
            std::printf("the player did not finish in %d steps: position %d from the race order\n", r.steps, position);
        }
        const RaceOutcome outcome = ApplyRaceResult(s.record, g, p.prizes, position, p.resultIndex, clock); // 0x80059A7C
        if (shown) *shown = outcome;
        std::snprintf(text, sizeof text, "result: position %d, prize %d cr%s; result entry %d = %d", outcome.position, outcome.prize,
                      outcome.prizeCar >= 0 ? (std::string(", prize car ") + UnpackCarId(p.prizes.prizeCars[outcome.prizeCar].carId) + (outcome.prizeCarAdded ? " added" : " (garage full)")).c_str() : "",
                      p.resultIndex, ResultAt(s.record, p.resultIndex));
        std::printf("%s\n", text);
        std::snprintf(text, sizeof text, "Position  %d", outcome.position);
        lines.push_back(text);
        std::snprintf(text, sizeof text, "Prize  %s cr", MenuThousands(uint32_t(outcome.prize)).c_str());
        lines.push_back(text);
        if (outcome.prizeCar >= 0)
            lines.push_back("Prize car  " + UnpackCarId(p.prizes.prizeCars[outcome.prizeCar].carId) + (outcome.prizeCarAdded ? "" : " (garage full)"));
        return lines;
    }
    // Championship race: the points at the player's finish (0x80013824 + 0x8005E67C) and the per-race payout
    // 0x80059704 when the player finished (0x80017200: 0x801D5E88 > 0).
    if (r.position > 0) {
        SetRacePoints(p.series, r.positions, p.pointsTable);
        AddRacePoints(p.series);
        const RaceOutcome outcome = ApplyChampionshipRace(s.record, g, p.prizes, r.position);
        if (shown) *shown = outcome;
        std::printf("race %d: position %d, prize %d cr\n", race + 1, outcome.position, outcome.prize);
        std::snprintf(text, sizeof text, "Race %d of %d  -  position %d", race + 1, p.races, outcome.position);
        lines.push_back(text);
        std::snprintf(text, sizeof text, "Prize  %s cr", MenuThousands(uint32_t(outcome.prize)).c_str());
        lines.push_back(text);
    } else {
        std::fill(std::begin(p.series.pointsRace), std::end(p.series.pointsRace), int8_t(0));
        std::printf("race %d: the player did not finish (no points, no prize)\n", race + 1);
        lines.push_back("Did not finish: no points, no prize");
    }
    std::printf("points:");
    std::string points = "Points ";
    for (size_t car = 0; car < p.grid.size(); car++) {
        std::printf(" %s %d (+%d)%s", UnpackCarId(p.grid[car].carId).c_str(), p.series.pointsTotal[car], p.series.pointsRace[car], car == 0 ? " [player]" : "");
        points += " " + std::to_string(p.series.pointsTotal[car]);
    }
    std::printf("\n");
    lines.push_back(points + "  (you first)");
    p.series.race = int16_t(race + 1);
    return lines;
}

// After the last race of a championship: 0x80017A28 -> 0x8005E6B0 order, result entry, 0x80059800 for a champion.
std::vector<std::string> FinishEvent(EventPlan& p, career::CareerState& s, const career::EventMenuData& menu, uint32_t clock,
                                     career::ChampionshipEnd* shown = nullptr) {
    using namespace gt2::career;
    std::vector<std::string> lines;
    if (!p.championship) return lines;
    const std::string prefix = p.name.substr(0, std::min<size_t>(3, p.name.size()));
    const bool gtw = prefix == menu.prefixes[2]; // 0x8001859C -> 0x801D5DDC
    const ChampionshipEnd end = FinishChampionship(s, p.series, p.prizes, p.resultIndex, gtw, clock);
    if (shown) *shown = end;
    char text[200];
    std::printf("championship result: place %d%s; result entry %d = %d", end.place, end.champion ? " (champion)" : "", p.resultIndex, ResultAt(s.record, p.resultIndex));
    if (end.champion)
        std::printf("; bonus %d cr%s", end.prize.prize,
                    end.prize.prizeCar >= 0 ? (std::string(", prize car ") + UnpackCarId(p.prizes.prizeCars[end.prize.prizeCar].carId) + (end.prize.prizeCarAdded ? " added" : " (garage full)")).c_str() : "");
    std::printf("\n");
    std::snprintf(text, sizeof text, "Championship place  %d%s", end.place, end.champion ? " - CHAMPION" : "");
    lines.push_back(text);
    if (end.champion) {
        std::snprintf(text, sizeof text, "Bonus  %s cr", MenuThousands(uint32_t(end.prize.prize)).c_str());
        lines.push_back(text);
        if (end.prize.prizeCar >= 0)
            lines.push_back("Prize car  " + UnpackCarId(p.prizes.prizeCars[end.prize.prizeCar].carId) + (end.prize.prizeCarAdded ? "" : " (garage full)"));
    }
    return lines;
}

// The whole event headless on `s` (the player's entry driven by the AI).
int RunEventHeadless(const DiscImage& disc, const GtfsVolume& vol, const career::CareerData& data, const career::EventMenuData& menu, career::CareerState& s,
                     const std::string& name, uint32_t seed, RaceOptions raceOptions, double seconds) {
    std::unique_ptr<EventPlan> plan = PrepareEvent(disc, data, menu, s, name, seed);
    uint32_t clock = seed; // the VSync counter the result routines read (here: seed + race steps)
    for (int32_t race = 0; race < plan->races; race++) {
        if (plan->championship) std::printf("--- race %d of %d: %s\n", race + 1, plan->races, plan->series.names[race]);
        RaceData rd;
        BuildEventRace(disc, vol, data, *plan, race, rd);
        RaceOptions ro = raceOptions;
        ro.laps = RaceLaps(*plan, race) ? RaceLaps(*plan, race) : ro.laps;
        std::printf("%s: %d m, %u lap(s), %zu car(s)%s\n", rd.trackName.c_str(), rd.track.lengthMetres, ro.laps, plan->grid.size(),
                    plan->settings[0] ? (", rolling start at " + std::to_string(plan->settings[0]) + " km/h").c_str() : "");
        const EventRaceOutcome r = RunHeadlessRace(rd, plan->grid.size(), ro, seconds);
        clock += uint32_t(r.steps);
        ApplyEventRace(*plan, s, race, r, clock);
    }
    FinishEvent(*plan, s, menu, clock);
    return 0;
}

// A panel alone (no race behind it) until Enter / Esc.
[[maybe_unused]] bool ShowPanel(GameWindow& window, Panels& panels, const Panels::Menu& menu, bool autoAdvance) {
    int frames = 0;
    window.Renderer().clearColor[0] = window.Renderer().clearColor[1] = window.Renderer().clearColor[2] = 0.0f;
    panels.Upload();
    while (window.BeginFrame()) {
        if (window.Pressed(gt2::keys::kReturn) || window.Pressed(gt2::keys::kEscape) || (autoAdvance && ++frames >= 60)) return true;
        std::vector<gt2view::DrawItem> items;
        panels.Clear();
        panels.DrawMenu(menu, 70);
        panels.Append(window.Renderer().AspectRatio(), items);
        window.EndFrame(items);
    }
    return false;
}

// The pad words of the race overlay's views (0x80083998's copy: pressed since the last read, auto-repeat pulses) from
// the keyboard / script, for the licence record screens (see career_race.h for the keys).
class RecordScreenPad {
public:
    MenuListPad Read(const GameWindow& window) {
        namespace pb = menu_list_pad;
        static const std::pair<int, uint32_t> kKeys[] = {
            {gt2::keys::kUp, pb::kUp}, {gt2::keys::kDown, pb::kDown}, {gt2::keys::kLeft, pb::kLeft}, {gt2::keys::kRight, pb::kRight}, {gt2::keys::kReturn, pb::kCross}, {gt2::keys::kSpace, pb::kCircle},
            {gt2::keys::kBack, pb::kTriangle}, {gt2::keys::kDelete, pb::kSquare}, {'Q', pb::kL1}, {gt2::keys::kPageUp, pb::kL1}, {'W', pb::kR1}, {gt2::keys::kPageDown, pb::kR1},
            {gt2::keys::kHome, pb::kStart}, {'S', pb::kStart}};
        MenuListPad pad;
        for (const auto& [key, bit] : kKeys) {
            if (window.Held(key)) pad.held |= bit;
            if (window.Pressed(key)) pad.pressed |= bit;
        }
        constexpr uint32_t kRepeating = pb::kUp | pb::kDown | pb::kLeft | pb::kRight | pb::kL1 | pb::kR1 | pb::kTriangle | pb::kSquare;
        const uint32_t held = pad.held & kRepeating;
        if (held && held == (previous_ & held)) {
            if (++timer_ >= 20 && (timer_ - 20) % 5 == 0) pad.repeat = held;
        } else {
            timer_ = 0;
        }
        previous_ = pad.held;
        return pad;
    }

private:
    uint32_t previous_ = 0;
    int timer_ = 0;
};

// One modal frame of a race-overlay screen (black around the 352 x 480 frame).
void PresentRecordFrame(GameWindow& window, Panels& panels, std::vector<MenuPrim> prims) {
    std::vector<gt2view::DrawItem> items;
    panels.Clear();
    panels.FullScreen(std::move(prims), Panels::Screen::kLicence);
    panels.Append(window.Renderer().AspectRatio(), items);
    window.EndFrame(items);
}

// The lap entry the race shell keeps of a passed licence test (0x801D5E90: the time as lap 1; our race view hands over
// the time only - the splits and the max-speed readout are not available here, so they stay "none" / 0).
career::TimeRecord LicenceLapRecord(uint32_t time) {
    career::TimeRecord t;
    career::InitTimeRecord(t);
    t.time[0] = int32_t(time);
    return t;
}

// SAVE GAME (view 0x8005B588: setup 0x80072F9C(0) = the executable's card manager in save mode, update 0x800728F0, draw
// 0x80072B78; left with sound 3 when 0x801C90C4 names the next view): the title's port of that manager (game/shell
// CardManager) on the cards of the options, drawn with the title's VRAM on the panel rows (the header is the title's;
// the race overlay's own header 0x80047024 "SAVE GAME" is not composed with it). Returns true when the career was written.
bool RunSaveGameScreen(GameWindow& window, Panels& panels, const DiscImage& disc, const GtfsVolume& vol, career::CareerState& state, const std::string& card1,
                       const std::string& card2) {
    const TitleAssets assets = TitleAssets::Load(disc, vol);
    gt2view::TitleView view(window.Renderer(), gt2view::PanelView::kRowBase);
    view.UploadVram(assets.vram);
    shell::CardManager card(assets, shell::CardManager::kSaveGame, state, {shell::CardSlot{card1}, shell::CardSlot{card2}});
    RecordScreenPad input;
    bool saved = false;
    while (window.BeginFrame()) {
        MenuListPad pad = input.Read(window);
        if (window.Pressed(gt2::keys::kEscape)) pad.pressed |= menu_list_pad::kTriangle;
        const bool running = card.Update(&pad);
        panels.Sounds(card.sounds);
        panels.SoundFrame();
        for (const std::string& line : card.log) std::printf("save game: %s\n", line.c_str());
        card.log.clear();
        saved = saved || card.Saved();
        if (!running) break;
        std::vector<gt2view::DrawItem> items;
        view.Build(card.Frame(), TitleAssets::kScreenWidth, window.Renderer().AspectRatio(), items);
        window.EndFrame(items);
    }
    panels.Upload(); // the panel rows held the title's VRAM
    std::printf("save game: %s (f%d)\n", saved ? "the career was written to the card" : "not saved", window.Field());
    return saved;
}

// SAVE REPLAY (view 0x8005B51C: init 0x8005019C -> EXE 0x80072E7C, the card manager in mode 0 with the replay packed by
// 0x800724F8 = 0x80069948 and described; update 0x800501BC, draw 0x8005021C; gt2view/race_card_screens.h), pushed by the licence
// menu's row 4 (code -2, 0x8004EEB0; + 0x80050494 the manager's fonts) / the event menu's row 3 (0x80058108) / the post-race
// menu's "Save Replay ..." (0x8004A0BC); the manager's exit goes back (2) to the menu. The payload as RAM holds it then: the race
// block and slots 0x801D585C (0x58C: ours the race block and slots, the rest zero), the player's parameter record 0x801DE8BA,
// the results record 0x801D5E88 and the stream object 0x801D5F84. Drawn on the menu's assets (`screen`); the cards of the
// options. Returns true when the replay was written. The view manager of the original (0x800483A4 push, 0x800483D8 pop, 16 fields
// each with both views drawn shifted): `menu` (lent by the caller: the licence / event menu, the post-race menu) slides out as the
// card view slides in; at the manager's exit `resetup` (the menu's setup(1)) runs and the card view slides out as the menu comes
// back; `carId` / `paint` = the menu's 3D car (0x80048754) when it has one.
struct LentMenu {
    screens::PostRaceView* view = nullptr;
    std::function<void()> resetup;
    uint32_t carId = 0;
    int paint = 0;
};
bool RunSaveReplayScreen(GameWindow& window, Panels& panels, const DiscImage& disc, const GtfsVolume& vol, Panels::Screen screen, const ReplayFile& replay,
                         const sim::CarParams& params, const sim::PlayerResults& results, const std::string& card1, const std::string& card2,
                         const std::array<uint8_t, 0x10>* raceTail = nullptr, const LentMenu& menu = {}) {
    const RaceMenuAssets& a = panels.MenuAssets(screen);
    const TitleAssets cardAssets = screens::RaceCardAssets(a, TitleAssets::Load(disc, vol));
    const shell::ReplayRowText text = shell::ReplayRowText::Load(vol, cardAssets);
    ReplayPayload payload = PayloadOfReplay(replay, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&params), sizeof params),
                                            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&results), sizeof results));
    if (raceTail) std::copy(raceTail->begin(), raceTail->end(), payload.race.begin() + 0x57C); // 0x801D5DD8..0x801D5DE7
    auto view = std::make_unique<screens::CardView>(a, cardAssets, text, screens::CardView::kSaveReplayView,
                                                    std::array<shell::CardSlot, 2>{shell::CardSlot{card1}, shell::CardSlot{card2}});
    view->Manager().SetSaveData(PackReplayPayload(payload), ReplayDescription(payload));
    screens::CardView* card = view.get();
    screens::SessionViewStack stack; // the view manager: the card view enters (16 fields) like a pushed view
    if (menu.view) {
        stack.Start(std::make_unique<screens::BorrowedView>(*menu.view), false);
        stack.Push(std::move(view));
    } else {
        stack.Start(std::move(view), true);
    }
    bool leaving = false; // the card view's exit: the pop's transition back to the menu
    float clear[3];
    std::copy(std::begin(window.Renderer().clearColor), std::end(window.Renderer().clearColor), clear);
    window.Renderer().clearColor[0] = window.Renderer().clearColor[1] = window.Renderer().clearColor[2] = 0.0f;
    RecordScreenPad input;
    bool saved = false;
    std::printf("save replay: f%d (%zu payload bytes)\n", window.Field(), PackReplayPayload(payload).size());
    while (window.BeginFrame()) {
        MenuListPad pad = input.Read(window);
        if (window.Pressed(gt2::keys::kEscape)) pad.pressed |= menu_list_pad::kTriangle;
        const int r = stack.Update(&pad);
        if (!leaving) {
            panels.Sounds(card->sounds);
            for (const std::string& line : card->Manager().log) std::printf("save replay: %s\n", line.c_str());
            card->Manager().log.clear();
            saved = saved || card->Manager().Saved();
        } else if (stack.Top()) {
            panels.Sounds(stack.Top()->sounds);
        }
        panels.SoundFrame();
        if (!leaving && r == 2) {
            if (!menu.view) break;
            if (menu.resetup) menu.resetup(); // 0x800483D8: the view below's setup(1)
            stack.Pop();
            leaving = true;
        }
        if (leaving && stack.Transition() == 0) break;
        size_t modelAt = 0;
        std::optional<screens::PostRaceModel> model;
        std::vector<gt2view::DrawItem> items;
        panels.Clear();
        std::vector<MenuPrim> frame = stack.Frame(a, modelAt, model);
        if (model && menu.carId != 0) panels.FullScreenModel(std::move(frame), screen, modelAt, model, menu.carId, menu.paint);
        else panels.FullScreen(std::move(frame), screen);
        panels.Append(window.Renderer().AspectRatio(), items);
        window.EndFrame(items);
    }
    std::copy(clear, clear + 3, window.Renderer().clearColor);
    std::printf("save replay: %s (f%d)\n", saved ? "the replay was written to the card" : "not saved", window.Field());
    return saved;
}

// The race block of a GT-mode race as the menus leave it in RAM (what Save Replay packs from 0x801D585C), over the one
// BuildReplayFile writes: + 1 .. + 7 = the career's option bytes + 2 .. + 8 (car damage, laps, tyre damage, 2P car damage, 2P
// laps, handicap, slow car boost - the menus' copies, menus_gtmode.md 10.3; + 4 is career + 5, not BuildReplayFile's 1). Both
// captured blocks (licence B-1 and event CBM0001 on work/memcards/gt2_save_1car.mcd) hold 00 02 01 00 02 00 01 there = that career.
void MenuRaceBlock(ReplayFile& f, const career::CareerState& s) {
    const uint8_t* career = reinterpret_cast<const uint8_t*>(&s);
    for (size_t k = 1; k <= 7; k++) f.raceBlock[k] = career[k + 1];
}

// Race block + 0x44 (0x801D58A0, 16 bytes): the sponsor category the menus copy from the event / licence test row + 0x94 (ovl4
// 0x80010078 and the event setups: strcpy of the row's name pool string, race block cleared before; + 9 = 1). Captured: LJB00
// "0", LJB06 / CBM0001 "General01", GT30501 "0", the attract's MSC0002 "General02".
void SetSponsorCategory(ReplayFile& f, const std::string& category) {
    std::fill(f.raceBlock.begin() + 0x44, f.raceBlock.begin() + 0x54, uint8_t(0));
    for (size_t k = 0; k < category.size() && k < 15; k++) f.raceBlock[0x44 + k] = uint8_t(category[k]);
}

// 0x800275E8's category of a race block: + 0x44 when + 9 != 0, else "General01" (0x8002F1B8).
std::string SponsorCategoryOf(const ReplayFile& f) {
    if (f.raceBlock[9] == 0) return "General01";
    std::string c;
    for (size_t k = 0x44; k < 0x54 && f.raceBlock[k] != 0; k++) c.push_back(char(f.raceBlock[k]));
    return c;
}

// Race block + 0x57C .. + 0x58B (0x801D5DD8..) as the menus leave it: s16 result index, u8 + 0x580 the GTW flag (0x8001859C),
// s16 + 0x582 player / + 0x584 garage slot (0x801D5DDE / 0x801D5DE0), s16 + 0x586 = the event info's power limit (0x800194FC:
// info + 0x1E), u32 + 0x588 flags: bit 0 = !(info + 0x1C & 1) (0x80019578), bits 1..2 = info + 0x1C bits 7..8 when 1 or 2
// (0x800195BC), bit 3 = the garage car's + 0x98 bit 14 (the race overlay's entry 0x80011F64). A licence test (ovl4 0x80010078):
// - 1, -1, -1, flags 1. Captured: LJB00 / LJB06 "ffff0000 0000ffff ffff0000 01000000", CBM0001 "16000000 00000000 00000000
// 01000000", GT30501 "77000000 00000000 01005802 01000000" (work/re/*/ram*.bin, card_b1_replay.mcd).
std::array<uint8_t, 0x10> LicenceRaceTail() {
    return {0xFF, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 1, 0, 0, 0};
}
std::array<uint8_t, 0x10> EventRaceTail(int32_t resultIndex, bool gtw, int16_t garageSlot, const career::EventInfo& info, bool gearboxFlag) {
    std::array<uint8_t, 0x10> t{};
    auto put16 = [&](size_t o, int32_t v) {
        t[o] = uint8_t(v & 0xFF);
        t[o + 1] = uint8_t((v >> 8) & 0xFF);
    };
    put16(0, resultIndex);
    t[4] = gtw ? 1 : 0;
    put16(6, 0);
    put16(8, garageSlot);
    put16(0xA, info.powerLimit);
    const uint32_t kind = (info.rules & 0x180u) >> 7;
    t[0xC] = uint8_t(((info.rules & 1u) ^ 1u) | ((kind == 1 || kind == 2) ? kind << 1 : 0u) | (gearboxFlag ? 8u : 0u));
    return t;
}

// The name of test `t` (0..9) of the licence of the test name `any` ("LJB00" -> "LJB0t": the formats 0x8005B1F0[licence]
// "LIS%02d" .. "LJB%02d" of 0x8004E320 / 0x8004E494).
std::string LicenceTestName(const std::string& any, int t) {
    char digits[8];
    std::snprintf(digits, sizeof digits, "%02d", t);
    return any.substr(0, 3) + digits;
}

// The index of a paint character in the car's .cdp paint list (0 when it is not there).
int CarPaintIndex(const GtfsVolume& vol, uint32_t carId, uint8_t paint) {
    try {
        const int index = ParseCarTexture(vol.Read("carobj/" + UnpackCarId(carId) + ".cdp")).PaintIndex(paint);
        return index < 0 ? 0 : index;
    } catch (const std::exception&) {
        return 0;
    }
}

// ---- the race overlay's post-race views (gt2view/race_result_screens.h) after an event race

// What the views show: RESULTS (0x80050FD0), the prize money (0x80059A7C / 0x80059704 / 0x80059800; none = skipped, as
// for a championship race the player did not finish: 0x80017200 needs 0x801D5E88 > 0) and the post-race menu
// (0x80049D90; none = skipped, e.g. after the championship end).
struct PostRaceShow {
    std::optional<screens::ResultsInput> results;
    std::optional<screens::BonusInput> bonus;
    std::optional<screens::PostMenuInput> menu;
    uint32_t carId = 0; // car 0's model (race slot 0x801D58B8) and paint index, for the 3D car of RESULTS / the menu
    int paint = 0;
    std::function<void()> saveGame; // "Save" of a bar: SAVE GAME (0x8005B588), then the chain goes on as after "Next"
    // The menu's "Save Replay ..." (code -2 -> the card manager's view 0x8005B51C; back to the menu, its setup with 1); empty =
    // the row is drawn disabled. "Replay" (code 0) ends the views with kReplay (state 11; the caller plays it and runs the views
    // again from the menu's wait view, `results` / `bonus` unset: 0x8001728C with + 0x5D1 = 0).
    std::function<void(const LentMenu& menu)> saveReplay;
    bool replay = false; // the race's replay exists (the Replay row)
};
enum class PostRaceChoice { kContinue, kExit, kClosed, kReplay };

// Runs the views with the view manager's switches (0x800474F4): the wait views between them (pre-results 0x8005B7A0
// 24 fields, pre-bonus 0x8005D558 / 0x8005D4C8 / 0x8005D510 16, pre-menu 0x8005AE0C 16, leave 0x8005AE30 20) draw only
// the header. Pad from the window (RecordScreenPad; Esc = triangle). "Save" of the bars runs SAVE GAME (show.saveGame), the
// menu's "Save Replay ..." the card manager (show.saveReplay) and back to the menu; "Replay" returns kReplay. Returns kExit
// when the post-race menu's Exit was confirmed ("Exit?" -> Yes). `autoAdvance` (--auto-race, a
// dev aid of automated runs): cross every 40 fields, down while the menu's selected row is disabled.
PostRaceChoice RunPostRaceViews(GameWindow& window, Panels& panels, const PostRaceShow& show, bool autoAdvance) {
    const RaceMenuAssets& a = panels.MenuAssets(Panels::Screen::kSettings);
    enum class Stage { kWaitResults, kResults, kWaitBonus, kBonus, kWaitMenu, kMenu, kLeave };
    screens::PostRaceFlow flow;
    Stage stage = Stage::kWaitResults;
    if (!show.results && !show.bonus && show.menu) { // after the menu's Replay: 0x8001728C with + 0x5D1 = 0 pushes the menu's wait view
        stage = Stage::kWaitMenu;
        flow.Start(std::make_unique<screens::WaitView>(a, 0x8005AE0Cu, 16), false);
    } else {
        flow.Start(std::make_unique<screens::WaitView>(a, 0x8005B7A0u, 24), false);
    }
    PostRaceChoice choice = PostRaceChoice::kContinue;
    auto makeMenu = [&](bool first) { // 0x80049D90(first ? 0 : 1): the rows 0x8005AD98 / 0x8005AD78 by the race's state
        auto v = std::make_unique<screens::PostRaceMenuView>(a);
        screens::PostMenuInput in = *show.menu;
        in.firstEntry = first;
        v->Setup(in);
        for (screens::PostRaceMenuView::Row& row : v->rows) {
            if (row.action == 0 && !show.replay) row.enabled = false;
            if (row.action == -2 && (!show.replay || !show.saveReplay)) row.enabled = false;
        }
        return v;
    };
    auto next = [&]() -> bool { // the current view was left: switch to the next one; false = done
        for (;;) {
            switch (stage) {
            case Stage::kWaitResults:
                stage = Stage::kResults;
                if (!show.results) continue;
                {
                    auto v = std::make_unique<screens::ResultsView>(a);
                    v->Setup(*show.results);
                    flow.Switch(std::move(v));
                }
                return true;
            case Stage::kResults:
                if (auto* v = dynamic_cast<screens::ResultsView*>(flow.Current()); v && v->SaveChosen() && show.saveGame) show.saveGame();
                stage = Stage::kWaitBonus;
                if (!show.bonus) continue;
                flow.Switch(std::make_unique<screens::WaitView>(a, show.bonus->kind == screens::BonusKind::kSingleRace ? 0x8005D558u
                                                                   : show.bonus->kind == screens::BonusKind::kChampionshipRace ? 0x8005D4C8u
                                                                                                                                : 0x8005D510u,
                                                                16));
                return true;
            case Stage::kWaitBonus:
                stage = Stage::kBonus;
                if (!show.bonus) continue;
                {
                    auto v = std::make_unique<screens::BonusView>(a);
                    v->Setup(*show.bonus);
                    flow.Switch(std::move(v));
                }
                return true;
            case Stage::kBonus:
                if (auto* v = dynamic_cast<screens::BonusView*>(flow.Current()); v && v->SaveChosen() && show.saveGame) show.saveGame();
                stage = Stage::kWaitMenu;
                if (!show.menu) {
                    stage = Stage::kLeave;
                    flow.Switch(std::make_unique<screens::WaitView>(a, 0x8005AE30u, 20));
                    return true;
                }
                flow.Switch(std::make_unique<screens::WaitView>(a, 0x8005AE0Cu, 16));
                return true;
            case Stage::kWaitMenu:
                stage = Stage::kMenu;
                if (!show.menu) continue;
                flow.Switch(makeMenu(true));
                return true;
            case Stage::kMenu:
                if (auto* v = dynamic_cast<screens::PostRaceMenuView*>(flow.Current())) {
                    if (v->Action() == -2 && show.saveReplay) { // "Save Replay ...": the card manager, then back to the menu
                        LentMenu lent;
                        lent.view = v;
                        lent.carId = show.carId, lent.paint = show.paint;
                        lent.resetup = [&] {
                            screens::PostMenuInput in = *show.menu; // back (code 2): 0x80049D90(1) - the same view, its list kept
                            in.firstEntry = false;
                            v->Setup(in);
                            for (screens::PostRaceMenuView::Row& row : v->rows) {
                                if (row.action == 0 && !show.replay) row.enabled = false;
                                if (row.action == -2 && (!show.replay || !show.saveReplay)) row.enabled = false;
                            }
                        };
                        show.saveReplay(lent);
                        return true;
                    }
                    // M+0x7C: 0 Replay -> state 11; 2 Exit (the dialog's Yes); else Continue / Next Session
                    choice = v->Action() == 0 ? PostRaceChoice::kReplay : v->Action() == 2 ? PostRaceChoice::kExit : PostRaceChoice::kContinue;
                }
                stage = Stage::kLeave;
                flow.Switch(std::make_unique<screens::WaitView>(a, 0x8005AE30u, 20));
                return true;
            case Stage::kLeave:
                return false;
            }
        }
    };
    float clear[3];
    std::copy(std::begin(window.Renderer().clearColor), std::end(window.Renderer().clearColor), clear);
    window.Renderer().clearColor[0] = window.Renderer().clearColor[1] = window.Renderer().clearColor[2] = 0.0f;
    RecordScreenPad input;
    bool closed = true;
    int frames = 0;
    while (window.BeginFrame()) {
        MenuListPad pad = input.Read(window);
        if (window.Pressed(gt2::keys::kEscape)) pad.pressed |= menu_list_pad::kTriangle;
        if (autoAdvance && ++frames % 40 == 0) {
            const auto* menu = dynamic_cast<const screens::PostRaceMenuView*>(flow.Current());
            const bool disabledRow = menu && menu->list.selection >= 0 && size_t(menu->list.selection) < menu->rows.size() &&
                                     (!menu->rows[size_t(menu->list.selection)].enabled || menu->rows[size_t(menu->list.selection)].action == 0 ||
                                      menu->rows[size_t(menu->list.selection)].action == -2); // the dev aid goes on: not Replay / Save Replay
            pad.pressed |= disabledRow ? menu_list_pad::kDown : menu_list_pad::kCross;
        }
        const int left = flow.Update(&pad);
        if (flow.Current()) panels.Sounds(flow.Current()->sounds); // the view's 0x80060840 requests of this field
        panels.SoundFrame();
        if (left == 1 && !next()) {
            closed = false;
            break;
        }
        std::vector<gt2view::DrawItem> items;
        panels.Clear();
        size_t modelAt = 0;
        std::optional<screens::PostRaceModel> model;
        std::vector<MenuPrim> frame = flow.Frame(a, modelAt, model); // with the 3D car / trophy (0x80048754)
        panels.FullScreenModel(std::move(frame), Panels::Screen::kSettings, modelAt, model, show.carId, show.paint);
        panels.Append(window.Renderer().AspectRatio(), items);
        window.EndFrame(items);
    }
    std::copy(clear, clear + 3, window.Renderer().clearColor);
    std::printf("post-race views: %s (f%d)\n",
                closed ? "window closed" : choice == PostRaceChoice::kExit ? "Exit" : choice == PostRaceChoice::kReplay ? "Replay" : "Continue", window.Field());
    return closed ? PostRaceChoice::kClosed : choice;
}

// ---- the GT-mode machine test (race sub-modes 7 / 8 / 9; docs/formats/race_screens.md 5.7)

// The results record's entry the machine test records (ovl0 0x80050D78): u32 0x801D5F58 (results + 0xD0: the kept lap's time =
// the 400 m / 1000 m time) and u16 0x801D5F68 (+ 0xE0: its max-speed readout).
struct MachineTestResult {
    uint32_t time = 0;
    uint16_t maxSpeed = 0;
};
MachineTestResult MachineTestResultOf(const sim::PlayerResults& r) { return {uint32_t(r.best.time), uint16_t(r.best.maxSpeed)}; }

// After the race of a test and its replay, 0x80017200 (sub-modes 7..9, M + 0x5D1 and the player's results record + 0 > 0): the wait
// view 0x8005B7A0 (24 fields; 0x80050EE4 -> 0x80050D78 writes the record, the music 0x800481C8(8)) -> NEW RECORD 0x8005B7E8 when the
// entry ranked (OK: 0x8005E03C stores the name) -> RESULTS 0x8005B80C ("Save": SAVE GAME 0x8005B588, then as "Next") -> the leave view
// 0x8005AE30 (20) -> the view 0x8005D348 (20 fields, 0x80057D24) that pushes the menu (the caller's). Returns false when the window
// was closed.
bool RunMachineTestViews(GameWindow& window, Panels& panels, career::CareerState& s, int subMode, uint32_t carId, int paint, const MachineTestResult& result,
                         const std::function<void()>& saveGame, bool autoAdvance) {
    using namespace gt2::career;
    const RaceMenuAssets& a = panels.MenuAssets(Panels::Screen::kSettings);
    enum class Stage { kWait, kNewRecord, kResults, kLeave, kEnter };
    Stage stage = Stage::kWait;
    screens::PostRaceFlow flow;
    flow.Start(std::make_unique<screens::WaitView>(a, 0x8005B7A0u, 24), false);
    MachineTestOutcome outcome;
    auto results = [&] {
        auto v = std::make_unique<screens::MachineResultsView>(a);
        screens::MachineResultsInput in;
        in.subMode = subMode;
        in.rank = outcome.rank;
        in.time = outcome.time;
        in.maxSpeed = outcome.maxSpeed;
        in.vsync = uint32_t(window.Field()); // 0x80050BC4 seeds the car's pose with the VSync counter
        v->Setup(in);
        return v;
    };
    auto next = [&]() -> bool {
        switch (stage) {
        case Stage::kWait: // 0x80050EE4 at the end of the wait: the record (0x80050D78), then NEW RECORD or RESULTS
            outcome = WriteMachineTestRecord(s, subMode, carId, result.time, result.maxSpeed);
            std::printf("machine test: record %s (0x80050D78: time %u, speed %u -> rank %d)\n", outcome.rank >= 0 ? "ranked" : "not ranked", outcome.time,
                        outcome.maxSpeed, int(outcome.rank));
            if (outcome.rank >= 0) {
                auto v = std::make_unique<screens::MachineNewRecordView>(a);
                v->Setup(EnteredName(s)); // 0x80051F54: the name buffer 0x801D156F
                stage = Stage::kNewRecord;
                flow.Switch(std::move(v));
            } else {
                stage = Stage::kResults;
                flow.Switch(results());
            }
            return true;
        case Stage::kNewRecord:
            if (auto* v = dynamic_cast<screens::MachineNewRecordView*>(flow.Current())) { // OK: 0x8005E03C(record, W + 2, 0x801D156F)
                SetEnteredName(s, v->keyboard.name);
                StoreMachineTestName(*MachineTestRecordOf(s, subMode), outcome.rank, v->keyboard.name);
                std::printf("machine test: new record %d \"%s\" (0x8005E03C)\n", int(outcome.rank) + 1, v->keyboard.name.c_str());
            }
            stage = Stage::kResults;
            flow.Switch(results());
            return true;
        case Stage::kResults:
            if (auto* v = dynamic_cast<screens::MachineResultsView*>(flow.Current()); v && v->SaveChosen() && saveGame) saveGame();
            stage = Stage::kLeave;
            flow.Switch(std::make_unique<screens::WaitView>(a, 0x8005AE30u, 20));
            return true;
        case Stage::kLeave:
            stage = Stage::kEnter;
            flow.Switch(std::make_unique<screens::WaitView>(a, 0x8005D348u, 20));
            return true;
        case Stage::kEnter:
            return false;
        }
        return false;
    };
    float clear[3];
    std::copy(std::begin(window.Renderer().clearColor), std::end(window.Renderer().clearColor), clear);
    window.Renderer().clearColor[0] = window.Renderer().clearColor[1] = window.Renderer().clearColor[2] = 0.0f;
    RecordScreenPad input;
    bool closed = true;
    int frames = 0;
    bool autoStart = false;
    while (window.BeginFrame()) {
        MenuListPad pad = input.Read(window);
        if (window.Pressed(gt2::keys::kEscape)) pad.pressed |= menu_list_pad::kTriangle;
        if (autoAdvance && ++frames % 20 == 0) { // the dev aid: NEW RECORD start (to OK) first, then cross; the others cross
            auto* record = stage == Stage::kNewRecord ? dynamic_cast<screens::MachineNewRecordView*>(flow.Current()) : nullptr;
            if (record && record->delay > 0) {
            } else if (record && !autoStart) pad.pressed |= menu_list_pad::kStart, autoStart = true;
            else if (frames % 40 == 0) pad.pressed |= menu_list_pad::kCross;
        }
        const int left = flow.Update(&pad);
        if (flow.Current()) panels.Sounds(flow.Current()->sounds);
        panels.SoundFrame();
        if (left == 1 && !next()) {
            closed = false;
            break;
        }
        std::vector<gt2view::DrawItem> items;
        panels.Clear();
        size_t modelAt = 0;
        std::optional<screens::PostRaceModel> model;
        std::vector<MenuPrim> frame = flow.Frame(a, modelAt, model); // RESULTS' 3D car (0x80048754)
        panels.FullScreenModel(std::move(frame), Panels::Screen::kSettings, modelAt, model, carId, paint);
        panels.Append(window.Renderer().AspectRatio(), items);
        window.EndFrame(items);
    }
    std::copy(clear, clear + 3, window.Renderer().clearColor);
    return !closed;
}

// The machine test from the menus (ovl4 0x80013628 with a machine-test name: 0x8001861C -> 0x80012C6C; the day counter stays) and the
// race overlay's loop of sub-modes 7..9 (0x80017500 / 0x80017784 / 0x80017A28): the machine-test menu, the race (one car, one lap
// of TC_lisence (400 m / 1000 m marks, race_shell.cpp LapCheck) or maxspeed), its replay at once, the record views, the menu again.
bool RunMachineTest(const DiscImage& disc, const GtfsVolume& vol, career::CareerState& s, const career::CareerData& data, const career::EventMenuData& menu,
                    const std::string& name, MenuRaceContext& ctx) {
    using namespace gt2::career;
    const EventInfoTable infos = BuildEventInfos(data, menu);
    const CourseInfoTable courses = ParseCourseInfo(vol.Read(".crsinfo"));
    auto sheet = std::make_unique<TuneSheet>();
    std::vector<uint8_t> scratch(0x400, 0);
    MachineTestRace m = PrepareMachineTest(s, data, menu, infos, courses, name, *sheet, scratch.data());
    career::GarageCar& car = s.garage.cars[s.garage.currentCar];
    // The race overlay's entry 0x80011F64: race block + 0x588 bit 3 = the garage car's + 0x98 bit 14 (a gearbox of fewer than 3 gears).
    if (car.powerFlags & 0x4000) m.raceBlock[0x588] = uint8_t(m.raceBlock[0x588] | 8);
    std::printf("machine test %s (sub-mode %d): course %s, car %s, tag \"%s\"%s\n", name.c_str(), m.subMode, m.course.c_str(), UnpackCarId(car.modelId).c_str(),
                m.tag.c_str(), m.dirt ? ", dirt tyres" : "");
    auto build = [&](RaceData& rd, RaceOptions& ro) {
        m.raceBlock[0x58] = uint8_t(car.wordA2 & 0xFF), m.raceBlock[0x59] = uint8_t(car.wordA2 >> 8); // 0x80017784: + 0x58 = the car's + 0xA2
        LoadRaceBlock(disc, vol, m.raceBlock, m.settings, m.course, ro, rd);
    };
    RaceData rd;
    RaceOptions ro = ctx.race;
    build(rd, ro);
    MachineTestRecord& record = *MachineTestRecordOf(s, m.subMode);
    if (!ctx.window) { // headless: the player's entry driven by the AI (class 2), the record written as after the race
        sim::RaceSim race;
        RaceOptions aro = ro;
        aro.aiPlayer = true;
        SetupRace(race, rd, 1, aro);
        std::vector<sim::PadRecord> pads(1);
        for (int step = 0, since = 0; step < int(ctx.headlessSeconds * 30) && !race.RaceTaskOver(); step++) {
            since = race.CarAt(0).body.finishFlag != 0 ? since + 1 : 0;
            const uint32_t buttons[2] = {since > 0 && since % 30 == 0 ? 0x200u : 0u, 0}; // X for the results wait (0x8002A700)
            race.Step(pads.data(), buttons);
        }
        const sim::RaceShellState& st = race.Shell().State();
        car.wordA2 = uint16_t(int32_t(race.CarAt(0).body.dirtiness << 12) / 600000); // 0x800131AC / 0x80017964
        if (st.results[0].position > 0) {
            const MachineTestResult mr = MachineTestResultOf(st.results[0]);
            const MachineTestOutcome o = WriteMachineTestRecord(s, m.subMode, car.modelId, mr.time, mr.maxSpeed); // the NEW RECORD name needs the window
            std::printf("machine test (headless): time %s, max speed %u -> rank %d (0x80050D78)\n", FormatMs(int32_t(mr.time)).c_str(), unsigned(mr.maxSpeed), int(o.rank));
        } else {
            std::printf("machine test (headless): not finished\n");
        }
        return true;
    }
    SettingsScreen settings(car, data);
    settings.SetRaceLimits(int16_t(m.raceBlock[0x586] | m.raceBlock[0x587] << 8), uint16_t(m.raceBlock[0x588] | m.raceBlock[0x589] << 8)); // CHANGE PARTS
    RaceViewConfig view = ctx.view;
    view.hudMode = m.subMode;
    view.sponsorCategory = m.tag; // race block + 0x44 with + 9 = 1 ("0": no boards)
    view.machineRecord = MachineTestEntryValue(record, 0);
    RaceFlow flow;
    flow.title = name;
    flow.machineTest = m.subMode;
    flow.raceEndMode = m.subMode;
    flow.carNames = {CarName(data, car.modelId)};
    flow.settings = &settings;
    flow.settingsChanged = [&] { // 0x80056FF0 wrote the garage slot; the race slot's configuration follows (flags | 0x40)
        CarConfig config = car.config;
        uint8_t* const cfg = reinterpret_cast<uint8_t*>(&config);
        cfg[0x7A] = uint8_t(cfg[0x7A] | 0x40);
        rd.params[0] = BuildCarParams(vol, data.tables, config);
        rd.bodies[0] = CarBodyOf(vol, data.tables, config);
        rd.sound[0].soundId = config.engineWord;
        rd.sound[0].exhaustByte = config.exhaustByte;
        rd.sound[0].turbo = (config.flags & 2) != 0;
        std::memcpy(m.raceBlock.data() + 0x5C + 8, &config, sizeof(config));
    };
    flow.transmission = &s.garage.byte401A;                       // 0x801D156E: the TRANSMISSION bar of Start / Try Again (0x800589BC)
    flow.transmissionDialog = (m.raceBlock[0x588] & 8) == 0;      // race block + 0x588 bit 3
    flow.menuCarId = car.modelId;                                 // M + 0x241: race slot 0's car
    const int paint = CarPaintIndex(vol, car.modelId, uint8_t(car.paint));
    flow.menuCarPaint = paint;
    const RaceMenuAssets& menuAssets = ctx.panels->MenuAssets(Panels::Screen::kSettings);
    flow.recordsView = [&]() -> std::unique_ptr<screens::PostRaceView> { // "Records ..." (0x8005D3B4) over the names of 0x80169894
        auto v = std::make_unique<screens::MachineRecordsView>(menuAssets, record, m.carNames, m.subMode);
        v->Setup();
        return v;
    };
    std::array<uint8_t, 0x10> tail{};
    std::copy(m.raceBlock.begin() + 0x57C, m.raceBlock.begin() + 0x58C, tail.begin());
    flow.saveReplay = [&](const ReplayFile& replay, const sim::PlayerResults& results) { // "Save Replay ..." (code -2, view 0x8005B51C)
        ReplayFile f = replay;
        MenuRaceBlock(f, s);
        SetSponsorCategory(f, m.tag);
        std::copy(m.raceBlock.begin() + 0x0B, m.raceBlock.begin() + 0x0D, f.raceBlock.begin() + 0x0B); // + 0xB = 5, + 0xC = 0
        std::copy(m.raceBlock.begin() + 0x10, m.raceBlock.begin() + 0x20, f.raceBlock.begin() + 0x10); // the event name
        LentMenu lent;
        lent.view = flow.lentMenu;
        lent.resetup = flow.resetupMenu;
        lent.carId = flow.menuCarId, lent.paint = flow.menuCarPaint;
        RunSaveReplayScreen(*ctx.window, *ctx.panels, disc, vol, Panels::Screen::kSettings, f, rd.params[0], results, ctx.card1Path, ctx.card2Path, &tail, lent);
    };
    flow.raceEnded = [&](const RaceViewResult& r) { // 0x80017964: race block + 0x582 = 0, + 0x584 = the garage index (both >= 0)
        car.wordA2 = r.playerDirt;
        rd.dirtLevel = r.playerDirt; // the next race's + 0x58 (0x80017784)
    };
    flow.finished = [&](const RaceViewResult& r) {
        std::vector<std::string> lines;
        if (r.playerResults.position <= 0) return lines; // 0x80017200: the record views need 0x801D5E88 > 0
        const MachineTestResult mr = MachineTestResultOf(r.playerResults);
        lines.push_back("Time " + FormatMs(int32_t(mr.time)));
        if (ctx.panels && !ctx.window->Closed())
            RunMachineTestViews(*ctx.window, *ctx.panels, s, m.subMode, car.modelId, paint, mr,
                                [&] { RunSaveGameScreen(*ctx.window, *ctx.panels, disc, vol, s, ctx.card1Path, ctx.card2Path); }, ctx.view.autoAdvance);
        else
            WriteMachineTestRecord(s, m.subMode, car.modelId, mr.time, mr.maxSpeed);
        view.machineRecord = MachineTestEntryValue(record, 0); // the HUD's Record of the next run (0x8002D20C reads the career)
        return lines;
    };
    const RaceViewResult r = RunRaceView(*ctx.window, ctx.panels, disc, vol, rd, 1, ro, view, &flow);
    std::printf("machine test %s: the view ended (%s)\n", name.c_str(), r.exit == RaceExit::kClosed ? "window closed" : "Exit");
    return true;
}

} // namespace

ReplayFile LicenceDemoReplay(const DiscImage& disc, const GtfsVolume& vol, int licence, int test, std::vector<uint8_t>* params) {
    if (licence < 0 || licence > 5 || test < 0 || test > 9) throw std::runtime_error("licence demo: no such test");
    const GuestImage exe = LoadExeImage(disc);
    std::string path; // 0x80010228: the path list 0x8009118C, entry 225 -> u16 0x801E2EF0 + 225 * 2 = 0x801E30B2
    for (uint32_t a = exe.Get<uint32_t>(0x8009118Cu + 225u * 4u); exe.Get<uint8_t>(a) != 0 && path.size() < 64; a++) path.push_back(char(exe.Get<uint8_t>(a)));
    if (!path.empty() && path[0] == '/') path.erase(0, 1);
    const GtfsEntry* first = vol.Find(path);
    if (!first) throw std::runtime_error("licence demo: no " + path + " in GT2.VOL");
    const uint32_t number = uint32_t(int32_t(first->index) + int32_t(exe.Get<int8_t>(0x80091C8Cu + uint32_t(licence))) * 10 + test); // 0x80069EF8
    const GtfsEntry* entry = nullptr;
    for (const GtfsEntry& e : vol.Files())
        if (e.index == number) entry = &e;
    if (!entry) throw std::runtime_error("licence demo: no VOL file " + std::to_string(number));
    const std::vector<uint8_t> bytes = vol.Read(*entry);
    constexpr size_t kStream = 0x801D5F84u - 0x801D585Cu, kParams = 0x801DA49Cu - 0x801D585Cu;
    if (bytes.size() < kParams + 0x1C0) throw std::runtime_error("licence demo: short file " + entry->path);
    ReplayFile r;
    std::copy(bytes.begin(), bytes.begin() + kReplayRaceBlockSize, r.raceBlock.begin());
    r.raceBlock[9] = 1; // 0x80069F28: 0x801D5865 = 1
    if (r.CarCount() > kReplayCars) throw std::runtime_error("licence demo: car count above 6");
    for (size_t i = 0; i < r.CarCount(); i++) {
        ReplayEntry e;
        std::copy(bytes.begin() + std::ptrdiff_t(kReplayRaceBlockSize + i * kReplayCarStride), bytes.begin() + std::ptrdiff_t(kReplayRaceBlockSize + (i + 1) * kReplayCarStride),
                  e.slot.begin());
        std::memcpy(&e.carId, e.slot.data(), 4);
        r.cars.push_back(e);
    }
    const size_t used = size_t(bytes[kStream + 0x10] | bytes[kStream + 0x11] << 8);
    if (kStream + 0x19 + used > kParams) throw std::runtime_error("licence demo: stream overruns the file");
    r.stream.assign(bytes.begin() + std::ptrdiff_t(kStream), bytes.begin() + std::ptrdiff_t(kStream + 0x19 + used));
    if (params) params->assign(bytes.begin() + std::ptrdiff_t(kParams), bytes.begin() + std::ptrdiff_t(kParams + 0x1C0));
    std::printf("licence demo: %s (VOL %u), %s, %d frames\n", entry->path.c_str(), number, r.EventName().c_str(), ReplayStream::FromBytes(r.stream).Frames());
    return r;
}

bool ParseLicenceLabel(const std::string& label, int& licence, int& test) {
    static const char* const kNames[] = {"S", "IA", "IB", "IC", "A", "B"};
    const size_t dash = label.find('-');
    if (dash == std::string::npos) return false;
    const std::string l = label.substr(0, dash);
    for (int i = 0; i < 6; i++)
        if (l == kNames[i]) {
            licence = i;
            test = std::atoi(label.c_str() + dash + 1) - 1;
            return test >= 0 && test <= 9;
        }
    return false;
}

bool RunNewRecordScreen(GameWindow& window, Panels& panels, std::string& name) {
    const RaceMenuAssets& assets = panels.MenuAssets(Panels::Screen::kLicence);
    screens::NewRecordView view(assets); // the race overlay's effects (0x80060840) are not played here: no player in this path
    view.Enter(name);
    float clear[3];
    std::copy(std::begin(window.Renderer().clearColor), std::end(window.Renderer().clearColor), clear);
    window.Renderer().clearColor[0] = window.Renderer().clearColor[1] = window.Renderer().clearColor[2] = 0.0f;
    RecordScreenPad input;
    bool ok = false;
    while (window.BeginFrame()) {
        const MenuListPad pad = input.Read(window);
        if (view.Update(&pad)) {
            name = view.keyboard.name;
            ok = true;
            PresentRecordFrame(window, panels, screens::BuildNewRecordFrame(assets, view.keyboard));
            break;
        }
        PresentRecordFrame(window, panels, screens::BuildNewRecordFrame(assets, view.keyboard));
    }
    std::copy(clear, clear + 3, window.Renderer().clearColor);
    std::printf("new record: %s \"%s\" (f%d)\n", ok ? "OK" : "window closed", view.keyboard.name.c_str(), window.Field());
    return ok;
}

void RunLicenceRecordsScreen(GameWindow& window, Panels& panels, const career::CareerState& career, int licence, int test) {
    const RaceMenuAssets& assets = panels.MenuAssets(Panels::Screen::kLicence);
    screens::RecordsView view(assets);
    view.Enter(licence, test);
    float clear[3];
    std::copy(std::begin(window.Renderer().clearColor), std::end(window.Renderer().clearColor), clear);
    window.Renderer().clearColor[0] = window.Renderer().clearColor[1] = window.Renderer().clearColor[2] = 0.0f;
    RecordScreenPad input;
    while (window.BeginFrame()) {
        MenuListPad pad = input.Read(window);
        if (window.Pressed(gt2::keys::kEscape)) pad.pressed |= menu_list_pad::kTriangle;
        if (view.Update(&pad)) break;
        PresentRecordFrame(window, panels, screens::BuildRecordsFrame(assets, view.State(career)));
    }
    std::copy(clear, clear + 3, window.Renderer().clearColor);
}

int RunCareerEvent(const DiscImage& disc, const GtfsVolume& vol, const CareerOptions& co, RaceOptions raceOptions, double seconds) {
    using namespace gt2::career;
    const std::vector<uint8_t> inputBytes = ReadFileBytes(co.savePath);
    CareerSave save = LoadCareer(co.savePath);
    const bool inputIsCard = inputBytes.size() == 128 * 1024 && inputBytes[0] == 'M' && inputBytes[1] == 'C';
    std::printf("career %s: CRC %08X (%s); %s\n", co.savePath.c_str(), save.storedCrc, save.CrcOk() ? "ok" : "MISMATCH", DescribeCareer(save.state).c_str());
    { // round trip of the unchanged career: the same bytes as read
        std::vector<uint8_t> again;
        if (inputIsCard) {
            again = inputBytes;
            StoreCareerOnCard(again, save);
        } else {
            again = BuildCareerSaveFile(save);
        }
        std::printf("save round trip (unchanged career): %s\n", again == inputBytes ? "byte-identical" : "DIFFERS");
    }
    const CareerData data = CareerData::Load(disc, vol);
    const EventMenuData menu = EventMenuData::Load(data.ovl4);
    const EventInfoTable infos = BuildEventInfos(data, menu);
    const CareerState before = save.state;
    if (MachineTestMode(menu, co.eventName) >= 0) { // a machine test (no entry check: the map's page offers it to any current car)
        MenuRaceContext ctx;
        ctx.race = raceOptions;
        ctx.headlessSeconds = seconds;
        ctx.seed = co.seed;
        RunMachineTest(disc, vol, save.state, data, menu, co.eventName, ctx);
    } else {
    uint32_t message = 0;
    const int32_t entry = CheckEntry(data, menu, infos, save.state, co.eventName, message);
    std::printf("entry check (0x8001973C): %d %s (message 0x%08X)\n", entry, EntryText(entry), message);
    if (entry != 1 && !co.force) return 1;
    RunEventHeadless(disc, vol, data, menu, save.state, co.eventName, co.seed, raceOptions, seconds);
    }
    std::printf("career before: %s\ncareer after:  %s\n", DescribeCareer(before).c_str(), DescribeCareer(save.state).c_str());
    if (!co.saveOut.empty()) {
        SaveCareer(co.saveOut, save, inputIsCard ? std::span<const uint8_t>(inputBytes) : std::span<const uint8_t>());
        const CareerSave reread = LoadCareer(co.saveOut);
        const bool same = std::memcmp(&reread.state, &save.state, sizeof(CareerState)) == 0;
        std::printf("saved %s: CRC %08X (%s), reloaded state %s\n", co.saveOut.c_str(), reread.storedCrc, reread.CrcOk() ? "ok" : "MISMATCH", same ? "identical" : "DIFFERS");
        if (!same || !reread.CrcOk()) return 1;
    }
    return 0;
}

bool RunMenuRace(const DiscImage& disc, const GtfsVolume& vol, career::CareerSave& save, const std::string& name, int racePath, MenuRaceContext& ctx) {
    using namespace gt2::career;
    CareerState& s = save.state;
    const CareerData data = CareerData::Load(disc, vol);
    const EventMenuData menu = EventMenuData::Load(data.ovl4);
    std::printf("career before: %s\n", DescribeCareer(s).c_str());
    struct SoundScope { // the race overlay screens' effects while this race runs (Panels::Sound), closed for the menus
        Panels* panels;
        SoundScope(Panels* p, bool on) : panels(p) {
            if (panels) panels->SetSound(on);
        }
        ~SoundScope() {
            if (panels) panels->CloseSound();
        }
    } soundScope(ctx.panels, ctx.view.sound && ctx.window != nullptr);
    // The race options of THIS career (+0xAE replay info, +0xAF camera position, +0xB0 chase view, +0xB1 course map, +0xB2
    // view angle, +0xB3 / +0xB4 volumes): the race overlay reads the career block (0x801C98E0 + offset), not the settings
    // file the process started with - a change on the title's OPTIONS page reaches the next race of the menus.
    const shell::PcSettings* startupSettings = ctx.view.pcSettings;
    shell::PcSettings raceSettings = startupSettings ? *startupSettings : shell::PcSettings{};
    raceSettings.options = shell::ReadGameOptions(s);
    raceSettings.haveCareerOptions = true;
    ctx.view.pcSettings = &raceSettings;
    struct RestoreSettings { // ctx outlives this call: do not leave it pointing at the local
        MenuRaceContext& c;
        const shell::PcSettings* previous;
        ~RestoreSettings() { c.view.pcSettings = previous; }
    } restoreSettings{ctx, startupSettings};

    // ---- licence tests (names starting with 'L'; the menus checked the entry with 0x80019B88)
    if (!name.empty() && name[0] == 'L') {
        const LicenseData lic = LicenseData::Load(vol);
        AdvanceDay(s); // 0x80013628: licence tests count as races for the day counter (retries / other tests of the overlay do not)
        // The race overlay's licence menu can switch to another test of the licence (its selector, row 0): the race of that
        // test is built like the first (0x8004E320 -> 0x8004C7A0 = ovl4 0x80010078's code) and started at once.
        std::string testName = name;
        bool startAtOnce = false;
        for (;;) {
        const LicenseTest test = lic.Test(testName);
        const int32_t licence = LicenceOfTest(menu, testName);
        int32_t testIndex = 0;
        if (!LicenceTestOfName(testName, testIndex)) throw std::runtime_error("licence test name " + testName + " has no test number");
        int nextTest = -1; // the selector's test when the view ended with it (kChangeTest / a demonstration of it)
        static const char* const kLicenceNames[] = {"S", "IA", "IB", "IC", "A", "B"};
        std::printf("licence test %s: licence %s (race block +0x0B = %d), test %d (+0x0C), course %s, car %s, type %u\n", testName.c_str(), kLicenceNames[licence], licence, testIndex,
                    test.course.c_str(), UnpackCarId(lic.RaceCar(test).carId).c_str(), test.Type());
        // 0x80013864: the menus prepare the prize block before a licence test with 0x80018C8C(block, name): for a licence
        // name (0x80018608) 0x80018C14 finds the licence of the prefix and the format of ovl4 0x80050C1C with 0 names its
        // FIRST test ("LJB00"), whose row (+0x78 prizes, +0x84 prize cars, +0x98 bonus: the event row layout) builds the
        // block like an event's (seed: the VSync counter). 0x8004DF04 gives prize car 0 when the licence becomes all gold.
        PrizeBlock licencePrizes{};
        {
            const LicenseTest first = lic.Test(testName.substr(0, 3) + "00");
            const RaceEvent row = ParseRaceEvent(first.bytes, lic.Names(), first.row);
            auto prizeSheet = std::make_unique<TuneSheet>();
            std::vector<uint8_t> prizeScratch(0x400, 0);
            PreparePrizes(licencePrizes, row, ctx.seed, data, *prizeSheet, BuildScratch{prizeScratch.data()});
            if (licencePrizes.prizeCarCount > 0)
                std::printf("licence %s: all-gold prize car %s (row %s +0x84)\n", kLicenceNames[licence], UnpackCarId(licencePrizes.prizeCars[0].modelId).c_str(), first.name.c_str());
        }
        RaceData rd;
        DataOptions options;
        options.license = &test;
        LoadRaceTrack(disc, vol, test.course, options, rd);
        BuildLicenseCar(vol, lic, test, rd);
        RaceOptions ro = ctx.race;
        ro.laps = 255; // the licence dump's lap count (0x801D586B)
        ro.license = &test;
        const std::string label = std::string(kLicenceNames[licence]) + "-" + std::to_string(testIndex + 1);
        auto recordResult = [&](int32_t result, uint32_t time) {
            std::vector<std::string> lines;
            static const char* const kFail[] = {"", "", "", "Failed - past the end of the box", "Failed - off the course", "Failed - hit the wall"};
            if (result == sim::kLicenseResultPass) lines.push_back("Passed  " + FormatMs(int32_t(time)));
            else lines.push_back(result >= 3 && result <= 5 ? kFail[result] : "Failed");
            lines.push_back("Gold " + FormatMs(int32_t(test.MedalTime(1))) + "  Silver " + FormatMs(int32_t(test.MedalTime(2))) + "  Bronze " + FormatMs(int32_t(test.MedalTime(3))));
            if (result != sim::kLicenseResultPass) return lines;
            const bool heldBefore = LicenceHeld(s, licence), goldBefore = LicenceAllGold(s, licence);
            const int32_t prize = RecordLicenceResult(s, licence, testIndex, time, test.settings.data()); // ovl0 0x8004DD80
            static const char* const kPrize[] = {"no prize", "passed (fourth prize)", "BRONZE", "SILVER", "GOLD"};
            const LicenceTestRecord& rec = s.licences[licence][testIndex];
            std::printf("licence %s: prize %d (%s), career record +1 = %d, +2 = %u (0x8004DD80)\n", label.c_str(), prize, kPrize[std::clamp(prize, 0, 4)], int(int8_t(rec.passed)),
                        rec.byte2);
            lines.push_back(std::string("Prize  ") + kPrize[std::clamp(prize, 0, 4)]);
            if (!heldBefore && LicenceHeld(s, licence)) lines.push_back(std::string("Licence ") + kLicenceNames[licence] + " obtained!");
            if (!goldBefore && LicenceAllGold(s, licence)) {
                // 0x8004E104 -> 0x8004DF04: prize block car 0 (0x801D55CC) into the garage (0x8005E7F0; full garage: no car).
                const int32_t added = AddPreparedCar(s.garage, licencePrizes.prizeCars[0]);
                lines.push_back(added ? "All gold! Prize car added to the garage" : "All gold! (the garage is full: no prize car)");
                std::printf("licence %s: all tests gold - prize car %s %s (0x8004DF04)\n", kLicenceNames[licence], UnpackCarId(licencePrizes.prizeCars[0].modelId).c_str(),
                            added ? "added" : "not added (garage full)");
            }
            return lines;
        };
        if (!ctx.window) { // headless: the stand-in test driver
            sim::RaceSim race;
            SetupRace(race, rd, 1, ro);
            std::vector<sim::PadRecord> pads(1);
            for (int step = 0; step < int(ctx.headlessSeconds * 30); step++) {
                pads[0] = ro.aiPlayer ? sim::PadRecord{} : LicenseDriverPad(race, test, ro.licenseDecel);
                race.Step(pads.data());
                if (race.CarAt(0).body.finishFlag != 0) break;
            }
            const sim::RaceShellState& st = race.Shell().State();
            for (const std::string& line : recordResult(st.licenseResult, st.licenseTime)) std::printf("  %s\n", line.c_str());
            if (st.licenseResult == sim::kLicenseResultPass) { // 0x8004E104 -> 0x8005DE8C; the name entry needs the window
                const int32_t rank = LicenceRecordRank(s.licences[licence][testIndex], LicenceLapRecord(st.licenseTime));
                if (rank >= 0) std::printf("licence %s: the time ranks %d of 5 (NEW RECORD) - headless: no name entry, not stored\n", label.c_str(), rank + 1);
            }
        } else {
            RaceFlow flow;
            flow.title = "Licence " + label + " (" + testName + ")";
            flow.licence = true;
            flow.raceEndMode = 3; // 0x801D5866 of a licence test
            for (uint32_t k = 0; k < 4; k++) flow.medalTimes[k] = test.MedalTime(k + 1); // 0x8003D7B8(record + 0x44, 1..4)
            { // 0x8002B170: a fourth prize counts for a never-passed test whose fourth-prize count reaches record + 0x74
                const LicenceTestRecord& rec = s.licences[licence][testIndex];
                flow.fourthPrizeCounts = int8_t(rec.passed) == 0 && uint32_t(rec.byte2) + 1 == uint32_t(test.settings[0x30]);
            }
            flow.carNames = {CarName(data, lic.RaceCar(test).carId)};
            if (ctx.panels) { // the race overlay's licence test menu (0x8004F474)
                // The menu of test t (0x8004CCF8: license_info_us; 0x8004CA90: the test record's launch speed and medal times;
                // 0x8004D7D0: the test's car of the menus' table 0x801DA4B8 + t * 0x44 + licence * 0x2A8: name, power, drive).
                auto menuOfTest = [&, licence](int t) {
                    gt2::screens::LicenceMenuState m = gt2::screens::LicenceMenuDefaults(ctx.panels->MenuAssets(Panels::Screen::kLicence), licence, t);
                    const LicenseTest other = t == testIndex ? test : lic.Test(LicenceTestName(testName, t));
                    RaceData carData;
                    if (t == testIndex) carData.params = rd.params;
                    else BuildLicenseCar(vol, lic, other, carData);
                    m.carName = CarName(data, lic.RaceCar(other).carId);
                    std::vector<uint8_t> figures(0x6C, 0);
                    m.carPower = int16_t(career::CarPowerFigures(carData.params[0], figures.data())); // max power (0x80075328), shown * 1000 / 0x3F6
                    m.carDrive = int16_t(carData.params[0].driveType);
                    m.launchSpeed = other.settings[0];
                    for (uint32_t k = 0; k < 3; k++) m.medalTimes[k] = other.MedalTime(k + 1);
                    for (size_t k = 0; k < 10; k++) m.medals[k] = uint8_t(s.licences[licence][k].passed);
                    return m;
                };
                flow.licenceMenu = menuOfTest(testIndex);
                flow.licenceMenuOfTest = menuOfTest;
                // "Records ..." (row 3): the RECORD view of the selector's test (0x8004FDC4: block + 0x4B8; left / right = the
                // other tests of the licence).
                flow.records = [&, licence] { RunLicenceRecordsScreen(*ctx.window, *ctx.panels, s, licence, flow.licenceMenu ? flow.licenceMenu->test : testIndex); };
                flow.transmission = &s.garage.byte401A; // 0x801D156E: the TRANSMISSION bar of Start (0x8004EEB0)
                flow.startAtOnce = startAtOnce;
            }
            flow.info.push_back(CourseDisplayName(vol, rd) + "  -  " + CarName(data, lic.RaceCar(test).carId));
            flow.info.push_back("Gold " + FormatMs(int32_t(test.MedalTime(1))) + "  Silver " + FormatMs(int32_t(test.MedalTime(2))) + "  Bronze " +
                                FormatMs(int32_t(test.MedalTime(3))));
            static const char* const kPrize[] = {"-", "passed", "bronze", "silver", "gold"};
            const int8_t held = int8_t(s.licences[licence][testIndex].passed);
            flow.info.push_back(std::string("Your best  ") + (held >= 0 && held <= 4 ? kPrize[held] : "?"));
            flow.finished = [&](const RaceViewResult& r) {
                std::vector<std::string> lines = recordResult(r.licenseResult, r.licenseTime);
                if (r.licenseResult == sim::kLicenseResultPass) {
                    // 0x8004E104: a passed time that ranks among the test's five best (0x8005DE8C) opens NEW RECORD
                    // (0x8005B494); OK stores it with the entered name (0x8005DEFC). Ours: right after the race task.
                    const career::TimeRecord lap = LicenceLapRecord(r.licenseTime);
                    LicenceTestRecord& best = s.licences[licence][testIndex];
                    const int32_t rank = LicenceRecordRank(best, lap);
                    if (rank >= 0 && ctx.panels) {
                        std::string entered = EnteredName(s);
                        if (RunNewRecordScreen(*ctx.window, *ctx.panels, entered)) {
                            SetEnteredName(s, entered);
                            StoreLicenceRecord(best, lap, entered);
                            lines.push_back("New record  " + std::to_string(rank + 1) + ".  " + entered);
                            std::printf("licence %s: new record %d of 5 \"%s\" (0x8005DEFC)\n", label.c_str(), rank + 1, entered.c_str());
                        }
                    }
                    if (flow.licenceMenu)
                        for (size_t t = 0; t < 10; t++) flow.licenceMenu->medals[t] = uint8_t(s.licences[licence][t].passed);
                }
                const LicenceTestRecord& rec = s.licences[licence][testIndex]; // for the next try's display
                flow.fourthPrizeCounts = int8_t(rec.passed) == 0 && uint32_t(rec.byte2) + 1 == uint32_t(test.settings[0x30]);
                return lines;
            };
            if (ctx.panels) {
                // "Save Replay ..." (row 4, code -2): the card manager's view 0x8005B51C with the last run's replay.
                flow.saveReplay = [&](const ReplayFile& replay, const sim::PlayerResults& results) {
                    ReplayFile f = replay;
                    MenuRaceBlock(f, s);
                    SetSponsorCategory(f, test.tag);
                    f.raceBlock[0xB] = uint8_t(licence);   // 0x801D5867 (ovl4 0x80010078)
                    f.raceBlock[0xC] = uint8_t(testIndex); // 0x801D5868
                    if (!f.cars.empty()) std::fill(f.cars[0].slot.begin() + 0x90, f.cars[0].slot.end(), uint8_t(0)); // the licence car's slot has no name (captured)
                    const std::array<uint8_t, 0x10> tail = LicenceRaceTail();
                    LentMenu lent;
                    lent.view = flow.lentMenu;
                    lent.resetup = flow.resetupMenu;
                    RunSaveReplayScreen(*ctx.window, *ctx.panels, disc, vol, Panels::Screen::kLicence, f, rd.params[0], results, ctx.card1Path, ctx.card2Path, &tail, lent);
                };
                flow.demonstration = true; // row 5 (code -5): the test's demo run (LicenceDemoReplay)
            }
            RaceViewConfig view = ctx.view;
            view.hudMode = 3;
            view.sponsorCategory = test.tag; // race block + 0x44 with + 9 = 1 (0x80010078): 0x800275E8's category ("0": no boards)
            for (;;) {
                const RaceViewResult r = RunRaceView(*ctx.window, ctx.panels, disc, vol, rd, 1, ro, view, &flow);
                if (r.exit == RaceExit::kClosed) std::printf("licence %s: the window was closed\n", label.c_str());
                if (r.exit == RaceExit::kChangeTest) { // Start on another test of the selector: that test's race at once
                    nextTest = r.licenceTest;
                    startAtOnce = true;
                    std::printf("licence %s: the selector's test %d is started (0x8004E320 -> 0x8004C7A0)\n", label.c_str(), nextTest + 1);
                    break;
                }
                if (r.exit != RaceExit::kDemonstration) break;
                // 0x8004E494: the demo file into 0x801D585C (its parameter record to 0x801DE8BA), M+0x240 = 0, code 0 -> the replay
                // state; its end (or its pause's Exit) -> 0x80017A28 -> the licence menu again (the Replay row off: no race of ours).
                // The selector's test (block + 0x4B8): a race block of another test is built first (0x8004C7A0), so the menu
                // after the demonstration is that test's.
                const int demoTest = r.licenceTest >= 0 ? r.licenceTest : testIndex;
                ReplayFile demo = LicenceDemoReplay(disc, vol, licence, demoTest);
                RaceData demoData;
                RaceOptions demoOptions = ctx.race;
                LoadReplayRace(disc, vol, demo, demoData, demoOptions);
                RaceViewConfig demoView = ctx.view;
                demoView.hudMode = 3;
                demoView.replay = &demo;
                demoView.replayEndLeaves = true;
                demoView.replayOut.clear();
                demoView.sponsorCategory = SponsorCategoryOf(demo); // the demo file's race block (+ 9, + 0x44)
                demoView.sponsorSeed = demo.SponsorSeed(); // race block + 0x54
                demoView.sponsorSeedGiven = true;
                const RaceViewResult d = RunRaceView(*ctx.window, ctx.panels, disc, vol, demoData, demoData.params.size(), demoOptions, demoView, nullptr);
                std::printf("licence %s: the demonstration ended after %d steps\n", label.c_str(), d.steps);
                if (d.exit == RaceExit::kClosed || ctx.window->Closed()) break;
                if (demoTest != testIndex) { // the menu of the demonstrated test
                    nextTest = demoTest;
                    startAtOnce = false;
                    break;
                }
            }
        }
        if (nextTest >= 0 && ctx.window && !ctx.window->Closed()) {
            testName = LicenceTestName(testName, nextTest);
            continue;
        }
        break;
        }
        std::printf("career after:  %s\n", DescribeCareer(s).c_str());
        return true;
    }

    // ---- the machine test (ovl4 0x8001861C: G400 / G1000 / GMAX -> race sub-modes 7 / 8 / 9, set up by 0x80012C6C)
    if (MachineTestMode(menu, name) >= 0) {
        RunMachineTest(disc, vol, s, data, menu, name, ctx);
        std::printf("career after:  %s\n", DescribeCareer(s).c_str());
        return true;
    }

    // ---- events and championships
    if (racePath == 2) { // the menus checked the entry (0x8001973C); again here only to report it
        const EventInfoTable infos = BuildEventInfos(data, menu);
        uint32_t message = 0;
        const int32_t entry = CheckEntry(data, menu, infos, s, name, message);
        std::printf("entry check (0x8001973C): %d %s\n", entry, EntryText(entry));
        if (entry != 1) return false;
    }
    if (!ctx.window) {
        RunEventHeadless(disc, vol, data, menu, s, name, ctx.seed, ctx.race, ctx.headlessSeconds);
        std::printf("career after:  %s\n", DescribeCareer(s).c_str());
        return true;
    }
    std::unique_ptr<EventPlan> plan = PrepareEvent(disc, data, menu, s, name, ctx.seed);
    career::GarageCar& car = s.garage.cars[s.garage.currentCar];
    std::array<uint8_t, 0x10> eventTail{};
    {
        const EventInfoTable infos = BuildEventInfos(data, menu);
        const std::string prefix = plan->name.substr(0, std::min<size_t>(3, plan->name.size()));
        const career::EventInfo info = plan->resultIndex >= 0 && size_t(plan->resultIndex) < infos.infos.size() ? infos.infos[size_t(plan->resultIndex)] : career::EventInfo{};
        eventTail = EventRaceTail(plan->resultIndex, prefix == menu.prefixes[2], s.garage.currentCar, info, (car.powerFlags & 0x4000) != 0);
    }
    SettingsScreen settings(car, data);
    // CHANGE PARTS reads the race block's + 0x586 (the event's power limit) and + 0x588 (course flags: bit 0 tarmac tyres, bits 1..2
    // the aspiration rules) - the tail the menus leave (EventRaceTail, 0x80019578 / 0x800195BC / 0x800194FC).
    settings.SetRaceLimits(int16_t(eventTail[0xA] | eventTail[0xB] << 8), uint16_t(eventTail[0xC] | eventTail[0xD] << 8));
    uint32_t clock = ctx.seed;
    bool completed = true;
    for (int32_t race = 0; race < plan->races; race++) {
        RaceData rd;
        BuildEventRace(disc, vol, data, *plan, race, rd);
        rd.dirtLevel = car.wordA2; // 0x80017784: race block + 0x58 = the garage car's + 0xA2 (+ 0x582 / + 0x584 >= 0)
        RaceOptions ro = ctx.race;
        ro.laps = RaceLaps(*plan, race) ? RaceLaps(*plan, race) : ro.laps;
        RaceFlow flow;
        flow.raceEnded = [&](const RaceViewResult& r) { // 0x80017964: the player's dirt back into the garage car
            car.wordA2 = r.playerDirt;
            rd.dirtLevel = r.playerDirt;
        };
        flow.title = plan->championship ? std::string(plan->series.names[race]) + "  (race " + std::to_string(race + 1) + " of " + std::to_string(plan->races) + ")" : name;
        flow.info.push_back(CourseDisplayName(vol, rd) + ", " + std::to_string(ro.laps) + " lap(s), " + std::to_string(plan->grid.size()) + " cars");
        flow.info.push_back("Your car  " + CarName(data, car.modelId));
        flow.info.push_back("Prize for 1st  " + MenuThousands(uint32_t(plan->prizes.prize[0])) + " cr");
        flow.settings = &settings;
        flow.settingsChanged = [&] { // 0x80056FF0 wrote the garage slot; the race slot's configuration follows (flags | 0x40)
            plan->grid[0].config = car.config;
            plan->grid[0].config.flags = uint8_t(plan->grid[0].config.flags | 0x40);
            CarConfig config = plan->grid[0].config;
            rd.params[0] = BuildCarParams(vol, data.tables, config);
            rd.bodies[0] = CarBodyOf(vol, data.tables, config); // a racing modification changes the body model
            rd.sound[0].soundId = config.engineWord;
            rd.sound[0].exhaustByte = config.exhaustByte;
            rd.sound[0].turbo = (config.flags & 2) != 0;
        };
        flow.continueLabel = plan->championship && race + 1 < plan->races ? "Next race" : "Continue";
        // 0x801D5866: "Start Race" of the event menu sets sub-mode 2 for every event race, single or series (ovl0
        // 0x800178B0 -> 0x8001710C writes race block +0xA; watched in the capture route of CBM0001, a single race).
        flow.raceEndMode = 2;
        // A replay of this race as Save Replay packs it (0x801D585C as the menus / 0x80017098 left it): the options' copies, the
        // race's name (+ 0x10, ovl4 0x8005E548 / the event), and in a Test Run (one car) the other five entries kept with + 0x8C = 0.
        auto eventReplay = [&](const ReplayFile& replay) {
            ReplayFile f = replay;
            MenuRaceBlock(f, s);
            SetSponsorCategory(f, plan->event.tag);
            const std::string raceName = plan->championship ? std::string(plan->series.names[race]) : name;
            std::fill(f.raceBlock.begin() + 0x10, f.raceBlock.begin() + 0x20, uint8_t(0));
            for (size_t k = 0; k < raceName.size() && k < 15; k++) f.raceBlock[0x10 + k] = uint8_t(raceName[k]);
            if (f.CarCount() == 1 && plan->grid.size() > 1) {
                const ReplayFile all = BuildReplayFile(vol, rd, plan->grid.size(), ro, ReplayStream::FromBytes(replay.stream));
                for (size_t i = 1; i < all.cars.size(); i++) {
                    ReplayEntry e = all.cars[i];
                    e.slot[0x8C] = 0;
                    f.cars.push_back(e);
                }
            }
            return f;
        };
        if (ctx.panels) { // the race overlay's event menu (0x800585C0, "SINGLE RACE")
            gt2::screens::EventMenuState m;
            m.course = CourseDisplayName(vol, rd);
            // The view's title: "SINGLE RACE" (0x801C6E29, captured); a championship race: the view image's own title
            // "SESSION %d" (0x801C78DE) with the race number (not captured: see docs/formats/race_screens.md).
            const gt2::RaceMenuAssets& menuAssets = ctx.panels->MenuAssets(Panels::Screen::kSettings);
            char title[64];
            std::snprintf(title, sizeof title, menuAssets.Text(plan->championship ? 0x801C78DEu : 0x801C6E29u).c_str(), race + 1);
            m.title = title;
            flow.eventMenu = m;
            // "Save Replay ..." (row 3, code -2 -> view 0x8005B51C) after a Test Run; the menu's car (M+0x241) is race slot 0's.
            flow.saveReplay = [&](const ReplayFile& replay, const sim::PlayerResults& results) {
                LentMenu lent;
                lent.view = flow.lentMenu;
                lent.resetup = flow.resetupMenu;
                lent.carId = flow.menuCarId, lent.paint = flow.menuCarPaint;
                RunSaveReplayScreen(*ctx.window, *ctx.panels, disc, vol, Panels::Screen::kSettings, eventReplay(replay), rd.params[0], results, ctx.card1Path, ctx.card2Path,
                                    &eventTail, lent);
            };
            flow.menuCarId = plan->grid[0].carId;
            flow.menuCarPaint = CarPaintIndex(vol, plan->grid[0].carId, uint8_t(plan->grid[0].paint));
            // TRANSMISSION before Test Run / Start Race (0x80058108) unless race block + 0x588 bit 3: the race overlay's entry
            // 0x80011F64 copies the garage car's + 0x98 bit 14 there (0x800178E4: a gearbox of fewer than 3 gears).
            flow.transmission = &s.garage.byte401A;
            flow.transmissionDialog = (car.powerFlags & 0x4000) == 0;
        }
        flow.seriesRaces = plan->races;
        for (const career::GridCar& gc : plan->grid) flow.carNames.push_back(CarName(data, gc.carId));
        std::optional<career::RaceOutcome> outcome; // for the post-race views
        flow.finished = [&](const RaceViewResult& r) {
            EventRaceOutcome o;
            o.finished = r.finished;
            o.position = r.position;
            o.orderPosition = r.orderPosition;
            o.positions = r.positionsAtPlayerFinish;
            o.steps = r.steps;
            clock += uint32_t(r.steps);
            std::vector<std::string> lines = {"Time " + FormatMs(r.finishTime) + "    Best lap " + FormatMs(r.bestLap)};
            for (const std::string& l : ApplyEventRace(*plan, s, race, o, clock, &outcome)) lines.push_back(l);
            return lines;
        };
        RaceViewConfig view = ctx.view;
        view.sponsorCategory = plan->event.tag; // race block + 0x44 with + 9 = 1: 0x800275E8's category ("0": no boards)
        view.hudMode = 2; // race block + 0xA = 2 (0x8001710C): the lap block with the position, Record / Best Lap (captured: work/re/ai_ev1 f10000)
        const RaceViewResult r = RunRaceView(*ctx.window, ctx.panels, disc, vol, rd, plan->grid.size(), ro, view, &flow);
        if (r.exit != RaceExit::kFinished) {
            completed = false;
            std::printf("event %s: left at race %d without a result (%s)\n", name.c_str(), race + 1, r.exit == RaceExit::kClosed ? "window closed" : "Exit");
            break;
        }
        if (ctx.panels && !ctx.window->Closed()) { // the race overlay's post-race views (race mode 0x801D5866 = 2)
            PostRaceShow show;
            screens::ResultsInput results;
            results.place = r.position;                             // s16 0x801D5E88
            results.totalTime = uint32_t(r.finishTime);             // 0x801D5F80
            results.fastestLap = uint32_t(r.bestLap);               // 0x801D5F58
            results.course = CourseDisplayName(vol, rd);            // 0x801D587C
            results.saveBar = false;                                // 0x80050D00: 0 in race mode 2 (the dialog "Next")
            results.laps = r.lapTimes;                              // 0x801D5E90 + i * 0x14 (the last 10 laps)
            results.firstLap = r.lapNumber - int(r.lapTimes.size()) + 1; // s16 0x801D5E8A - count + 1
            show.results = results;
            if (outcome) {
                screens::BonusInput bonus;
                bonus.kind = plan->championship ? screens::BonusKind::kChampionshipRace : screens::BonusKind::kSingleRace;
                bonus.place = outcome->position;
                bonus.prize = uint32_t(outcome->prize);
                bonus.money = uint32_t(s.garage.money);
                bonus.prizeCar = outcome->prizeCar >= 0 && outcome->prizeCarAdded;
                show.bonus = bonus;
            }
            screens::PostMenuInput pm;
            pm.mode = 2;
            pm.race = plan->championship ? race : 0;                // s16 0x801D5DF4
            pm.races = plan->championship ? plan->races : 0;        // s16 0x801D5DF6 (0 for a single race, captured)
            pm.place = r.position;
            pm.totalTime = results.totalTime;
            pm.fastestLap = results.fastestLap;
            pm.course = results.course;
            show.menu = pm;
            show.carId = plan->grid[0].carId;
            show.paint = CarPaintIndex(vol, plan->grid[0].carId, uint8_t(plan->grid[0].paint));
            show.results->vsync = uint32_t(ctx.window->Field()); // 0x80050BC4 seeds the car's pose with the VSync counter
            show.saveGame = [&] { RunSaveGameScreen(*ctx.window, *ctx.panels, disc, vol, s, ctx.card1Path, ctx.card2Path); };
            // The race's replay (the race view's recorded stream): the menu's Replay (state 11 -> 12 -> 13 -> 6) and Save Replay.
            std::optional<ReplayFile> replay;
            if (!r.recordedStream.empty()) replay = eventReplay(BuildReplayFile(vol, rd, plan->grid.size(), ro, ReplayStream::FromBytes(r.recordedStream)));
            show.replay = replay.has_value();
            const sim::PlayerResults raceResults = r.playerResults;
            if (replay)
                show.saveReplay = [&](const LentMenu& lent) {
                    RunSaveReplayScreen(*ctx.window, *ctx.panels, disc, vol, Panels::Screen::kSettings, *replay, rd.params[0], raceResults, ctx.card1Path, ctx.card2Path,
                                        &eventTail, lent);
                };
            PostRaceChoice choice = RunPostRaceViews(*ctx.window, *ctx.panels, show, ctx.view.autoAdvance);
            while (choice == PostRaceChoice::kReplay && replay) { // the replay, then the menu again (without RESULTS / BONUS)
                RaceData replayData;
                BuildEventRace(disc, vol, data, *plan, race, replayData);
                RaceViewConfig replayView = ctx.view;
                replayView.hudMode = 2;
                replayView.sponsorCategory = SponsorCategoryOf(*replay);
                replayView.replay = &*replay;
                replayView.replayEndLeaves = true;
                replayView.replayOut.clear();
                const RaceViewResult rr = RunRaceView(*ctx.window, ctx.panels, disc, vol, replayData, plan->grid.size(), ro, replayView, nullptr);
                if (rr.exit == RaceExit::kClosed || ctx.window->Closed()) {
                    choice = PostRaceChoice::kClosed;
                    break;
                }
                show.results.reset();
                show.bonus.reset();
                choice = RunPostRaceViews(*ctx.window, *ctx.panels, show, ctx.view.autoAdvance);
            }
            if (choice != PostRaceChoice::kContinue && race + 1 < plan->races) {
                completed = false;
                std::printf("event %s: left after race %d (%s)\n", name.c_str(), race + 1, choice == PostRaceChoice::kExit ? "Exit" : "window closed");
                break;
            }
        }
    }
    if (completed && plan->championship) {
        career::ChampionshipEnd end;
        const std::vector<std::string> lines = FinishEvent(*plan, s, menu, clock, &end);
        for (const std::string& line : lines) std::printf("  %s\n", line.c_str());
        // 0x80017A28 after the last race's post-race menu ("Continue"): a champion gets the championship end view
        // 0x8005D534 (0x80059800: bonus, the random prize car, the trophy; captured 2026-09-19, GT300 won with the FTO LM:
        // the view follows the menu's Continue, then the leave view 0x8005AE30 and back to the GT-mode menus); anyone
        // else goes straight back to the menus.
        if (ctx.panels && !ctx.window->Closed() && end.champion) {
            PostRaceShow show;
            screens::BonusInput bonus;
            bonus.kind = screens::BonusKind::kChampionshipEnd;
            bonus.place = 1;
            bonus.prize = uint32_t(end.prize.prize);
            bonus.money = uint32_t(s.garage.money);
            bonus.prizeCar = end.prize.prizeCar >= 0 && end.prize.prizeCarAdded;
            show.bonus = bonus;
            show.saveGame = [&] { RunSaveGameScreen(*ctx.window, *ctx.panels, disc, vol, s, ctx.card1Path, ctx.card2Path); };
            RunPostRaceViews(*ctx.window, *ctx.panels, show, ctx.view.autoAdvance);
        }
    }
    std::printf("career after:  %s\n", DescribeCareer(s).c_str());
    return true;
}

} // namespace gt2game
