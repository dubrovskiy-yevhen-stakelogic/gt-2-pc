#include "game/net/protocol.h"
#include "game/net/state_codec.h"
#include "game/net/state_archive.h"
#include "game/net/room_protocol.h"
#include <cstdio>
#include <stdexcept>
#include <string>
using namespace gt2::net;
namespace {
int checks = 0;
void Check(bool ok, const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }
template<class F> void Reject(F f, const char* why) {
    bool threw = false;
    try { f(); } catch (const std::exception&) { threw = true; }
    Check(threw, why);
}
}
int main() {
    try {
        Check(RaceSession::FinishSeconds(100,0,false,false,900)==0,"no countdown before a human finishes");
        Check(RaceSession::FinishSeconds(100,100,true,false,900)==30,"finish countdown starts at 30");
        Check(RaceSession::FinishSeconds(129,100,true,false,900)==30,"countdown rounds up partial seconds");
        Check(RaceSession::FinishSeconds(130,100,true,false,900)==29,"countdown decrements each second");
        Check(RaceSession::FinishSeconds(999,100,true,false,900)==1,"last countdown second remains visible");
        Check(RaceSession::FinishSeconds(1000,100,true,false,900)==0,"countdown expires");
        Check(RaceSession::FinishSeconds(130,100,true,true,900)==0,"all-finished ends countdown early");
        StateWriter archive;
        archive.FieldsOf(int16_t(-2), uint32_t(0x12345678), true, int32_t(INT32_MIN));
        Check(archive.bytes == std::vector<uint8_t>({254,255,120,86,52,18,1,0,0,0,128}), "state scalar byte order and signed endpoints");
        StateReader reader(archive.bytes); int16_t signedValue = 0; uint32_t word = 0; bool flag = false; int32_t minimum = 0;
        reader.FieldsOf(signedValue, word, flag, minimum);
        Check(reader.Done() && signedValue == -2 && word == 0x12345678 && flag && minimum == INT32_MIN, "state scalar decode");
        Reject([&] { reader(word); }, "state scalar truncation rejected");
        const std::array<uint8_t, 1> noncanonical{2}; StateReader boolReader(noncanonical);
        Reject([&] { boolReader(flag); }, "state boolean is canonical");
        std::vector<uint8_t> stateBytes(StateCodec::kMaxBytes);
        for (size_t i = 0; i < stateBytes.size(); ++i) stateBytes[i] = uint8_t(i * 7);
        const auto chunks = EncodeStateChunks(42, 123, stateBytes);
        StateAssembler assembler(42, 123);
        Reject([&] { assembler.Take(); }, "incomplete state cannot be applied");
        Reject([&] { assembler.Feed(chunks[1]); }, "state cannot skip a chunk");
        for (size_t at : {size_t(0), size_t(4), size_t(6), size_t(8), size_t(16), size_t(20), size_t(24), size_t(28)}) {
            auto malformed = chunks[0]; malformed[at] ^= 128;
            Reject([&] { assembler.Feed(malformed); }, "malformed state transfer header rejected");
        }
        assembler.Feed(chunks[0]);
        Reject([&] { assembler.Feed(chunks[0]); }, "duplicate state chunk rejected");
        for (size_t i = 1; i < chunks.size(); ++i) assembler.Feed(chunks[i]);
        Check(assembler.Complete(), "maximum bounded state assembled after rejected packets");
        const auto assembled = assembler.Take();
        Check(assembled.nextTick == 123 && assembled.bytes == stateBytes, "assembled state bytes preserved");
        Reject([&] { EncodeStateChunks(0, 123, stateBytes); }, "state transfer requires session");
        Reject([&] { EncodeStateChunks(42, 123, {}); }, "empty state transfer rejected");
        stateBytes.push_back(0);
        Reject([&] { EncodeStateChunks(42, 123, stateBytes); }, "oversized state transfer rejected");
        const gt2::sim::PadRecord full{7, -4096, 2048, 4096, -1, 1, 1, 0};
        Check(ValidInput(full), "analog endpoints accepted");
        auto bad = full;
        bad.throttle = 4097; Check(!ValidInput(bad), "excess throttle rejected");
        bad = full; bad.flags = 0; Check(!ValidInput(bad), "digital control bounds enforced");
        bad = full; bad.flags |= 0x4000; Check(!ValidInput(bad), "unknown flags rejected");
        bad = full; bad.shift = 2; Check(!ValidInput(bad), "invalid shift rejected");
        bad = full; bad.reverse = 2; Check(!ValidInput(bad), "invalid reverse rejected");
        auto wheel = full; wheel.flags = 0x1f7; wheel.reserved = 0xff;
        Check(ValidInput(wheel), "wheel clutch and sequential gear preserved");
        wheel.flags |= gt2::sim::kWheelIgnoreShiftSpeed;
        Check(!ValidInput(wheel) && ValidInput(wheel, true), "room shift-speed policy enforced");

        Packet p; p.session = 0x0807060504030201ull; p.tick = 0x12345678; p.inputs[0] = full;
        auto bytes = EncodePacket(p);
        Check(bytes.size() == 36, "input packet size");
        Check(bytes[0] == 'G' && bytes[3] == 'P' && bytes[8] == 1 && bytes[15] == 8 && bytes[16] == 0x78 && bytes[19] == 0x12,
              "wire header has explicit little endian order");
        Check(bytes[26] == 0 && bytes[27] == 0xf0 && bytes[32] == 0xff, "signed wire fields have fixed encoding");
        Packet decoded;
        Check(DecodePacket(bytes, p.session, decoded) && SameInput(decoded.inputs[0], full), "input wire round trip");
        for (size_t length = 0; length < bytes.size(); ++length) {
            decoded.tick = 999;
            Check(!DecodePacket(std::span<const uint8_t>(bytes).first(length), p.session, decoded) && decoded.tick == 999,
                  "truncated packet is rejected without mutation");
        }
        Check(!DecodePacket(bytes, p.session + 1, decoded), "other session rejected");
        auto broken = bytes; broken.push_back(0); Check(!DecodePacket(broken, p.session, decoded), "trailing data rejected");
        for (const size_t offset : {size_t(0), size_t(4), size_t(6), size_t(7), size_t(20), size_t(22)}) {
            broken = bytes; broken[offset] = 0xff;
            Check(!DecodePacket(broken, p.session, decoded), "malformed header rejected");
        }
        broken = bytes; broken[31] = 0x7f; Check(!DecodePacket(broken, p.session, decoded), "malicious pedal rejected");
        p.kind = MessageKind::Authority; p.cars = 6;
        for (size_t i = 0; i < kSlots; ++i) { p.inputs[i] = full; p.inputs[i].steer = int16_t(i * 500); }
        bytes = EncodePacket(p);
        Check(bytes.size() == 96 && DecodePacket(bytes, p.session, decoded) && SameInputs(p.inputs, decoded.inputs), "six-player authority frame");
        p.stateDigest = 0x0807060504030201ull;
        bytes = EncodePacket(p);
        Check(bytes.size() == 104 && bytes[22] == 1 && bytes[96] == 1 && bytes[103] == 8 &&
            DecodePacket(bytes, p.session, decoded) && decoded.stateDigest == p.stateDigest, "authority digest is explicit little endian");
        for (size_t length = 96; length < bytes.size(); ++length)
            Check(!DecodePacket(std::span<const uint8_t>(bytes).first(length), p.session, decoded), "truncated digest rejected");
        broken = bytes; broken[22] = 0;
        Check(!DecodePacket(broken, p.session, decoded), "unflagged digest rejected");
        broken = bytes; broken[4] = 1;
        Check(!DecodePacket(broken, p.session, decoded), "old input protocol rejected");
        p.stateDigest = 0;
        Check(DecodePacket(EncodePacket(p), p.session, decoded) && decoded.stateDigest == uint64_t(0), "zero is a valid present digest");
        p.kind = MessageKind::Input; p.cars = 1;
        Reject([&] { EncodePacket(p); }, "client input cannot supply authority digest");
        broken = EncodePacket(Packet{MessageKind::Authority, p.session, 1, 1, {}, uint64_t(9)}); broken[6] = uint8_t(MessageKind::Input);
        Check(!DecodePacket(broken, p.session, decoded), "decoder rejects input digest");

        Roster roster{};
        for (size_t i = 0; i < kSlots; ++i) roster[i].player = PlayerId(100 + i);
        InputInbox inbox(roster, kSlots);
        Check(!inbox.Submit(0, 0, full) && !inbox.Submit(99, 0, full), "unknown connection cannot control a car");
        Check(!inbox.Submit(100, 120, full), "future input window bounded");
        Check(inbox.Submit(105, 1, full), "out-of-order future input accepted");
        Check(inbox.Submit(100, 0, full) && inbox.Submit(100, 0, full), "identical retransmit accepted");
        bad = full; bad.throttle = 0;
        Check(!inbox.Submit(100, 0, bad), "already submitted input cannot be rewritten");
        auto frame = inbox.Consume();
        Check(SameInput(frame[0], full) && SameInput(frame[5], SafeIdleInput()), "ownership maps to stable car index");
        Check(!inbox.Submit(100, 0, full), "late input rejected");
        frame = inbox.Consume();
        Check(SameInput(frame[5], full) && frame[0].shift == 0 && frame[0].throttle == full.throttle, "loss holds axes without repeating shift edge");
        for (unsigned i = 0; i < InputInbox::kHoldTicks; ++i) frame = inbox.Consume();
        Check(SameInput(frame[0], SafeIdleInput()) && SameInput(frame[5], SafeIdleInput()), "disconnect releases accelerator and brakes");
        for (int i = 0; i < 500; ++i) {
            Check(inbox.Submit(101, inbox.NextTick(), full), "input ring reused safely");
            Check(SameInput(inbox.Consume()[1], full), "input ring does not leak old input");
        }
        const auto queuedTick = inbox.NextTick() + 2;
        Check(inbox.Submit(105, queuedTick, full), "future command queued before disconnect");
        inbox.Release(105);
        for (int i = 0; i < 4; ++i) Check(SameInput(inbox.Consume()[5], SafeIdleInput()), "disconnect clears future commands immediately");
        RoomPacket hello; hello.names[0]="Deck Driver"; hello.content[0] = 0x81; hello.content[31] = 0x72;
        auto roomBytes = EncodeRoomPacket(hello);
        RoomPacket room;
        Check(roomBytes.size() == kRoomPacketBytes && DecodeRoomPacket(roomBytes, room) && room.content == hello.content, "room greeting round trip");
        Check(room.names==hello.names,"nickname round trip");
        {
            RoomPacket emote;emote.kind=RoomMessage::Emote;emote.session=42;emote.slot=0;emote.actor=1;emote.emote=6;emote.emoteSerial=123;
            Check(DecodeRoomPacket(EncodeRoomPacket(emote),room)&&room.actor==1&&room.emote==6&&room.emoteSerial==123,"emote round trip");
            auto invalidEmote=emote;invalidEmote.emote=7;Reject([&]{EncodeRoomPacket(invalidEmote);},"unknown emote rejected");
            invalidEmote=emote;invalidEmote.actor=6;Reject([&]{EncodeRoomPacket(invalidEmote);},"AI slot emote rejected");
            invalidEmote=emote;invalidEmote.kind=RoomMessage::Status;Reject([&]{EncodeRoomPacket(invalidEmote);},"emote data forbidden in status");
        }
        auto badName=hello;badName.names[0]="bad\nname";Reject([&]{EncodeRoomPacket(badName);},"control codes in names rejected");
        badName.names[0]=std::string(24,'a');Reject([&]{EncodeRoomPacket(badName);},"oversized nickname rejected");
        for (size_t i = 0; i < roomBytes.size(); ++i) {
            room.tick = 888;
            Check(!DecodeRoomPacket(std::span<const uint8_t>(roomBytes).first(i), room) && room.tick == 888, "truncated room message leaves destination unchanged");
        }
        for (const size_t offset : {size_t(0), size_t(4), size_t(6), size_t(7), size_t(8), size_t(9), size_t(10), size_t(11), size_t(12), size_t(20)}) {
            auto malformed = roomBytes; malformed[offset] = 0xfe;
            Check(!DecodeRoomPacket(malformed, room), "invalid room schema rejected");
        }
        hello.expectedSession = 0x0807060504030201ull;
        roomBytes = EncodeRoomPacket(hello);
        Check(roomBytes[56] == 1 && roomBytes[63] == 8 && DecodeRoomPacket(roomBytes, room) && room.expectedSession == hello.expectedSession,
              "setup query session pin survives Hello");
        hello.expectedSession = 0;
        hello.kind = RoomMessage::Start; hello.session = 0x0807060504030201ull;
        hello.slot = 1; hello.connected = hello.ready = 3;
        roomBytes = EncodeRoomPacket(hello);
        Check(roomBytes[12] == 1 && roomBytes[19] == 8 && DecodeRoomPacket(roomBytes, room), "room session is little endian");
        hello.ready = 1;
        Reject([&] { EncodeRoomPacket(hello); }, "start requires every participant ready");
        hello.ready = 3; hello.expectedSession = 1;
        Reject([&] { EncodeRoomPacket(hello); }, "session pin only allowed in Hello");
        RoomPacket resume;
        resume.kind = RoomMessage::Resume; resume.slot = 1; resume.session = 42; resume.tick = 250;
        resume.token[0] = 0x12; resume.token[15] = 0x34;
        roomBytes = EncodeRoomPacket(resume);
        Check(roomBytes.size() == kRoomPacketBytes && roomBytes[64] == 0x12 && roomBytes[79] == 0x34 &&
              DecodeRoomPacket(roomBytes, room) && room.token == resume.token && room.tick == 250, "resume ticket and cursor round trip");
        auto invalidResume = resume; invalidResume.token = {};
        Reject([&] { EncodeRoomPacket(invalidResume); }, "resume requires nonzero private ticket");
        invalidResume = resume; invalidResume.slot = 0;
        Check(DecodeRoomPacket(EncodeRoomPacket(invalidResume), room), "dedicated server permits remote slot zero with a valid ticket");
        invalidResume = resume; invalidResume.kind = RoomMessage::Status; invalidResume.tick = 0;
        Reject([&] { EncodeRoomPacket(invalidResume); }, "private ticket cannot enter broadcast status");
        resume.kind = RoomMessage::Resumed; resume.connected = 3;
        Check(DecodeRoomPacket(EncodeRoomPacket(resume), room), "resume target permits ongoing race tick");
        resume.kind = RoomMessage::ReplayAck; resume.token = {}; resume.connected = 0;
        Check(DecodeRoomPacket(EncodeRoomPacket(resume), room), "catch-up acknowledgement round trip");
        roomBytes[4] = 4;
        Check(!DecodeRoomPacket(roomBytes, room), "previous room schema rejected");

        AuthorityHistory history;
        Check(history.CanResume(0) && !history.CanResume(1), "empty history has one valid boundary");
        Packet retained; retained.kind = MessageKind::Authority; retained.cars = 2; retained.session = 42;
        for (Tick tick = 0; tick < 1000; ++tick) { retained.tick = tick; history.Append(retained); }
        Check(history.NextTick() == 1000 && !history.Find(99) && history.Find(100) && history.Find(999) && !history.Find(1000),
              "history retains exactly its bounded tick range");
        Check(!history.CanResume(99) && history.CanResume(100) && history.CanResume(1000) && !history.CanResume(1001),
              "expired and future resume cursors rejected");
        retained.tick = 1001;
        Reject([&] { history.Append(retained); }, "history cannot contain an input gap");
        retained.tick = 1000; retained.session = 43;
        Reject([&] { history.Append(retained); }, "history cannot switch sessions");
        Check(history.NextTick() == 1000 && history.Find(100), "rejected history append leaves state unchanged");
        SetupPacket request, offer, setupDecoded;
        auto setupBytes = EncodeSetupPacket(request);
        Check(DecodeSetupPacket(setupBytes, setupDecoded) && setupDecoded == request, "empty setup query round trip");
        setupBytes[12] = 1;
        Check(!DecodeSetupPacket(setupBytes, setupDecoded), "setup query cannot carry arbitrary state");
        offer.kind = SetupMessage::Offer; offer.session = 0x0807060504030201ull; offer.content = hello.content;
        offer.setup.track = "new_parmaS"; offer.setup.humans = 4; offer.setup.laps = 7; offer.setup.manual = true;
        offer.setup.carIds.fill("us36n"); offer.setup.paints[5] = 0x12345678;
        setupBytes = EncodeSetupPacket(offer);
        Check(setupBytes.size() == 192 && setupBytes[12] == 1 && setupBytes[19] == 8 && setupBytes[176] == 0x78 &&
              setupBytes[179] == 0x12 && DecodeSetupPacket(setupBytes, setupDecoded) && setupDecoded == offer,
              "all stock setup fields round trip with fixed byte order");
        for (size_t length = 0; length < setupBytes.size(); ++length) {
            setupDecoded = offer;
            Check(!DecodeSetupPacket(std::span<const uint8_t>(setupBytes).first(length), setupDecoded) && setupDecoded == offer,
                  "truncated setup cannot mutate accepted state");
        }
        for (size_t offset : {size_t(0), size_t(4), size_t(6), size_t(7), size_t(8), size_t(9), size_t(10), size_t(11), size_t(83), size_t(95), size_t(180), size_t(191)}) {
            auto malformed = setupBytes; malformed[offset] = 0xff;
            Check(!DecodeSetupPacket(malformed, setupDecoded), "malformed setup rejected");
        }
        auto malformedSetup = setupBytes; malformedSetup.push_back(0);
        Check(!DecodeSetupPacket(malformedSetup, setupDecoded), "setup trailing data rejected");
        auto invalidSetup = offer;
        invalidSetup.setup.track = "../seattle";
        Reject([&] { EncodeSetupPacket(invalidSetup); }, "track path traversal rejected");
        invalidSetup = offer; invalidSetup.setup.carIds[1] = "a/bcd";
        Reject([&] { EncodeSetupPacket(invalidSetup); }, "car path traversal rejected");
        malformedSetup = setupBytes; malformedSetup[52] = '.';
        Check(!DecodeSetupPacket(malformedSetup, setupDecoded), "decoder validates asset identifiers");
        invalidSetup = offer; invalidSetup.setup.cars = 4;
        Reject([&] { EncodeSetupPacket(invalidSetup); }, "inactive slot data rejected");
        invalidSetup.setup.carIds[4].clear(); invalidSetup.setup.carIds[5].clear(); invalidSetup.setup.paints[5] = 0;
        Check(DecodeSetupPacket(EncodeSetupPacket(invalidSetup), setupDecoded), "four-car stock setup supported");
        auto soloSetup=offer;soloSetup.setup.cars=soloSetup.setup.humans=1;
        for(size_t i=1;i<kSlots;++i){soloSetup.setup.carIds[i].clear();soloSetup.setup.paints[i]=0;}
        for(uint8_t level=0;level<3;++level){soloSetup.setup.botDifficulty=level;Check(DecodeSetupPacket(EncodeSetupPacket(soloSetup),setupDecoded)&&setupDecoded==soloSetup,"solo bot-free setup and difficulty round trip");}
        Check(RoomRoster(1,1)[0].player==1,"solo bot-free roster");
        const auto roomRoster = RoomRoster(6, 4);
        Check(roomRoster[0].player == 1 && roomRoster[3].player == 4 && roomRoster[4].player == 0, "room ownership includes server bots");
        Reject([] { RoomRoster(2, 3); }, "room cannot overbook cars");
        roster[1] = roster[0];
        Reject([&] { InputInbox duplicate(roster, 6); }, "duplicate ownership rejected");
        gt2::sim::RaceSim unconfigured;
        Reject([&] { (void)unconfigured.CaptureCheckpoint(); }, "unconfigured checkpoint rejected");
        std::printf("network checks: %d passed\n", checks);
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "network check failed: %s\n", e.what()); return 1; }
}
