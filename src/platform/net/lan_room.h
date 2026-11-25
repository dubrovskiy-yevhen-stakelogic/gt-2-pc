#pragma once
#include "game/net/protocol.h"
#include "game/net/room_protocol.h"
#include "game/net/state_codec.h"
#include "platform/net/private_transport.h"
#include "platform/net/room_files.h"
#include <memory>
#include <chrono>
#include <optional>
#include <string>

namespace gt2::net {
enum class RoomPhase { Connecting, Lobby, Racing, Complete, Failed, Closed, Reconnecting, Recovering };
struct LanRoomConfig {
    std::string nickname = "Player";
    std::array<ResumeToken,kSlots> admissions{}; // host: slot capabilities issued by directory
    uint8_t admissionSlot=255; ResumeToken admission{}; // initial client claim
    bool host = false;
    bool dedicated = false; // host process has no local participant
    std::string address = "127.0.0.1";
    uint16_t port = 29600;
    uint8_t cars = 6, humans = 2;
    // The host supplies a fresh nonzero identifier. This is NOT an access token.
    uint64_t session = 0;
    ContentKey content{};
    std::optional<RoomSetup> setup; // host advertisement, immutable for this room
    uint64_t expectedSession = 0; // client pins the queried room; not authentication
    std::optional<RoomSecurity> security;
    std::optional<ResumeIdentity> resume;
};
struct RoomStatus {
    RoomPhase phase = RoomPhase::Connecting;
    uint8_t localSlot = 255, connected = 0, ready = 0;
    uint16_t boundPort = 0;
    uint64_t session = 0;
    Tick nextTick = 0;
    bool recoveryRequired = false;
    bool restoringView = false; // local host view only; authority keeps racing
    uint32_t reconnects = 0;
    uint32_t recoveredTicks = 0;
    uint32_t stateRestores = 0;
    uint32_t stateChecks = 0, desyncs = 0;
    bool viewFailed = false; // repeated divergence stops local play, not the authoritative worker
    std::array<std::string,kSlots> names{};
    std::array<uint8_t,kSlots> emotes{};
    std::array<std::chrono::steady_clock::time_point,kSlots> emoteUntil{};
    std::string error;
};

// Race room: private invitations enable authentication and encryption; omitted
// security is reserved for explicitly trusted LAN diagnostics and local games.
// ENet and the authoritative simulation live on one owned worker thread. Render
// pauses never stop it. The caller owns immutable track/extras until destruction.
// No RaceSim pointers or mutable physics state cross the worker boundary.
class LanRoom {
public:
    static constexpr size_t kFrameHistory = 300;
    static constexpr Tick kResumeHistory = AuthorityHistory::kCapacity;
    LanRoom(LanRoomConfig config, std::unique_ptr<sim::RaceSim> authority = {});
    ~LanRoom();
    LanRoom(const LanRoom&) = delete;
    LanRoom& operator=(const LanRoom&) = delete;
    RoomStatus Status() const;
    std::optional<ResumeIdentity> Identity() const; // private client credential; never log or broadcast
    void SendEmote(uint8_t emote);
    void SetReady(bool ready);
    void Kick(uint8_t slot); // Host revokes this race admission and reconnect ticket.
    void RequestStart(); // host; waits until all configured participants are ready
    bool Reconnect(bool fullState = false); // client repair; fullState also clears lost presentation history
    bool RecoverView(); // host local snapshot, or client full-state reconnect
    // Call immediately after confirming each frame, before exposing its events.
    // False means discard the remaining drained frames and wait for recovery.
    bool VerifyState(const Packet& frame, const RaceSession& session, const PredictionTimeline& prediction);
    std::optional<RoomState> TakeState(); // apply with StateCodec, reset prediction, then acknowledge
    void StateApplied(Tick nextTick);
    bool Submit(Tick tick, const sim::PadRecord& input);
    std::vector<Packet> DrainFrames();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Nonblocking, caller-thread-owned query. No slot, readiness or simulation state
// is allocated by querying. Poll during UI frames or a headless event loop.
class LanSetupQuery {
public:
    LanSetupQuery(const std::string& address, uint16_t port, std::optional<RoomSecurity> security = {}, bool allowRunning = false);
    ~LanSetupQuery();
    void Poll();
    const std::optional<SetupPacket>& Offer() const;
    const std::string& Error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace gt2::net
