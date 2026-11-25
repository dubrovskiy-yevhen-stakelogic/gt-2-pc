#pragma once
#include "platform/net/room_files.h"
#include <chrono>
#include <string_view>
namespace gt2::net {
// Wire revision 6: expose each player's return-to-room status.
// Only the recipient receives its slot capability; public listings contain none.
inline constexpr std::array<const char*,50> kLobbyTracks{"seattle","autumn","circuit","mountain","parma","new_parmaS","tahiti_d_new","nn_dirt_2","nn_dirt_3","no_name_dirt","tahiti_test7","pikes","tahiti_d_new_rev","no_name_dirt_rev","pikes_rev","seattle_r","rev_autumn","rev_circuit","rev_mountain","parma_rev","new_parmaS_rev","roma","rev_roma","roma_night","rev_roma_night","roma_short","rev_roma_short","seatt_s","seatt_s_r","short","rev_short","mini","rev_mini","test_in2","rev_test_in2","testline","rev_testline","laguna","sprint2","rev_sprint2","tahiti_t","tahiti_t_rev","Gtest","reverse","highway","Rhighway","shortway","Rshortway","s_speed","nn_dirt_2_rev"};
struct LobbyTrackFamily { const char* name; std::array<uint8_t,4> variants; bool dirt; };
inline constexpr std::array<LobbyTrackFamily,26> kLobbyTrackFamilies{{
    {"Seattle",{0,15,255,255},false},
    {"Autumn Ring",{1,16,255,255},false},
    {"Grand Valley",{2,17,255,255},false},
    {"Trial Mountain",{3,18,255,255},false},
    {"Midfield",{4,19,255,255},false},
    {"Apricot Hill",{5,20,255,255},false},
    {"Rome",{21,22,23,24},false},
    {"Rome Short",{25,26,255,255},false},
    {"Seattle Short",{27,28,255,255},false},
    {"Grand Valley East",{29,30,255,255},false},
    {"Autumn Ring Mini",{31,32,255,255},false},
    {"Deep Forest",{33,34,255,255},false},
    {"High Speed Ring",{35,36,255,255},false},
    {"Laguna Seca",{37,255,255,255},false},
    {"Red Rock Valley",{38,39,255,255},false},
    {"Tahiti Road",{40,41,255,255},false},
    {"Grindelwald",{42,43,255,255},false},
    {"Special Stage Route 5",{255,255,44,45},false},
    {"Clubman Stage Route 5",{255,255,46,47},false},
    {"Super Speedway",{48,255,255,255},false},
    {"Tahiti Circuit",{6,12,255,255},true},
    {"Smokey Mountain South",{7,49,255,255},true},
    {"Green Forest",{8,255,255,255},true},
    {"Smokey Mountain North",{9,13,255,255},true},
    {"Tahiti Maze",{10,255,255,255},true},
    {"Pikes Peak",{11,14,255,255},true},
}};
inline size_t LobbyTrackFamilyOf(uint8_t track) {
    for(size_t i=0;i<kLobbyTrackFamilies.size();++i)for(auto v:kLobbyTrackFamilies[i].variants)if(v==track)return i;
    return 0;
}
inline const char* LobbyTrackPicture(uint8_t track) {return std::string_view(kLobbyTracks[track])=="nn_dirt_2_rev"?"nn_dirt_2":kLobbyTracks[track];}
inline bool LobbyTrackDirt(uint8_t track) {return kLobbyTrackFamilies[LobbyTrackFamilyOf(track)].dirt;}
inline bool LobbyTrackPointToPoint(uint8_t track) {return track==11||track==14;}

struct LobbyOptions { uint8_t track=0,players=2,laps=2,manual=0; uint8_t carRule=0,carClass=1,bots=1,botDifficulty=1; std::string fixedCar="ccrcn"; bool operator==(const LobbyOptions&)const=default; };
struct LobbyMember { std::string name,car; uint8_t paint=0; bool occupied=false,ready=false,returned=false; };
struct LobbyRoom { uint64_t id=0;uint16_t port=0;LobbyOptions options;uint8_t connected=0,phase=0;RoomSecurity security; std::string name="GT2 room"; bool passwordRequired=false; uint8_t localSlot=255,hostSlot=255,raceSlot=255; uint32_t revision=1; ResumeToken admission{}; std::array<LobbyMember,6> members{}; };
// Returning is an acknowledged transition, not a one-shot button command.
class LobbyReturnState {
public:
    void Begin(uint64_t room) { room_ = room; }
    void Observe(const LobbyRoom& room) { if(room.id==room_ && room.phase==0) room_=0; }
    bool Pending(const LobbyRoom& room) const { return room_==room.id && room.phase!=0; }
    void Clear() { room_=0; }
private:
    uint64_t room_=0;
};
enum class LobbyKind:uint8_t { List=1,Create,Rooms,Created,Error,Join,Joined,Update,Waiting,Leave,Start,Kick,Configure,Return };
struct LobbyPacket { LobbyKind kind=LobbyKind::List;uint32_t request=0;uint8_t error=0;LobbyOptions options;std::vector<LobbyRoom> rooms; uint64_t roomId=0; std::string name,password; std::string car,nickname; uint8_t paint=0; bool ready=false; uint8_t target=255; uint32_t revision=0; };
bool ValidLobbyOptions(const LobbyOptions& options);
std::vector<uint8_t> EncodeLobbyPacket(const LobbyPacket& packet);
bool DecodeLobbyPacket(std::span<const uint8_t> bytes,LobbyPacket& packet);
RoomInvitation DefaultLobby();
class LobbyClient {
public:
    explicit LobbyClient(RoomInvitation endpoint=DefaultLobby());
    ~LobbyClient();
    void Poll();void RequestList();void Create(const LobbyOptions& options, const std::string& name="GT2 room", const std::string& password={});
    void Join(uint64_t roomId, const std::string& password={});
    void Update(uint64_t roomId,const std::string& nickname,const std::string& car,uint8_t paint,bool ready,uint32_t revision=0);
    void Command(LobbyKind kind,uint64_t roomId,uint32_t revision,uint8_t target=255,LobbyOptions options={});
    void Leave(uint64_t roomId);
    bool Busy()const;const std::vector<LobbyRoom>& Rooms()const;const std::string& Error()const;
    std::optional<LobbyRoom> TakeCreated();const std::string& Address()const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
