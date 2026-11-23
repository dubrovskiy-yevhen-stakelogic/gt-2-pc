#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace gt2::net {
using ConnectionId = uint32_t;
enum class Channel : uint8_t { Control, Input, Authority };
struct TransportEvent {
    enum class Kind { Connected, Disconnected, Message } kind = Kind::Connected;
    ConnectionId connection = 0;
    Channel channel = Channel::Control;
    std::vector<uint8_t> bytes;
};

// Single-thread-owned host. Connection IDs never expose native ENet pointers.
// Authentication and content agreement must complete above this transport before
// a connection receives race ownership. All ports default to loopback explicitly.
class EnetTransport {
public:
    static constexpr size_t kMaxMessage = 4096;
    EnetTransport(const std::string& bindAddress, uint16_t port, size_t maxPeers = 8);
    ~EnetTransport();
    EnetTransport(const EnetTransport&) = delete;
    EnetTransport& operator=(const EnetTransport&) = delete;
    ConnectionId Connect(const std::string& address, uint16_t port);
    bool Poll(TransportEvent& event, uint32_t waitMs = 0);
    void Send(ConnectionId peer, Channel channel, std::span<const uint8_t> bytes);
    void Disconnect(ConnectionId peer);
    void DisconnectAfterSend(ConnectionId peer); // deliver queued rejection/final control before closing
    void Flush();
    uint16_t BoundPort() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace gt2::net
