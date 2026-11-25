#include "platform/net/private_transport.h"
#include "platform/net/room_files.h"
#include <fstream>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
using namespace gt2::net;
using Clock = std::chrono::steady_clock;
namespace {
void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
enum class Attack { None, WrongInvitation, WrongHost, Tamper, Replay, Channel, Plaintext };
void CheckFiles() {
    const auto folder = std::filesystem::temp_directory_path() / ("gt2-private-check-" + std::to_string(NewSessionId()));
    std::filesystem::create_directory(folder);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path, e); } } cleanup{folder};
    const auto host = NewRoomSecurity();
    WriteHostKey(folder / "host.gt2host", host);
    Require(ReadHostKey(folder / "host.gt2host").hostSecret == host.hostSecret, "host key did not round trip");
    RoomInvitation invite{"room.example.com", 29600, host.Public()};
    WriteInvitation(folder / "friend.gt2invite", invite);
    const auto loaded = ReadInvitation(folder / "friend.gt2invite");
    Require(loaded.address == invite.address && loaded.port == invite.port && loaded.security.hostPublic == host.hostPublic &&
        loaded.security.invitation == host.invitation && !loaded.security.Hosting(), "invitation exposes or loses identity");
    bool rejected = false;
    try { WriteInvitation(folder / "friend.gt2invite", invite); } catch (const std::runtime_error&) { rejected = true; }
    Require(rejected, "invitation was silently overwritten");
    SavedResume saved; saved.address = invite.address; saved.port = invite.port; saved.hostPublic = host.hostPublic;
    saved.content[0] = 6; saved.identity.session = NewSessionId(); saved.identity.slot = 0; SecureRandom(saved.identity.token);
    WriteResume(folder / "player.gt2resume", saved);
    saved.identity.slot = 1; saved.identity.session = NewSessionId();
    WriteResume(folder / "player.gt2resume", saved);
    const auto resumed = ReadResume(folder / "player.gt2resume");
    Require(resumed.address == saved.address && resumed.port == 29600 && resumed.hostPublic == host.hostPublic &&
        resumed.content == saved.content && resumed.identity.session == saved.identity.session && resumed.identity.slot == 1 &&
        resumed.identity.token == saved.identity.token, "persistent reconnect identity lost fields");
    { std::ofstream bad(folder / "bad.gt2invite"); bad << "GT2-INVITE-1\n../../wrong\n29600\n"; }
    rejected = false;
    try { (void)ReadInvitation(folder / "bad.gt2invite"); } catch (const std::exception&) { rejected = true; }
    Require(rejected, "malformed invitation accepted");
    Require(std::distance(std::filesystem::directory_iterator(folder), std::filesystem::directory_iterator{}) == 4,
        "temporary reconnect credential leaked");
}
void Check(Attack attack) {
    auto key = NewRoomSecurity(); auto invite = key.Public();
    if (attack == Attack::WrongInvitation) invite.invitation[0] ^= 1;
    if (attack == Attack::WrongHost) invite.hostPublic[0] ^= 1;
    PrivateTransport server("127.0.0.1", 0, 2, key), client("127.0.0.1", 0, 1, invite);
    // The proxy has no invitation or session keys; it only observes opaque traffic.
    EnetTransport ingress("127.0.0.1", 0, 1), egress("127.0.0.1", 0, 1);
    ConnectionId incoming = 0;
    const auto outgoing = egress.Connect("127.0.0.1", server.BoundPort());
    const auto clientPeer = client.Connect("127.0.0.1", ingress.BoundPort());
    bool linked = false, clientReady = false, serverReady = false, sent = false, attacked = false, closed = false, echoed = false;
    size_t messages = 0; std::vector<std::vector<uint8_t>> pending;
    const std::vector<uint8_t> payload{1,3,5,7,9,11,13,15};
    const auto deadline = Clock::now() + std::chrono::seconds(4);
    while (Clock::now() < deadline) {
        TransportEvent e;
        while (egress.Poll(e)) {
            if (e.kind == TransportEvent::Kind::Connected) {
                linked = true; for (const auto& b : pending) egress.Send(outgoing, Channel::Control, b); pending.clear();
            } else if (e.kind == TransportEvent::Kind::Message) ingress.Send(incoming, e.channel, e.bytes);
            else closed = true;
        }
        while (ingress.Poll(e)) {
            if (e.kind == TransportEvent::Kind::Connected) incoming = e.connection;
            else if (e.kind == TransportEvent::Kind::Message) {
                if (!linked) { pending.push_back(e.bytes); continue; }
                if (sent && !attacked && attack != Attack::None) {
                    attacked = true;
                    if (attack == Attack::Tamper) e.bytes.back() ^= 1;
                    if (attack == Attack::Channel) e.channel = Channel::Authority;
                    if (attack == Attack::Plaintext) e.bytes = payload;
                    if (attack == Attack::Replay) egress.Send(outgoing, e.channel, e.bytes);
                }
                egress.Send(outgoing, e.channel, e.bytes);
            }
        }
        while (client.Poll(e)) {
            if (e.kind == TransportEvent::Kind::Connected) clientReady = true;
            else if (e.kind == TransportEvent::Kind::Disconnected) closed = true;
            else { Require(e.bytes == payload, "private echo changed payload"); echoed = true; }
        }
        while (server.Poll(e)) {
            if (e.kind == TransportEvent::Kind::Connected) serverReady = true;
            else if (e.kind == TransportEvent::Kind::Disconnected) closed = true;
            else {
                Require(e.bytes == payload, "unauthenticated or changed plaintext exposed"); ++messages;
                if (attack == Attack::None) server.Send(e.connection, Channel::Authority, e.bytes);
            }
        }
        if (clientReady && serverReady && !sent) {
            client.Send(clientPeer, Channel::Control, payload); client.Flush(); sent = true;
        }
        ingress.Flush(); egress.Flush(); server.Flush(); client.Flush();
        if (attack == Attack::None && messages == 1 && echoed) return;
        if (closed) {
            Require(attack != Attack::None, "private connection closed unexpectedly");
            if (attack == Attack::WrongInvitation || attack == Attack::WrongHost)
                Require(!serverReady && !clientReady && messages == 0, "invalid invitation admitted");
            else Require(attacked && messages == (attack == Attack::Replay ? 1u : 0u), "attack reached the application");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("private transport test timed out");
}
}
int main() {
    try {
        CheckFiles();
        for (auto attack : {Attack::None, Attack::WrongInvitation, Attack::WrongHost, Attack::Tamper, Attack::Replay, Attack::Channel, Attack::Plaintext}) Check(attack);
        std::puts("private transport: PASS, authenticated round trip, invitation and host authentication, tampering, replay, channel binding and plaintext rejection");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "private transport: %s\n", e.what()); return 1; }
}
