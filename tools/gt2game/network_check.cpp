#include "platform/net/lobby_cars.h"
#include <deque>
#include "network_check.h"
#include "network_setup.h"
#include "game/net/protocol.h"
#include "game/net/state_codec.h"
#include "platform/net/enet_transport.h"
#include "platform/net/lan_room.h"
#include "gt2formats/exe_profile.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <thread>

namespace gt2game {
namespace sim = gt2::sim;
namespace net = gt2::net;
namespace {
void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void SetupNetwork(sim::RaceSim& race, const RaceData& data, size_t count, const net::Roster& roster, bool countdown = true,
                  const RaceOptions* chosen = nullptr) {
    RaceOptions options;
    options.countdown = countdown;
    auto shell = data.shell;
    shell.countdown = countdown;
    if (chosen) { options = *chosen; shell.lapCount = chosen->laps; shell.countdown = chosen->countdown; }
    auto slots = GridSlots(data, count, options);
    net::SetupNetworkRace(race, data.track, data.extras, data.constants, data.course,
        std::vector<sim::CarParams>(data.params.begin(), data.params.begin() + std::ptrdiff_t(count)),
        std::move(slots), shell, roster, options.botDifficulty);
}
net::Inputs Controls(net::Tick tick, size_t cars) {
    net::Inputs inputs{};
    for (size_t i = 0; i < cars; ++i) {
        auto& p = inputs[i];
        p.flags = 7;
        p.throttle = uint16_t(2500 + ((tick / 23 + i * 5) % 9) * 170);
        p.brake = tick > 100 && (tick + i * 23) % 210 < 25 ? 1900 : 0;
        p.steer = tick < 150 ? 0 : int16_t((int((tick / 17 + i * 3) % 9) - 4) * 340);
        p.shift = tick % 67 == 0 ? 1 : 0;
        if (i == 0) { // full native wheel fields, including clutch and direct gears
            const uint8_t clutch = tick % 121 < 5 ? 175 : 0;
            p.flags |= uint16_t(sim::kWheelPad | ((clutch & 15) << 4));
            p.reserved = uint8_t((clutch & 0xf0) | (tick < 120 ? 3 : 15));
        }
    }
    return inputs;
}
std::string StateDigest(const sim::RaceSim& race) {
    uint64_t hash = 14695981039346656037ull;
    for (const uint8_t byte : race.Snapshot()) { hash ^= byte; hash *= 1099511628211ull; }
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}
}

int NetworkStateCheck(const RaceData& data, size_t carCount, const std::string& input, const std::string& output) {
    using namespace net;
    // Diagnostic key only. Production callers must use the negotiated stock-content key.
    ContentKey content{}; content[0] = 6; content[31] = 1;
    Roster roster = RoomRoster(uint8_t(carCount), 2);
    sim::RaceSim sourceRace, restoredRace;
    SetupNetwork(sourceRace, data, carCount, roster);
    SetupNetwork(restoredRace, data, carCount, roster);
    RaceSession source(sourceRace, roster), restored(restoredRace, roster);
    PredictionTimeline prediction(restored);
    StateCodec::Restore(restored, content, StateCodec::Encode(source, content));
    Require(sourceRace.Snapshot() == restoredRace.Snapshot(), "state check: countdown restore differs");
    for (Tick tick = 0; tick < 1500; ++tick) source.Step(Controls(tick, carCount));
    auto bytes = StateCodec::Encode(source, content);
    if (!output.empty()) {
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        Require(bool(file), "state check: cannot write diagnostic snapshot");
    }
    if (!input.empty()) {
        std::ifstream file(input, std::ios::binary | std::ios::ate);
        Require(bool(file), "state check: cannot open diagnostic snapshot");
        const auto size = file.tellg();
        Require(size > 0 && size <= std::streamoff(StateCodec::kMaxBytes), "state check: invalid diagnostic file size");
        std::vector<uint8_t> remote(size_t(size), uint8_t(0));
        file.seekg(0); file.read(reinterpret_cast<char*>(remote.data()), std::streamsize(remote.size()));
        Require(bool(file) && remote == bytes, "state check: foreign snapshot differs from local authority");
        bytes = std::move(remote);
    }
    // A previously predicting client can jump farther than the 900-tick input history.
    for (Tick tick = 0; tick < 4; ++tick) prediction.Predict(Controls(tick + 50, carCount));
    StateCodec::Restore(restored, content, bytes);
    prediction.AcceptRestoredState();
    Require(prediction.PendingTicks() == 0 && prediction.ConfirmedNextTick() == 1500 &&
        sourceRace.Snapshot() == restoredRace.Snapshot() && source.Results() == restored.Results() &&
        StateCodec::Encode(restored, content) == bytes, "state check: initial restore differs");
    const auto stable = StateCodec::Encode(restored, content);
    auto reject = [&](const std::vector<uint8_t>& bad, const ContentKey& key) {
        bool rejected = false;
        try { StateCodec::Restore(restored, key, bad); } catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected && StateCodec::Encode(restored, content) == stable && restored.NextTick() == 1500,
            "state check: malformed restore accepted or partially mutated race");
    };
    auto wrongKey = content; ++wrongKey[0]; reject(bytes, wrongKey);
    for (size_t size : {size_t(0), size_t(7), size_t(8), size_t(70), bytes.size() - 1})
        reject(std::vector<uint8_t>(bytes.begin(), bytes.begin() + std::ptrdiff_t(size)), content);
    auto corrupt = bytes; corrupt[bytes.size() / 2] ^= 1; reject(corrupt, content);
    auto reseal = [](std::vector<uint8_t>& buffer) {
        uint64_t hash = 14695981039346656037ull;
        for (size_t i = 0; i < buffer.size() - 8; ++i) { hash ^= buffer[i]; hash *= 1099511628211ull; }
        for (size_t i = 0; i < 8; ++i) buffer[buffer.size() - 8 + i] = uint8_t(hash >> (i * 8));
    };
    // Recomputed checksum exercises the schema parser, not just the corruption guard.
    for (size_t offset : {size_t(0), size_t(4), size_t(6), size_t(7), size_t(40), size_t(44), size_t(76)}) {
        corrupt = bytes; corrupt[offset] = 255; reseal(corrupt); reject(corrupt, content);
    }
    corrupt = bytes; corrupt.insert(corrupt.end() - 8, 0); reseal(corrupt); reject(corrupt, content);
    for (Tick tick = 1500; tick < 1740; ++tick) {
        const auto controls = Controls(tick, carCount);
        const auto events = source.Step(controls);
        prediction.Predict(controls);
        Require(prediction.Confirm(tick, controls) == events, "state check: restored events differ");
        Require(sourceRace.Snapshot() == restoredRace.Snapshot() && source.Results() == restored.Results() &&
            StateCodec::Encode(source, content) == StateCodec::Encode(restored, content), "state check: continuation diverged");
    }
    std::printf("network state check: PASS, %zu cars, %zu canonical bytes, restore at tick 1500, 240 continuation ticks; "
        "atomic rejection and prediction reset; final digest %s\n", carCount, bytes.size(), StateDigest(sourceRace).c_str());
    return 0;
}

int NetworkRaceCheck(const RaceData& data, size_t carCount) {
    using namespace net;
    if (carCount < 2 || carCount > kSlots) throw std::invalid_argument("--network-check requires --cars 2..6");
    const auto grid=GridSlots(data,carCount,RaceOptions{});
    for(size_t i=0;i<grid.size();++i)for(size_t j=0;j<i;++j){
        const int64_t dx=int64_t(grid[i].x)-grid[j].x,dy=int64_t(grid[i].y)-grid[j].y;
        Require(dx*dx+dy*dy>=int64_t(3*4096)*(3*4096),"network check: overlapping starting grid slots");
    }
    Roster roster{};
    for (size_t i = 0; i < carCount; ++i) roster[i].player = PlayerId(101 + i);
    {
        sim::RaceSim digitalRace;
        SetupNetwork(digitalRace, data, carCount, roster, false);
        RaceSession digital(digitalRace, roster);
        Inputs pads{};
        for (size_t i = 0; i < carCount; ++i) pads[i].throttle = 1;
        digital.Step(pads);
        Require(digitalRace.CarAt(0).body.throttle > 0, "network check: digital throttle was not applied");
        for (size_t i = 1; i < carCount; ++i)
            Require(digitalRace.CarAt(i).body.throttle == digitalRace.CarAt(0).body.throttle,
                "network check: human pedal ramp depends on car index");
        // Invalid laps must not replace a best lap for any participant, including
        // those beyond the original game's two result records.
        const int lastCar = int(carCount - 1);
        sim::OnLapLine(digitalRace.Shell().Context(), lastCar, 1, 10000, 10000, 0, 0);
        sim::OnLapLine(digitalRace.Shell().Context(), lastCar, 1, 5000, 15000, 0, 1);
        Require(digital.Results()[carCount - 1].bestLap == 10000 && (data.shell.pointToPoint ?
                digital.Results()[carCount-1].finishTime==10000 && digital.Results()[carCount-1].lastLapValid : !digital.Results()[carCount - 1].lastLapValid),
            "network check: invalid lap replaced participant record");
    }
    {
        sim::RaceSim cleanRace, damagedRace;
        SetupNetwork(cleanRace,data,carCount,roster,false);SetupNetwork(damagedRace,data,carCount,roster,false);
        RaceSession clean(cleanRace,roster), damaged(damagedRace,roster);
        damagedRace.CarAt(0).body.wheels[1].damage=255;
        Inputs neutral{};neutral.fill(SafeIdleInput());
        for(int tick=0;tick<30;++tick){clean.Step(neutral);damaged.Step(neutral);
            for(int w=0;w<4;++w)Require(cleanRace.CarAt(0).body.wheels[w].steerAngle==damagedRace.CarAt(0).body.wheels[w].steerAngle,"network inherited wall damage bends neutral steering");}
        std::puts("network neutral steering after corner damage PASS");
    }
    {
        RaceOptions easy,hard;easy.countdown=hard.countdown=false;easy.botDifficulty=0;hard.botDifficulty=2;
        sim::RaceSim a,b;const auto solo=RoomRoster(1,1);
        SetupNetwork(a,data,1,solo,false,&easy);SetupNetwork(b,data,1,solo,false,&hard);
        RaceSession sa(a,solo),sb(b,solo);Inputs input{};input.fill(SafeIdleInput());input[0].throttle=3000;input[0].brake=0;
        for(int i=0;i<180;++i){sa.Step(input);sb.Step(input);Require(a.Snapshot()==b.Snapshot(),"bot difficulty changed human physics");}
        const auto mixed=RoomRoster(uint8_t(carCount),1);sim::RaceSim c,d;
        SetupNetwork(c,data,carCount,mixed,false,&easy);SetupNetwork(d,data,carCount,mixed,false,&hard);
        RaceSession sc(c,mixed),sd(d,mixed);bool different=false;
        for(int i=0;i<900;++i){sc.Step(input);sd.Step(input);if(c.CarAt(1).body.courseDistance!=d.CarAt(1).body.courseDistance)different=true;}
        Require(different,"bot difficulty had no effect on AI driving");
        std::puts("bot difficulty changes AI only; solo human physics unchanged PASS");
    }
    { // Burst delivery must not replay the same speculative tail for every packet.
        sim::RaceSim sequentialRace,batchRace;SetupNetwork(sequentialRace,data,carCount,roster);SetupNetwork(batchRace,data,carCount,roster);
        RaceSession sequential(sequentialRace,roster),batch(batchRace,roster);PredictionTimeline slow(sequential),fast(batch);
        for(Tick i=0;i<8;++i){slow.Predict(Controls(i+100,carCount));fast.Predict(Controls(i+100,carCount));}
        for(Tick i=0;i<8;++i)fast.StageAuthority(i,Controls(i,carCount));
        ContentKey key{};key[0]=1;
        for(Tick i=0;i<8;++i){Require(slow.Confirm(i,Controls(i,carCount))==fast.Confirm(i,Controls(i,carCount)),"batch correction effects differ");
            Require(StateCodec::Digest(sequential,key,slow.ConfirmedState())==StateCodec::Digest(batch,key,fast.ConfirmedState()),"batch confirmed boundary differs");}
        Require(sequentialRace.Snapshot()==batchRace.Snapshot()&&fast.ReplayedTicks()<slow.ReplayedTicks(),"batch replay did not preserve state or reduce work");
        std::printf("Batch authority: same state/events, replayed %llu -> %llu ticks\n",static_cast<unsigned long long>(slow.ReplayedTicks()),static_cast<unsigned long long>(fast.ReplayedTicks()));
    }
    sim::RaceSim serverRace, clientRace;
    SetupNetwork(serverRace, data, carCount, roster);
    SetupNetwork(clientRace, data, carCount, roster);
    RaceSession server(serverRace, roster), client(clientRace, roster);
    PredictionTimeline prediction(client, 120);
    InputInbox inbox(roster, carCount);
    std::vector<Inputs> authoritative;
    std::vector<std::vector<Event>> events;
    ContentKey verificationKey{}; verificationKey[0] = 6;
    std::vector<uint64_t> stateDigests;
    uint64_t confirmations = 0, contactSteps = 0, wallSteps = 0, digest = 14695981039346656037ull;
    auto confirm = [&] {
        const Tick tick = prediction.ConfirmedNextTick();
        Require(prediction.Confirm(tick, authoritative[tick]) == events[tick], "network check: confirmed events differ");
        Require(prediction.Confirm(tick, authoritative[tick]).empty(), "network check: duplicate effects");
        const auto beforeCheck = clientRace.Snapshot();
        Require(StateCodec::Digest(client, verificationKey, prediction.ConfirmedState()) == stateDigests[tick],
            "network check: confirmed digest differs while prediction is ahead");
        Require(clientRace.Snapshot() == beforeCheck, "network check: digest mutated speculative state");
        ++confirmations;
    };
    constexpr Tick steps = 1800;
    for (Tick tick = 0; tick < steps; ++tick) {
        const auto controls = Controls(tick, carCount);
        for (size_t car = 0; car < carCount; ++car) {
            // Deliberate bursts of missing inputs, with recovery and retransmits.
            if ((tick + Tick(car) * 7) % 83 < 8) continue;
            Packet input;
            input.session = 42; input.tick = tick; input.inputs[0] = controls[car];
            Packet decoded;
            Require(DecodePacket(EncodePacket(input), 42, decoded), "network check: decode input");
            Require(inbox.Submit(roster[car].player, decoded.tick, decoded.inputs[0]), "network check: accepted input rejected");
            Require(inbox.Submit(roster[car].player, decoded.tick, decoded.inputs[0]), "network check: retransmit rejected");
        }
        const auto frame = inbox.Consume();
        authoritative.push_back(frame);
        events.push_back(server.Step(frame));
        stateDigests.push_back(StateCodec::Digest(server, verificationKey, server.Capture()));
        for (size_t car = 0; car < carCount; ++car) {
            const auto& body = serverRace.CarAt(car).body;
            if (body.contactFlags & 2) ++contactSteps;
            if (body.contactFlags & 1) ++wallSteps;
        }
        for (const uint8_t b : serverRace.Snapshot()) { digest ^= b; digest *= 1099511628211ull; }
        auto guessed = controls;
        for (size_t car = 1; car < carCount; ++car) guessed[car] = tick ? authoritative[tick - 1][car] : SafeIdleInput();
        prediction.Predict(guessed);
        // 6..17 ticks (200..567 ms) of delayed authority, then drain in order.
        const size_t lag = 6 + (tick / 53) % 12;
        while (prediction.PendingTicks() > lag) confirm();
        if (tick % 137 == 136 || tick + 1 == steps) {
            while (prediction.PendingTicks()) confirm();
            Require(serverRace.Snapshot() == clientRace.Snapshot(), "network check: state diverged after correction");
            Require(server.Results() == client.Results(), "network check: participant results diverged");
        }
    }
    Require(prediction.ReplayedTicks() > steps, "network check: corrections did not exercise resimulation");
    // A mid-race checkpoint must reproduce contact history and native wheel state.
    const auto checkpoint = server.Capture();
    for (Tick i = 0; i < 90; ++i) server.Step(Controls(i + steps, carCount));
    const auto expected = serverRace.Snapshot();
    const auto expectedResults = server.Results();
    server.Restore(checkpoint);
    for (Tick i = 0; i < 90; ++i) server.Step(Controls(i + steps, carCount));
    Require(serverRace.Snapshot() == expected && server.Results() == expectedResults, "network check: checkpoint continuation differs");
    const auto beforeRejectedRestore = clientRace.Snapshot();
    bool rejected = false;
    try { client.Restore(checkpoint); } catch (const std::exception&) { rejected = true; }
    Require(rejected && clientRace.Snapshot() == beforeRejectedRestore, "network check: another race's checkpoint accepted");

    // Mixed human/AI grids use the same authority and restore path.
    roster[1].player = 0;
    sim::RaceSim mixedRace;
    SetupNetwork(mixedRace, data, carCount, roster, false);
    RaceSession mixed(mixedRace, roster);
    const auto mixedStart = mixed.Capture();
    for (Tick i = 0; i < 300; ++i) mixed.Step(Controls(i, carCount));
    const auto mixedExpected = mixedRace.Snapshot();
    mixed.Restore(mixedStart);
    for (Tick i = 0; i < 300; ++i) mixed.Step(Controls(i, carCount));
    Require(mixedRace.Snapshot() == mixedExpected, "network check: AI continuation differs");

    // Finishing bots must not start a human race deadline; humans need not wait for bots.
    if(carCount>=3){
        const auto people=RoomRoster(uint8_t(carCount),2);
        sim::RaceSim policyRace;SetupNetwork(policyRace,data,carCount,people,false);
        RaceSession policy(policyRace,people);
        const auto& hooks=policyRace.Shell().Hooks();
        hooks.finished(hooks.user,2,1,100);
        Require(!policy.Capture().finishing,"bot finish started human deadline");
        Inputs idle{};idle.fill(SafeIdleInput());
        hooks.finished(hooks.user,0,2,200);
        Require(policy.Capture().finishing,"human finish did not start deadline");
        for(unsigned i=0;i<900;++i){policy.Step(idle);Require(!policy.Complete(),"human deadline shorter than thirty seconds");}
        policy.Step(idle);Require(policy.Complete()&&policy.Results()[1].dnf,"thirty-second deadline failed");
        sim::RaceSim allRace;SetupNetwork(allRace,data,carCount,people,false);RaceSession all(allRace,people);
        const auto& allHooks=allRace.Shell().Hooks();allHooks.finished(allHooks.user,0,1,100);allHooks.finished(allHooks.user,1,2,200);
        all.Step(idle);Require(all.Complete(),"finished humans were forced to wait for bots");
        std::puts("human finish policy: bots ignored, thirty seconds, all humans complete PASS");
    }

    // Drive a real AI race to its finish. A one-tick finish deadline exercises
    // authoritative DNF and terminal events without relying on synthetic hooks.
    sim::RaceSim finishRace;
    Roster bots{};
    SetupNetwork(finishRace, data, carCount, bots, false);
    RaceSession finishSession(finishRace, bots, 1);
    Inputs empty{};
    uint32_t finishEvents = 0, terminalEvents = 0;
    for (Tick i = 0; i < 18000 && !finishSession.Complete(); ++i) {
        const auto before = finishSession.Capture();
        const auto finalEvents = finishSession.Step(empty);
        for (const auto& event : finalEvents) {
            if (event.kind == EventKind::Finish) ++finishEvents;
            if (event.kind == EventKind::RaceEnd) ++terminalEvents;
        }
        if (finishSession.Complete()) {
            const auto finalState = finishRace.Snapshot();
            const auto finalResults = finishSession.Results();
            finishSession.Restore(before);
            Require(finishSession.Step(empty) == finalEvents && finishSession.Results() == finalResults &&
                finishRace.Snapshot() == finalState, "network check: finish rollback differs");
        }
    }
    Require(finishSession.Complete() && finishEvents > 0 && terminalEvents == 1, "network check: AI race did not finish once");
    {
        ContentKey key{}; key[0] = 6;
        sim::RaceSim recoveredRace;
        SetupNetwork(recoveredRace, data, carCount, bots, false);
        RaceSession recovered(recoveredRace, bots, 1);
        StateCodec::Restore(recovered, key, StateCodec::Encode(finishSession, key));
        Require(StateCodec::Digest(recovered, key, recovered.Capture()) == StateCodec::Digest(finishSession, key, finishSession.Capture()),
            "network check: restored terminal digest differs");
        Require(recovered.Complete() && recovered.Results() == finishSession.Results() &&
            recoveredRace.Snapshot() == finishRace.Snapshot(), "network check: terminal snapshot differs");
        Require(recovered.Step(empty).empty(), "network check: terminal snapshot repeated finish events");
    }
    const auto frozenResults = finishSession.Results();
    for (Tick i = 0; i < 120; ++i)
        Require(finishSession.Step(empty).empty() && finishSession.Results() == frozenResults, "network check: finish effects repeated");

    // Full prediction history stalls before mutating the simulation. An out of
    // order confirmation is rejected and remains recoverable by the next frame.
    PredictionTimeline bounded(mixed, 2);
    bounded.Predict(Controls(0, carCount));
    bounded.Predict(Controls(1, carCount));
    const auto beforeFull = mixedRace.Snapshot();
    rejected = false;
    try { bounded.Predict(Controls(2, carCount)); } catch (const std::exception&) { rejected = true; }
    Require(rejected && mixedRace.Snapshot() == beforeFull, "network check: full history mutated simulation");
    rejected = false;
    try { bounded.Confirm(bounded.ConfirmedNextTick() + 1, empty); } catch (const std::exception&) { rejected = true; }
    Require(rejected && bounded.PendingTicks() == 2, "network check: out-of-order authority mutated history");
    bounded.Confirm(bounded.ConfirmedNextTick(), Controls(0, carCount));
    bounded.Confirm(bounded.ConfirmedNextTick(), Controls(1, carCount));
    const auto confirmedBoundary = mixedRace.Snapshot();
    const auto confirmedTick = mixed.NextTick();
    bounded.Predict(Controls(2, carCount));
    bounded.DiscardPrediction();
    Require(bounded.PendingTicks() == 0 && mixed.NextTick() == confirmedTick && mixedRace.Snapshot() == confirmedBoundary,
            "network check: discarding speculation changed the confirmed boundary");

    std::printf("network race check: PASS, %zu cars, %u authority ticks, %llu confirmations, %llu replayed ticks, "
                "%llu car-contact samples, %llu wall-contact samples, local-state digest %016llx\n",
        carCount, steps, static_cast<unsigned long long>(confirmations), static_cast<unsigned long long>(prediction.ReplayedTicks()),
        static_cast<unsigned long long>(contactSteps), static_cast<unsigned long long>(wallSteps), static_cast<unsigned long long>(digest));
    std::printf("network race finish: %u ticks, %u finish event(s), one terminal event; history and ownership guards passed\n",
        finishSession.NextTick() - 120, finishEvents);
    return 0;
}

// A bounded, explicitly invoked diagnostic, not a public game server. It uses
// scripted drivers and diagnostic snapshot hashes. Production admission still
// requires authentication and canonical content/state serialization.
int NetworkPeerCheck(const RaceData& data, size_t carCount, bool listen, const std::string& address, uint16_t port) {
    using namespace net;
    using Clock = std::chrono::steady_clock;
    constexpr Tick count = 600;
    constexpr uint64_t sessionId = 0x0600475432524143ull;
    if (carCount < 2 || carCount > kSlots) throw std::invalid_argument("peer check requires 2..6 cars");
    const auto grid=GridSlots(data,carCount,RaceOptions{});
    for(size_t i=0;i<grid.size();++i)for(size_t j=0;j<i;++j){
        const int64_t dx=int64_t(grid[i].x)-grid[j].x,dy=int64_t(grid[i].y)-grid[j].y;
        Require(dx*dx+dy*dy>=int64_t(3*4096)*(3*4096),"network check: overlapping starting grid slots");
    }
    Roster roster{};
    for (size_t i = 0; i < carCount; ++i) roster[i].player = PlayerId(101 + i);
    sim::RaceSim race;
    SetupNetwork(race, data, carCount, roster);
    RaceSession session(race, roster);
    InputInbox inbox(roster, carCount);
    PredictionTimeline prediction(session);
    EnetTransport transport(listen ? address : "0.0.0.0", listen ? port : 0, 1);
    ConnectionId peer = listen ? 0 : transport.Connect(address, port);
    const std::string greeting = "GT2NETTEST1|" + std::string(gt2::ActiveProfile().exeSha1) + "|" + data.trackName + "|" +
        std::to_string(carCount) + "|" + StateDigest(race);
    auto sendText = [&](const std::string& text) {
        transport.Send(peer, Channel::Control, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
    };
    auto predictTo = [&](Tick end) {
        while (session.NextTick() < end && session.NextTick() < count) {
            const Tick tick = session.NextTick();
            const auto frame = Controls(tick, carCount);
            prediction.Predict(frame);
            // Deliberately lose short input bursts. Authority must correct the
            // speculative driving instead of waiting for the missing packet.
            if (tick % 47 >= 4) {
                Packet input;
                input.session = sessionId; input.tick = tick; input.inputs[0] = frame[1];
                transport.Send(peer, Channel::Input, EncodePacket(input));
            }
        }
    };
    bool ready = false, doneSent = false, closing = false;
    std::optional<std::string> finalDigest;
    size_t acceptedRemote = 0;
    auto nextStep = Clock::now();
    const auto deadline = nextStep + std::chrono::seconds(45);
    std::printf("race peer diagnostic: %s %s:%u, %zu cars, %u ticks\n", listen ? "listen" : "connect", address.c_str(), port, carCount, count);
    std::fflush(stdout);
    while (Clock::now() < deadline) {
        TransportEvent event;
        for (unsigned poll = 0; poll < 128 && transport.Poll(event); ++poll) {
            if (event.kind == TransportEvent::Kind::Connected) {
                peer = event.connection;
                if (!listen) sendText(greeting);
            } else if (event.kind == TransportEvent::Kind::Disconnected) {
                if (listen && doneSent && session.NextTick() == count && acceptedRemote > count / 2) {
                    std::printf("race peer authority: PASS, %u ticks, %zu client inputs, final digest %s\n", count, acceptedRemote, StateDigest(race).c_str());
                    return 0;
                }
                if (!listen && closing) {
                    std::printf("race peer client: PASS, %u confirmed ticks, %llu resimulated ticks, final digest %s\n", count,
                        static_cast<unsigned long long>(prediction.ReplayedTicks()), StateDigest(race).c_str());
                    return 0;
                }
                throw std::runtime_error("race peer: early disconnect");
            } else if (event.channel == Channel::Control) {
                const std::string text(event.bytes.begin(), event.bytes.end());
                if (!ready) {
                    Require(text == greeting, "race peer: diagnostic setup mismatch");
                    ready = true;
                    nextStep = Clock::now() + std::chrono::milliseconds(200);
                    // Reply only after admission, so input on a separate ENet
                    // channel cannot overtake the client's setup agreement.
                    if (listen) sendText(greeting);
                    else predictTo(4);
                } else if (!listen && text.starts_with("DONE|")) {
                    finalDigest = text.substr(5);
                } else throw std::runtime_error("race peer: unexpected control message");
            } else {
                Require(ready, "race peer: data before setup agreement");
                Packet p;
                Require(DecodePacket(event.bytes, sessionId, p), "race peer: malformed packet");
                if (listen) {
                    Require(event.channel == Channel::Input && p.kind == MessageKind::Input, "race peer: expected input");
                    if (inbox.Submit(roster[1].player, p.tick, p.inputs[0])) ++acceptedRemote;
                } else {
                    Require(event.channel == Channel::Authority && p.kind == MessageKind::Authority && p.cars == carCount &&
                        p.tick == prediction.ConfirmedNextTick(), "race peer: authority order/schema mismatch");
                    predictTo(p.tick + 1);
                    prediction.Confirm(p.tick, p.inputs);
                    predictTo(prediction.ConfirmedNextTick() + 4);
                }
            }
        }
        if (listen && ready && session.NextTick() < count && Clock::now() >= nextStep) {
            const Tick tick = session.NextTick();
            const auto controls = Controls(tick, carCount);
            for (size_t car = 0; car < carCount; ++car)
                if (car != 1) Require(inbox.Submit(roster[car].player, tick, controls[car]), "race peer: local input rejected");
            const auto inputs = inbox.Consume();
            session.Step(inputs);
            Packet p;
            p.kind = MessageKind::Authority; p.cars = uint8_t(carCount); p.session = sessionId; p.tick = tick; p.inputs = inputs;
            transport.Send(peer, Channel::Authority, EncodePacket(p));
            // Absolute 30 Hz schedule; render and headset focus are absent here.
            nextStep += std::chrono::nanoseconds(1000000000 / 30);
        }
        if (listen && session.NextTick() == count && !doneSent) {
            sendText("DONE|" + StateDigest(race));
            doneSent = true;
        }
        if (!listen && prediction.ConfirmedNextTick() == count && finalDigest && !closing) {
            Require(prediction.PendingTicks() == 0 && *finalDigest == StateDigest(race), "race peer: final states differ");
            Require(prediction.ReplayedTicks() > 0, "race peer: no correction was exercised");
            transport.Disconnect(peer);
            closing = true;
        }
        transport.Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("race peer diagnostic timed out");
}
namespace {
// A diagnostic compatibility key only. Production rooms need a canonical hash
// of every simulation input, not this disc/profile + local snapshot comparison.
net::ContentKey DiagnosticRoomKey(const sim::RaceSim& initial) {
    const std::string value = StateDigest(initial) + std::string(gt2::ActiveProfile().exeSha1);
    net::ContentKey key{};
    for (size_t i = 0; i < value.size(); ++i) key[i % key.size()] ^= uint8_t(value[i]);
    return key;
}
struct RoomDriver {
    sim::RaceSim race;
    net::Inputs lastAuthority{};
    net::ContentKey content{};
    std::unique_ptr<net::RaceSession> session;
    std::unique_ptr<net::PredictionTimeline> prediction;
    std::unique_ptr<net::LanRoom> room;
    unsigned injectDesyncs = 0, injected = 0;
    std::deque<net::Packet> deferredAuthority;
    void Setup(const RaceData& data, const net::LanRoomConfig& config, const RaceOptions* options = nullptr) {
        content = config.content;
        const auto roster = net::RoomRoster(config.cars, config.humans);
        SetupNetwork(race, data, config.cars, roster, true, options);
        session = std::make_unique<net::RaceSession>(race, roster);
        prediction = std::make_unique<net::PredictionTimeline>(*session);
        std::unique_ptr<sim::RaceSim> authority;
        if (config.host) {
            authority = std::make_unique<sim::RaceSim>();
            SetupNetwork(*authority, data, config.cars, roster, true, options);
        }
        room = std::make_unique<net::LanRoom>(config, std::move(authority));
    }
    void Pump(net::Tick limit) {
        const auto status = room->Status();
        if (auto state = room->TakeState()) {
            Require(state->nextTick < limit, "room check: snapshot past diagnostic limit");
            net::StateCodec::Restore(*session, content, state->bytes, state->nextTick);
            prediction->AcceptRestoredState(); lastAuthority.fill(net::SafeIdleInput());
            deferredAuthority.clear();
            room->StateApplied(state->nextTick);
        }
        Require(status.phase != net::RoomPhase::Failed, status.error.c_str());
        if (status.viewFailed) return;
        if (status.recoveryRequired) { room->RecoverView(); return; }
        if (status.phase != net::RoomPhase::Racing && status.phase != net::RoomPhase::Complete &&
            status.phase != net::RoomPhase::Reconnecting && status.phase != net::RoomPhase::Recovering) return;
        if (injected < injectDesyncs && prediction->ConfirmedNextTick() >= 60 + injected * 60 &&
            status.stateRestores == injected && !status.restoringView && status.phase == net::RoomPhase::Racing) {
            prediction->DiscardPrediction();
            // Corrupt retained race results, not the checksum. Input rollback cannot
            // fix this change; verification must trigger a real state restore.
            auto damaged = session->Capture(); damaged.results[status.localSlot].bestLap = 123;
            session->Restore(damaged); ++injected;
        }
        auto predict = [&](net::Tick end) {
            while (session->NextTick() < end && session->NextTick() < limit) {
                const auto tick = session->NextTick();
                const auto controls = Controls(tick, session->CarCount());
                // Loss is intentional; ready-state and authority delivery remain reliable.
                if (tick % 47 >= 4) room->Submit(tick, controls[status.localSlot]);
                prediction->Predict(controls);
            }
        };
        // A diagnostic boundary is not a network acknowledgement: retain frames
        // beyond it for the next phase, rather than silently dropping authority.
        for (auto& packet : room->DrainFrames()) deferredAuthority.push_back(std::move(packet));
        while (!deferredAuthority.empty() && deferredAuthority.front().tick < limit) {
            const auto p = std::move(deferredAuthority.front()); deferredAuthority.pop_front();
            predict(p.tick + 1);
            prediction->Confirm(p.tick, p.inputs);
            if (!room->VerifyState(p, *session, *prediction)) return;
            lastAuthority = p.inputs;
        }
        predict(prediction->ConfirmedNextTick() + 4);
    }
};
}

int NetworkDirectoryCheck(const gt2::DiscImage& disc,const gt2::GtfsVolume& volume,const std::string& key,const std::string& track,unsigned players){
    using namespace net;using Clock=std::chrono::steady_clock;
    RoomInvitation endpoint=DefaultLobby();
    if(key!="public"){auto security=ReadHostKey(key).Public();security.invitation={};endpoint=RoomInvitation{"127.0.0.1",29610,security};}
    const auto catalogue=gt2::ArcadeMenuData::Load(disc,1);
    const auto trackIt=std::find_if(kLobbyTracks.begin(),kLobbyTracks.end(),[&](auto name){return track==name;});Require(trackIt!=kLobbyTracks.end(),"unknown directory track");
    LobbyOptions options;options.track=uint8_t(trackIt-kLobbyTracks.begin());options.players=uint8_t(players);options.carClass=LobbyTrackDirt(options.track)?6:1;options.carRule=players==2?0:1;
    const auto allowed=LobbyClassCars(catalogue,options.carClass);options.fixedCar=allowed.front();
    std::vector<std::unique_ptr<LobbyClient>> clients;for(unsigned i=0;i<players;++i)clients.push_back(std::make_unique<LobbyClient>(endpoint));
    auto wait=[&](bool allowError=false){const auto until=Clock::now()+std::chrono::seconds(30);bool busy=true;while(busy&&Clock::now()<until){busy=false;for(auto& c:clients){c->Poll();busy|=c->Busy();}std::this_thread::sleep_for(std::chrono::milliseconds(2));}Require(!busy,"directory request timed out");if(!allowError)for(auto& c:clients)Require(c->Error().empty(),c->Error().c_str());};
    wait();clients[0]->Create(options,"Six-player regression","private");wait();auto created=clients[0]->TakeCreated();Require(created&&created->phase==0,"room did not enter staging");const auto id=created->id;
    clients[1]->Join(id,"wrong");wait(true);Require(!clients[1]->TakeCreated()&&!clients[1]->Error().empty(),"wrong password accepted");std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    for(unsigned i=1;i<players;++i){clients[i]->Join(id,"private");wait();Require(clients[i]->TakeCreated().has_value(),"join failed");}
    // Rejected choice must not mutate the roster or disconnect other players.
    clients[0]->Update(id,"Driver0","not_a_car",0,false);wait(true);Require(!clients[0]->Error().empty(),"invalid car accepted");
    clients[0]->Update(id,"Driver0",catalogue.classes[0].cars.front(),0,false);wait(true);Require(!clients[0]->Error().empty(),"wrong class accepted");
    for(unsigned i=0;i<players;++i)clients[i]->Update(id,"Driver"+std::to_string(i),options.carRule?allowed[i%allowed.size()]:options.fixedCar,0,false);
    wait();for(auto& c:clients){auto room=c->TakeCreated();Require(room&&room->phase==0,"race started before Ready");}
    for(unsigned i=0;i+1<players;++i)clients[i]->Update(id,"Driver"+std::to_string(i),options.carRule?allowed[i%allowed.size()]:options.fixedCar,0,true);
    wait();for(unsigned i=0;i+1<players;++i){auto room=clients[i]->TakeCreated();Require(room&&room->phase==0,"ready barrier skipped missing player");}
    clients.back()->Update(id,"Driver"+std::to_string(players-1),options.carRule?allowed[(players-1)%allowed.size()]:options.fixedCar,0,true);wait();
    std::vector<LobbyRoom> offers(players);
    for(unsigned i=0;i<players;++i){if(i+1!=players){clients[i]->Update(id,"Driver"+std::to_string(i),options.carRule?allowed[i%allowed.size()]:options.fixedCar,0,true);wait();}auto room=clients[i]->TakeCreated();Require(room&&room->phase==1&&room->localSlot==i,"race slot lost after staging");offers[i]=*room;}
    LanSetupQuery query(endpoint.address,offers[0].port,offers[0].security);const auto until=Clock::now()+std::chrono::seconds(10);
    while(!query.Offer()&&query.Error().empty()&&Clock::now()<until){query.Poll();std::this_thread::sleep_for(std::chrono::milliseconds(2));}Require(query.Offer().has_value(),"race setup unavailable");auto setup=*query.Offer();Require(setup.setup.arcadeCars,"server omitted arcade physics");
    RaceData data;LoadRaceTrack(disc,volume,track,{},data);BuildNetworkCarRecords(volume,std::vector<std::string>(setup.setup.carIds.begin(),setup.setup.carIds.begin()+6),data);data.paints.assign(setup.setup.paints.begin(),setup.setup.paints.end());
    RaceOptions raceOptions;raceOptions.laps=setup.setup.laps;raceOptions.manual=setup.setup.manual;raceOptions.countdown=setup.setup.countdown;
    Require(NetworkContent(disc,volume,data,6,raceOptions,uint8_t(players))==setup.content,"client physics differs from server");
    { LanRoomConfig forged;forged.address=endpoint.address;forged.port=offers[0].port;forged.cars=6;forged.humans=uint8_t(players);forged.security=offers[0].security;forged.expectedSession=setup.session;forged.content=setup.content;forged.admissionSlot=1;forged.admission=offers[0].admission;
      LanRoom rejected(forged);const auto stop=Clock::now()+std::chrono::seconds(3);while(rejected.Status().phase!=RoomPhase::Failed&&Clock::now()<stop)std::this_thread::sleep_for(std::chrono::milliseconds(2));Require(rejected.Status().error.find("reason 6")!=std::string::npos,"another player's car slot was accepted"); }
    std::vector<std::unique_ptr<RoomDriver>> drivers(players);
    for(unsigned reverse=players;reverse>0;--reverse){const unsigned i=reverse-1;LanRoomConfig config;config.address=endpoint.address;config.port=offers[i].port;config.cars=6;config.humans=uint8_t(players);config.security=offers[i].security;config.expectedSession=setup.session;config.content=setup.content;config.admissionSlot=uint8_t(i);config.admission=offers[i].admission;config.nickname="Driver"+std::to_string(i);
        drivers[i]=std::make_unique<RoomDriver>();drivers[i]->Setup(data,config,&raceOptions);
        Require(data.carIds[i]==(options.carRule?allowed[i%allowed.size()]:options.fixedCar),"selected car replaced");
    }
    const auto deadline=Clock::now()+std::chrono::seconds(35);constexpr Tick ticks=300;bool done=false;
    while(Clock::now()<deadline){done=true;for(unsigned i=0;i<players;++i){auto& d=*drivers[i];auto status=d.room->Status();Require(status.phase!=RoomPhase::Failed,status.error.c_str());if(status.localSlot<players){Require(status.localSlot==i,"connection order stole another car");d.room->SetReady(true);}d.Pump(ticks);done&=d.prediction->ConfirmedNextTick()>=ticks;}if(done)break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    Require(done,"six-player authority did not reach target tick");
    const auto hash=StateCodec::Encode(*drivers[0]->session,setup.content);for(auto& d:drivers)Require(StateCodec::Encode(*d->session,setup.content)==hash,"six clients diverged");
    std::printf("directory race PASS: %s, %u humans, selected cars, reverse connection order, %u identical ticks\n",track.c_str(),players,ticks);
    return 0;
}

int NetworkRoomCheck(const RaceData& data) {
    using namespace net;
    using Clock = std::chrono::steady_clock;
    constexpr Tick count = 345;
    LanRoomConfig config;
    config.host = true; config.port = 0; config.cars = config.humans = 6; config.session = 0x6000600060006000ull;
    config.setup.emplace(); config.setup->humans = 6; config.setup->track = data.trackName;
    config.setup->laps = data.shell.lapCount;
    for (size_t i = 0; i < 6; ++i) { config.setup->carIds[i] = data.carIds[i]; config.setup->paints[i] = data.paints[i]; }
    const auto advertised = *config.setup;
    sim::RaceSim initial;
    SetupNetwork(initial, data, 6, RoomRoster(6, 6));
    { RaceSession prepared(initial, RoomRoster(6, 6)); config.content = DiagnosticRoomKey(initial); }
    std::array<std::unique_ptr<RoomDriver>, 6> drivers;
    drivers[0] = std::make_unique<RoomDriver>(); drivers[0]->Setup(data, config);
    const auto deadline = Clock::now() + std::chrono::seconds(25);
    while (!drivers[0]->room->Status().boundPort && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    config.host = false; config.session = 0; config.port = drivers[0]->room->Status().boundPort;
    config.setup.reset(); config.expectedSession = 0x6000600060006000ull;
    Require(config.port != 0, "room check: host did not bind");
    for (unsigned i = 0; i < 12; ++i) {
        LanSetupQuery query(config.address, config.port);
        while (!query.Offer() && query.Error().empty() && Clock::now() < deadline) {
            query.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(query.Offer() && query.Offer()->setup == advertised && query.Offer()->session == config.expectedSession &&
                query.Offer()->content == config.content, "room check: setup query differs from host");
        Require(drivers[0]->room->Status().connected == 1 && drivers[0]->room->Status().ready == 0,
                "room check: setup query allocated a player slot or readiness");
    }
    {
        auto stale = config; ++stale.expectedSession;
        LanRoom rejected(stale);
        while (rejected.Status().phase != RoomPhase::Failed && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Require(rejected.Status().error.find("reason 2") != std::string::npos && drivers[0]->room->Status().connected == 1,
                "room check: stale room pin did not reject before ownership");
    }
    for (size_t i = 1; i < drivers.size(); ++i) {
        drivers[i] = std::make_unique<RoomDriver>(); drivers[i]->Setup(data, config);
    }
    while (drivers[0]->room->Status().connected != 63 && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(drivers[0]->room->Status().connected == 63, "room check: six participants did not join");
    {
        LanSetupQuery query(config.address, config.port);
        while (!query.Offer() && query.Error().empty() && Clock::now() < deadline) {
            query.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(query.Error() == "room is full", "room check: full room advertised as joinable");
    }
    uint8_t occupied = 0;
    for (auto& driver : drivers) {
        while (driver->room->Status().phase == RoomPhase::Connecting && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto slot = driver->room->Status().localSlot;
        Require(slot < 6 && !(occupied & (1u << slot)), "room check: duplicated car ownership");
        occupied |= uint8_t(1u << slot);
    }
    auto checkReject = [&](bool mismatch) {
        auto rejectedConfig = config;
        if (mismatch) rejectedConfig.content[0] ^= 1;
        LanRoom extra(rejectedConfig);
        const auto until = Clock::now() + std::chrono::seconds(2);
        while (extra.Status().phase != RoomPhase::Failed && Clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Require(extra.Status().phase == RoomPhase::Failed, "room check: invalid admission was accepted");
        if (extra.Status().error.find(mismatch ? "reason 2" : "reason 4") == std::string::npos)
            throw std::runtime_error("room check: wrong admission rejection: " + extra.Status().error);
    };
    checkReject(true); checkReject(false);
    {
        EnetTransport outsider("127.0.0.1", 0, 1);
        const auto id = outsider.Connect(config.address, config.port);
        bool rejected = false;
        const auto until = Clock::now() + std::chrono::seconds(2);
        while (!rejected && Clock::now() < until) {
            TransportEvent event;
            if (!outsider.Poll(event, 5)) continue;
            if (event.kind == TransportEvent::Kind::Connected) {
                RoomPacket claim; claim.kind = RoomMessage::Resume; claim.cars = claim.humans = 6;
                claim.slot = drivers[1]->room->Status().localSlot; claim.session = config.expectedSession; claim.content = config.content;
                claim.token.fill(0xff);
                outsider.Send(id, Channel::Control, EncodeRoomPacket(claim)); outsider.Flush();
            } else if (event.kind == TransportEvent::Kind::Message) {
                RoomPacket response;
                rejected = DecodeRoomPacket(event.bytes, response) && response.kind == RoomMessage::Reject && response.tick == 6;
            }
        }
        Require(rejected && drivers[0]->room->Status().connected == 63, "room check: unauthorized resume changed ownership");
    }
    for (size_t i = 0; i < drivers.size() - 1; ++i) drivers[i]->room->SetReady(true);
    drivers[0]->room->RequestStart();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    Require(drivers[0]->room->Status().phase == RoomPhase::Lobby, "room check: start ignored readiness barrier");
    drivers[5]->room->SetReady(true);
    bool disconnected = false, pauseObserved = false, emoteSent=false, emoteReceived=false;
    unsigned repairs = 0;
    const auto resumedSlot = drivers[1]->room->Status().localSlot;
    uint8_t releasedSlot = 255;
    while (Clock::now() < deadline) {
        const auto tick = drivers[0]->room->Status().nextTick;
        if ((repairs == 0 && tick >= 100) || (repairs == 1 && tick >= 180)) {
            Require(drivers[1]->room->Reconnect(repairs == 1), "room check: client repair rejected"); ++repairs;
        }
        if(!emoteSent && tick>=10 && drivers[0]->room->Status().phase==RoomPhase::Racing) {
            drivers[1]->room->SendEmote(3);drivers[0]->room->SendEmote(6);emoteSent=true;
        }
        if(emoteSent && !emoteReceived && tick>=20) {
            for(const auto& driver:drivers) {
                const auto status=driver->room->Status();
                Require(status.emotes[resumedSlot]==3 && status.emotes[0]==6,
                        "room check: client/host emotes not delivered to every participant");
                Require(status.emoteUntil[resumedSlot]>Clock::now(),"room check: emote expired before display");
            }
            emoteReceived=true;
        }
        // Freeze the host presentation/input briefly. The independent worker and
        // other clients must continue. Never drain client 5: its bounded queue
        // must demand recovery without stalling the server or growing forever.
        for (size_t i = 0; i < 5; ++i) if (drivers[i] && !(i == 0 && tick >= 30 && tick < 55)) drivers[i]->Pump(count);
        if (tick >= 50 && tick < 55) pauseObserved = drivers[1]->prediction->ConfirmedNextTick() >= 45 &&
            SameInput(drivers[1]->lastAuthority[0], SafeIdleInput());
        if (!disconnected && tick >= 60) {
            releasedSlot = drivers[4]->room->Status().localSlot;
            drivers[4].reset(); disconnected = true;
        }
        if (drivers[0]->prediction->ConfirmedNextTick() == count && drivers[1]->prediction->ConfirmedNextTick() == count &&
            drivers[2]->prediction->ConfirmedNextTick() == count && drivers[3]->prediction->ConfirmedNextTick() == count) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(emoteReceived,"room check: emote scenario did not run");
    Require(pauseObserved && disconnected, "room check: pause/disconnect scenario did not run");
    Require(repairs == 2 && drivers[1]->room->Status().reconnects == 2 && drivers[1]->room->Status().localSlot == resumedSlot,
            "room check: reconnect did not preserve ownership");
    Require(drivers[1]->room->Status().stateRestores == 1, "room check: full-state repair was not applied");
    const auto host = drivers[0]->room->Status();
    Require(host.phase == RoomPhase::Racing && !(host.connected & (1u << releasedSlot)), "room check: disconnected ownership remained active");
    Require(drivers[5]->room->Status().recoveryRequired, "room check: stalled consumer history was not bounded");
    Require(SameInput(drivers[0]->lastAuthority[releasedSlot], SafeIdleInput()), "room check: disconnected driver did not brake");
    {
        LanSetupQuery query(config.address, config.port);
        while (!query.Offer() && query.Error().empty() && Clock::now() < deadline) {
            query.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(!query.Offer() && query.Error() == "race has already started", "room check: running race advertised as joinable");
    }
    for (size_t i = 0; i < 4; ++i) {
        Require(drivers[i]->prediction->ConfirmedNextTick() == count && drivers[i]->prediction->PendingTicks() == 0, "room check: authority did not drain");
        Require(drivers[i]->race.Snapshot() == drivers[0]->race.Snapshot(), "room check: participant states differ");
    }
    // Resume the previously stalled presentation consumer. Its discarded queue
    // requires a state transfer even though transport and host never stopped.
    constexpr Tick recoveredCount = 425;
    bool recoveredAll = false;
    while (Clock::now() < deadline) {
        recoveredAll = true;
        for (size_t i : {size_t(0), size_t(1), size_t(2), size_t(3), size_t(5)}) {
            drivers[i]->Pump(recoveredCount);
            recoveredAll = recoveredAll && drivers[i]->prediction->ConfirmedNextTick() == recoveredCount;
        }
        if (recoveredAll) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(recoveredAll && drivers[5]->room->Status().stateRestores == 1 && !drivers[5]->room->Status().recoveryRequired,
        "room check: stalled client did not restore state");
    for (size_t i : {size_t(1), size_t(2), size_t(3), size_t(5)})
        Require(drivers[i]->race.Snapshot() == drivers[0]->race.Snapshot(), "room check: recovered consumer state differs");
    std::printf("LAN room check: PASS, 6 participants, %u ticks, unique ownership, ready barrier, admission rejection, "
                "setup queries, stale session rejection, two reconnects including full state, host presentation pause, disconnect and "
                "stalled consumer recovery; final digest %s\n", recoveredCount, StateDigest(drivers[0]->race).c_str());
    return 0;
}

int NetworkInvitationCheck(const gt2::DiscImage& disc, const gt2::GtfsVolume& volume, const std::string& invitation, bool restart) {
    using namespace net; using Clock = std::chrono::steady_clock;
    (void)volume;
    const gt2::GtfsVolume physics(disc, gt2::GtfsVolume::Source::DiscOnly);
    const auto invite = ReadInvitation(invitation);
    LanSetupQuery query(invite.address, invite.port, invite.security);
    const auto deadline = Clock::now() + std::chrono::seconds(35);
    while (!query.Offer() && query.Error().empty() && Clock::now() < deadline) {
        query.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(query.Offer().has_value(), query.Error().empty() ? "invitation setup timed out" : query.Error().c_str());
    const auto& offer = *query.Offer(); const auto& setup = offer.setup;
    RaceData data; LoadRaceTrack(disc, physics, setup.track, {}, data);
    BuildCarRecords(physics, setup.carIds[0], setup.cars, {}, data,
        std::vector<std::string>(setup.carIds.begin() + 1, setup.carIds.begin() + setup.cars));
    data.paints.assign(setup.paints.begin(), setup.paints.begin() + setup.cars);
    RaceOptions options; options.laps = setup.laps; options.countdown = setup.countdown; options.manual = setup.manual;
    LanRoomConfig config; config.address = invite.address; config.port = invite.port; config.security = invite.security;
    config.nickname=restart?"DeckRestart":"DeckDriver";
    config.cars = setup.cars; config.humans = setup.humans; config.expectedSession = offer.session;
    config.content = NetworkContent(disc, physics, data, setup.cars, options, setup.humans);
    Require(config.content == offer.content, "invitation content differs from dedicated server");
    auto driver = std::make_unique<RoomDriver>(); driver->Setup(data, config, &options); driver->room->SetReady(true);
    bool restarted = false; constexpr Tick count = 300;
    std::optional<Clock::time_point> done;
    while (Clock::now() < deadline) {
        driver->Pump(count);
        if (restart && !restarted && driver->prediction->ConfirmedNextTick() >= 100) {
            config.resume = driver->room->Identity(); Require(config.resume.has_value(), "invitation has no player identity");
            driver.reset(); driver = std::make_unique<RoomDriver>(); driver->Setup(data, config, &options); restarted = true;
        }
        if (driver->prediction->ConfirmedNextTick() == count && !done) done = Clock::now();
        if (done && Clock::now() - *done > std::chrono::seconds(1)) {
            const auto state = driver->room->Status();
            Require(state.names[state.localSlot]==config.nickname,"nickname lost on admission or reconnect");
            Require(!state.desyncs && state.stateChecks > 0 && (!restart || (restarted && state.stateRestores == 1)),
                "invitation state verification or restart failed");
            std::printf("Invitation peer: PASS, slot %u, %u confirmed ticks, %u restores, %u checks, checksum %016llx\n",
                state.localSlot, count, state.stateRestores, state.stateChecks,
                static_cast<unsigned long long>(StateCodec::Digest(*driver->session, config.content, driver->session->Capture())));
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("invited race timed out");
}

int NetworkPrivateRoomCheck(const RaceData& data) {
    using namespace net;
    using Clock = std::chrono::steady_clock;
    Require(data.params.size() >= 6, "private room check requires six cars");
    LanRoomConfig host;
    host.host = host.dedicated = true; host.port = 0; host.session = NewSessionId(); host.security = NewRoomSecurity();
    host.setup.emplace(); host.setup->track = data.trackName; host.setup->laps = data.shell.lapCount;
    for (size_t i = 0; i < 6; ++i) { host.setup->carIds[i] = data.carIds[i]; host.setup->paints[i] = data.paints[i]; }
    auto authority = std::make_unique<sim::RaceSim>(); SetupNetwork(*authority, data, 6, RoomRoster(6, 2));
    { RaceSession initial(*authority, RoomRoster(6, 2)); host.content = DiagnosticRoomKey(*authority); }
    LanRoom server(host, std::move(authority)); server.RequestStart();
    const auto deadline = Clock::now() + std::chrono::seconds(25);
    while (!server.Status().boundPort && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(server.Status().localSlot == 255, "dedicated server reserved a player car");
    auto client = host; client.host = client.dedicated = false; client.session = 0; client.setup.reset();
    client.port = server.Status().boundPort; client.expectedSession = host.session; client.security = host.security->Public();
    auto first = std::make_unique<RoomDriver>(); first->Setup(data, client); first->room->SetReady(true);
    RoomDriver second; second.Setup(data, client); second.room->SetReady(true);
    std::optional<ResumeIdentity> identity;
    bool restarted = false;
    constexpr Tick count = 300;
    while (Clock::now() < deadline) {
        if (first) first->Pump(count);
        second.Pump(count);
        if (!identity && server.Status().nextTick >= 70) {
            identity = first->room->Identity(); Require(identity.has_value(), "private room has no reconnect credential");
            Require(identity->slot != second.room->Status().localSlot, "private room assigned duplicate cars");
            first.reset();
        }
        if (identity && !restarted && server.Status().nextTick >= 100) {
            LanSetupQuery query(client.address, client.port, client.security, true);
            while (!query.Offer() && query.Error().empty() && Clock::now() < deadline) {
                query.Poll(); second.Pump(count); std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Require(query.Offer() && query.Offer()->running && query.Offer()->session == identity->session,
                "private room cannot query active session for restart");
            client.resume = identity;
            first = std::make_unique<RoomDriver>(); first->Setup(data, client); restarted = true;
        }
        if (restarted && first->prediction->ConfirmedNextTick() == count && second.prediction->ConfirmedNextTick() == count) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(restarted && first->prediction->ConfirmedNextTick() == count && second.prediction->ConfirmedNextTick() == count,
        "private room restart did not finish");
    const auto state = first->room->Status();
    Require(state.localSlot == identity->slot && state.stateRestores == 1 && state.desyncs == 0 && state.stateChecks > 0,
        "private room restart lost ownership or state");
    // Compare portable simulation fields, not padding in native Car/Shell structs.
    const auto canonical = StateCodec::Encode(*first->session, client.content);
    Require(canonical == StateCodec::Encode(*second.session, client.content),
        "private room restart diverged from continuing player");
    std::printf("Private dedicated room: PASS, slots 0/1, process identity restored, %u ticks, identical %zu-byte canonical state\n",
        count, canonical.size());
    return 0;
}

int NetworkRoomPeerCheck(const RaceData& data, size_t carCount, bool listen, const std::string& address, uint16_t port, bool longRecovery, bool hostPause, unsigned desyncs) {
    using namespace net;
    using Clock = std::chrono::steady_clock;
    const Tick count = longRecovery ? 1500 : hostPause ? 480 : 300;
    LanRoomConfig config;
    config.host = listen; config.address = address; config.port = port; config.cars = uint8_t(carCount);
    config.session = listen ? 0x6000600060006000ull : 0;
    sim::RaceSim initial;
    SetupNetwork(initial, data, carCount, RoomRoster(config.cars, config.humans));
    { RaceSession prepared(initial, RoomRoster(config.cars, config.humans)); config.content = DiagnosticRoomKey(initial); }
    RoomSetup advertised;
    advertised.cars = config.cars; advertised.humans = config.humans; advertised.track = data.trackName; advertised.laps = data.shell.lapCount;
    for (size_t i = 0; i < carCount; ++i) { advertised.carIds[i] = data.carIds[i]; advertised.paints[i] = data.paints[i]; }
    if (listen) config.setup = advertised;
    else {
        LanSetupQuery query(address, port);
        const auto until = Clock::now() + std::chrono::seconds(12);
        while (!query.Offer() && query.Error().empty() && Clock::now() < until) {
            query.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(query.Offer() && query.Offer()->setup == advertised && query.Offer()->content == config.content,
                "room peer: remote setup query failed or differs from local data");
        config.expectedSession = query.Offer()->session;
        std::printf("LAN room peer: setup query matched host track, cars, rules and content\n");
    }
    RoomDriver driver; driver.Setup(data, config);
    driver.injectDesyncs = desyncs;
    driver.room->SetReady(true);
    if (listen) driver.room->RequestStart();
    const auto deadline = Clock::now() + std::chrono::seconds(longRecovery ? 85 : 40);
    std::optional<Clock::time_point> doneAt;
    bool overflowObserved = false, safeHostInput = false, delayedStateRequested = false;
    std::printf("LAN room peer: %s %s:%u, 2 human slots and %zu AI slots\n", listen ? "host" : "client", address.c_str(), port, carCount - 2);
    std::fflush(stdout);
    while (Clock::now() < deadline) {
        const auto state = driver.room->Status();
        if (desyncs > 3 && state.viewFailed) {
            Require(state.desyncs == 4 && state.stateRestores == 3 && driver.injected == 4 && state.connected == 3,
                "room peer: persistent desync was not bounded");
            if (!doneAt) doneAt = Clock::now();
            if (Clock::now() - *doneAt > std::chrono::seconds(2)) {
                Require(state.nextTick >= 300 && !driver.room->Submit(state.nextTick, Controls(state.nextTick, carCount)[state.localSlot]),
                    "room peer: stopped local view interrupted authority or accepted controls");
                std::printf("LAN room peer: PASS, slot %u, persistent divergence stopped local play after 3 repairs and 4 mismatches\n", state.localSlot);
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue;
        }
        Require(!state.viewFailed, "room peer: unexpected persistent divergence");
        if (hostPause && longRecovery && listen && !delayedStateRequested && state.nextTick >= 390) {
            Require(driver.room->RecoverView(), "room peer: delayed host restore request failed");
            delayedStateRequested = true;
        }
        const bool pause = hostPause && listen && state.nextTick >= 30 && state.nextTick < (longRecovery ? 1400u : 390u);
        if (pause) overflowObserved = overflowObserved || state.recoveryRequired;
        if (!doneAt && !pause) driver.Pump(count);
        if (hostPause && !listen && state.nextTick >= 100 && state.nextTick < 380)
            safeHostInput = safeHostInput || SameInput(driver.lastAuthority[0], SafeIdleInput());
        if (!doneAt && driver.prediction->ConfirmedNextTick() == count) doneAt = Clock::now();
        if (doneAt && Clock::now() - *doneAt > std::chrono::seconds(1)) {
            Require(driver.prediction->PendingTicks() == 0, "room peer: prediction not drained");
            const auto verified = driver.room->Status();
            Require(verified.stateChecks > 0 && verified.desyncs == desyncs &&
                (!desyncs || verified.stateRestores == desyncs), "room peer: state verification or automatic desync repair failed");
            if (hostPause) {
                Require(driver.room->Status().connected == 3 && driver.room->Status().reconnects == 0,
                    "room peer: host view repair disturbed ownership or transport");
                Require(listen ? (overflowObserved && driver.room->Status().stateRestores == (longRecovery ? 2u : 1u) &&
                    !driver.room->Status().restoringView && !driver.room->Status().recoveryRequired) : safeHostInput,
                    "room peer: host view recovery or safe input was not exercised");
            }
            std::printf("LAN room peer: PASS, slot %u, %u confirmed ticks, %u reconnects, %u recovered ticks, %u state restores, %u state checks, %u mismatches, final digest %s\n",
                driver.room->Status().localSlot, count, driver.room->Status().reconnects, driver.room->Status().recoveredTicks,
                verified.stateRestores, verified.stateChecks, verified.desyncs, StateDigest(driver.race).c_str());
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("LAN room peer timed out");
}
} // namespace gt2game
