#include "network_setup.h"
#include "platform/net/lan_room.h"
#include "platform/net/room_files.h"
#include "gt2formats/exe_profile.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>
#include <stdexcept>
namespace { volatile std::sig_atomic_t stopping = 0; void Stop(int) { stopping = 1; } }
int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::puts("GT2 dedicated host\nusage: gt2server <disc-folder> [--track autumn] [--cars 6] [--players 2] [--laps 2] [--manual]\n"
                "  [--bind 127.0.0.1] [--port 29600] --host-key <private-file> [--invite-out <file> --public-address <host-name>]\n"
                "  [--rounds N] [--seconds N] [--trusted-lan]\nNo game window, audio device or career save is opened. Keep the host key private; share only the invitation.");
            return 2;
        }
        using namespace gt2; using namespace gt2game; using namespace gt2::net;
        std::string track = "autumn", bind = "127.0.0.1", publicAddress, keyPath, invitePath;
        unsigned cars = 6, players = 2, port = 29600, rounds = 0, seconds = 0;
        bool trustedLan = false; RaceOptions options;
        auto number = [](const std::string& value, unsigned low, unsigned high) {
            size_t used = 0; const auto n = std::stoul(value, &used);
            if (used != value.size() || n < low || n > high) throw std::invalid_argument("invalid server numeric option");
            return unsigned(n);
        };
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&]() -> std::string { if (++i >= argc) throw std::invalid_argument("missing server option value"); return argv[i]; };
            if (arg == "--track") track = value();
            else if (arg == "--bind") bind = value();
            else if (arg == "--public-address") publicAddress = value();
            else if (arg == "--host-key") keyPath = value();
            else if (arg == "--invite-out") invitePath = value();
            else if (arg == "--cars") cars = number(value(), 2, 6);
            else if (arg == "--players") players = number(value(), 2, 6);
            else if (arg == "--laps") options.laps = uint8_t(number(value(), 1, 10));
            else if (arg == "--port") port = number(value(), 1, 65535);
            else if (arg == "--rounds") rounds = number(value(), 1, 100000);
            else if (arg == "--seconds") seconds = number(value(), 1, 86400);
            else if (arg == "--manual") options.manual = true;
            else if (arg == "--trusted-lan") trustedLan = true;
            else throw std::invalid_argument("unknown server option");
        }
        if (players > cars || (keyPath.empty() && !trustedLan) || (!keyPath.empty() && trustedLan))
            throw std::invalid_argument("choose a host key or explicit trusted LAN, with players <= cars");
        std::optional<RoomSecurity> security;
        if (!keyPath.empty()) {
            if (std::filesystem::exists(keyPath)) security = ReadHostKey(keyPath);
            else { security = NewRoomSecurity(); WriteHostKey(keyPath, *security); }
        }
        if (!invitePath.empty()) {
            if (!security || publicAddress.empty()) throw std::invalid_argument("invitation needs host key and public address");
            WriteInvitation(invitePath, {publicAddress, uint16_t(port), security->Public()});
            std::puts("Private invitation written. Share the invitation file; keep the host key on this machine.");
        }
        DiscImage disc(argv[1]); GtfsVolume volume(disc,GtfsVolume::Source::DiscOnly); const auto& profile = ProfileOf(disc);
        if ((!profile.reference && profile.build != gt2::ExeBuild::kArcadeUs11)) throw std::invalid_argument("server requires stock US Simulation v1.2 or US Arcade v1.1 data");
        SetActiveProfile(profile);
        RaceData data; LoadRaceTrack(disc, volume, track, {}, data); BuildCarRecords(volume, "us36n", cars, {}, data);
        LanRoomConfig config;
        config.host = config.dedicated = true; config.address = bind; config.port = uint16_t(port);
        config.cars = uint8_t(cars); config.humans = uint8_t(players); config.security = security;
        config.content = NetworkContent(disc, volume, data, cars, options, uint8_t(players));
        config.setup = DescribeNetworkSetup(data, cars, options, uint8_t(players));
        std::signal(SIGINT, Stop); std::signal(SIGTERM, Stop);
        const auto started = std::chrono::steady_clock::now();
        for (unsigned round = 1; !stopping && (!rounds || round <= rounds); ++round) {
            config.session = NewSessionId();
            auto authority = std::make_unique<sim::RaceSim>();
            auto shell = data.shell; shell.lapCount = options.laps; shell.countdown = options.countdown;
            SetupNetworkRace(*authority, data.track, data.extras, data.constants, data.course, data.params,
                GridSlots(data, cars, options), shell, RoomRoster(config.cars, config.humans));
            LanRoom room(config, std::move(authority)); room.RequestStart();
            std::printf("server: round %u, %s:%u, %s, %u remote players, %u cars, %s\n", round, bind.c_str(), port,
                track.c_str(), players, cars, security ? "private encrypted room" : "trusted LAN only"); std::fflush(stdout);
            RoomPhase phase = RoomPhase::Connecting; uint8_t connected = 255, ready = 255;
            std::optional<std::chrono::steady_clock::time_point> finished;
            while (!stopping) {
                const auto s = room.Status();
                if (s.phase == RoomPhase::Failed) throw std::runtime_error(s.error);
                if (s.phase != phase || s.connected != connected || s.ready != ready) {
                    phase = s.phase; connected = s.connected; ready = s.ready;
                    std::printf("server: phase %d, connected %02x, ready %02x, tick %u\n", int(phase), connected, ready, s.nextTick);
                    std::fflush(stdout);
                }
                const auto now = std::chrono::steady_clock::now();
                if (seconds && now - started >= std::chrono::seconds(seconds)) { stopping = 1; break; }
                if (phase == RoomPhase::Complete && !finished) finished = now;
                if (finished && now - *finished >= std::chrono::seconds(20)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        std::puts("server: stopped cleanly"); return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "server: %s\n", e.what()); return 1; }
}
