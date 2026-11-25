#include "network_setup.h"
#include "gt2formats/car_info.h"
#include "gt2formats/arcade_data.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/sha1.h"
#include "gt2vfs/gtfs.h"
#include <algorithm>
#include <stdexcept>
namespace gt2game {
namespace net = gt2::net;
void BuildNetworkCarRecords(const gt2::GtfsVolume& volume,const std::vector<std::string>& ids,RaceData& data){
    data.networkArcadeCars=true;
    const auto arcade=gt2::ArcadeData::Load(volume);
    for(const auto& id:ids){
        const auto row=arcade.FindPlayerCar(gt2::PackCarId(id));
        if(!row)throw std::invalid_argument("car missing from Arcade physics catalogue");
        auto config=arcade.PlayerCarConfig(*row);
        data.bodies.push_back(CarBodyOf(volume,arcade.Tables(),config));
        data.params.push_back(BuildCarParams(volume,arcade.Tables(),config));
        data.configs.push_back(config);data.carIds.push_back(id);data.paints.push_back(0);
        gt2::audio::CarSoundSetup sound;sound.soundId=config.engineWord;sound.exhaustByte=config.exhaustByte;sound.turbo=(config.flags&2)!=0;data.sound.push_back(sound);
    }
}
net::RoomSetup DescribeNetworkSetup(const RaceData& data, size_t cars, const RaceOptions& options, uint8_t humans) {
    net::RoomSetup setup;
    setup.arcadeCars=data.networkArcadeCars;setup.cars = uint8_t(cars); setup.humans = humans; setup.laps = options.laps;
    setup.botDifficulty=options.botDifficulty;setup.manual = options.manual; setup.countdown = options.countdown; setup.track = data.trackName;
    for (size_t i = 0; i < cars; ++i) { setup.carIds.at(i) = data.carIds.at(i); setup.paints.at(i) = data.paints.at(i); }
    if (!net::ValidRoomSetup(setup)) throw std::invalid_argument("unsupported stock room setup");
    return setup;
}
net::ContentKey NetworkContent(const gt2::DiscImage& disc, const gt2::GtfsVolume& volume, const RaceData& data,
                        size_t cars, const RaceOptions& options, uint8_t humans) {
    // Source bytes and explicit text fields, never C++ object layout or padding.
    // This preview only admits stock standalone circuit setups. Bump the physics
    // revision whenever the network simulation or its setup rules change.
    std::string manifest = "GT2-LAN-100-physics-5\n";
    manifest+=data.networkArcadeCars?"arcade-cars\n":"stock-cars\n";
    auto add = [&](const std::string& label, std::span<const uint8_t> bytes) {
        manifest += label + ":" + gt2::Sha1Hex(bytes) + "\n";
    };
    add("exe", gt2::LoadExeImage(disc).bytes);
    add("race-overlay", gt2::LoadOverlayImage(disc, gt2::kRaceOverlayIndex).bytes);
    add("course-info", volume.Read(".crsinfo"));
    add("track", volume.Read("crsobj/" + data.trackName + ".tro"));
    for (const auto& file : volume.Files()) if (file.path.starts_with("carparam/")) add(file.path, volume.Read(file));
    manifest += data.trackName + "\n" + std::to_string(cars) + ":" + std::to_string(humans) + ":" +
        std::to_string(options.laps) + ":" + std::to_string(options.countdown) + ":" + std::to_string(options.manual) + ":" + std::to_string(options.botDifficulty) + "\n";
    for (size_t i = 0; i < cars; ++i) {
        add(data.carIds.at(i), volume.Read("carobj/" + data.carIds.at(i) + ".cdo"));
        manifest += std::to_string(i) + ":" + data.carIds.at(i) + ":" + std::to_string(data.paints.at(i)) + "\n";
    }
    const auto digest = gt2::Sha1Hex(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()));
    net::ContentKey key{};
    std::copy_n(digest.begin(), key.size(), key.begin()); // 128-bit hexadecimal compatibility ID, not authentication
    return key;
}
}
