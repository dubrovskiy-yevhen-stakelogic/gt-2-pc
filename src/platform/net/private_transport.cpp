#include "platform/net/private_transport.h"
#include <hydrogen.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <stdexcept>

namespace gt2::net {
namespace {
constexpr char kContext[8] = {'G','T','2','N','E','T','0','6'};
constexpr size_t kPrefix = 16;
enum class Wire : uint8_t { Hello = 1, Reply, Data };
void Init() { if (hydro_init() != 0) throw std::runtime_error("secure random initialization failed"); }
std::vector<uint8_t> Header(Wire kind, uint64_t sequence = 0) {
    std::vector<uint8_t> b(kPrefix);
    b[0] = 'G'; b[1] = '2'; b[2] = 'S'; b[3] = 'E'; b[4] = 1; b[5] = uint8_t(kind);
    for (size_t i = 0; i < 8; ++i) b[8 + i] = uint8_t(sequence >> (i * 8));
    return b;
}
uint64_t Sequence(std::span<const uint8_t> bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value |= uint64_t(bytes[8 + i]) << (i * 8);
    return value;
}
}
void SecureRandom(std::span<uint8_t> bytes) { Init(); hydro_random_buf(bytes.data(), bytes.size()); }
uint64_t NewSessionId() {
    std::array<uint8_t, 8> bytes{}; SecureRandom(bytes);
    uint64_t value = 0; for (size_t i = 0; i < 8; ++i) value |= uint64_t(bytes[i]) << (8 * i);
    return value ? value : 1;
}
RoomSecurity NewRoomSecurity() {
    Init(); RoomSecurity result;
    hydro_kx_keypair pair; hydro_kx_keygen(&pair);
    std::copy_n(pair.pk, 32, result.hostPublic.begin()); std::copy_n(pair.sk, 32, result.hostSecret.begin());
    SecureRandom(result.invitation); hydro_memzero(&pair, sizeof(pair));
    return result;
}
struct PrivateTransport::Impl {
    using Clock = std::chrono::steady_clock;
    struct Peer {
        enum class Phase { Hello, Reply, Finish, Confirm, Ready } phase = Phase::Hello;
        hydro_kx_state handshake{};
        hydro_kx_session_keypair keys{};
        std::array<uint64_t, 3> sent{}, received{};
        Clock::time_point deadline = Clock::now() + std::chrono::seconds(5);
        ~Peer() { hydro_memzero(&handshake, sizeof(handshake)); hydro_memzero(&keys, sizeof(keys)); }
    };
    AccessPolicy policy;
    EnetTransport raw;
    std::optional<RoomSecurity> security;
    std::map<ConnectionId, Peer> peers;
    std::deque<TransportEvent> early;
    Impl(const std::string& address, uint16_t port, size_t maxPeers, std::optional<RoomSecurity> s, AccessPolicy access)
        : policy(access), raw(address, port, maxPeers), security(s) {
        if (security && (security->hostPublic == std::array<uint8_t, 32>{} || (policy == AccessPolicy::Invitation && security->invitation == std::array<uint8_t, 32>{})))
            throw std::invalid_argument("private room needs a host key and invitation");
        if (policy == AccessPolicy::PublicDirectory && !security) throw std::invalid_argument("directory requires a pinned identity");
        if (security) Init();
    }
    ~Impl() { if (security) hydro_memzero(&*security, sizeof(*security)); }
    void SendData(ConnectionId id, Channel channel, std::span<const uint8_t> payload, bool finish = false) {
        auto& p = peers.at(id);
        auto& sequence = p.sent.at(size_t(channel));
        if (sequence == UINT64_MAX) throw std::runtime_error("private connection sequence exhausted");
        auto b = Header(Wire::Data, ++sequence);
        std::vector<uint8_t> plain{uint8_t(finish ? 0 : 1), uint8_t(channel)};
        plain.insert(plain.end(), payload.begin(), payload.end());
        b.resize(kPrefix + hydro_secretbox_HEADERBYTES + plain.size());
        if (hydro_secretbox_encrypt(b.data() + kPrefix, plain.data(), plain.size(), sequence, kContext, p.keys.tx) != 0)
            throw std::runtime_error("private message encryption failed");
        raw.Send(id, channel, b);
    }
    bool Handle(TransportEvent& e) {
        const auto id = e.connection;
        if (e.kind == TransportEvent::Kind::Connected) {
            auto& p = peers.try_emplace(id).first->second;
            if (!security->Hosting()) {
                p.phase = Peer::Phase::Reply;
                auto b = Header(Wire::Hello); b.resize(kPrefix + hydro_kx_NK_PACKET1BYTES);
                if (hydro_kx_nk_1(&p.handshake, b.data() + kPrefix, policy == AccessPolicy::PublicDirectory ? nullptr : security->invitation.data(), security->hostPublic.data()) != 0)
                    throw std::runtime_error("private handshake creation failed");
                raw.Send(id, Channel::Control, b);
            }
            return false;
        }
        if (e.kind == TransportEvent::Kind::Disconnected) { peers.erase(id); return true; }
        auto found = peers.find(id);
        if (found == peers.end()) return false;
        auto& p = found->second;
        const auto& b = e.bytes;
        auto reject = [&] { raw.Disconnect(id); peers.erase(id); return false; };
        if (b.size() < kPrefix || b[0] != 'G' || b[1] != '2' || b[2] != 'S' || b[3] != 'E' || b[4] != 1 || b[6] || b[7]) return reject();
        const auto kind = Wire(b[5]); const auto sequence = Sequence(b);
        if (kind == Wire::Hello && p.phase == Peer::Phase::Hello && security->Hosting() && e.channel == Channel::Control &&
            sequence == 0 && b.size() == kPrefix + hydro_kx_NK_PACKET1BYTES) {
            hydro_kx_keypair pair;
            std::copy_n(security->hostPublic.begin(), 32, pair.pk); std::copy_n(security->hostSecret.begin(), 32, pair.sk);
            auto reply = Header(Wire::Reply); reply.resize(kPrefix + hydro_kx_NK_PACKET2BYTES);
            const int result = hydro_kx_nk_2(&p.keys, reply.data() + kPrefix, b.data() + kPrefix, policy == AccessPolicy::PublicDirectory ? nullptr : security->invitation.data(), &pair);
            hydro_memzero(&pair, sizeof(pair));
            if (result != 0) return reject();
            p.phase = Peer::Phase::Finish; raw.Send(id, Channel::Control, reply); return false;
        }
        if (kind == Wire::Reply && p.phase == Peer::Phase::Reply && !security->Hosting() && e.channel == Channel::Control &&
            sequence == 0 && b.size() == kPrefix + hydro_kx_NK_PACKET2BYTES) {
            if (hydro_kx_nk_3(&p.handshake, &p.keys, b.data() + kPrefix) != 0) return reject();
            p.phase = Peer::Phase::Confirm; SendData(id, Channel::Control, {}, true);
            hydro_memzero(&p.handshake, sizeof(p.handshake));
            return false;
        }
        if (kind != Wire::Data || (p.phase != Peer::Phase::Ready && p.phase != Peer::Phase::Finish && p.phase != Peer::Phase::Confirm) ||
            b.size() < kPrefix + hydro_secretbox_HEADERBYTES + 2 || size_t(e.channel) >= 3) return reject();
        auto& last = p.received[size_t(e.channel)];
        if (!sequence || sequence <= last) return reject();
        std::vector<uint8_t> plain(b.size() - kPrefix - hydro_secretbox_HEADERBYTES);
        if (hydro_secretbox_decrypt(plain.data(), b.data() + kPrefix, b.size() - kPrefix, sequence, kContext, p.keys.rx) != 0 ||
            plain[1] != uint8_t(e.channel)) return reject();
        if (p.phase == Peer::Phase::Confirm && plain[0] == 1 && e.channel != Channel::Control) {
            if (early.size() >= 64) return reject();
            early.push_back(e); return false;
        }
        if (p.phase == Peer::Phase::Finish || p.phase == Peer::Phase::Confirm) {
            if (plain.size() != 2 || plain[0] != 0 || e.channel != Channel::Control) return reject();
            if (p.phase == Peer::Phase::Finish) SendData(id, Channel::Control, {}, true);
            p.phase = Peer::Phase::Ready; last = sequence;
            e.kind = TransportEvent::Kind::Connected; e.bytes.clear(); return true;
        }
        if (plain[0] != 1 || plain.size() == 2) return reject();
        last = sequence; e.bytes.assign(plain.begin() + 2, plain.end()); return true;
    }
};
PrivateTransport::PrivateTransport(const std::string& address, uint16_t port, size_t maxPeers, std::optional<RoomSecurity> security, AccessPolicy policy)
    : impl_(std::make_unique<Impl>(address, port, maxPeers, security, policy)) {}
PrivateTransport::~PrivateTransport() = default;
ConnectionId PrivateTransport::Connect(const std::string& address, uint16_t port) { return impl_->raw.Connect(address, port); }
bool PrivateTransport::Poll(TransportEvent& event, uint32_t waitMs) {
    if (!impl_->security) return impl_->raw.Poll(event, waitMs);
    for (auto it = impl_->peers.begin(); it != impl_->peers.end();) {
        if (it->second.phase != Impl::Peer::Phase::Ready && Impl::Clock::now() > it->second.deadline) {
            impl_->raw.Disconnect(it->first); it = impl_->peers.erase(it);
        } else ++it;
    }
    for (unsigned i = 0; i < 32; ++i) {
        TransportEvent next;
        auto early = std::find_if(impl_->early.begin(), impl_->early.end(), [&](const TransportEvent& e) {
            const auto p = impl_->peers.find(e.connection);
            return p == impl_->peers.end() || p->second.phase == Impl::Peer::Phase::Ready;
        });
        if (early != impl_->early.end()) {
            next = std::move(*early); impl_->early.erase(early);
            if (impl_->Handle(next)) { event = std::move(next); return true; }
            continue;
        }
        if (!impl_->raw.Poll(next, i == 0 ? waitMs : 0)) return false;
        if (impl_->Handle(next)) { event = std::move(next); return true; }
    }
    return false;
}
void PrivateTransport::Send(ConnectionId id, Channel channel, std::span<const uint8_t> bytes) {
    if (!impl_->security) { impl_->raw.Send(id, channel, bytes); return; }
    if (size_t(channel) >= 3 || bytes.empty() || bytes.size() > EnetTransport::kMaxMessage - kPrefix - hydro_secretbox_HEADERBYTES - 2)
        throw std::invalid_argument("invalid private message size or channel");
    const auto found = impl_->peers.find(id);
    if (found == impl_->peers.end() || found->second.phase != Impl::Peer::Phase::Ready) throw std::runtime_error("private connection is not authenticated");
    impl_->SendData(id, channel, bytes);
}
void PrivateTransport::Disconnect(ConnectionId id) { impl_->raw.Disconnect(id); impl_->peers.erase(id); }
void PrivateTransport::DisconnectAfterSend(ConnectionId id) { impl_->raw.DisconnectAfterSend(id); }
void PrivateTransport::Flush() { impl_->raw.Flush(); }
uint16_t PrivateTransport::BoundPort() const { return impl_->raw.BoundPort(); }
} // namespace gt2::net
