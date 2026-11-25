#include "platform/net/lan_room.h"
#include "platform/net/enet_transport.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>

namespace gt2::net {
struct LanRoom::Impl {
    using Clock = std::chrono::steady_clock;
    LanRoomConfig config;
    std::unique_ptr<sim::RaceSim> authority;
    mutable std::mutex mutex;
    RoomStatus status;
    std::deque<Packet> frames;
    std::vector<std::pair<Tick, sim::PadRecord>> commands;
    uint8_t pendingEmote = 0, pendingKicks = 0;
    bool desiredReady = false, requestStart = false, reconnectRequested = false;
    bool fullStateRequested = false;
    bool localStateRequested = false;
    std::optional<RoomState> pendingState;
    std::optional<ResumeIdentity> identity;
    std::optional<Tick> stateApplied;
    Tick deliveredNext = 0;
    std::atomic_bool stop{false};
    std::thread worker;

    Impl(LanRoomConfig c, std::unique_ptr<sim::RaceSim> race) : config(std::move(c)), authority(std::move(race)) {
        (void)RoomRoster(config.cars, config.humans);
        if ((config.dedicated && !config.host) || (config.security && config.security->Hosting() != config.host))
            throw std::invalid_argument("invalid room hosting identity");
        if (config.resume && (config.host || !config.security || !config.resume->session || config.resume->slot >= config.humans ||
            config.resume->token == ResumeToken{} || config.expectedSession != config.resume->session))
            throw std::invalid_argument("invalid persisted resume identity");
        if (config.address.empty() || (!config.host && !config.port) ||
            (config.host && (!config.session || !authority || authority->CarCount() != config.cars)) ||
            (!config.host && (authority || config.session))) throw std::invalid_argument("invalid LAN room configuration");
        if ((config.host && config.expectedSession) || (config.setup && (!config.host || !ValidRoomSetup(*config.setup) ||
            config.setup->cars != config.cars || config.setup->humans != config.humans)))
            throw std::invalid_argument("invalid LAN advertised setup");
        worker = std::thread([this] {
            try { Run(); }
            catch (const std::exception& e) {
                std::lock_guard lock(mutex);
                status.phase = RoomPhase::Failed;
                status.error = e.what();
            }
        });
    }
    ~Impl() { stop = true; if (worker.joinable()) worker.join(); }
    void Publish(const RoomStatus& current) {
        std::lock_guard lock(mutex);
        const bool gap = status.recoveryRequired;
        const auto checks = status.stateChecks, desyncs = status.desyncs;
        const bool viewFailed = status.viewFailed;
        status = current;
        status.recoveryRequired = gap;
        status.stateChecks = checks; status.desyncs = desyncs; status.viewFailed = viewFailed;
    }
    void Frame(const Packet& p) {
        std::lock_guard lock(mutex);
        if (status.recoveryRequired || status.viewFailed) return;
        if (frames.size() == kFrameHistory) {
            frames.clear();
            status.recoveryRequired = true;
            return; // the authority and other participants continue
        }
        frames.push_back(p);
    }
    void Run() {
        PrivateTransport transport(config.host ? config.address : "0.0.0.0", config.host ? config.port : 0, 8, config.security);
        RoomStatus current;
        current.boundPort = transport.BoundPort();
        if (config.host) {
            current.phase = RoomPhase::Lobby;
            current.localSlot = config.dedicated ? 255 : 0; current.connected = config.dedicated ? 0 : 1; current.session = config.session;
        }
        Publish(current);
        const auto roster = RoomRoster(config.cars, config.humans);
        std::unique_ptr<RaceSession> session;
        if (config.host) session = std::make_unique<RaceSession>(*authority, roster);
        InputInbox inbox(roster, config.cars);
        struct Peer {
            uint8_t slot = 255; Clock::time_point admittedAt; unsigned invalidInputs = 0;
            std::optional<Tick> replayNext;
            Tick replayAck = 0;
            std::optional<Tick> awaitingState;
        };
        std::map<ConnectionId, Peer> peers;
        std::set<ConnectionId> failedPeers;
        ConnectionId server = config.host ? 0 : transport.Connect(config.address, config.port);
        std::array<ResumeToken, kSlots> tickets{};
        ResumeToken clientTicket{};
        AuthorityHistory history;
        auto newTicket = [&] {
            for (unsigned attempt = 0; attempt < 8; ++attempt) {
                ResumeToken ticket;
                SecureRandom(ticket);
                if (ticket != ResumeToken{} && std::find(tickets.begin(), tickets.end(), ticket) == tickets.end()) return ticket;
            }
            throw std::runtime_error("could not create unique LAN resume ticket");
        };
        const uint8_t fullMask = uint8_t((1u << config.humans) - 1);
        auto message = [&](RoomMessage kind, uint8_t slot) {
            RoomPacket p;
            p.kind = kind; p.cars = config.cars; p.humans = config.humans; p.slot = slot;
            p.names=current.names; p.session = current.session; p.connected = current.connected; p.ready = current.ready; p.content = config.content;
            return p;
        };
        auto sendBytes = [&](ConnectionId id, Channel channel, std::span<const uint8_t> bytes) {
            if (failedPeers.contains(id)) return;
            try { transport.Send(id, channel, bytes); }
            catch (const std::runtime_error&) {
                if (!config.host) throw;
                // A saturated or closing peer cannot terminate everybody's race.
                failedPeers.insert(id);
            }
        };
        auto send = [&](ConnectionId id, const RoomPacket& p) { sendBytes(id, Channel::Control, EncodeRoomPacket(p)); };
        auto broadcast = [&](RoomMessage kind) {
            for (const auto& [id, peer] : peers) if (peer.slot != 255) {
                if (kind == RoomMessage::End && (peer.replayNext || peer.awaitingState)) continue;
                auto p = message(kind, peer.slot);
                if (kind == RoomMessage::End) p.tick = current.nextTick;
                send(id, p);
            }
        };
        std::array<Clock::time_point,kSlots> emoteNext{};
        std::array<uint32_t,kSlots> emoteSequence{};
        auto publishEmote=[&](uint8_t actor,uint8_t value) {
            const auto now=Clock::now();
            if(actor>=config.humans||value<1||value>6||now<emoteNext[actor])return;
            emoteNext[actor]=now+std::chrono::seconds(1);
            current.emotes[actor]=value;current.emoteUntil[actor]=now+std::chrono::seconds(3);
            const auto serial=++emoteSequence[actor];
            for(const auto& [id,peer]:peers)if(peer.slot!=255) {
                auto p=message(RoomMessage::Emote,peer.slot);p.actor=actor;p.emote=value;p.emoteSerial=serial;send(id,p);
            }
        };
        auto remove = [&](ConnectionId id) {
            const auto found = peers.find(id);
            if (found == peers.end()) return;
            if (found->second.slot != 255) {
                const auto slot = found->second.slot;
                current.connected &= uint8_t(~(1u << slot));
                current.ready &= uint8_t(~(1u << slot));
                inbox.Release(PlayerId(slot + 1));
            }
            peers.erase(found);
        };
        auto reject = [&](ConnectionId id, Tick reason) {
            auto p = message(RoomMessage::Reject, 255); p.tick = reason;
            send(id, p);
            transport.DisconnectAfterSend(id);
            remove(id);
        };
        bool sentReady = false, lastReady = false;
        auto nextStep = Clock::now();
        const auto connectDeadline = nextStep + std::chrono::seconds(10);
        auto lastAuthority = nextStep, retryAt = nextStep, reconnectUntil = nextStep;
        Tick lastAck = 0, resumeFrom = 0;
        bool forceState = false, awaitingApplication = false;
        std::optional<StateAssembler> assembling;
        std::optional<Tick> resumeTarget;
        auto beginReconnect = [&] {
            if (config.host || clientTicket == ResumeToken{} || current.localSlot == 255)
                throw std::runtime_error("LAN connection cannot resume");
            current.phase = RoomPhase::Reconnecting;
            reconnectUntil = Clock::now() + std::chrono::seconds(60);
            retryAt = Clock::now(); resumeTarget.reset();
            assembling.reset(); awaitingApplication = false;
            { std::lock_guard lock(mutex); commands.clear(); pendingState.reset(); stateApplied.reset(); }
        };
        std::optional<Tick> endTick;
        if (config.resume) {
            current.session = config.resume->session; current.localSlot = config.resume->slot; clientTicket = config.resume->token;
            forceState = true; beginReconnect();
            retryAt = Clock::now() + std::chrono::seconds(2);
            { std::lock_guard lock(mutex); identity = config.resume; }
        }
        std::optional<Tick> localStateBoundary, localReplayNext;
        auto restoreHostView = [&] {
            RoomState snapshot{current.nextTick, StateCodec::Encode(*session, config.content)};
            inbox.Release(1);
            current.restoringView = true;
            localStateBoundary = current.nextTick; localReplayNext.reset();
            std::lock_guard lock(mutex);
            frames.clear(); commands.clear();
            status.recoveryRequired = false;
            pendingState = std::move(snapshot);
        };
        uint8_t revoked=0;
        while (!stop) {
            uint8_t emote=0,kicks=0;
            bool ready, start, repair, overflow, restoreLocal, viewFailed;
            std::optional<Tick> applied;
            Tick delivered;
            std::vector<std::pair<Tick, sim::PadRecord>> inputs;
            {
                std::lock_guard lock(mutex);
                emote=pendingEmote;pendingEmote=0;kicks=pendingKicks;pendingKicks=0;
                ready = desiredReady; start = requestStart;
                repair = reconnectRequested; reconnectRequested = false; delivered = deliveredNext;
                if (fullStateRequested) { forceState = true; fullStateRequested = false; }
                overflow = status.recoveryRequired;
                viewFailed = status.viewFailed;
                applied = stateApplied; stateApplied.reset();
                restoreLocal = localStateRequested; localStateRequested = false;
                inputs.swap(commands);
            }
            if(config.host && kicks){
                revoked|=kicks;
                for(uint8_t slot=0;slot<config.humans;++slot)if(kicks&(1u<<slot)){
                    tickets[slot]={};config.admissions[slot]={};inbox.Release(PlayerId(slot+1));
                    std::vector<ConnectionId> removed;
                    for(const auto& [id,peer]:peers)if(peer.slot==slot)removed.push_back(id);
                    for(auto id:removed)reject(id,7);
                }
                broadcast(RoomMessage::Status);
            }
            if(emote && current.phase==RoomPhase::Racing && !viewFailed) {
                if(config.host)publishEmote(current.localSlot,emote);
                else {auto p=message(RoomMessage::Emote,current.localSlot);p.actor=current.localSlot;p.emote=emote;
                    try{send(server,p);}catch(const std::runtime_error&){/* Cosmetic event may be dropped during reconnect. */}}
            }
            if (viewFailed) {
                inputs.clear();
                if (config.host && !config.dedicated) inbox.Release(1);
            }
            if (config.host && applied) {
                if (!localStateBoundary || *applied != *localStateBoundary)
                    throw std::runtime_error("LAN host view application boundary mismatch");
                ++current.stateRestores;
                if (!history.CanResume(*applied)) restoreHostView();
                else { localReplayNext = *applied; localStateBoundary.reset(); }
                inputs.clear();
            }
            // Consume an outstanding application ACK before replacing its boundary.
            // The consumer can report a new mismatch while draining that snapshot's replay.
            if (config.host && restoreLocal && (!current.restoringView || overflow) &&
                (current.phase == RoomPhase::Racing || current.phase == RoomPhase::Complete)) {
                restoreHostView(); inputs.clear();
            }
            if (!config.host && (current.phase == RoomPhase::Racing || current.phase == RoomPhase::Complete || current.phase == RoomPhase::Recovering) &&
                (repair || (current.phase == RoomPhase::Racing && Clock::now() - lastAuthority > std::chrono::seconds(1)))) {
                forceState = forceState || overflow;
                beginReconnect(); inputs.clear();
            }
            if (!config.host && current.phase == RoomPhase::Reconnecting) {
                if (Clock::now() >= reconnectUntil) throw std::runtime_error("LAN reconnect timed out");
                if (Clock::now() >= retryAt) {
                    try { transport.Disconnect(server); transport.Flush(); } catch (const std::invalid_argument&) {}
                    server = transport.Connect(config.address, config.port);
                    retryAt = Clock::now() + std::chrono::seconds(2);
                }
            }
            TransportEvent event;
            for (unsigned i = 0; i < 128 && transport.Poll(event); ++i) {
                if (!config.host && event.connection != server) continue; // stale transport generation
                if (event.kind == TransportEvent::Kind::Connected) {
                    if (config.host) { Peer peer; peer.admittedAt = Clock::now(); peers.emplace(event.connection, peer); }
                    else {
                        if (current.phase == RoomPhase::Reconnecting) {
                            auto p = message(RoomMessage::Resume, current.localSlot);
                            resumeFrom = current.nextTick;
                            p.tick = forceState ? UINT32_MAX : resumeFrom; p.connected = p.ready = 0; p.token = clientTicket;
                            try { send(server, p); } catch (const std::runtime_error&) { retryAt = Clock::now(); }
                        } else {
                            RoomPacket hello; hello.names[0]=config.nickname; hello.cars = config.cars; hello.humans = config.humans; hello.content = config.content;
                            hello.expectedSession = config.expectedSession;
                            hello.slot=config.admissionSlot;hello.token=config.admission;
                            send(server, hello);
                        }
                    }
                    continue;
                }
                if (event.kind == TransportEvent::Kind::Disconnected) {
                    if (!config.host) {
                        if (current.phase == RoomPhase::Racing || current.phase == RoomPhase::Recovering) { beginReconnect(); inputs.clear(); }
                        else if (current.phase != RoomPhase::Complete && current.phase != RoomPhase::Reconnecting) throw std::runtime_error("LAN host disconnected");
                        continue;
                    }
                    remove(event.connection); broadcast(RoomMessage::Status);
                    continue;
                }
                if (config.host && (!peers.contains(event.connection) || failedPeers.contains(event.connection))) continue;
                if (event.channel == Channel::Control) {
                    if (!config.host && assembling) {
                        assembling->Feed(event.bytes);
                        if (assembling->Complete()) {
                            auto state = assembling->Take();
                            current.nextTick = state.nextTick;
                            { std::lock_guard lock(mutex); pendingState = std::move(state); }
                            assembling.reset(); awaitingApplication = true;
                        }
                        continue;
                    }
                    SetupPacket query;
                    if (config.host && DecodeSetupPacket(event.bytes, query) && query.kind == SetupMessage::Request && peers.at(event.connection).slot == 255) {
                        if (current.phase != RoomPhase::Lobby && !query.running) { reject(event.connection, 3); continue; }
                        if (current.connected == fullMask && !query.running) { reject(event.connection, 4); continue; }
                        if (!config.setup) { reject(event.connection, 2); continue; }
                        query.kind = SetupMessage::Offer; query.session = current.session;
                        query.content = config.content; query.setup = *config.setup;
                        query.running = current.phase != RoomPhase::Lobby;
                        sendBytes(event.connection, Channel::Control, EncodeSetupPacket(query));
                        transport.DisconnectAfterSend(event.connection);
                        remove(event.connection);
                        continue;
                    }
                    RoomPacket p;
                    if (!DecodeRoomPacket(event.bytes, p)) {
                        if (config.host) { reject(event.connection, 1); continue; }
                        throw std::runtime_error("invalid LAN room message");
                    }
                    if (config.host) {
                        auto& peer = peers.at(event.connection);
                        if (p.kind == RoomMessage::Hello && peer.slot == 255) {
                            if (current.phase != RoomPhase::Lobby) { reject(event.connection, 3); continue; }
                            if (p.content != config.content || p.cars != config.cars || p.humans != config.humans ||
                                (p.expectedSession && p.expectedSession != current.session)) {
                                reject(event.connection, 2); continue;
                            }
                            uint8_t slot = config.dedicated ? 0 : 1;
                            const bool reserved=std::any_of(config.admissions.begin(),config.admissions.end(),[](const auto& t){return t!=ResumeToken{};});
                            if(reserved){
                                if(p.slot>=config.humans||p.token==ResumeToken{}||p.token!=config.admissions[p.slot]){reject(event.connection,6);continue;}
                                slot=p.slot;
                                if(current.connected & (1u<<slot)){reject(event.connection,4);continue;}
                            }else{
                                if(p.slot!=255||p.token!=ResumeToken{}){reject(event.connection,6);continue;}
                                while (slot < config.humans && (current.connected & (1u << slot))) ++slot;
                            }
                            if (slot == config.humans || (revoked & (1u<<slot))) { reject(event.connection, 4); continue; }
                            peer.slot = slot;
                            current.names[slot]=p.names[0].empty()?"Player "+std::to_string(slot+1):p.names[0];
                            tickets[slot] = newTicket();
                            current.connected |= uint8_t(1u << slot);
                            auto welcome = message(RoomMessage::Welcome, slot); welcome.token = tickets[slot];
                            send(event.connection, welcome);
                            broadcast(RoomMessage::Status);
                        } else if (p.kind == RoomMessage::Resume && peer.slot == 255) {
                            if (p.session != current.session || p.content != config.content || p.cars != config.cars ||
                                p.humans != config.humans || (revoked & (1u<<p.slot)) || p.token != tickets[p.slot]) { reject(event.connection, 6); continue; }
                            if ((current.phase != RoomPhase::Racing && current.phase != RoomPhase::Complete) ||
                                (p.tick != UINT32_MAX && p.tick > current.nextTick)) {
                                reject(event.connection, 5); continue;
                            }
                            // A valid ticket replaces the old transport even if its timeout has
                            // not fired. Release all queued controls before restoring ownership.
                            std::vector<ConnectionId> previous;
                            for (const auto& [id, owner] : peers) if (id != event.connection && owner.slot == p.slot) previous.push_back(id);
                            for (auto id : previous) { transport.Disconnect(id); remove(id); }
                            inbox.Release(PlayerId(p.slot + 1));
                            peer.slot = p.slot;
                            current.connected |= uint8_t(1u << p.slot);
                            if (p.tick == UINT32_MAX || !history.CanResume(p.tick)) {
                                const auto bytes = StateCodec::Encode(*session, config.content);
                                peer.awaitingState = current.nextTick;
                                auto begin = message(RoomMessage::StateBegin, p.slot); begin.tick = current.nextTick;
                                send(event.connection, begin);
                                for (const auto& chunk : EncodeStateChunks(current.session, current.nextTick, bytes))
                                    sendBytes(event.connection, Channel::Control, chunk);
                            } else {
                                peer.replayNext = p.tick; peer.replayAck = p.tick;
                                auto resumed = message(RoomMessage::Resumed, p.slot);
                                resumed.tick = current.nextTick; resumed.token = tickets[p.slot];
                                send(event.connection, resumed);
                            }
                            broadcast(RoomMessage::Status);
                        } else if (p.kind == RoomMessage::StateAck && peer.awaitingState && p.tick == *peer.awaitingState &&
                                   p.slot == peer.slot && p.session == current.session && p.content == config.content &&
                                   p.cars == config.cars && p.humans == config.humans) {
                            if (!history.CanResume(p.tick)) { reject(event.connection, 5); continue; }
                            peer.replayNext = p.tick; peer.replayAck = p.tick; peer.awaitingState.reset();
                        } else if (p.kind == RoomMessage::ReplayAck && peer.slot != 255 && p.slot == peer.slot &&
                                   p.session == current.session && p.content == config.content && p.cars == config.cars && p.humans == config.humans &&
                                   p.tick <= current.nextTick) {
                            if (peer.replayNext && p.tick > *peer.replayNext) { reject(event.connection, 1); continue; }
                            peer.replayAck = std::max(peer.replayAck, p.tick);
                        } else if(p.kind==RoomMessage::Emote && peer.slot!=255 && p.actor==peer.slot && p.slot==peer.slot &&
                                  !p.emoteSerial && p.session==current.session && p.content==config.content &&
                                  p.cars==config.cars && p.humans==config.humans) {
                            if(current.phase==RoomPhase::Racing)publishEmote(peer.slot,p.emote);
                        } else if (p.kind == RoomMessage::Ready && peer.slot != 255 && current.phase == RoomPhase::Lobby &&
                                   p.session == current.session && p.slot == peer.slot && p.content == config.content &&
                                   p.cars == config.cars && p.humans == config.humans) {
                            const uint8_t mask = uint8_t(1u << peer.slot);
                            current.ready = uint8_t((current.ready & ~mask) | (p.ready ? mask : 0));
                            broadcast(RoomMessage::Status);
                        } else { reject(event.connection, 1); continue; }
                    } else {
                        if (p.kind == RoomMessage::Reject && p.tick==7) throw std::runtime_error("Removed by the room host.");
                        if (p.kind == RoomMessage::Reject) throw std::runtime_error("LAN admission rejected (reason " + std::to_string(p.tick) + ")");
                        if (p.content != config.content || p.cars != config.cars || p.humans != config.humans ||
                            (config.expectedSession && p.session != config.expectedSession) ||
                            (current.session && (p.session != current.session || p.slot != current.localSlot)))
                            throw std::runtime_error("LAN setup or ownership mismatch");
                        if(p.kind==RoomMessage::Emote) {
                            if(p.emoteSerial>emoteSequence[p.actor]) {
                                emoteSequence[p.actor]=p.emoteSerial;
                                current.emotes[p.actor]=p.emote;
                                current.emoteUntil[p.actor]=Clock::now()+std::chrono::seconds(3);
                            }
                            continue;
                        }
                        if (p.kind == RoomMessage::Welcome && current.phase == RoomPhase::Connecting) {
                            clientTicket = p.token;
                            current.localSlot = p.slot; current.session = p.session; current.phase = RoomPhase::Lobby;
                            { std::lock_guard lock(mutex); identity = ResumeIdentity{p.session, p.slot, p.token}; }
                        } else if (p.kind == RoomMessage::Start && current.phase == RoomPhase::Lobby) {
                            current.phase = RoomPhase::Racing; lastAuthority = Clock::now();
                        } else if (p.kind == RoomMessage::Resumed && current.phase == RoomPhase::Reconnecting && p.token == clientTicket) {
                            if (p.tick < resumeFrom) throw std::runtime_error("LAN resume target precedes retained state");
                            current.phase = RoomPhase::Recovering; resumeTarget = p.tick;
                            lastAuthority = Clock::now(); lastAck = 0; ++current.reconnects;
                            current.recoveredTicks += p.tick - resumeFrom;
                        } else if (p.kind == RoomMessage::StateBegin && current.phase == RoomPhase::Reconnecting) {
                            if (p.tick < resumeFrom) throw std::runtime_error("LAN snapshot precedes retained authority");
                            current.phase = RoomPhase::Recovering; resumeTarget = p.tick;
                            // Until application is acknowledged, nextTick may describe a
                            // downloaded snapshot that the presentation has not applied yet.
                            // A second disconnect must request another snapshot, not replay
                            // from that uncommitted boundary.
                            forceState = true;
                            assembling.emplace(current.session, p.tick);
                            ++current.reconnects;
                            // Old speculative/pending frames are superseded, but the main
                            // thread applies the snapshot before acknowledging this boundary.
                            { std::lock_guard lock(mutex); frames.clear(); commands.clear(); status.recoveryRequired = false; }
                        } else if (p.kind == RoomMessage::End && (current.phase == RoomPhase::Racing || current.phase == RoomPhase::Recovering || current.phase == RoomPhase::Reconnecting)) {
                            endTick = p.tick;
                        } else if (p.kind != RoomMessage::Status || current.phase == RoomPhase::Connecting)
                            throw std::runtime_error("unexpected LAN room transition");
                        current.connected = p.connected; current.ready = p.ready; current.names=p.names;
                    }
                } else if (config.host) {
                    auto& peer = peers.at(event.connection);
                    Packet p;
                    if (peer.slot == 255 || (current.phase != RoomPhase::Racing && current.phase != RoomPhase::Complete) || event.channel != Channel::Input ||
                        !DecodePacket(event.bytes, current.session, p) || p.kind != MessageKind::Input) {
                        reject(event.connection, 1); continue;
                    }
                    // Inputs already in flight at the terminal tick are valid, but cannot advance the race.
                    if(current.phase==RoomPhase::Complete)continue;
                    // The connection supplies ownership; there is no client-selected player ID.
                    if (!inbox.Submit(PlayerId(peer.slot + 1), p.tick, p.inputs[0]) &&
                        p.tick >= inbox.NextTick() && ++peer.invalidInputs > 30) { reject(event.connection, 1); continue; }
                } else {
                    Packet p;
                    // Start and authority use different channels. A valid authority frame may
                    // arrive first; retain it until the ordered Start control is processed.
                    if ((current.phase != RoomPhase::Racing && current.phase != RoomPhase::Lobby && current.phase != RoomPhase::Reconnecting && current.phase != RoomPhase::Recovering) ||
                        event.channel != Channel::Authority || !DecodePacket(event.bytes, current.session, p) ||
                        p.kind != MessageKind::Authority || p.cars != config.cars || p.tick != current.nextTick)
                        throw std::runtime_error("LAN authority sequence mismatch");
                    Frame(p); ++current.nextTick; lastAuthority = Clock::now();
                }
            }
            if (!config.host && current.phase == RoomPhase::Connecting && Clock::now() >= connectDeadline)
                throw std::runtime_error("LAN connection timed out");
            if (!config.host && applied && current.phase != RoomPhase::Reconnecting) {
                if (!awaitingApplication || !resumeTarget || *applied != *resumeTarget)
                    throw std::runtime_error("LAN snapshot application boundary mismatch");
                auto ack = message(RoomMessage::StateAck, current.localSlot);
                ack.tick = *applied; ack.connected = ack.ready = 0;
                try {
                    send(server, ack);
                    awaitingApplication = false; forceState = false; lastAck = *applied; ++current.stateRestores;
                } catch (const std::runtime_error&) { beginReconnect(); inputs.clear(); }
            }
            if (!config.host && current.phase == RoomPhase::Recovering) {
                if (!assembling && !awaitingApplication && resumeTarget && current.nextTick >= *resumeTarget) {
                    current.phase = RoomPhase::Racing; resumeTarget.reset(); lastAuthority = Clock::now();
                } else if (Clock::now() >= reconnectUntil) throw std::runtime_error("LAN catch-up timed out");
            }
            if (!config.host && !assembling && !awaitingApplication &&
                (current.phase == RoomPhase::Racing || current.phase == RoomPhase::Recovering) && delivered > lastAck) {
                auto ack = message(RoomMessage::ReplayAck, current.localSlot);
                ack.tick = delivered; ack.connected = ack.ready = 0;
                try { send(server, ack); lastAck = delivered; }
                catch (const std::runtime_error&) { beginReconnect(); inputs.clear(); }
            }
            if (config.host) {
                std::vector<ConnectionId> expired;
                for (const auto& [id, peer] : peers)
                    if (peer.slot == 255 && Clock::now() - peer.admittedAt > std::chrono::seconds(5)) expired.push_back(id);
                for (const auto id : expired) { transport.Disconnect(id); remove(id); }
            }
            if (current.phase == RoomPhase::Lobby) {
                if (config.host) {
                    if (!config.dedicated && bool(current.ready & 1) != ready) {
                        current.ready = uint8_t((current.ready & ~1u) | (ready ? 1 : 0));
                        broadcast(RoomMessage::Status);
                    }
                    if (start && (current.connected|revoked) == fullMask && (current.ready|revoked) == fullMask) {
                        broadcast(RoomMessage::Start);
                        current.phase = RoomPhase::Racing;
                        nextStep = Clock::now() + std::chrono::milliseconds(200);
                    }
                } else if (!sentReady || lastReady != ready) {
                    auto p = message(RoomMessage::Ready, current.localSlot);
                    p.connected = 0; p.ready = ready ? 1 : 0;
                    send(server, p); sentReady = true; lastReady = ready;
                }
            }
            if (current.phase == RoomPhase::Racing) {
                if (config.host && current.restoringView) inputs.clear();
                for (const auto& [tick, input] : inputs) {
                    if (config.host) inbox.Submit(1, tick, input);
                    else {
                        Packet p; p.session = current.session; p.tick = tick; p.inputs[0] = input;
                        try { transport.Send(server, Channel::Input, EncodePacket(p)); }
                        catch (const std::runtime_error&) { beginReconnect(); break; }
                    }
                }
                if (config.host) {
                    for (unsigned catchUp = 0; catchUp < 4 && Clock::now() >= nextStep; ++catchUp) {
                        Packet p;
                        p.kind = MessageKind::Authority; p.cars = config.cars; p.session = current.session;
                        p.tick = inbox.NextTick(); p.inputs = inbox.Consume();
                        session->Step(p.inputs);
                        if (session->NextTick() % 30 == 0 || session->Complete())
                            p.stateDigest = StateCodec::Digest(*session, config.content, session->Capture());
                        history.Append(p);
                        const auto bytes = EncodePacket(p);
                        for (const auto& [id, peer] : peers) if (peer.slot != 255 && !peer.replayNext && !peer.awaitingState)
                            sendBytes(id, Channel::Authority, bytes);
                        if (!config.dedicated && !current.restoringView) Frame(p);
                        current.nextTick = session->NextTick();
                        nextStep += std::chrono::nanoseconds(1000000000 / 30);
                        if (session->Complete()) { current.phase = RoomPhase::Complete; broadcast(RoomMessage::End); break; }
                    }
                    if (Clock::now() - nextStep > std::chrono::seconds(1)) throw std::runtime_error("LAN server simulation cannot keep up");
                }
            }
            if (config.host) {
                if (localReplayNext) {
                    if (!history.CanResume(*localReplayNext)) restoreHostView();
                    else {
                        // The host's renderer uses the same bounded drain window as a
                        // remote peer, without sharing the mutable authoritative race.
                        for (unsigned sent = 0; sent < 32 && *localReplayNext < current.nextTick &&
                             *localReplayNext - delivered < 128; ++sent) {
                            Frame(*history.Find(*localReplayNext)); ++*localReplayNext;
                        }
                        if (*localReplayNext == current.nextTick) {
                            localReplayNext.reset(); current.restoringView = false;
                        }
                    }
                }
                std::vector<ConnectionId> expired;
                for (auto& [id, peer] : peers) if (peer.replayNext) {
                    for (unsigned sent = 0; sent < 32 && *peer.replayNext < current.nextTick &&
                         *peer.replayNext - peer.replayAck < 128; ++sent) {
                        const auto* p = history.Find(*peer.replayNext);
                        if (!p) { expired.push_back(id); break; }
                        sendBytes(id, Channel::Authority, EncodePacket(*p)); ++*peer.replayNext;
                    }
                    if (*peer.replayNext == current.nextTick) {
                        peer.replayNext.reset();
                        if (current.phase == RoomPhase::Complete) {
                            auto end = message(RoomMessage::End, peer.slot); end.tick = current.nextTick; send(id, end);
                        }
                    }
                }
                for (auto id : expired) reject(id, 5);
            }
            if (endTick && current.nextTick == *endTick && current.phase == RoomPhase::Racing) current.phase = RoomPhase::Complete;
            if (endTick && current.nextTick > *endTick) throw std::runtime_error("LAN terminal tick mismatch");
            if (!failedPeers.empty()) {
                const auto failed = std::move(failedPeers);
                failedPeers.clear();
                for (const auto id : failed) if (peers.contains(id)) { transport.Disconnect(id); remove(id); }
                broadcast(RoomMessage::Status);
            }
            Publish(current);
            transport.Flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (config.host) for (const auto& [id, peer] : peers) { (void)peer; transport.Disconnect(id); }
        else if (current.phase != RoomPhase::Connecting) {
            try { transport.Disconnect(server); } catch (const std::invalid_argument&) {}
        }
        transport.Flush();
        current.phase = RoomPhase::Closed;
        Publish(current);
    }
};
LanRoom::LanRoom(LanRoomConfig config, std::unique_ptr<sim::RaceSim> authority)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(authority))) {}
LanRoom::~LanRoom() = default;
RoomStatus LanRoom::Status() const { std::lock_guard lock(impl_->mutex); return impl_->status; }
std::optional<ResumeIdentity> LanRoom::Identity() const { std::lock_guard lock(impl_->mutex); return impl_->identity; }
void LanRoom::SendEmote(uint8_t emote) {
    if(emote<1||emote>6)return;
    std::lock_guard lock(impl_->mutex);
    if(impl_->status.phase==RoomPhase::Racing)impl_->pendingEmote=emote;
}
void LanRoom::Kick(uint8_t slot) { if(!impl_->config.host || slot>=impl_->config.humans)return;std::lock_guard lock(impl_->mutex);impl_->pendingKicks|=uint8_t(1u<<slot); }
void LanRoom::SetReady(bool ready) { std::lock_guard lock(impl_->mutex); impl_->desiredReady = ready; }
void LanRoom::RequestStart() {
    if (!impl_->config.host) throw std::logic_error("only the host can start a LAN room");
    std::lock_guard lock(impl_->mutex); impl_->requestStart = true;
}
bool LanRoom::Reconnect(bool fullState) {
    std::lock_guard lock(impl_->mutex);
    const auto& s = impl_->status;
    if (impl_->config.host || s.viewFailed || (s.phase != RoomPhase::Racing && s.phase != RoomPhase::Complete && s.phase != RoomPhase::Recovering)) return false;
    impl_->reconnectRequested = true;
    impl_->fullStateRequested = fullState || impl_->status.recoveryRequired;
    return true;
}
bool LanRoom::RecoverView() {
    if (!impl_->config.host) return Reconnect(true);
    std::lock_guard lock(impl_->mutex);
    const auto& status = impl_->status;
    if (impl_->config.dedicated) return false;
    if ((status.phase != RoomPhase::Racing && status.phase != RoomPhase::Complete) || status.restoringView || status.viewFailed) return false;
    impl_->localStateRequested = true;
    return true;
}
bool LanRoom::VerifyState(const Packet& frame, const RaceSession& session, const PredictionTimeline& prediction) {
    if (!frame.stateDigest) return true;
    if (frame.kind != MessageKind::Authority || frame.tick == UINT32_MAX || prediction.ConfirmedNextTick() != frame.tick + 1)
        throw std::logic_error("state verification requires the confirmed authority boundary");
    const auto digest = StateCodec::Digest(session, impl_->config.content, prediction.ConfirmedState());
    std::lock_guard lock(impl_->mutex);
    auto& s = impl_->status;
    if (s.viewFailed || s.recoveryRequired) return false;
    ++s.stateChecks;
    if (digest == *frame.stateDigest) return true;
    ++s.desyncs;
    impl_->frames.clear(); impl_->commands.clear();
    // A persistent build/setup divergence must not create an endless snapshot loop.
    s.viewFailed = s.desyncs > 3;
    s.recoveryRequired = !s.viewFailed;
    if (!s.viewFailed) {
        if (impl_->config.host) impl_->localStateRequested = true;
        else { impl_->reconnectRequested = true; impl_->fullStateRequested = true; }
    }
    return false;
}
std::optional<RoomState> LanRoom::TakeState() {
    std::lock_guard lock(impl_->mutex);
    auto state = std::move(impl_->pendingState); impl_->pendingState.reset(); return state;
}
void LanRoom::StateApplied(Tick nextTick) {
    std::lock_guard lock(impl_->mutex);
    impl_->stateApplied = nextTick; impl_->deliveredNext = nextTick;
}
bool LanRoom::Submit(Tick tick, const sim::PadRecord& input) {
    if (!ValidInput(input)) return false;
    std::lock_guard lock(impl_->mutex);
    const auto& s = impl_->status;
    if (impl_->config.dedicated || s.phase != RoomPhase::Racing || s.recoveryRequired || s.restoringView || s.viewFailed || tick < s.nextTick ||
        tick - s.nextTick >= InputInbox::kFutureWindow || impl_->commands.size() >= InputInbox::kFutureWindow) return false;
    for (const auto& [existing, value] : impl_->commands) if (existing == tick) return SameInput(value, input);
    impl_->commands.emplace_back(tick, input);
    return true;
}
std::vector<Packet> LanRoom::DrainFrames() {
    std::lock_guard lock(impl_->mutex);
    std::vector<Packet> result(impl_->frames.begin(), impl_->frames.end());
    impl_->frames.clear();
    if (!result.empty()) impl_->deliveredNext = result.back().tick + 1;
    return result;
}

struct LanSetupQuery::Impl {
    PrivateTransport transport;
    ConnectionId server;
    std::optional<SetupPacket> offer;
    std::string error;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool allowRunning;
    Impl(const std::string& address, uint16_t port, std::optional<RoomSecurity> security, bool running)
        : transport("0.0.0.0", 0, 1, security), server(transport.Connect(address, port)), allowRunning(running) {}
    ~Impl() {
        try { transport.Disconnect(server); transport.Flush(); }
        catch (const std::exception&) { /* Already disconnected. */ }
    }
};
LanSetupQuery::LanSetupQuery(const std::string& address, uint16_t port, std::optional<RoomSecurity> security, bool allowRunning)
    : impl_(std::make_unique<Impl>(address, port, security, allowRunning)) {}
LanSetupQuery::~LanSetupQuery() = default;
const std::optional<SetupPacket>& LanSetupQuery::Offer() const { return impl_->offer; }
const std::string& LanSetupQuery::Error() const { return impl_->error; }
void LanSetupQuery::Poll() {
    auto& q = *impl_;
    if (q.offer || !q.error.empty()) return;
    try {
        TransportEvent event;
        for (unsigned i = 0; i < 64 && q.transport.Poll(event); ++i) {
            if (event.connection != q.server) throw std::runtime_error("unexpected setup connection");
            if (event.kind == TransportEvent::Kind::Connected) {
                SetupPacket request; request.running = q.allowRunning;
                q.transport.Send(q.server, Channel::Control, EncodeSetupPacket(request));
            }
            else if (event.kind == TransportEvent::Kind::Disconnected) throw std::runtime_error("host closed setup query");
            else {
                SetupPacket p;
                if (event.channel != Channel::Control) throw std::runtime_error("invalid setup channel");
                if (!DecodeSetupPacket(event.bytes, p) || p.kind != SetupMessage::Offer) {
                    RoomPacket rejection;
                    if (DecodeRoomPacket(event.bytes, rejection) && rejection.kind == RoomMessage::Reject && rejection.tick == 3)
                        throw std::runtime_error("race has already started");
                    if (rejection.kind == RoomMessage::Reject && rejection.tick == 4) throw std::runtime_error("room is full");
                    throw std::runtime_error("host cannot provide a supported race setup");
                }
                q.offer = std::move(p);
                q.transport.Flush();
                q.transport.Disconnect(q.server); q.transport.Flush();
                return;
            }
        }
        q.transport.Flush();
        if (std::chrono::steady_clock::now() >= q.deadline) throw std::runtime_error("setup query timed out");
    } catch (const std::exception& e) { q.error = e.what(); }
}
} // namespace gt2::net
