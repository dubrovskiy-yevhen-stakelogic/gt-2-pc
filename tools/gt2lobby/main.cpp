#include "network_setup.h"
#include "gt2formats/car_info.h"
#include "platform/net/lobby_cars.h"
#include "platform/net/lobby_rules.h"
#include "platform/net/lan_room.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/gtmode_tables.h"
#include "gt2vfs/gtfs.h"
#include <algorithm>
#include <bit>
#include <csignal>
#include <cstdio>
#include <map>
#include <thread>
namespace { volatile std::sig_atomic_t stopping=0;void Stop(int){stopping=1;} }
int main(int argc,char** argv){try{
    using namespace gt2;using namespace gt2game;using namespace gt2::net;
    if(argc<3){std::puts("gt2lobby <arcade disc> <directory host key> [bind address] [seconds]");return 2;}
    const std::string bind=argc>3?argv[3]:"127.0.0.1";
    unsigned seconds=0;if(argc>4){size_t used=0;seconds=unsigned(std::stoul(argv[4],&used));if(used!=std::string(argv[4]).size()||seconds>86400)throw std::invalid_argument("invalid duration");}
    DiscImage disc(argv[1]);GtfsVolume volume(disc,GtfsVolume::Source::DiscOnly);const auto& profile=ProfileOf(disc);
    if(profile.build!=ExeBuild::kArcadeUs11)throw std::invalid_argument("directory requires US Arcade v1.1 data");SetActiveProfile(profile);
    const auto catalogue=ArcadeMenuData::Load(disc,1);const auto carInfo=CarInfoDirectory::Load(volume);
    auto security=ReadHostKey(argv[2]);security.invitation={};
    PrivateTransport directory(bind,29610,32,security,AccessPolicy::PublicDirectory);
    using Clock=std::chrono::steady_clock;const auto started=Clock::now();auto lastCreate=started-std::chrono::seconds(5);
    struct Entry {
        std::string password;RaceData data;LobbyRoom listing;LanRoomConfig config;std::unique_ptr<LanRoom> room;
        std::array<ConnectionId,6> peers{};Clock::time_point created=Clock::now(),occupied=created,raceCreated=created;
        std::array<bool,6> returned{};std::array<uint8_t,6> raceSlots{255,255,255,255,255,255};
        bool everRaced=false;std::optional<Clock::time_point> finished;
    };
    std::array<std::unique_ptr<Entry>,4> rooms;
    struct Client{Clock::time_point seen=Clock::now(),window=seen,nextJoin=seen;unsigned requests=0;uint64_t room=0;uint8_t slot=255;};
    std::map<ConnectionId,Client> clients;
    auto count=[](const Entry& e){return uint8_t(std::count_if(e.listing.members.begin(),e.listing.members.end(),[](const auto& m){return m.occupied;}));};
    auto removeClient=[&](ConnectionId peer){
        auto c=clients.find(peer);if(c==clients.end())return;
        for(auto& e:rooms)if(e&&e->listing.id==c->second.room){
            if(c->second.slot<6&&e->peers[c->second.slot]==peer){if(e->room)e->room->Kick(e->raceSlots[c->second.slot]);e->peers[c->second.slot]=0;e->listing.members[c->second.slot]={};}
            ElectLobbyHost(e->listing);if(!count(*e))e.reset();
        }clients.erase(c);
    };
    auto describe=[&](Entry& e,uint8_t slot=255){auto r=e.listing;r.connected=count(e);r.localSlot=slot;r.security.invitation={};r.admission={};
        if(e.room){for(size_t i=0;i<6;++i)r.members[i].returned=r.members[i].occupied&&e.returned[i];auto status=e.room->Status();r.phase=(status.phase==RoomPhase::Complete||(slot<6&&e.returned[slot]))?2:1;
            if(slot<r.options.players&&r.phase==1){r.security=e.config.security->Public();r.raceSlot=e.raceSlots[slot];r.admission=e.config.admissions[r.raceSlot];}}
        return r;};
    auto startRace=[&](Entry& e){
        const auto& o=e.listing.options;RaceOptions options;options.botDifficulty=o.botDifficulty;options.laps=o.laps;options.manual=o.manual!=0;
        LoadRaceTrack(disc,volume,kLobbyTracks[o.track],{},e.data);
        if(e.data.shell.pointToPoint)options.laps=1;
        std::vector<std::string> cars;std::vector<uint8_t> paints;e.raceSlots.fill(255);
        for(uint8_t i=0;i<o.players;++i)if(e.listing.members[i].occupied){e.raceSlots[i]=LobbyRaceSlot(e.listing,i);cars.push_back(e.listing.members[i].car);paints.push_back(e.listing.members[i].paint);}
        const auto humans=uint8_t(cars.size());const auto allowed=LobbyClassCars(catalogue,o.carClass);
        while(o.bots&&cars.size()<6)cars.push_back(o.carRule==0?o.fixedCar:allowed[cars.size()%allowed.size()]);
        BuildNetworkCarRecords(volume,cars,e.data);for(size_t i=0;i<paints.size();++i)e.data.paints[i]=paints[i];
        auto& c=e.config;c.host=c.dedicated=true;c.address=bind;c.port=e.listing.port;c.cars=uint8_t(cars.size());c.humans=humans;c.session=NewSessionId();
        c.security=NewRoomSecurity();e.listing.security=c.security->Public();e.listing.security.invitation={};c.admissions={};
        c.content=NetworkContent(disc,volume,e.data,c.cars,options,c.humans);c.setup=DescribeNetworkSetup(e.data,c.cars,options,c.humans);
        for(size_t i=0;i<humans;++i){auto random=NewRoomSecurity();std::copy_n(random.invitation.begin(),16,c.admissions[i].begin());}
        e.returned.fill(false);e.everRaced=false;e.finished.reset();
        auto authority=std::make_unique<sim::RaceSim>();auto shell=e.data.shell;shell.lapCount=options.laps;shell.countdown=options.countdown;
        SetupNetworkRace(*authority,e.data.track,e.data.extras,e.data.constants,e.data.course,e.data.params,GridSlots(e.data,c.cars,options),shell,RoomRoster(c.cars,c.humans),options.botDifficulty);
        e.room=std::make_unique<LanRoom>(c,std::move(authority));e.room->RequestStart();e.raceCreated=e.occupied=Clock::now();
        const auto until=Clock::now()+std::chrono::seconds(2);while(e.room->Status().phase==RoomPhase::Connecting&&Clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if(e.room->Status().phase!=RoomPhase::Lobby)throw std::runtime_error("race startup failed");
    };
    std::signal(SIGINT,Stop);std::signal(SIGTERM,Stop);std::puts("directory: ready on UDP 29610 (staging protocol 6)");std::fflush(stdout);
    while(!stopping){const auto now=Clock::now();if(seconds&&now-started>=std::chrono::seconds(seconds))break;
        for(auto& e:rooms)if(e){
            if(e->room){const auto s=e->room->Status();if(s.connected){e->occupied=now;e->everRaced=true;}
                if(s.phase==RoomPhase::Complete&&!e->finished){e->finished=now;for(auto& member:e->listing.members)member.ready=false;std::fprintf(stderr,"room completed at tick %u\n",s.nextTick);}
                if(s.phase==RoomPhase::Failed)std::fprintf(stderr,"race worker failed at tick %u: %s\n",s.nextTick,s.error.c_str());
                const bool allReturned=LobbyAllReturned(e->listing,e->returned);
                unsigned loadingPlayers=0;for(size_t i=0;i<6;++i)if(e->listing.members[i].occupied&&!e->returned[i])++loadingPlayers;
                const bool insufficientPlayers=s.phase==RoomPhase::Lobby&&loadingPlayers<2;
                // Keep the room, membership and host. Results stay available until every remaining player returns.
                if(s.phase==RoomPhase::Failed||s.phase==RoomPhase::Closed||allReturned||insufficientPlayers||(s.phase==RoomPhase::Lobby&&now-e->raceCreated>std::chrono::seconds(120))||(!s.connected&&now-e->occupied>std::chrono::seconds(120))){
                    e->room.reset();e->data=RaceData{};e->listing.phase=0;ResetLobbyReady(e->listing);e->finished.reset();e->everRaced=false;
                }
            }
            if(e&&!count(*e))e.reset();
        }
        TransportEvent event;
        for(unsigned n=0;n<64&&directory.Poll(event);++n){
            if(event.kind==TransportEvent::Kind::Connected){clients.emplace(event.connection,Client{});continue;}
            if(event.kind==TransportEvent::Kind::Disconnected){removeClient(event.connection);continue;}
            auto it=clients.find(event.connection);if(it==clients.end())continue;auto& client=it->second;
            LobbyPacket q;if(event.channel!=Channel::Control||!DecodeLobbyPacket(event.bytes,q)||(q.kind!=LobbyKind::List&&q.kind!=LobbyKind::Create&&q.kind!=LobbyKind::Join&&q.kind!=LobbyKind::Update&&q.kind!=LobbyKind::Leave&&q.kind!=LobbyKind::Start&&q.kind!=LobbyKind::Kick&&q.kind!=LobbyKind::Configure&&q.kind!=LobbyKind::Return)){directory.Disconnect(event.connection);removeClient(event.connection);continue;}
            if(now-client.window>=std::chrono::seconds(1)){client.window=now;client.requests=0;}
            if(++client.requests>8){directory.Disconnect(event.connection);removeClient(event.connection);continue;}client.seen=now;
            LobbyPacket reply;reply.request=q.request;reply.kind=LobbyKind::Rooms;
            auto error=[&](uint8_t code){reply.kind=LobbyKind::Error;reply.error=code;reply.rooms.clear();};
            auto find=[&](uint64_t id){return std::find_if(rooms.begin(),rooms.end(),[&](const auto& e){return e&&e->listing.id==id;});};
            auto enter=[&](Entry& e){uint8_t slot=0;while(slot<e.listing.options.players&&e.listing.members[slot].occupied)++slot;
                if(slot==e.listing.options.players){error(6);return;}
                client.room=e.listing.id;client.slot=slot;e.peers[slot]=event.connection;auto& m=e.listing.members[slot];m.occupied=true;m.name="Player";m.car=e.listing.options.carRule==0?e.listing.options.fixedCar:LobbyClassCars(catalogue,e.listing.options.carClass).front();ElectLobbyHost(e.listing);reply.rooms.push_back(describe(e,slot));};
            if(q.kind==LobbyKind::Create){
                auto free=std::find_if(rooms.begin(),rooms.end(),[](const auto& e){return !e;});
                if(free==rooms.end())error(1);
                else if(find(client.room)!=rooms.end()||now-lastCreate<std::chrono::seconds(3))error(2);
                else if(!LobbyCarAllowed(catalogue,q.options,q.options.carRule==0?q.options.fixedCar:LobbyClassCars(catalogue,q.options.carClass).front()))error(7);
                else{lastCreate=now;auto e=std::make_unique<Entry>();e->listing.id=NewSessionId();e->listing.port=uint16_t(29620+std::distance(rooms.begin(),free));e->listing.options=q.options;e->listing.name=q.name.empty()?"GT2 room":q.name;e->listing.passwordRequired=!q.password.empty();e->password=q.password;e->config.security=NewRoomSecurity();e->listing.security=e->config.security->Public();e->listing.security.invitation={};reply.kind=LobbyKind::Created;enter(*e);*free=std::move(e);}
            }else if(q.kind==LobbyKind::Join){auto found=find(q.roomId);
                if(now<client.nextJoin)error(2);
                else if(found==rooms.end()||(*found)->room||count(**found)>=(*found)->listing.options.players||find(client.room)!=rooms.end())error(6);
                else if((*found)->password!=q.password){client.nextJoin=now+std::chrono::seconds(2);error(5);}
                else{reply.kind=LobbyKind::Joined;enter(**found);}
            }else if(q.kind==LobbyKind::Update){auto found=find(q.roomId);
                if(found==rooms.end()||client.room!=q.roomId||client.slot>=6||(*found)->peers[client.slot]!=event.connection)error(6);
                else{auto& e=**found;auto& m=e.listing.members[client.slot];
                    // Paint indices are visual (GT4 palettes can exceed retail GT2 counts); both catalogues cap them at 32.
                    if(!e.room&&q.revision==e.listing.revision){const auto* info=LobbyCarAllowed(catalogue,e.listing.options,q.car)?carInfo.Find(PackCarId(q.car)):nullptr;
                        if(!LobbyCarAllowed(catalogue,e.listing.options,q.car)||!info||q.paint>=32)error(7);
                        else{const bool changed=m.car!=q.car||m.paint!=q.paint;m.name=q.nickname;m.car=q.car;m.paint=q.paint;m.ready=changed?false:q.ready;

                        }
                    }
                    if(reply.kind!=LobbyKind::Error){reply.kind=LobbyKind::Waiting;reply.rooms.push_back(describe(e,client.slot));}
                }
            }else if(q.kind==LobbyKind::Start||q.kind==LobbyKind::Kick||q.kind==LobbyKind::Configure||q.kind==LobbyKind::Return){auto found=find(q.roomId);
                if(found==rooms.end()||client.room!=q.roomId||client.slot>=6||(*found)->peers[client.slot]!=event.connection)error(6);
                else{auto& e=**found;
                    if(q.kind==LobbyKind::Return){if(e.room&&q.revision==e.listing.revision){e.returned[client.slot]=true;e.listing.members[client.slot].ready=false;e.room->Kick(e.raceSlots[client.slot]);}}
                    else if(client.slot!=e.listing.hostSlot)error(8);
                    else if((e.room&&q.kind!=LobbyKind::Kick)||q.revision!=e.listing.revision)error(9);
                    else if(q.kind==LobbyKind::Start){
                        if(!LobbyCanStart(e.listing))error(9);
                        else try{startRace(e);}catch(const std::exception& ex){std::fprintf(stderr,"room start: %s\n",ex.what());e.room.reset();e.data=RaceData{};ResetLobbyReady(e.listing);error(3);}
                    }else if(q.kind==LobbyKind::Kick){
                        if(q.target==client.slot||!e.listing.members[q.target].occupied)error(6);
                        else{if(e.room)e.room->Kick(e.raceSlots[q.target]);const auto peer=e.peers[q.target];auto kicked=clients.find(peer);if(kicked!=clients.end()){kicked->second.room=0;kicked->second.slot=255;}e.peers[q.target]=0;e.listing.members[q.target]={};}
                    }else{
                        // Capacity is fixed for the lifetime of a room; track/car rules may change between races.
                        if(q.options.players!=e.listing.options.players||!LobbyCarAllowed(catalogue,q.options,q.options.carRule==0?q.options.fixedCar:LobbyClassCars(catalogue,q.options.carClass).front()))error(7);
                        else{e.listing.options=q.options;for(auto& m:e.listing.members)if(m.occupied&&!LobbyCarAllowed(catalogue,q.options,m.car)){m.car=q.options.carRule==0?q.options.fixedCar:LobbyClassCars(catalogue,q.options.carClass).front();m.paint=0;}ResetLobbyReady(e.listing);}
                    }
                    if(reply.kind!=LobbyKind::Error){reply.kind=LobbyKind::Waiting;reply.rooms.push_back(describe(e,client.slot));}
                }
            }else if(q.kind==LobbyKind::Leave){auto found=find(q.roomId);if(found!=rooms.end()&&client.room==q.roomId&&client.slot<6){auto& e=**found;if(e.room)e.room->Kick(e.raceSlots[client.slot]);e.listing.members[client.slot]={};e.peers[client.slot]=0;ElectLobbyHost(e.listing);if(!count(e))found->reset();}client.room=0;client.slot=255;
            }else for(auto& e:rooms)if(e){auto r=describe(*e);if(r.connected)reply.rooms.push_back(r);}
            directory.Send(event.connection,Channel::Control,EncodeLobbyPacket(reply));directory.Flush();
        }
        for(auto it=clients.begin();it!=clients.end();)if(now-it->second.seen>std::chrono::minutes(10)){auto peer=it->first;++it;directory.Disconnect(peer);removeClient(peer);}else ++it;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }return 0;
}catch(const std::exception& e){std::fprintf(stderr,"directory: %s\n",e.what());return 1;}}
