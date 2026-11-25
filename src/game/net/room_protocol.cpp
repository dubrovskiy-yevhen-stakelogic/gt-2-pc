#include "game/net/room_protocol.h"
#include <algorithm>
#include <stdexcept>

namespace gt2::net {
namespace {
bool Valid(const RoomPacket& p) {
    if (p.cars < 1 || p.cars > kSlots || p.humans < 1 || p.humans > p.cars ||
        (p.slot != 255 && p.slot >= p.humans) ||
        uint8_t(p.kind) < 1 || uint8_t(p.kind) > uint8_t(RoomMessage::Emote)) return false;
    for(const auto& name:p.names)if(name.size()>23||std::any_of(name.begin(),name.end(),[](unsigned char c){return c<32||c>126;}))return false;
    if(p.kind==RoomMessage::Emote) {
        if(p.emote<1||p.emote>6||p.actor>=p.humans)return false;
    } else if(p.emote||p.actor!=255||p.emoteSerial)return false;
    const bool ticket = p.token != ResumeToken{};
    if (p.kind!=RoomMessage::Hello && ticket != (p.kind == RoomMessage::Welcome || p.kind == RoomMessage::Resume || p.kind == RoomMessage::Resumed)) return false;
    const uint8_t mask = uint8_t((1u << p.humans) - 1);
    if ((p.connected | p.ready) & ~mask) return false;
    if (p.kind == RoomMessage::Hello)
        return !p.session && ((p.slot==255&&!ticket)||(p.slot<p.humans&&ticket)) && !p.tick && !p.connected && !p.ready;
    if (p.expectedSession) return false;
    if (!p.session) return false;
    if (p.kind == RoomMessage::Resume || p.kind == RoomMessage::ReplayAck || p.kind == RoomMessage::StateAck)
        return p.slot != 255 && !p.connected && !p.ready;
    if (p.kind == RoomMessage::Ready)
        return p.slot != 255 && !p.tick && !p.connected && p.ready <= 1;
    if (p.ready & ~p.connected) return false;
    if (p.kind == RoomMessage::Reject) return p.slot == 255 && p.tick >= 1 && p.tick <= 7;
    if (p.slot == 255) return false;
    if (p.kind == RoomMessage::Start && (!p.connected || p.ready != p.connected || p.tick)) return false;
    return (p.kind == RoomMessage::End || p.kind == RoomMessage::Resumed || p.kind == RoomMessage::StateBegin || !p.tick);
}
void Put(std::vector<uint8_t>& b, size_t at, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) b[at + i] = uint8_t(v >> (i * 8));
}
uint64_t Get(std::span<const uint8_t> b, size_t at, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= uint64_t(b[at + i]) << (i * 8);
    return v;
}
}
Roster RoomRoster(uint8_t cars, uint8_t humans) {
    RoomPacket p; p.cars = cars; p.humans = humans;
    if (!Valid(p)) throw std::invalid_argument("room requires 2..6 cars and 1..cars human participants");
    Roster roster{};
    for (uint8_t i = 0; i < humans; ++i) roster[i].player = PlayerId(i + 1);
    return roster;
}
std::vector<uint8_t> EncodeRoomPacket(const RoomPacket& p) {
    if (!Valid(p)) throw std::invalid_argument("invalid room packet");
    std::vector<uint8_t> b(kRoomPacketBytes);
    Put(b, 0, 0x4d523247, 4); // G2RM
    Put(b, 4, 9, 2);
    b[6] = uint8_t(p.kind); b[7] = p.cars; b[8] = p.humans; b[9] = p.slot;
    b[10] = p.connected; b[11] = p.ready;
    Put(b, 12, p.session, 8); Put(b, 20, p.tick, 4);
    std::copy(p.content.begin(), p.content.end(), b.begin() + 24);
    Put(b, 56, p.expectedSession, 8);
    std::copy(p.token.begin(), p.token.end(), b.begin() + 64);
    for(size_t i=0;i<kSlots;++i)std::copy(p.names[i].begin(),p.names[i].end(),b.begin()+80+24*i);
    b[80+24*kSlots]=p.emote;b[81+24*kSlots]=p.actor;Put(b,84+24*kSlots,p.emoteSerial,4);
    return b;
}
bool DecodeRoomPacket(std::span<const uint8_t> b, RoomPacket& output) {
    if (b.size() != kRoomPacketBytes || Get(b, 0, 4) != 0x4d523247 || Get(b, 4, 2) != 9) return false;
    RoomPacket p;
    p.kind = RoomMessage(b[6]); p.cars = b[7]; p.humans = b[8]; p.slot = b[9];
    p.connected = b[10]; p.ready = b[11]; p.session = Get(b, 12, 8); p.tick = Tick(Get(b, 20, 4));
    p.expectedSession = Get(b, 56, 8);
    std::copy_n(b.begin() + 24, p.content.size(), p.content.begin());
    std::copy_n(b.begin() + 64, p.token.size(), p.token.begin());
    for(size_t i=0;i<kSlots;++i){const auto text=b.subspan(80+24*i,24);auto end=std::find(text.begin(),text.end(),0);
        if(end==text.end()||std::any_of(end,text.end(),[](auto c){return c!=0;}))return false;p.names[i].assign(text.begin(),end);}
    p.emote=b[80+24*kSlots];p.actor=b[81+24*kSlots];p.emoteSerial=uint32_t(Get(b,84+24*kSlots,4));
    if(b[82+24*kSlots]||b[83+24*kSlots]||!Valid(p))return false;
    output = p;
    return true;
}
bool ValidRoomSetup(const RoomSetup& s) {
    auto identifier = [](const std::string& text, size_t max) {
        return !text.empty() && text.size() <= max && std::all_of(text.begin(), text.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        });
    };
    if (s.botDifficulty > 2 || s.cars < 1 || s.cars > kSlots || s.humans < 1 || s.humans > s.cars || !s.laps || s.laps > 99 || !identifier(s.track, 31)) return false;
    for (size_t i = 0; i < kSlots; ++i) {
        if (i < s.cars) { if (s.carIds[i].size() != 5 || !identifier(s.carIds[i], 5)) return false; }
        else if (!s.carIds[i].empty() || s.paints[i]) return false;
    }
    return true;
}
std::vector<uint8_t> EncodeSetupPacket(const SetupPacket& p) {
    auto request = p; request.running = false;
    if ((p.kind == SetupMessage::Request && request != SetupPacket{}) ||
        (p.kind == SetupMessage::Offer && (!p.session || !ValidRoomSetup(p.setup))) ||
        (p.kind != SetupMessage::Request && p.kind != SetupMessage::Offer)) throw std::invalid_argument("invalid room setup packet");
    std::vector<uint8_t> b(kSetupPacketBytes);
    Put(b, 0, 0x53523247, 4); // G2RS
    Put(b, 4, 5, 2); b[6] = uint8_t(p.kind); b[11] = p.running ? 1 : 0;
    if (p.kind == SetupMessage::Request) return b;
    const auto& s = p.setup; b[180]=s.botDifficulty;
    b[7] = s.cars; b[8] = s.humans; b[9] = s.laps; b[10] = uint8_t((s.manual ? 1 : 0) | (s.countdown ? 2 : 0) | (s.arcadeCars ? 4 : 0));
    Put(b, 12, p.session, 8);
    std::copy(p.content.begin(), p.content.end(), b.begin() + 20);
    std::copy(s.track.begin(), s.track.end(), b.begin() + 52);
    for (size_t i = 0; i < kSlots; ++i) {
        std::copy(s.carIds[i].begin(), s.carIds[i].end(), b.begin() + 84 + std::ptrdiff_t(i * 16));
        Put(b, 96 + i * 16, s.paints[i], 4);
    }
    return b;
}
bool DecodeSetupPacket(std::span<const uint8_t> b, SetupPacket& output) {
    if (b.size() != kSetupPacketBytes || Get(b, 0, 4) != 0x53523247 || Get(b, 4, 2) != 5 || b[11] > 1) return false;
    SetupPacket p; p.kind = SetupMessage(b[6]); p.running = b[11] != 0;
    if (p.kind == SetupMessage::Request) {
        for (size_t i = 7; i < b.size(); ++i) if (i != 11 && b[i]) return false;
    } else if (p.kind == SetupMessage::Offer) {
        if (b[10] > 7 || std::any_of(b.begin() + 181, b.end(), [](uint8_t v) { return v != 0; })) return false;
        auto text = [&](size_t at, size_t count, std::string& target) {
            const auto bytes = b.subspan(at, count);
            const auto zero = std::find(bytes.begin(), bytes.end(), 0);
            if (zero == bytes.end() || std::any_of(zero, bytes.end(), [](uint8_t v) { return v != 0; })) return false;
            target.assign(bytes.begin(), zero); return true;
        };
        auto& s = p.setup; s.botDifficulty=b[180];
        s.cars = b[7]; s.humans = b[8]; s.laps = b[9]; s.manual = (b[10] & 1) != 0; s.countdown = (b[10] & 2) != 0;s.arcadeCars=(b[10]&4)!=0;
        p.session = Get(b, 12, 8);
        std::copy_n(b.begin() + 20, p.content.size(), p.content.begin());
        if (!text(52, 32, s.track)) return false;
        for (size_t i = 0; i < kSlots; ++i) {
            if (!text(84 + i * 16, 12, s.carIds[i])) return false;
            s.paints[i] = uint32_t(Get(b, 96 + i * 16, 4));
        }
        if (!p.session || !ValidRoomSetup(s)) return false;
    } else return false;
    output = std::move(p); return true;
}
} // namespace gt2::net
