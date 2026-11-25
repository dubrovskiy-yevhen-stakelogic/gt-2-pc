#pragma once
#include "platform/net/enet_transport.h"
#include <array>
#include <optional>

namespace gt2::net {
// Invitations contain only the host public key and a random room capability.
// Keep hostSecret on the host; sharing it would allow host impersonation.
struct RoomSecurity {
    std::array<uint8_t, 32> hostPublic{}, hostSecret{}, invitation{};
    bool Hosting() const { return hostSecret != std::array<uint8_t, 32>{}; }
    RoomSecurity Public() const { auto copy = *this; copy.hostSecret = {}; return copy; }
};
RoomSecurity NewRoomSecurity();
void SecureRandom(std::span<uint8_t> bytes);
uint64_t NewSessionId();

// Noise NK with a random invitation PSK, then authenticated per-direction data.
// A Connected event is exposed only after key confirmation. Raw ENet remains
// available for explicit trusted-LAN rooms without a RoomSecurity argument.
enum class AccessPolicy { Invitation, PublicDirectory };
class PrivateTransport {
public:
    PrivateTransport(const std::string& bindAddress, uint16_t port, size_t maxPeers = 8,
        std::optional<RoomSecurity> security = {}, AccessPolicy policy = AccessPolicy::Invitation);
    ~PrivateTransport();
    ConnectionId Connect(const std::string& address, uint16_t port);
    bool Poll(TransportEvent& event, uint32_t waitMs = 0);
    void Send(ConnectionId peer, Channel channel, std::span<const uint8_t> bytes);
    void Disconnect(ConnectionId peer);
    void DisconnectAfterSend(ConnectionId peer);
    void Flush();
    uint16_t BoundPort() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace gt2::net
