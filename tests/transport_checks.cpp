#include "platform/net/enet_transport.h"
#include "game/net/protocol.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

using namespace gt2::net;
using Clock = std::chrono::steady_clock;
namespace {
constexpr uint64_t kTestSession = 0x06004754324d50ull;
constexpr Tick kFrames = 120;
std::vector<uint8_t> Payload(Tick tick) {
    Packet p;
    p.kind = MessageKind::Authority; p.cars = 6; p.session = kTestSession; p.tick = tick;
    for (size_t i = 0; i < kSlots; ++i) {
        p.inputs[i] = SafeIdleInput();
        p.inputs[i].steer = int16_t(int(tick) * 11 - int(i) * 499);
        p.inputs[i].throttle = uint16_t((tick * 19 + i * 211) % 4097);
    }
    return EncodePacket(p);
}
void CheckMessage(const TransportEvent& event) {
    Packet p;
    if (!DecodePacket(event.bytes, kTestSession, p) || p.kind != MessageKind::Authority || p.tick >= kFrames || event.bytes != Payload(p.tick))
        throw std::runtime_error("transport packet contents differ");
}
int Listen(const std::string& address, uint16_t port) {
    EnetTransport server(address, port, 1);
    std::printf("transport diagnostic listening on %s:%u (20 second limit)\n", address.c_str(), server.BoundPort());
    std::fflush(stdout);
    Tick received = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (Clock::now() < deadline) {
        TransportEvent e;
        if (!server.Poll(e, 2)) continue;
        if (e.kind == TransportEvent::Kind::Message) {
            CheckMessage(e);
            Packet p;
            DecodePacket(e.bytes, kTestSession, p);
            if (p.tick != received || e.channel != Channel::Authority) throw std::runtime_error("ordered delivery failed");
            ++received;
            server.Send(e.connection, Channel::Authority, e.bytes);
            server.Flush();
        } else if (e.kind == TransportEvent::Kind::Disconnected) {
            if (received != kFrames) throw std::runtime_error("early diagnostic disconnect");
            std::printf("transport listener: PASS, %u six-car packets received and echoed\n", received);
            return 0;
        }
    }
    throw std::runtime_error("transport listener timed out");
}
int Connect(const std::string& address, uint16_t port) {
    EnetTransport client("0.0.0.0", 0, 1);
    const auto peer = client.Connect(address, port);
    Tick sent = 0, received = 0;
    bool connected = false, closing = false;
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (Clock::now() < deadline) {
        TransportEvent e;
        if (client.Poll(e, 2)) {
            if (e.kind == TransportEvent::Kind::Connected) connected = true;
            else if (e.kind == TransportEvent::Kind::Message) {
                CheckMessage(e);
                if (e.bytes != Payload(received++)) throw std::runtime_error("echo order differs");
            } else if (e.kind == TransportEvent::Kind::Disconnected) {
                if (received != kFrames) throw std::runtime_error("early diagnostic disconnect");
                std::printf("transport client: PASS, %u six-car packets round-tripped\n", received);
                return 0;
            }
        }
        if (connected && sent < kFrames && sent - received < 12) {
            client.Send(peer, Channel::Authority, Payload(sent++));
            client.Flush();
        }
        if (received == kFrames && !closing) { client.Disconnect(peer); client.Flush(); closing = true; }
    }
    throw std::runtime_error("transport client timed out");
}
void ReuseConnections() {
    EnetTransport server("127.0.0.1", 0, 1);
    {
        EnetTransport client("127.0.0.1", 0, 1);
        const auto cancelled = client.Connect("127.0.0.1", server.BoundPort());
        client.Disconnect(cancelled);
        const auto replacement = client.Connect("127.0.0.1", server.BoundPort());
        if (replacement == cancelled) throw std::runtime_error("cancelled connection ID reused");
        bool rejected = false;
        try { client.Disconnect(cancelled); }
        catch (const std::invalid_argument&) { rejected = true; }
        if (!rejected) throw std::runtime_error("stale connection ID still targets replacement");
        client.Disconnect(replacement);
    }
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        {
            EnetTransport client("127.0.0.1", 0, 1);
            client.Connect("127.0.0.1", server.BoundPort());
            bool accepted = false, connected = false;
            const auto deadline = Clock::now() + std::chrono::seconds(1);
            while ((!accepted || !connected) && Clock::now() < deadline) {
                TransportEvent e;
                if (server.Poll(e, 1) && e.kind == TransportEvent::Kind::Connected) accepted = true;
                if (client.Poll(e, 1) && e.kind == TransportEvent::Kind::Connected) connected = true;
            }
            if (!accepted || !connected) throw std::runtime_error("remote slot was not reusable");
            // Exercise exception/scope teardown without a graceful Disconnect.
        }
        bool released = false;
        const auto deadline = Clock::now() + std::chrono::seconds(1);
        while (!released && Clock::now() < deadline) {
            TransportEvent e;
            if (server.Poll(e, 1) && e.kind == TransportEvent::Kind::Disconnected) released = true;
        }
        if (!released) throw std::runtime_error("transport teardown left remote slot occupied");
    }
    std::puts("transport reuse: PASS, stale IDs rejected and three abrupt teardowns release remote slot");
}
int Loopback() {
    EnetTransport server("127.0.0.1", 0, 1), client("127.0.0.1", 0, 1);
    const auto peer = client.Connect("127.0.0.1", server.BoundPort());
    Tick sent = 0, received = 0;
    bool connected = false, disconnected = false;
    std::array<bool, 3> lanes{};
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < deadline) {
        TransportEvent e;
        if (server.Poll(e)) {
            if (e.kind == TransportEvent::Kind::Message) {
                CheckMessage(e);
                server.Send(e.connection, e.channel, e.bytes);
            }
        }
        if (client.Poll(e)) {
            if (e.kind == TransportEvent::Kind::Connected) connected = true;
            else if (e.kind == TransportEvent::Kind::Disconnected) disconnected = true;
            else if (e.kind == TransportEvent::Kind::Message) {
                CheckMessage(e);
                lanes[size_t(e.channel)] = true;
                if (e.channel == Channel::Authority && e.bytes != Payload(received++)) throw std::runtime_error("authority order failed");
            }
        }
        if (connected && sent < kFrames && sent - received < 12) {
            client.Send(peer, Channel::Authority, Payload(sent++));
            if (sent == 1) {
                client.Send(peer, Channel::Control, Payload(0));
                client.Send(peer, Channel::Input, Payload(0));
                bool rejected = false;
                try { client.Send(peer, Channel::Control, std::vector<uint8_t>(EnetTransport::kMaxMessage + 1)); }
                catch (const std::invalid_argument&) { rejected = true; }
                if (!rejected) throw std::runtime_error("oversized message accepted");
            }
        }
        if (received == kFrames && connected && lanes[0] && lanes[1]) {
            client.Disconnect(peer);
            connected = false;
        }
        if (disconnected) {
            if (received != kFrames || !lanes[0] || !lanes[1] || !lanes[2]) throw std::runtime_error("missing transport lane");
            std::puts("transport loopback: PASS, 120 six-car frames, three lanes, message bound and disconnect");
            return 0;
        }
        client.Flush(); server.Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("transport loopback timed out");
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 1) { ReuseConnections(); return Loopback(); }
        if (argc != 4) throw std::invalid_argument("usage: gt2transportcheck [--listen|--connect IPv4 port]");
        const auto port = std::stoul(argv[3]);
        if (!port || port > 65535) throw std::invalid_argument("invalid port");
        if (std::string(argv[1]) == "--listen") return Listen(argv[2], uint16_t(port));
        if (std::string(argv[1]) == "--connect") return Connect(argv[2], uint16_t(port));
        throw std::invalid_argument("unknown transport diagnostic mode");
    } catch (const std::exception& e) { std::fprintf(stderr, "transport diagnostic: %s\n", e.what()); return 1; }
}
