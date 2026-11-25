#include "platform/net/enet_transport.h"
#include <enet/enet.h>
#include <limits>
#include <cerrno>
#include <cstring>
#include <map>
#include <stdexcept>

namespace gt2::net {
namespace {
constexpr enet_uint32 kConnectionProtocol = 0x47320601;
constexpr size_t kChannels = 3;
struct Runtime {
    Runtime() { if (enet_initialize() != 0) throw std::runtime_error("ENet initialization failed"); }
    ~Runtime() { enet_deinitialize(); }
};
void Initialize() { static const Runtime runtime; (void)runtime; }
}
struct EnetTransport::Impl {
    ENetHost* host = nullptr;
    std::map<ConnectionId, ENetPeer*> peers;
    ConnectionId next = 1;
    ~Impl() {
        if (!host) return;
        // In particular, exception exits must release the remote slot. Destroying
        // the UDP socket alone leaves a delivered rejection waiting for its ACK.
        for (size_t i = 0; i < host->peerCount; ++i) enet_peer_disconnect_now(&host->peers[i], 0);
        enet_host_flush(host);
        enet_host_destroy(host);
    }
    ConnectionId Register(ENetPeer* peer, bool fresh = false) {
        if (fresh) {
            for (auto it = peers.begin(); it != peers.end();) {
                if (it->second == peer) it = peers.erase(it); else ++it;
            }
        }
        for (const auto& [id, existing] : peers) if (existing == peer) return id;
        if (next == std::numeric_limits<ConnectionId>::max()) throw std::overflow_error("connection IDs exhausted");
        const auto id = next++;
        peers.emplace(id, peer);
        return id;
    }
    ENetPeer* Find(ConnectionId id) {
        const auto it = peers.find(id);
        if (it == peers.end()) throw std::invalid_argument("unknown connection");
        return it->second;
    }
    bool Knows(ENetPeer* peer) const {
        for (const auto& [id, existing] : peers) { (void)id; if (existing == peer) return true; }
        return false;
    }
};
EnetTransport::EnetTransport(const std::string& address, uint16_t port, size_t maxPeers) : impl_(std::make_unique<Impl>()) {
    if (maxPeers == 0 || maxPeers > 64 || address.empty()) throw std::invalid_argument("invalid transport bind options");
    Initialize();
    ENetAddress bind{};
    bind.port = port;
    if (enet_address_set_host_ip(&bind, address.c_str()) != 0) throw std::invalid_argument("invalid IPv4 bind address");
    impl_->host = enet_host_create(&bind, maxPeers, kChannels, 0, 0);
    if (!impl_->host) throw std::runtime_error("cannot bind ENet host");
    impl_->host->maximumPacketSize = kMaxMessage;
    impl_->host->maximumWaitingData = kMaxMessage * 64;
    impl_->host->checksum = enet_crc32;
}
EnetTransport::~EnetTransport() = default;
ConnectionId EnetTransport::Connect(const std::string& address, uint16_t port) {
    if (address.empty() || !port) throw std::invalid_argument("invalid peer address");
    ENetAddress remote{};
    remote.port = port;
    if (enet_address_set_host(&remote, address.c_str()) != 0) throw std::invalid_argument("cannot resolve host IPv4 address");
    ENetPeer* peer = enet_host_connect(impl_->host, &remote, kChannels, kConnectionProtocol);
    if (!peer) throw std::runtime_error("no free connection slot");
    enet_peer_timeout(peer, 8, 2000, 10000);
    return impl_->Register(peer, true);
}
bool EnetTransport::Poll(TransportEvent& output, uint32_t waitMs) {
    if (waitMs > 50) throw std::invalid_argument("transport poll wait exceeds 50 ms");
    ENetEvent event{};
#ifndef _WIN32
    errno = 0;
#endif
    const int result = enet_host_service(impl_->host, &event, waitMs);
    if (result < 0) {
#ifndef _WIN32
        const int error = errno;
        // Signals and transient UDP/network errors are not a terminal room
        // failure. ENet timeouts and the bounded room reconnect path handle
        // lost connectivity; retain fatal errors with their actual cause.
        if (error == EINTR || error == EAGAIN || error == EWOULDBLOCK ||
            error == ECONNREFUSED || error == ENETUNREACH || error == EHOSTUNREACH ||
            error == ENOBUFS) return false;
        throw std::runtime_error("ENet service failed: " + std::string(std::strerror(error)) + " (" + std::to_string(error) + ")");
#else
        throw std::runtime_error("ENet service failed");
#endif
    }
    if (result == 0 || event.type == ENET_EVENT_TYPE_NONE) return false;
    TransportEvent out;
    if (event.type == ENET_EVENT_TYPE_CONNECT) {
        if (!impl_->Knows(event.peer) && event.data != kConnectionProtocol) {
            enet_peer_disconnect_now(event.peer, 1);
            return false;
        }
        out.kind = TransportEvent::Kind::Connected;
        out.connection = impl_->Register(event.peer);
        enet_peer_timeout(event.peer, 8, 2000, 10000);
    } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
        out.kind = TransportEvent::Kind::Disconnected;
        out.connection = impl_->Register(event.peer);
        impl_->peers.erase(out.connection);
    } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
        std::unique_ptr<ENetPacket, decltype(&enet_packet_destroy)> packet(event.packet, enet_packet_destroy);
        if (event.channelID >= kChannels || packet->dataLength > kMaxMessage) {
            enet_peer_disconnect(event.peer, 2);
            return false;
        }
        out.kind = TransportEvent::Kind::Message;
        out.connection = impl_->Register(event.peer);
        out.channel = Channel(event.channelID);
        out.bytes.assign(packet->data, packet->data + packet->dataLength);
    } else return false;
    output = std::move(out);
    return true;
}
void EnetTransport::Send(ConnectionId id, Channel channel, std::span<const uint8_t> bytes) {
    if (size_t(channel) >= kChannels || bytes.empty() || bytes.size() > kMaxMessage)
        throw std::invalid_argument("invalid transport message");
    ENetPeer* peer = impl_->Find(id);
    if (peer->state != ENET_PEER_STATE_CONNECTED) throw std::runtime_error("connection is not established");
    const size_t queued = enet_list_size(&peer->outgoingCommands) + enet_list_size(&peer->outgoingSendReliableCommands) +
        enet_list_size(&peer->sentReliableCommands);
    if (queued >= 256) throw std::runtime_error("network send backlog limit reached");
    // Authoritative input frames are an ordered history. Future independently
    // decodable state snapshots can use the unreliable input lane instead.
    const enet_uint32 flags = channel == Channel::Input ? 0 : ENET_PACKET_FLAG_RELIABLE;
    ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), flags);
    if (!packet) throw std::bad_alloc();
    if (enet_peer_send(peer, enet_uint8(channel), packet) != 0) {
        enet_packet_destroy(packet);
        throw std::runtime_error("cannot queue network message");
    }
}
void EnetTransport::Disconnect(ConnectionId id) { enet_peer_disconnect(impl_->Find(id), 0); }
void EnetTransport::DisconnectAfterSend(ConnectionId id) { enet_peer_disconnect_later(impl_->Find(id), 0); }
void EnetTransport::Flush() { enet_host_flush(impl_->host); }
uint16_t EnetTransport::BoundPort() const { return impl_->host->address.port; }
} // namespace gt2::net
