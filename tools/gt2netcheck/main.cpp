#include "network_setup.h"
#include "gt2formats/arcade_menu_data.h"
#include "network_check.h"
#include "platform/net/lobby_cars.h"
#include "gt2formats/course_map.h"
#include "gt2formats/exe_profile.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include <cstdio>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc < 2 || argc > 7) {
            std::fprintf(stderr, "usage: gt2netcheck <disc-folder-or-image> [track=seattle] [cars=6] [--room-check | --state-check | --state-export|--state-import path | --listen|--connect|--room-listen|--room-connect|--recovery-listen|--recovery-connect|--view-listen|--view-connect|--view-late-listen|--view-late-connect|--desync-listen|--desync-connect|--desync-repeat-listen|--desync-repeat-connect IPv4 port]\n");
            return 2;
        }
        const size_t cars = argc > 3 ? size_t(std::stoul(argv[3])) : 6;
        if (cars < 2 || cars > 6) throw std::invalid_argument("cars must be 2..6");
        gt2::DiscImage disc(argv[1]);
        gt2::GtfsVolume volume(disc);
        const auto& profile = gt2::ProfileOf(disc);
        gt2::SetActiveProfile(profile);
        if(argc>2&&std::string(argv[2])=="--lobby-catalog") {
            const auto menu=gt2::ArcadeMenuData::Load(disc,1);
            for(size_t i=0;i<gt2::net::kLobbyTracks.size();++i){
                const auto name=gt2::net::kLobbyTracks[i];gt2game::RaceData loaded;
                gt2game::LoadRaceTrack(disc,volume,name,{},loaded);
                const auto picture=gt2::LoadCoursePicture(volume,gt2::net::LobbyTrackPicture(uint8_t(i)));
                if(picture.words!=47||picture.rows!=200)throw std::runtime_error(std::string("bad picture: ")+name);
                gt2::net::LobbyOptions options;options.track=uint8_t(i);options.carClass=gt2::net::LobbyTrackDirt(options.track)?6:1;
                options.fixedCar=gt2::net::LobbyClassCars(menu,options.carClass).front();
                if(!gt2::net::LobbyCarAllowed(menu,options,options.fixedCar))throw std::runtime_error("invalid track/car rules");
                if(gt2::net::LobbyTrackPointToPoint(options.track)!=loaded.shell.pointToPoint)throw std::runtime_error(std::string("point-to-point mismatch: ")+name);
                std::printf("catalog PASS %s picture %ux%u\n",name,picture.words*4,picture.rows);
            }
            gt2::net::LobbyOptions options,road;options.carClass=2;options.carRule=1;options.fixedCar=gt2::net::LobbyClassCars(menu,2).back();const auto before=options;
            gt2::net::SelectLobbyTrack(menu,options,6,road);
            if(options.carClass!=6||options.carRule!=before.carRule)throw std::runtime_error("rally changed car rule");
            gt2::net::SelectLobbyTrack(menu,options,0,road);
            if(!(options==before))throw std::runtime_error("road car settings were not restored");
            std::puts("road/rally/road rules PASS");return 0;
        }
        if(argc>2&&std::string(argv[2])=="--catalog") {
            const auto menu=gt2::ArcadeMenuData::Load(disc,1);
            for(const auto& c:menu.courses[4])std::printf("rally track %s %s\n",c.file.c_str(),c.display.c_str());
            for(size_t i:{size_t(0),size_t(1),size_t(2),size_t(3),size_t(6)}){std::printf("class %zu:",i);for(const auto& c:menu.classes[i].cars)std::printf(" %s",c.c_str());std::puts("");}return 0;
        }
        if(argc==6&&std::string(argv[4])=="--directory-check")return gt2game::NetworkDirectoryCheck(disc,volume,argv[5],argv[2],unsigned(cars));
        gt2game::RaceData data;
        gt2game::LoadRaceTrack(disc, volume, argc > 2 ? argv[2] : "seattle", {}, data);
        if(argc>4&&std::string(argv[4])=="--rally-check") {
            const auto menu=gt2::ArcadeMenuData::Load(disc,1);std::vector<std::string> ids;for(size_t i=0;i<cars;++i)ids.push_back(menu.classes[6].cars[i%4]);
            gt2game::BuildNetworkCarRecords(volume,ids,data);return gt2game::NetworkRaceCheck(data,cars);
        }
        gt2game::BuildCarRecords(volume, "us36n", cars, {}, data);
        std::printf("network check disc: %s (%s)\n", profile.name, profile.exeSha1);
        if (argc == 5) {
            if (std::string(argv[4]) == "--private-room-check") return gt2game::NetworkPrivateRoomCheck(data);
            if (std::string(argv[4]) == "--state-check") return gt2game::NetworkStateCheck(data, cars);
            if (std::string(argv[4]) != "--room-check" || cars != 6) throw std::invalid_argument("room check requires six cars and --room-check");
            return gt2game::NetworkRoomCheck(data);
        }
        if (argc == 6) {
            const std::string mode = argv[4];
            if (mode == "--invite-check" || mode == "--invite-restart-check")
                return gt2game::NetworkInvitationCheck(disc, volume, argv[5], mode == "--invite-restart-check");
            if (mode == "--state-export") return gt2game::NetworkStateCheck(data, cars, {}, argv[5]);
            if (mode == "--state-import") return gt2game::NetworkStateCheck(data, cars, argv[5]);
            throw std::invalid_argument("invalid state diagnostic mode");
        }
        if (argc == 7) {
            const std::string mode = argv[4];
            const auto port = std::stoul(argv[6]);
            if ((mode != "--listen" && mode != "--connect" && mode != "--room-listen" && mode != "--room-connect" &&
                 mode != "--recovery-listen" && mode != "--recovery-connect" && mode != "--view-listen" && mode != "--view-connect" &&
                 mode != "--view-late-listen" && mode != "--view-late-connect" &&
                 mode != "--desync-listen" && mode != "--desync-connect" && mode != "--desync-repeat-listen" && mode != "--desync-repeat-connect") || !port || port > 65535)
                throw std::invalid_argument("invalid peer diagnostic options");
            if (mode.starts_with("--room-")) return gt2game::NetworkRoomPeerCheck(data, cars, mode == "--room-listen", argv[5], uint16_t(port));
            if (mode.starts_with("--recovery-")) return gt2game::NetworkRoomPeerCheck(data, cars, mode == "--recovery-listen", argv[5], uint16_t(port), true);
            if (mode.starts_with("--view-")) return gt2game::NetworkRoomPeerCheck(data, cars, mode.ends_with("-listen"), argv[5], uint16_t(port), mode.starts_with("--view-late-"), true);
            if (mode.starts_with("--desync-")) return gt2game::NetworkRoomPeerCheck(data, cars, mode.ends_with("-listen"), argv[5], uint16_t(port), false, false, mode.starts_with("--desync-repeat-") ? 4 : 1);
            return gt2game::NetworkPeerCheck(data, cars, mode == "--listen", argv[5], uint16_t(port));
        }
        return gt2game::NetworkRaceCheck(data, cars);
    } catch (const std::exception& e) { std::fprintf(stderr, "network check: %s\n", e.what()); return 1; }
}
