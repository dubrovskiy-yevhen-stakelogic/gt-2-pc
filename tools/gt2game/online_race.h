#pragma once
#include <stdexcept>
#include <functional>
#include "race_common.h"
#include "platform/net/lan_room.h"

namespace gt2game {
class GameWindow;
class Panels;
struct RaceViewConfig;
void RunLanMenu(GameWindow& window, const gt2::DiscImage& disc, const gt2::GtfsVolume& volume, const RaceViewConfig& view, const std::function<void()>& menuAudioTick = {},
                const std::function<void(bool)>& raceAudio = {});
struct LanOptions {
    std::string nickname="Player",roomName="GT2 room";
    std::function<void()> heartbeat;
    std::function<std::vector<std::pair<uint8_t,std::string>>()> kickTargets;
    std::function<void(uint8_t)> kick;
    bool host = false;
    uint8_t admissionSlot=255; gt2::net::ResumeToken admission{};
    std::string address = "127.0.0.1";
    uint16_t port = 29600;
    uint8_t humans = 2;
    std::optional<gt2::net::SetupPacket> offer;
    std::optional<gt2::net::RoomSecurity> security;
    std::filesystem::path resumePath;
    std::optional<gt2::net::ResumeIdentity> resume;
    uint64_t previousSession = 0; // wait for a fresh room after results
};
bool PrepareLanJoin(GameWindow& window, Panels& panels, const gt2::DiscImage& disc, const gt2::GtfsVolume& volume,
                    RaceData& data, size_t& cars, RaceOptions& options, LanOptions& lan);
bool RunOnlineSession(GameWindow& window, Panels& panels, const gt2::DiscImage& disc, const gt2::GtfsVolume& volume,
                      RaceData& data, size_t cars, RaceOptions options, LanOptions lan, RaceViewConfig view);

#ifdef __EMSCRIPTEN__
class OnlineRace {
public:
    size_t LocalCar() const { return 0; }
    gt2::sim::RaceSim& Race() { throw std::logic_error("Native networking is unavailable in the browser"); }
    bool Active() const { return false; }
    bool Complete() const { return false; }
    bool CanPredict() const { return false; }
    void SceneLoaded() {}
    void Poll() {} void Predict(gt2::sim::PadRecord) {} void ToggleReady() {} void Reconnect() {}
    void SendEmote(uint8_t) {}
    std::vector<std::pair<uint8_t,std::string>> KickTargets() const {return {};}
    void Kick(uint8_t) {}
    void DrawPanel(Panels&,const GameWindow&) const {}
    unsigned FinishSeconds() const {return 0;}
    gt2::net::RoomStatus Status() const { return {}; }
    const std::array<gt2::net::Result, gt2::net::kSlots>& Results() const { static const std::array<gt2::net::Result, gt2::net::kSlots> empty{}; return empty; }
    std::vector<gt2::net::Event> TakeEvents() { return {}; }
};
#else
// Presentation-owned prediction; LanRoom owns the separate authoritative race.
class OnlineRace {
public:
    OnlineRace(const gt2::DiscImage& disc, const gt2::GtfsVolume& volume, const RaceData& data,
               size_t cars, const RaceOptions& options, const LanOptions& lan);
    bool WaitForSlot(GameWindow& window, Panels& panels);
    void Poll();
    bool CanPredict() const;
    void Predict(gt2::sim::PadRecord input);
    std::vector<gt2::net::Event> TakeEvents();
    void SendEmote(uint8_t emote) { if(Active())room_->SendEmote(emote); }
    void SceneLoaded();
    void ToggleReady();
    void Reconnect();
    void DrawPanel(Panels& panels,const GameWindow& window) const;
    unsigned FinishSeconds() const {return prediction_->ConfirmedFinishSeconds();}
    bool Active() const;
    bool Complete() const;
    std::vector<std::pair<uint8_t,std::string>> KickTargets() const {return kickTargets_?kickTargets_():std::vector<std::pair<uint8_t,std::string>>{};}
    void Kick(uint8_t slot) {if(kick_)kick_(slot);}
    bool IsHost() const { return host_; }
    size_t LocalCar() const { return status_.localSlot; }
    gt2::sim::RaceSim& Race() { return race_; }
    const gt2::net::RoomStatus& Status() const { return status_; }
    const std::array<gt2::net::Result, gt2::net::kSlots>& Results() const { return session_->Results(); }
private:
    gt2::sim::RaceSim race_;
    std::unique_ptr<gt2::net::RaceSession> session_;
    std::unique_ptr<gt2::net::PredictionTimeline> prediction_;
    std::unique_ptr<gt2::net::LanRoom> room_;
    gt2::net::RoomStatus status_;
    std::array<std::string,gt2::net::kSlots> racerNames_{};
    gt2::net::Inputs lastInputs_{};
    gt2::net::ContentKey content_{};
    std::vector<gt2::net::Event> events_;
    std::function<void()> heartbeat_;
    std::function<std::vector<std::pair<uint8_t,std::string>>()> kickTargets_;
    std::function<void(uint8_t)> kick_;
    bool host_ = false, ready_ = false, sceneLoaded_ = false, autoReady_ = false;
    uint8_t humans_ = 2;
    std::string track_, address_, roomName_;
    uint16_t port_ = 29600;
    uint8_t laps_ = 0;
    bool manual_ = false;
    std::filesystem::path resumePath_;
    std::optional<gt2::net::RoomSecurity> security_;
};
#endif
} // namespace gt2game
