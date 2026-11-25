#ifndef __EMSCRIPTEN__
#include "online_race.h"
#include "gt2formats/car_info.h"
#include "network_setup.h"
#include "platform/net/lobby_cars.h"
#include "platform/net/lobby_rules.h"
#include "game/arcade/arcade_car_page.h"
#include "game/shell/title_draw.h"
#include "gt2view/title_view.h"
#include "gt2view/menu_view.h"
#include <functional>
#include "game_window.h"
#include "online_controls.h"
#include "panel.h"
#include "race_view.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/course_data.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/sha1.h"
#include "gt2vfs/gtfs.h"
#include "platform/os/paths.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <fstream>
#include <stdexcept>

namespace gt2game {
namespace net = gt2::net;
namespace {
// A separate low-frequency log distinguishes network correction CPU from GPU frame time.
struct NetworkTiming {
    std::ofstream file;std::chrono::steady_clock::time_point next{};double pollMs=0,predictMs=0,maxPollMs=0;uint64_t polls=0,predictions=0,lastReplay=0;
    void Start(){file.close();std::error_code ec;const auto path=gt2::os::DataRoot()/"logs";std::filesystem::create_directories(path,ec);if(ec)return;
        const auto stamp=std::chrono::system_clock::now().time_since_epoch().count();file.open(path/("network-"+std::to_string(stamp)+".csv"));
        file<<"time_ms,poll_calls,poll_total_ms,poll_max_ms,predict_calls,predict_total_ms,replayed_ticks,pending_ticks\n";next=std::chrono::steady_clock::now()+std::chrono::seconds(1);pollMs=predictMs=maxPollMs=0;polls=predictions=lastReplay=0;}
    void Record(double ms,bool predict,uint64_t replay,size_t pending){if(predict){predictMs+=ms;++predictions;}else{pollMs+=ms;maxPollMs=std::max(maxPollMs,ms);++polls;}
        const auto now=std::chrono::steady_clock::now();if(now<next)return;
        if(file){file<<std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()<<','<<polls<<','<<pollMs<<','<<maxPollMs<<','<<predictions<<','<<predictMs<<','<<(replay-lastReplay)<<','<<pending<<'\n';file.flush();}
        pollMs=predictMs=maxPollMs=0;polls=predictions=0;lastReplay=replay;next=now+std::chrono::seconds(1);}
};
NetworkTiming networkTiming;
struct NetworkTimer{net::PredictionTimeline& prediction;bool predict;std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();~NetworkTimer(){networkTiming.Record(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(),predict,prediction.ReplayedTicks(),prediction.PendingTicks());}};
bool MultiplayerBack(const GameWindow& window) {
    return window.Pressed(gt2::keys::kEscape) || window.Pressed(gt2::keys::kBack) ||
        window.Pressed(gt2::keys::kSpace) || window.PadPressed(gt2::input::ps1::kStart);
}
}

OnlineRace::OnlineRace(const gt2::DiscImage& disc, const gt2::GtfsVolume& volume, const RaceData& data,
                       size_t cars, const RaceOptions& options, const LanOptions& lan)
    : host_(lan.host), humans_(lan.humans), track_(data.trackName), address_(lan.address), laps_(options.laps), manual_(options.manual) {
    kickTargets_=lan.kickTargets;kick_=lan.kick;
    autoReady_ = lan.admissionSlot != 255;
    networkTiming.Start(); roomName_=lan.roomName;heartbeat_=lan.heartbeat;
    resumePath_ = lan.resumePath; security_ = lan.security; port_ = lan.port;
    if ((!gt2::ProfileOf(disc).reference && gt2::ProfileOf(disc).build != gt2::ExeBuild::kArcadeUs11) || data.modCar || data.modCourse || data.haveDump || options.license || options.aiPlayer ||
        data.constants.flag800A951C || (data.constants.gameMode != 0 && data.constants.gameMode != 2) || !data.entryKinds.empty() || !data.transmissions.empty() ||
        !data.gridSlots.empty() || !options.laps || options.laps > 99)
        throw std::invalid_argument("LAN preview requires a stock US Simulation v1.2 or US Arcade v1.1 standalone circuit race");
    const auto roster = net::RoomRoster(uint8_t(cars), humans_);
    auto setup = [&](gt2::sim::RaceSim& race) {
        auto shell = data.shell; shell.lapCount = options.laps; shell.countdown = options.countdown;
        net::SetupNetworkRace(race, data.track, data.extras, data.constants, data.course,
            std::vector<gt2::sim::CarParams>(data.params.begin(), data.params.begin() + std::ptrdiff_t(cars)),
            GridSlots(data, cars, options), shell, roster, options.botDifficulty);
    };
    setup(race_);
    session_ = std::make_unique<net::RaceSession>(race_, roster);
    prediction_ = std::make_unique<net::PredictionTimeline>(*session_);
    lastInputs_.fill(net::SafeIdleInput());
    net::LanRoomConfig config;
    config.admissionSlot=lan.admissionSlot;config.admission=lan.admission;
    config.host = host_; config.address = address_; config.port = lan.port; config.cars = uint8_t(cars); config.humans = humans_;
    config.nickname = lan.nickname;
    config.security = lan.security;
    config.resume = lan.resume;
    const gt2::GtfsVolume physics(disc, gt2::GtfsVolume::Source::DiscOnly);
    config.content = NetworkContent(disc, physics, data, cars, options, humans_);
    (void)volume; // The renderer still uses installed visual replacements.
    content_ = config.content;
    const auto description = DescribeNetworkSetup(data, cars, options, humans_);
    if (lan.offer) {
        if (host_ || description != lan.offer->setup || config.content != lan.offer->content)
            throw std::runtime_error("Local game data does not match the host.");
        config.expectedSession = lan.offer->session;
    }
    std::unique_ptr<gt2::sim::RaceSim> authority;
    if (host_) {
        config.session = net::NewSessionId();
        config.setup = description;
        authority = std::make_unique<gt2::sim::RaceSim>(); setup(*authority);
    }
    room_ = std::make_unique<net::LanRoom>(config, std::move(authority));
}
bool PrepareLanJoin(GameWindow& window, Panels& panels, const gt2::DiscImage& disc, const gt2::GtfsVolume& volume,
                    RaceData& data, size_t& cars, RaceOptions& options, LanOptions& lan) {
    (void)volume;
    if (lan.host) return true;
    lan.offer.reset();
    lan.resume.reset();
    std::optional<net::SavedResume> saved;
    if (lan.security && !lan.resumePath.empty() && std::filesystem::exists(lan.resumePath)) {
        try {
            auto value = net::ReadResume(lan.resumePath);
            if (value.address == lan.address && value.port == lan.port && value.hostPublic == lan.security->hostPublic) saved = value;
        } catch (const std::exception&) { std::fprintf(stderr, "online: saved network identity is invalid; normal admission only\n"); }
    }
    auto query = std::make_unique<net::LanSetupQuery>(lan.address, lan.port, lan.security, saved.has_value() || lan.previousSession != 0);
    auto retryAt = std::chrono::steady_clock::now();
    panels.Upload();
    std::string loggedError;
    while (window.BeginFrame()) {
        if(lan.heartbeat)lan.heartbeat();
        if (MultiplayerBack(window)) return false;
        if (!query && std::chrono::steady_clock::now() >= retryAt)
            query = std::make_unique<net::LanSetupQuery>(lan.address, lan.port, lan.security, true);
        if (query) query->Poll();
        if (query && query->Offer() && query->Offer()->session != lan.previousSession) {
            const auto& offer = *query->Offer();
            const auto& setup = offer.setup;
            if (offer.running) {
                if (!saved || saved->identity.session != offer.session || saved->content != offer.content)
                    throw std::runtime_error("This race has already started; no saved player identity matches it.");
                lan.resume = saved->identity;
                std::puts("online: restoring the saved player identity after process restart");
            }
            // Rebuild from local stock assets only. No host-supplied file paths or physics blobs.
            const gt2::GtfsVolume physics(disc, gt2::GtfsVolume::Source::DiscOnly);
            RaceData loaded;
            LoadRaceTrack(disc, physics, setup.track, {}, loaded);
            if(setup.arcadeCars)BuildNetworkCarRecords(physics,std::vector<std::string>(setup.carIds.begin(),setup.carIds.begin()+setup.cars),loaded);
            else BuildCarRecords(physics,setup.carIds[0],setup.cars,{},loaded,std::vector<std::string>(setup.carIds.begin()+1,setup.carIds.begin()+setup.cars));
            loaded.paints.assign(setup.paints.begin(), setup.paints.begin() + setup.cars);
            RaceOptions chosen;
            chosen.botDifficulty=setup.botDifficulty;chosen.laps = setup.laps; chosen.manual = setup.manual; chosen.countdown = setup.countdown;
            if (NetworkContent(disc, physics, loaded, setup.cars, chosen, setup.humans) != offer.content)
                throw std::runtime_error("Local game data does not match the host.");
            data = std::move(loaded); cars = setup.cars; options = chosen; lan.humans = setup.humans; lan.offer = offer;
            std::printf("online: received setup %s, %u cars, %u players, %u laps, %s, countdown %s\n",
                setup.track.c_str(), setup.cars, setup.humans, setup.laps, setup.manual ? "MT" : "AT", setup.countdown ? "on" : "off");
            return true;
        }
        if (query && !query->Error().empty() && loggedError != query->Error()) {
            loggedError = query->Error(); std::fprintf(stderr, "online setup: %s\n", loggedError.c_str());
        }
        if (lan.previousSession && query && (query->Offer() || !query->Error().empty())) {
            query.reset(); retryAt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        Panels::Menu menu;
        menu.title = "Multiplayer - Join room";
        menu.lines = {"Host: " + lan.address, lan.previousSession ? "Waiting for the host's next race..." :
            loggedError.empty() ? "Getting race settings..." :
            loggedError == "race has already started" ? "This race has already started." :
            loggedError == "room is full" ? "The room is full." : "Host unavailable or incompatible."};
        menu.footer = {NetworkMenuHint(window)}; menu.width = 470;
        panels.Clear(); panels.DrawMenu(menu, 45);
        std::vector<gt2view::DrawItem> items; AppendNetworkPanel(panels,window,window.Renderer().AspectRatio(),items); window.EndFrame(items);
    }
    return false;
}
bool RunOnlineSession(GameWindow& window, Panels& panels, const gt2::DiscImage& disc, const gt2::GtfsVolume& volume,
                      RaceData& data, size_t cars, RaceOptions options, LanOptions lan, RaceViewConfig view) {
    view.replay = nullptr; view.replayOut.clear(); view.ghostReplay = false; view.exitFade = false;
    for (;;) {
        if (!PrepareLanJoin(window, panels, disc, volume, data, cars, options, lan)) return false;
        OnlineRace online(disc, volume, data, cars, options, lan);
        if (!online.WaitForSlot(window, panels)) return false;
        view.online = &online;
        const auto result = RunRaceView(window, &panels, disc, volume, data, cars, options, view, nullptr);
        if (result.exit != RaceExit::kNetworkNext || lan.admissionSlot!=255) return result.exit==RaceExit::kNetworkNext;
        lan.previousSession = online.Status().session;
        lan.resume.reset(); lan.offer.reset();
        panels.Upload(); window.ResetPacing();
    }
}
bool OnlineRace::WaitForSlot(GameWindow& window, Panels& panels) {
    panels.Upload();
    while (window.BeginFrame()) {
        if(heartbeat_)heartbeat_();
        status_ = room_->Status();
        if (status_.phase == net::RoomPhase::Failed || status_.phase == net::RoomPhase::Closed) return false;
        if (MultiplayerBack(window)) return false;
        if (status_.localSlot < humans_ && status_.phase != net::RoomPhase::Failed && status_.phase != net::RoomPhase::Closed) {
            if (security_ && !host_ && !resumePath_.empty()) {
                if (const auto identity = room_->Identity()) {
                    try { net::WriteResume(resumePath_, {address_, port_, security_->hostPublic, content_, *identity}); }
                    catch (const std::exception& e) { std::fprintf(stderr, "online: cannot save reconnect identity: %s\n", e.what()); }
                }
            }
            std::printf("online: admitted as car %zu, %s\n", LocalCar(), host_ ? "host" : "client");
            return true;
        }
        panels.Clear(); DrawPanel(panels,window);
        std::vector<gt2view::DrawItem> items;
        AppendNetworkPanel(panels,window,window.Renderer().AspectRatio(),items);
        window.EndFrame(items);
    }
    return false;
}
void OnlineRace::Poll() {
    if(heartbeat_)heartbeat_();
    NetworkTimer timer{*prediction_,false};
    const auto previous = status_.phase;
    status_ = room_->Status();
    for(size_t i=0;i<humans_;++i)if(!status_.names[i].empty())racerNames_[i]=status_.names[i];
    if (auto state = room_->TakeState()) {
        net::StateCodec::Restore(*session_, content_, state->bytes, state->nextTick);
        prediction_->AcceptRestoredState();
        events_.clear(); lastInputs_.fill(net::SafeIdleInput());
        room_->StateApplied(state->nextTick);
        std::printf("online: restored authority snapshot at tick %u, %zu bytes\n", state->nextTick, state->bytes.size());
    }
    if (status_.viewFailed) return;
    if (status_.recoveryRequired) room_->RecoverView();
    if (status_.phase != previous)
        std::printf("online: phase %d, car %zu, authority tick %u, reconnects %u, recovered ticks %u%s%s\n", int(status_.phase), LocalCar(), status_.nextTick,
                    status_.reconnects, status_.recoveredTicks,
                    status_.error.empty() ? "" : ": ", status_.error.c_str());
    if ((!Active() && !status_.restoringView && status_.phase != net::RoomPhase::Complete && status_.phase != net::RoomPhase::Reconnecting &&
         status_.phase != net::RoomPhase::Recovering) || status_.recoveryRequired) return;
    const auto frames = room_->DrainFrames();
    for (const auto& frame : frames) prediction_->StageAuthority(frame.tick, frame.inputs);
    for (const auto& frame : frames) {
        if (prediction_->PendingTicks() == 0) prediction_->Predict(frame.inputs);
        auto confirmed = prediction_->Confirm(frame.tick, frame.inputs);
        if (!room_->VerifyState(frame, *session_, *prediction_)) {
            const auto previousDesyncs = status_.desyncs;
            status_ = room_->Status(); events_.clear();
            if (status_.desyncs != previousDesyncs) std::printf("online: state mismatch at tick %u, mismatches %u%s\n", frame.tick + 1, status_.desyncs,
                status_.viewFailed ? ", local play stopped" : ", automatic repair requested");
            return;
        }
        events_.insert(events_.end(), confirmed.begin(), confirmed.end());
        lastInputs_ = frame.inputs;
        for (auto& input : lastInputs_) input.shift = 0;
    }
    if (status_.phase == net::RoomPhase::Complete && prediction_->ConfirmedNextTick() == status_.nextTick)
        prediction_->DiscardPrediction();
}
bool OnlineRace::Active() const { return status_.phase == net::RoomPhase::Racing && !status_.recoveryRequired && !status_.restoringView && !status_.viewFailed; }
bool OnlineRace::Complete() const { return status_.phase == net::RoomPhase::Complete && !status_.recoveryRequired && !status_.restoringView && !status_.viewFailed && prediction_->ConfirmedNextTick() == status_.nextTick; }
bool OnlineRace::CanPredict() const { return Active() && prediction_->PendingTicks() < 4; }
void OnlineRace::Predict(gt2::sim::PadRecord input) {
    NetworkTimer timer{*prediction_,true};
    if (!CanPredict()) throw std::logic_error("online prediction is not ready");
    input.flags &= uint16_t(~gt2::sim::kWheelIgnoreShiftSpeed);
    auto inputs = lastInputs_;
    inputs[LocalCar()] = input;
    room_->Submit(session_->NextTick(), input);
    prediction_->Predict(inputs);
}
std::vector<net::Event> OnlineRace::TakeEvents() { auto result = std::move(events_); events_.clear(); return result; }
void OnlineRace::SceneLoaded() {
    if(sceneLoaded_)return;
    sceneLoaded_=true;
    std::puts("online: race scene loaded; ready for synchronized intro");
    if(autoReady_){ready_=true;room_->SetReady(true);}
}
void OnlineRace::ToggleReady() {
    if (autoReady_ || !sceneLoaded_ || status_.phase != net::RoomPhase::Lobby) return;
    ready_ = !ready_; room_->SetReady(ready_);
    if (host_) room_->RequestStart();
}
void OnlineRace::Reconnect() { if (room_->RecoverView()) std::printf("online: full state repair requested\n"); }
void OnlineRace::DrawPanel(Panels& panels,const GameWindow& window) const {
    if (Active()) return;
    Panels::Menu menu;
    menu.title = roomName_.empty()?"Multiplayer":roomName_;
    menu.lines = {track_ + " - " + std::to_string(humans_) + " players - " + std::to_string(laps_) + " laps - " + (manual_ ? "MT" : "AT"),
                  "Players can join from any supported platform."};
    if (status_.viewFailed) {
        menu.lines.push_back("Race state keeps differing from the host.");
        menu.lines.push_back("Leave this room and check both game builds.");
    } else if (status_.recoveryRequired || status_.restoringView) {
        menu.lines.push_back("Restoring the race after a pause...");
        menu.lines.push_back("Other players keep racing.");
    }
    else if (status_.phase == net::RoomPhase::Failed) {
        const auto& error = status_.error;
        menu.lines.push_back(error.find("reason 2") != std::string::npos ? "Race settings or game data do not match." :
            error.find("reason 3") != std::string::npos ? "This race has already started." :
            error.find("reason 4") != std::string::npos ? "The room is full." :
            error.find("reason 5") != std::string::npos ? "Too much race history was missed. Leave this race." :
            error.find("reason 6") != std::string::npos ? "The host cannot restore this player." : "Connection lost or host unavailable.");
    } else if (status_.phase == net::RoomPhase::Reconnecting || status_.phase == net::RoomPhase::Recovering) {
        menu.lines.push_back(status_.phase == net::RoomPhase::Reconnecting ? "Connection interrupted. Reconnecting..." : "Restoring the race...");
        menu.lines.push_back("Your car is reserved. The race continues.");
    } else if (Complete()) {
        menu.title = "Multiplayer Results";
        menu.footer.push_back(NetworkControllerHints(window)?"A: next race":"Enter: next race");
        for (size_t i = 0; i < session_->CarCount(); ++i) {
            const auto& r = Results()[i];
            const auto name=i>=humans_?"BOT "+std::to_string(i-humans_+1):
                racerNames_[i].empty()?"Player "+std::to_string(i+1):racerNames_[i];
            menu.lines.push_back(std::string(i == LocalCar() ? "> " : "  ") + name +
                "  " + (r.dnf ? "DNF" : FormatMs(r.finishTime)) + "  P" + std::to_string(r.position));
        }
    } else if (status_.localSlot == 255) menu.lines.push_back("Connecting...");
    else {
        menu.lines.push_back("Your car: " + std::to_string(LocalCar() + 1));
        for (uint8_t i = 0; i < humans_; ++i) menu.lines.push_back((status_.names[i].empty()?"Player " + std::to_string(i + 1):status_.names[i]) + ": " +
            (!(status_.connected & (1u << i)) ? "waiting" : (status_.ready & (1u << i)) ? "ready" : "not ready"));
        menu.items = {ready_ ? "Not ready" : "Ready"};
        menu.footer.push_back(NetworkReadyHint(window));
    }
    menu.footer.push_back(NetworkRoomHint(window));
    if(!NetworkControllerHints(window))menu.footer.push_back("F8: restore race view");
    menu.width = 470;
    panels.DrawMenu(menu, 45);
}

namespace {
bool EditNetworkText(GameWindow& window,Panels& panels,const std::string& title,std::string& output,size_t limit,bool password,net::LobbyClient* lobby,const std::function<void()>& audioTick) {
    std::string text=output;int row=0,column=0;bool lower=false;
    const std::string keys="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_ .";
    window.BlockMenuInputUntilRelease();
    while(window.BeginFrame()) {
        if(audioTick)audioTick();
        if(lobby){lobby->Poll();if(!lobby->Busy()&&lobby->Error().empty()&&window.Field()%300==0)lobby->RequestList();}
        if(window.KeyPressed(gt2::keys::kEscape)||window.PadPressed(gt2::input::ps1::kCircle)||window.PadPressed(gt2::input::ps1::kTriangle))return false;
        if(window.Pressed(gt2::keys::kUp))row=(row+7)%8;
        if(window.Pressed(gt2::keys::kDown))row=(row+1)%8;
        if(window.Pressed(gt2::keys::kLeft))column=(column+9)%10;
        if(window.Pressed(gt2::keys::kRight))column=(column+1)%10;
        auto add=[&](char c){if(text.size()<limit)text.push_back(lower&&c>='A'&&c<='Z'?char(c+32):c);};
        for(int key='A';key<='Z';++key)if(window.KeyPressed(key))add(char(key));
        for(int key='0';key<='9';++key)if(window.KeyPressed(key))add(char(key));
        if(window.KeyPressed(gt2::keys::kBack)&&!text.empty())text.pop_back();
        if(window.Pressed(gt2::keys::kReturn)){
            if(row<4)add(keys[size_t(row*10+column)]);
            else if(row==4)lower=!lower;
            else if(row==5){if(!text.empty())text.pop_back();}
            else if(row==6){output=text;window.BlockMenuInputUntilRelease();return true;}
            else return false;
        }
        Panels::Menu menu;menu.title=title;menu.width=470;menu.selected=row;
        menu.lines={(password?std::string(text.size(),'*'):text)+"_",std::to_string(text.size())+" / "+std::to_string(limit)};
        for(int y=0;y<4;++y){std::string line;for(int x=0;x<10;++x){char c=keys[size_t(y*10+x)];if(lower&&c>='A'&&c<='Z')c=char(c+32);line+=(row==y&&column==x?"[":" ");line+=c;line+=(row==y&&column==x?"]":" ");}menu.items.push_back(line);}
        menu.items.insert(menu.items.end(),{lower?"Letters: abc":"Letters: ABC","Delete last character","Done","Cancel"});menu.footer={NetworkMenuHint(window)};
        panels.Clear();panels.DrawMenu(menu,10);std::vector<gt2view::DrawItem> items;AppendNetworkPanel(panels,window,window.Renderer().AspectRatio(),items);window.EndFrame(items);
    }return false;
}
}
namespace {
bool NetworkGarage(GameWindow& window,const gt2::DiscImage& disc,const gt2::GtfsVolume& volume,uint8_t cls,
                   const std::string& fixed,std::string& selectedCar,uint8_t& selectedPaint,const std::function<void()>& heartbeat){
    using namespace gt2;namespace mp=gt2::menu_list_pad;
    const auto assets=arcade::ArcadeMenuAssets::Load(disc,volume);const auto data=ArcadeData::Load(volume);const auto cars=CarInfoDirectory::Load(volume);
    arcade::ArcadeCarPage page(assets,data,cars);auto vram=assets.vram;arcade::TextCtx text;
    const auto allowed=net::LobbyClassCars(assets.data,cls);std::vector<uint8_t> available;
    for(const auto& id:assets.data.classes[cls].cars)available.push_back(uint8_t(std::find(allowed.begin(),allowed.end(),id)!=allowed.end()&&(fixed.empty()||id==fixed)));
    if(std::none_of(available.begin(),available.end(),[](auto x){return x!=0;}))return false;
    page.Enter(cls,available,false,1,vram,text);
    auto& renderer=window.Renderer();gt2view::TitleView view(renderer);gt2view::MenuCarView car(renderer,volume);car.SetFrameSize(352,480);car.LoadMenuReflection();
    std::array<uint8_t,512> selection{};uint32_t uploaded=~0u,shown=0,previous=0;int repeat=0;
    window.BlockMenuInputUntilRelease();
    while(window.BeginFrame()){
        heartbeat();window.SetDrivingActive(false);
        if(MultiplayerBack(window)){window.BlockMenuInputUntilRelease();return false;}
        gt2::MenuListPad pad;const std::pair<int,uint32_t> keys[]={{gt2::keys::kUp,mp::kUp},{gt2::keys::kDown,mp::kDown},{gt2::keys::kLeft,mp::kLeft},{gt2::keys::kRight,mp::kRight},{gt2::keys::kReturn,mp::kCross}};
        for(auto [key,bit]:keys){if(window.Held(key))pad.held|=bit;if(window.Pressed(key))pad.pressed|=bit;}
        pad.pressed|=pad.held&~previous;pad.released=previous&~pad.held;
        const auto dirs=pad.held&(mp::kUp|mp::kDown|mp::kLeft|mp::kRight);if(dirs&&(previous&dirs)==dirs){if(++repeat>=20&&(repeat-20)%5==0)pad.repeat=dirs;}else repeat=0;previous=pad.held;
        std::vector<int> sounds;const auto result=page.Update(&pad,selection.data(),sounds,vram,text,true);page.ModelTick();
        if(result==arcade::ArcadeCarPage::kChosen){selectedCar=UnpackCarId(page.CarId());selectedPaint=uint8_t(page.Paint());window.BlockMenuInputUntilRelease();return true;}
        if(result==arcade::ArcadeCarPage::kBack)return false;
        if(uploaded!=page.Uploads()){view.UploadVram(vram);uploaded=page.Uploads();}
        auto prims=shell::TitleFrameStart();size_t layer=1;std::vector<gt2view::DrawItem> model,items;
        if(page.CarShown()){if(shown!=page.CarId()){shown=page.CarId();car.Use(shown);}auto floor=page.Floor();prims.insert(prims.end(),floor.begin(),floor.end());layer=prims.size();car.Append(model,page.Projection(true),page.Paint(),renderer.AspectRatio(),false);if(!model.empty())model.front().clearDepth=1;}
        arcade::ViewOt ot;arcade::AddArcadeHeader(ot.slot[4],ot.slot[6],assets.fonts[0],"CHOOSE YOUR CAR",0x2084b6,128);page.Draw(ot,text);ot.Emit(prims,0x200);
        view.Build(prims,352,renderer.AspectRatio(),items,false,layer,model);window.EndFrame(items);
    }return false;
}
}
void RunLanMenu(GameWindow& window,const gt2::DiscImage& disc,const gt2::GtfsVolume& volume,const RaceViewConfig& view,const std::function<void()>& menuAudioTick,const std::function<void(bool)>& raceAudio){
    Panels panels(window.Renderer(),disc,volume);panels.Upload();
    enum class Page{Home,Browse,Create,Creating,Joining,Room,Kick};Page page=Page::Home;int selected=0;net::LobbyOptions settings;
    const auto catalogue=gt2::ArcadeMenuData::Load(disc,1);const auto cars=gt2::CarInfoDirectory::Load(volume);const auto courses=gt2::ParseCourseInfo(volume.Read(".crsinfo"));
    std::string nickname="Player",roomName="GT2 room",password,error,chosenCar;uint8_t chosenPaint=0;bool ready=false;
    const auto profilePath=gt2::os::SavesDir().parent_path()/"multiplayer-name.txt";
    {std::ifstream f(profilePath);std::string name;if(std::getline(f,name)&&!name.empty()&&name.size()<=23&&std::all_of(name.begin(),name.end(),[](unsigned char c){return c>=32&&c<=126;}))nickname=name;}
    std::unique_ptr<net::LobbyClient> lobby;std::optional<net::LobbyRoom> waiting;
    bool inRace=false;net::LobbyReturnState returning;
    std::optional<net::LobbyKind> command;uint8_t kickSlot=255;net::LobbyOptions changedOptions;
    auto refreshAt=std::chrono::steady_clock::now();auto updateAt=refreshAt;
    auto carName=[&](const std::string& id){const auto* info=cars.Find(gt2::PackCarId(id));return info?info->name:id;};
    auto botName=[](uint8_t level){return std::string(level==0?"Easy":level==2?"Hard":"Normal");};
    auto className=[](uint8_t cls){return cls==6?std::string("Rally"):std::string(1,"SABC"[std::min<unsigned>(cls,3)]);};
    auto trackName=[&](uint8_t t){const int i=courses.FindByFileName(net::kLobbyTracks[t]);return i>=0?courses.entries[size_t(i)].name:std::string(net::kLobbyTracks[t]);};
    auto show=[&](Page p){page=p;selected=0;error.clear();window.BlockMenuInputUntilRelease();};
    auto connect=[&]{returning.Clear();command.reset();lobby.reset();waiting.reset();error.clear();try{lobby=std::make_unique<net::LobbyClient>();}catch(const std::exception&){error="Server connection failed. Please retry.";}};
    auto heartbeat=[&]{if(menuAudioTick)menuAudioTick();if(!lobby)return;lobby->Poll();if(inRace)if(auto reply=lobby->TakeCreated())waiting=*reply;if(waiting&&!lobby->Busy()){
        if(command){lobby->Command(*command,waiting->id,waiting->revision,kickSlot,changedOptions);command.reset();kickSlot=255;}
        else if(std::chrono::steady_clock::now()>=updateAt){
            if(!inRace&&returning.Pending(*waiting))lobby->Command(net::LobbyKind::Return,waiting->id,waiting->revision);
            else lobby->Update(waiting->id,nickname,chosenCar,chosenPaint,ready,waiting->revision);
            updateAt=std::chrono::steady_clock::now()+std::chrono::seconds(1);
        }
    }};
    auto garage=[&](uint8_t cls,const std::string& fixed,std::string& id,uint8_t& paint){const bool picked=NetworkGarage(window,disc,volume,cls,fixed,id,paint,heartbeat);panels.Upload();window.ResetPacing();return picked;};
    net::LobbyOptions roadSettings=settings;
    auto chooseTrack=[&](uint8_t current)->std::optional<uint8_t>{
        size_t family=net::LobbyTrackFamilyOf(current);int variant=0,row=0;
        for(int i=0;i<4;++i)if(net::kLobbyTrackFamilies[family].variants[i]==current)variant=i;
        window.BlockMenuInputUntilRelease();
        while(window.BeginFrame()){
            heartbeat();if(MultiplayerBack(window)){window.BlockMenuInputUntilRelease();return {};}
            if(window.Pressed(gt2::keys::kUp))row=(row+4)%5;if(window.Pressed(gt2::keys::kDown))row=(row+1)%5;
            const bool select=window.Pressed(gt2::keys::kReturn);
            const int delta=int(window.Pressed(gt2::keys::kRight))-int(window.Pressed(gt2::keys::kLeft));
            if(row==0&&(delta||select)){
                family=size_t((int(family)+(delta?delta:1)+int(net::kLobbyTrackFamilies.size()))%int(net::kLobbyTrackFamilies.size()));
                const auto& v=net::kLobbyTrackFamilies[family].variants;
                if(v[variant]==255){variant=0;while(v[variant]==255)++variant;}
            }
            const auto& f=net::kLobbyTrackFamilies[family];
            if(row==1&&(delta||select)&&f.variants[variant^2]!=255)variant^=2;
            if(row==2&&(delta||select)&&f.variants[variant^1]!=255)variant^=1;
            const uint8_t track=f.variants[variant];
            if(select&&row>=3){window.BlockMenuInputUntilRelease();return row==3?std::optional<uint8_t>(track):std::nullopt;}
            Panels::Menu m;m.width=470;m.selected=row;
            m.items={std::string("Track: ")+f.name+"  < >",std::string("Time: ")+(variant&2?"Night":"Day")+(f.variants[variant^2]==255?" (fixed)":"  < >"),std::string("Direction: ")+(variant&1?"Reverse":"Normal")+(f.variants[variant^1]==255?" (fixed)":"  < >"),"Use this track","Cancel"};
            m.footer={NetworkMenuHint(window)};
            panels.Clear();panels.TextCentre(256,12,"Choose track");panels.TextCentre(256,38,trackName(track));
            panels.CoursePreview(net::LobbyTrackPicture(track),181,65,150,160);
            if(std::string(f.name)=="Rome")panels.TextCentre(256,225,"Rome-Night has a different layout.");
            panels.DrawMenu(m,245);std::vector<gt2view::DrawItem> items;AppendNetworkPanel(panels,window,window.Renderer().AspectRatio(),items);window.EndFrame(items);
        }return {};
    };
    auto chooseRules=[&](net::LobbyOptions options)->std::optional<net::LobbyOptions>{
        int row=0;window.BlockMenuInputUntilRelease();
        while(window.BeginFrame()){
            heartbeat();if(MultiplayerBack(window)){window.BlockMenuInputUntilRelease();return {};}
            if(window.Pressed(gt2::keys::kUp))row=(row+4)%5;if(window.Pressed(gt2::keys::kDown))row=(row+1)%5;
            const bool select=window.Pressed(gt2::keys::kReturn);const int delta=int(window.Pressed(gt2::keys::kRight))-int(window.Pressed(gt2::keys::kLeft));
            if(row==0&&(delta||select))options.carRule=!options.carRule;
            if(row==1&&(delta||select)&&!net::LobbyTrackDirt(options.track)){
                const int classes[]={0,1,2,3,6};int i=int(std::find(std::begin(classes),std::end(classes),options.carClass)-std::begin(classes));
                options.carClass=uint8_t(classes[(i+(delta?delta:1)+5)%5]);options.fixedCar=net::LobbyClassCars(catalogue,options.carClass).front();
            }
            if(row==2&&select&&options.carRule==0){uint8_t paint=0;garage(options.carClass,{},options.fixedCar,paint);}
            if(select&&row>=3){window.BlockMenuInputUntilRelease();return row==3?std::optional<net::LobbyOptions>(options):std::nullopt;}
            Panels::Menu m;m.title="Car rules";m.width=470;m.selected=row;
            m.items={options.carRule?"Cars: choose from class":"Cars: same car for everyone","Class: "+className(options.carClass)+(net::LobbyTrackDirt(options.track)?" (dirt track)":"  < >"),options.carRule?"Players choose cars in their garages":"Car: "+carName(options.fixedCar),"Apply","Cancel"};m.footer={NetworkMenuHint(window)};
            panels.Clear();panels.DrawMenu(m,60);std::vector<gt2view::DrawItem> items;AppendNetworkPanel(panels,window,window.Renderer().AspectRatio(),items);window.EndFrame(items);
        }return {};
    };
    while(window.BeginFrame()){
        window.SetDrivingActive(false);auto& renderer=window.Renderer();renderer.clearColor[0]=renderer.clearColor[1]=renderer.clearColor[2]=0;
        if(MultiplayerBack(window)){if(page==Page::Kick){show(Page::Room);continue;}if(page==Page::Home)return;command.reset();lobby.reset();waiting.reset();show(Page::Home);continue;}
        heartbeat();std::optional<net::LobbyRoom> launch;
        if(lobby)if(auto reply=lobby->TakeCreated()){
            returning.Observe(*reply);
            if(reply->phase==1){if(!returning.Pending(*reply)&&(!waiting||waiting->phase!=2))launch=reply;}
            else {const bool initial=!waiting;const bool reset=initial||waiting->revision!=reply->revision;waiting=reply;
                if(reply->phase==2){returning.Begin(reply->id);ready=false;}
                if(reset){const auto& m=reply->members.at(reply->localSlot);chosenCar=m.car;chosenPaint=m.paint;ready=false;if(initial)show(Page::Room);updateAt={};}}

        }
        if((page==Page::Creating||page==Page::Joining)&&lobby&&!lobby->Busy()&&!waiting&&!launch){show(page==Page::Creating?Page::Create:Page::Browse);}
        if(waiting&&lobby&&lobby->Error()=="This room is no longer available."){lobby.reset();waiting.reset();command.reset();show(Page::Home);error="You left the room or were removed by its host.";}
        const bool owner=waiting&&waiting->localSlot==waiting->hostSlot;
        std::vector<uint8_t> kickTargets;if(waiting)for(uint8_t i=0;i<waiting->options.players;++i)if(waiting->members[i].occupied&&i!=waiting->localSlot)kickTargets.push_back(i);
        int count=page==Page::Home?4:page==Page::Browse?int(lobby?lobby->Rooms().size():0)+2:page==Page::Create?13:page==Page::Room?(owner?9:3):page==Page::Kick?int(kickTargets.size())+1:1;
        selected=std::clamp(selected,0,count-1);if(window.Pressed(gt2::keys::kUp))selected=(selected+count-1)%count;if(window.Pressed(gt2::keys::kDown))selected=(selected+1)%count;
        const bool enter=window.Pressed(gt2::keys::kReturn);const int delta=int(window.Pressed(gt2::keys::kRight))-int(window.Pressed(gt2::keys::kLeft));
        if(page==Page::Home&&enter){if(selected==3)return;if(selected==2){if(EditNetworkText(window,panels,"Your nickname",nickname,23,false,nullptr,menuAudioTick)){if(nickname.empty())nickname="Player";std::filesystem::create_directories(profilePath.parent_path());std::ofstream(profilePath)<<nickname;}}
            else{show(selected==0?Page::Browse:Page::Create);connect();}}
        else if(page==Page::Browse&&enter){const int n=int(lobby?lobby->Rooms().size():0);if(selected<n){auto room=lobby->Rooms()[selected];if(room.phase)error="This race is already in progress.";else if(room.connected>=room.options.players)error="This room is full.";else if(!lobby->Busy()){std::string pass;if(!room.passwordRequired||EditNetworkText(window,panels,"Room password",pass,31,true,lobby.get(),menuAudioTick)){lobby->Join(room.id,pass);show(Page::Joining);}}}
            else if(selected==n){connect();selected=0;}else{lobby.reset();show(Page::Home);}}
        else if(page==Page::Create){
            if(selected==0&&enter)EditNetworkText(window,panels,"Room name",roomName,31,false,lobby.get(),menuAudioTick);
            if(selected==1&&enter)EditNetworkText(window,panels,"Password (empty = public)",password,31,true,lobby.get(),menuAudioTick);
            if(selected==2&&(delta||enter)){if(auto track=chooseTrack(settings.track))net::SelectLobbyTrack(catalogue,settings,*track,roadSettings);}
            if(delta){if(selected==3)settings.players=uint8_t(std::clamp(int(settings.players)+delta,2,6));if(selected==4&&!net::LobbyTrackPointToPoint(settings.track))settings.laps=uint8_t(std::clamp(int(settings.laps)+delta,1,10));}
            if(selected==5&&(delta||enter))settings.manual=!settings.manual;
            if(selected==6&&(delta||enter))settings.carRule=!settings.carRule;
            if(selected==7&&(delta||enter)&&!net::LobbyTrackDirt(settings.track)){const int classes[]={0,1,2,3,6};int i=int(std::find(std::begin(classes),std::end(classes),settings.carClass)-std::begin(classes));settings.carClass=uint8_t(classes[(i+(delta?delta:1)+5)%5]);settings.fixedCar=net::LobbyClassCars(catalogue,settings.carClass).front();}
            if(selected==8&&enter&&settings.carRule==0){uint8_t paint=0;garage(settings.carClass,{},settings.fixedCar,paint);}
            if(selected==9&&(delta||enter))settings.bots=!settings.bots;
            if(selected==10&&(delta||enter))settings.botDifficulty=uint8_t((int(settings.botDifficulty)+(delta?delta:1)+3)%3);
            if(selected==11&&enter){if(!lobby||!lobby->Error().empty()){connect();error="Reconnecting. Select Create room when connected.";}else if(!lobby->Busy()){lobby->Create(settings,roomName,password);show(Page::Creating);}}
            if(selected==12&&enter){lobby.reset();show(Page::Home);}
        }else if(page==Page::Room&&waiting){
            if(enter&&selected==0&&waiting->phase==0){
                const bool canChoose=!ready;ready=false;
                if(canChoose&&owner&&waiting->options.carRule==0){changedOptions=waiting->options;uint8_t paint=0;if(garage(changedOptions.carClass,{},changedOptions.fixedCar,paint))command=net::LobbyKind::Configure;}
                else if(canChoose)garage(waiting->options.carClass,waiting->options.carRule==0?waiting->options.fixedCar:std::string{},chosenCar,chosenPaint);
                updateAt={};
            }
            if(enter&&selected==1&&waiting->phase==0){ready=!ready;updateAt={};}
            if(enter&&selected==(owner?8:2)){lobby.reset();waiting.reset();command.reset();show(Page::Home);}
            if(owner&&waiting){
                if(enter&&selected==2&&net::LobbyCanStart(*waiting)&&ready)command=net::LobbyKind::Start;
                if(selected==3&&(delta||enter)&&waiting->phase==0){if(auto track=chooseTrack(waiting->options.track)){changedOptions=waiting->options;net::SelectLobbyTrack(catalogue,changedOptions,*track,roadSettings);ready=false;command=net::LobbyKind::Configure;}}
                if((selected==4||selected==5)&&(delta||enter)&&waiting->phase==0){changedOptions=waiting->options;if(selected==4)changedOptions.bots=!changedOptions.bots;else changedOptions.botDifficulty=uint8_t((int(changedOptions.botDifficulty)+(delta?delta:1)+3)%3);ready=false;command=net::LobbyKind::Configure;}
                if(enter&&selected==6&&waiting->phase==0){if(auto rules=chooseRules(waiting->options)){changedOptions=*rules;ready=false;command=net::LobbyKind::Configure;}}
                if(enter&&selected==7)show(Page::Kick);
            }
        }else if(page==Page::Kick&&waiting&&enter){if(selected<int(kickTargets.size())){kickSlot=kickTargets[selected];command=net::LobbyKind::Kick;}show(Page::Room);}
        else if((page==Page::Creating||page==Page::Joining)&&enter){lobby.reset();show(Page::Home);}
        if(page==Page::Browse&&lobby&&!enter&&!lobby->Busy()&&lobby->Error().empty()&&std::chrono::steady_clock::now()>=refreshAt){lobby->RequestList();refreshAt=std::chrono::steady_clock::now()+std::chrono::seconds(2);}
        Panels::Menu menu;menu.title="Multiplayer";menu.width=470;
        if(page==Page::Home){menu.lines={"Race together on PC, Steam Deck and Quest."};menu.items={"Find a room","Create a room","Nickname: "+nickname,"Back"};}
        else if(page==Page::Browse){menu.title="Find a room";if(lobby)for(const auto& r:lobby->Rooms())menu.items.push_back((r.passwordRequired?"[Private] ":"")+r.name+"  "+std::to_string(r.connected)+"/"+std::to_string(r.options.players)+(r.phase?" Racing":""));if(lobby&&selected<int(lobby->Rooms().size())){const auto& r=lobby->Rooms()[selected];menu.lines={trackName(r.options.track),r.options.carRule==0?"Same car: "+carName(r.options.fixedCar):"Class: "+className(r.options.carClass)};}if(menu.items.empty())menu.lines={"No rooms yet. Create one to invite friends."};menu.items.push_back("Refresh");menu.items.push_back("Back");}
        else if(page==Page::Create){menu.title="Create a room";menu.items={"Name: "+roomName,password.empty()?"Password: none (public)":"Password: "+std::string(password.size(),'*'),"Track: "+trackName(settings.track),"Players: "+std::to_string(settings.players),"Laps: "+std::to_string(settings.laps),settings.manual?"Transmission: MT":"Transmission: AT",settings.carRule?"Cars: choose from a class":"Cars: same car for everyone","Class: "+className(settings.carClass),settings.carRule?"Players choose in the garage":"Car: "+carName(settings.fixedCar),settings.bots?"Bots: fill free places":"Bots: off","Bot difficulty: "+botName(settings.botDifficulty),lobby&&lobby->Busy()?"Connecting...":"Create room","Back"};}
        else if(page==Page::Room&&waiting){menu.title=waiting->name;menu.lines={trackName(waiting->options.track)};
            for(size_t i=0;i<waiting->options.players;++i){const auto& m=waiting->members[i];menu.lines.push_back(!m.occupied?(waiting->options.bots?"AI (free place)":"Empty"):m.name.substr(0,12)+(i==waiting->hostSlot?" [Host]":"")+" - "+carName(m.car).substr(0,20)+(waiting->phase?(m.returned?" [In room]":" [In race/results]"):(m.ready?" [Ready]":" [Not ready]")));}
            menu.items={"Garage: "+carName(chosenCar),waiting->phase?"Ready (waiting for players to return)":ready?"Ready - cancel readiness":"Ready"};
            if(owner){menu.items.push_back(waiting->phase?"Start race (players still in race/results)":net::LobbyHumanCount(*waiting)<2?"Start race (need 2 real players)":net::LobbyCanStart(*waiting)&&ready?"Start race":"Start race (waiting for Ready)");menu.items.push_back("Track: "+trackName(waiting->options.track)+"  < >");menu.items.push_back(waiting->options.bots?"Bots: fill free places":"Bots: off");menu.items.push_back("Bot difficulty: "+botName(waiting->options.botDifficulty));menu.items.push_back("Car rules...");menu.items.push_back("Remove player");}
            menu.items.push_back("Leave room");menu.footer={waiting->phase?"On EACH device: A / Enter on results returns here.":waiting->options.bots?"All players must be ready. Free places use AI.":"All players must be ready. No AI cars."};}
        else if(page==Page::Kick&&waiting){menu.title="Remove player";for(auto slot:kickTargets)menu.items.push_back(waiting->members[slot].name);menu.items.push_back("Back");}
        else{menu.title="Connecting";menu.lines={"Preparing your room..."};menu.items={"Cancel"};}
        if(!error.empty())menu.lines.push_back(error);if(lobby&&!lobby->Error().empty())menu.lines.push_back(lobby->Error());menu.selected=selected;menu.footer.push_back(NetworkMenuHint(window));
        panels.Clear();panels.DrawMenu(menu,10);std::vector<gt2view::DrawItem> items;AppendNetworkPanel(panels,window,renderer.AspectRatio(),items);window.EndFrame(items);
        if(launch&&lobby){LanOptions options;options.nickname=nickname;options.roomName=launch->name;options.address=lobby->Address();options.port=launch->port;options.security=launch->security;options.humans=launch->options.players;options.admissionSlot=launch->raceSlot;options.admission=launch->admission;options.heartbeat=heartbeat;bool nextRace=false;waiting=launch;
            options.kickTargets=[&]{std::vector<std::pair<uint8_t,std::string>> result;if(waiting&&waiting->localSlot==waiting->hostSlot)for(uint8_t i=0;i<waiting->options.players;++i)if(i!=waiting->localSlot&&waiting->members[i].occupied)result.emplace_back(i,waiting->members[i].name);return result;};
            options.kick=[&](uint8_t slot){kickSlot=slot;command=net::LobbyKind::Kick;};
            inRace=true;
            if(raceAudio)raceAudio(true);
            try{const auto identities=gt2::os::SavesDir().parent_path()/"network-identities";std::filesystem::create_directories(identities);options.resumePath=identities/("room-"+std::to_string(launch->id)+".gt2resume");RaceData data;RaceOptions raceOptions;nextRace=RunOnlineSession(window,panels,disc,volume,data,6,raceOptions,options,view);}
            catch(const std::exception& e){std::fprintf(stderr,"online room: %s\n",e.what());error="Cannot join this race. Please refresh and retry.";}
            inRace=false;
            if(raceAudio)raceAudio(false);
            if(nextRace){(void)lobby->TakeCreated();ready=false;command.reset();returning.Begin(waiting->id);waiting->phase=2;show(Page::Room);updateAt={};}
            else{command.reset();waiting.reset();show(Page::Browse);connect();}panels.Upload();window.ResetPacing();
        }
    }
}

} // namespace gt2game

#else
#include "online_race.h"
namespace gt2game { void RunLanMenu(GameWindow&, const gt2::DiscImage&, const gt2::GtfsVolume&, const RaceViewConfig&, const std::function<void()>&, const std::function<void(bool)>&) {} }
#endif
