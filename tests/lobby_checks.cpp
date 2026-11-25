#include "platform/net/lobby.h"
#include "platform/net/lobby_rules.h"
#include "platform/net/lan_room.h"
#include <cstdio>
#include <algorithm>
#include <thread>
#include <stdexcept>
using namespace gt2::net;using Clock=std::chrono::steady_clock;
void Require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void DirectoryLifecycle(const char* key) {
    auto endpoint=DefaultLobby();if(key){auto security=ReadHostKey(key).Public();security.invitation={};endpoint={"127.0.0.1",29610,security};}
    std::vector<std::unique_ptr<LobbyClient>> clients;
    auto poll=[&]{for(auto& c:clients)c->Poll();std::this_thread::sleep_for(std::chrono::milliseconds(2));};
    auto wait=[&](size_t i){const auto deadline=Clock::now()+std::chrono::seconds(30);while(clients[i]->Busy()&&Clock::now()<deadline)poll();Require(!clients[i]->Busy(),"directory request timed out");
        const auto pace=Clock::now()+std::chrono::milliseconds(160);while(Clock::now()<pace)poll();};
    for(size_t i=0;i<6;++i){clients.push_back(std::make_unique<LobbyClient>(endpoint));wait(i);Require(clients[i]->Error().empty(),"directory connection failed");}
    auto response=[&](size_t i){wait(i);if(!clients[i]->Error().empty())throw std::runtime_error(clients[i]->Error());auto offer=clients[i]->TakeCreated();Require(bool(offer),"missing room response");return *offer;};
    LobbyOptions options;options.players=6;clients[0]->Create(options,"Lifecycle regression");auto room=response(0);const auto id=room.id;
    std::array<LobbyRoom,6> views{};views[0]=room;
    auto update=[&](size_t i,bool ready){const auto& v=views[i];clients[i]->Update(id,"Driver"+std::to_string(i),v.members[v.localSlot].car,0,ready,v.revision);views[i]=response(i);return views[i];};
    auto command=[&](size_t i,LobbyKind kind,uint8_t target=255){clients[i]->Command(kind,id,views[i].revision,target);views[i]=response(i);return views[i];};
    for(size_t i=1;i<3;++i){clients[i]->Join(id);views[i]=response(i);}
    update(0,true);update(2,true);
    clients[0]->Command(LobbyKind::Start,id,views[0].revision);wait(0);Require(!clients[0]->Error().empty(),"unready participant did not block start");
    clients[2]->Command(LobbyKind::Kick,id,views[2].revision,0);wait(2);Require(!clients[2]->Error().empty(),"guest could kick host");
    clients[2]->Command(LobbyKind::Start,id,views[2].revision);wait(2);Require(!clients[2]->Error().empty(),"guest could start race");
    views[0]=command(0,LobbyKind::Kick,1);Require(!views[0].members[1].occupied,"kick failed");
    clients[1]->Update(id,"Kicked",options.fixedCar,0,true,views[1].revision);wait(1);Require(!clients[1]->Error().empty(),"kicked client retained membership");
    views[0]=command(0,LobbyKind::Start);views[2]=update(2,true);
    Require(views[0].phase==1&&views[0].raceSlot==0&&views[2].raceSlot==1&&views[2].localSlot==2,"sparse lobby slots mapped incorrectly");
    LobbyReturnState returning;returning.Begin(id);
    returning.Observe(views[0]);Require(returning.Pending(views[0]),"stale racing reply cancelled return");
    auto firstKey=views[0].security.hostPublic;
    auto raceSmoke=[&](const std::vector<size_t>& people){
        LanSetupQuery query(endpoint.address,views[people[0]].port,views[people[0]].security);
        const auto deadline=Clock::now()+std::chrono::seconds(10);while(!query.Offer()&&query.Error().empty()&&Clock::now()<deadline){query.Poll();poll();}
        Require(bool(query.Offer()),"race setup query failed");const auto offer=*query.Offer();Require(offer.setup.cars==(views[people[0]].options.bots?6:people.size())&&offer.setup.humans==people.size(),"bot population differs from settings");Require(offer.setup.botDifficulty==views[people[0]].options.botDifficulty,"bot difficulty missing from race setup");
        std::vector<std::unique_ptr<LanRoom>> racers;LanRoomConfig lastConfig;
        for(auto i:people){LanRoomConfig c;c.address=endpoint.address;c.port=views[i].port;c.cars=offer.setup.cars;c.humans=uint8_t(people.size());c.expectedSession=offer.session;c.content=offer.content;c.security=views[i].security;c.admissionSlot=views[i].raceSlot;c.admission=views[i].admission;
            lastConfig=c;racers.push_back(std::make_unique<LanRoom>(c));if(racers.size()<people.size())racers.back()->SetReady(true);}
        const auto loadingUntil=Clock::now()+std::chrono::seconds(3);
        while(Clock::now()<loadingUntil){poll();for(const auto& racer:racers){const auto status=racer->Status();Require(status.phase!=RoomPhase::Racing&&status.nextTick==0,"race advanced before the slowest scene loaded");}}
        racers.back()->SetReady(true);
        bool racing=false;while(Clock::now()<deadline){poll();racing=std::all_of(racers.begin(),racers.end(),[](const auto& c){return c->Status().phase==RoomPhase::Racing;});if(racing)break;}
        Require(racing,"authoritative race did not start");
        const auto driveUntil=Clock::now()+std::chrono::seconds(5);
        while(Clock::now()<driveUntil){poll();for(auto& racer:racers){racer->DrainFrames();Require(racer->Status().phase==RoomPhase::Racing,"race disconnected during countdown");}}
        for(const auto& racer:racers)Require(racer->Status().nextTick>=120,"race stopped during countdown");
        if(people.size()==6){
            const auto identity=racers.back()->Identity();Require(bool(identity),"missing kick-test identity");
            views[0]=command(0,LobbyKind::Kick,5);
            const auto kickedUntil=Clock::now()+std::chrono::seconds(3);
            while(racers.back()->Status().phase!=RoomPhase::Failed&&Clock::now()<kickedUntil)poll();
            Require(racers.back()->Status().phase==RoomPhase::Failed,"kicked racer still connected");
            for(size_t i=0;i+1<racers.size();++i)Require(racers[i]->Status().phase==RoomPhase::Racing,"kick stopped another racer");
            lastConfig.resume=*identity;LanRoom retry(lastConfig);
            const auto retryUntil=Clock::now()+std::chrono::seconds(3);while(retry.Status().phase!=RoomPhase::Failed&&Clock::now()<retryUntil)poll();
            Require(retry.Status().phase==RoomPhase::Failed,"kicked racer reused reconnect ticket");
            std::puts("race kick: peer removed, other racers continue, reconnect denied PASS");
        }
        std::printf("race start PASS: %zu human(s), %zu AI\n",people.size(),size_t(offer.setup.cars)-people.size());
    };
    // Losing the second human before scene readiness cancels the pending race.
    views[2]=command(2,LobbyKind::Return);
    views[0]=update(0,false);views[2]=update(2,false);
    Require(views[0].phase==0&&!views[0].members[0].ready,"pending race started with only one human");
    update(0,true);update(2,true);views[0]=command(0,LobbyKind::Start);views[2]=update(2,true);
    raceSmoke({0,2});
    views[0]=command(0,LobbyKind::Return);Require(views[0].phase==2,"first return should wait for other racers");views[2]=command(2,LobbyKind::Return);
    views[0]=update(0,false);views[2]=update(2,false);Require(views[0].id==id&&views[0].phase==0&&!views[0].members[0].ready&&!views[0].members[2].ready,"room/ready state not preserved/reset");
    returning.Observe(views[0]);Require(!returning.Pending(views[0]),"lobby acknowledgement did not finish return");
    const auto oldRevision=views[0].revision;options.track=uint8_t(std::find_if(kLobbyTracks.begin(),kLobbyTracks.end(),[](auto track){return std::string_view(track)=="roma_night";})-kLobbyTracks.begin());
    clients[0]->Command(LobbyKind::Configure,id,oldRevision,255,options);views[0]=response(0);Require(views[0].options.track==options.track&&views[0].revision!=oldRevision,"next track not applied");
    clients[2]->Update(id,"Driver2",options.fixedCar,0,true,oldRevision);views[2]=response(2);Require(!views[2].members[2].ready,"stale ready survived track change");
    for(size_t i=1;i<6;++i)if(i!=2){clients[i]->Join(id);views[i]=response(i);}
    for(size_t i=0;i<6;++i)update(i,true);
    views[0]=command(0,LobbyKind::Start);for(size_t i=1;i<6;++i)views[i]=update(i,true);
    Require(views[0].security.hostPublic!=firstKey,"new race reused old admission key");raceSmoke({0,1,2,3,4,5});
    for(size_t i=0;i<5;++i)views[i]=command(i,LobbyKind::Return);
    for(size_t i=1;i<6;++i){clients[i]->Leave(id);wait(i);Require(clients[i]->Error().empty(),"leave failed");}
    for(uint8_t bots=0;bots<2;++bots){
        views[0]=update(0,false);options.bots=bots;options.botDifficulty=2;
        clients[0]->Command(LobbyKind::Configure,id,views[0].revision,255,options);views[0]=response(0);views[0]=update(0,true);
        clients[0]->Command(LobbyKind::Start,id,views[0].revision);wait(0);
        Require(!clients[0]->Error().empty(),"server allowed solo start");
        views[0]=update(0,false);Require(views[0].phase==0,"solo start created a race worker");
    }
    clients[0]->Leave(id);wait(0);clients[0]->RequestList();wait(0);Require(std::none_of(clients[0]->Rooms().begin(),clients[0]->Rooms().end(),[&](const auto& r){return r.id==id;}),"empty room was not removed");
    std::puts("directory lifecycle checks PASS");
}
int main(int argc,char** argv){try{
    if(argc==2&&std::string(argv[1])=="--public-directory"){DirectoryLifecycle(nullptr);return 0;}
    if(argc==3&&std::string(argv[1])=="--directory"){DirectoryLifecycle(argv[2]);return 0;}
    if(argc==3&&std::string(argv[1])=="--key"){WriteHostKey(argv[2],NewRoomSecurity());return 0;}
    std::array<unsigned,kLobbyTracks.size()> seen{};
    for(const auto& family:kLobbyTrackFamilies)for(auto variant:family.variants)if(variant!=255){Require(variant<kLobbyTracks.size(),"track variant outside catalogue");++seen[variant];Require(LobbyTrackDirt(variant)==family.dirt,"wrong track surface");}
    Require(std::all_of(seen.begin(),seen.end(),[](auto n){return n==1;}),"track missing/duplicated in selector");
    LobbyRoom policy;policy.options.players=6;policy.hostSlot=0;policy.members[0]={"Host","ccrcn",0,true,true};
    Require(!LobbyCanStart(policy),"solo host started with bots");policy.options.bots=0;Require(!LobbyCanStart(policy),"solo host started without bots");policy.options.bots=1;policy.members[5]={"Guest","ccrcn",0,true,false};
    Require(!LobbyCanStart(policy),"unready guest allowed start");policy.members[5].ready=true;Require(LobbyCanStart(policy),"ready partial room blocked");
    Require(LobbyRaceSlot(policy,5)==1&&LobbyRaceSlot(policy,4)==255,"sparse slot mapping failed");
    std::array<bool,6> returned{};returned[0]=true;Require(!LobbyAllReturned(policy,returned),"room reset before last player returned");returned[5]=true;Require(LobbyAllReturned(policy,returned),"empty seats blocked return to room");
    ResetLobbyReady(policy);Require(!LobbyCanStart(policy)&&!policy.members[5].ready,"new race retained readiness");
    policy.members[0]={};ElectLobbyHost(policy);Require(policy.hostSlot==5,"host migration failed");
    auto solo=RoomRoster(6,1);Require(solo[0].player&&std::all_of(solo.begin()+1,solo.end(),[](const auto& slot){return !slot.player;}),"solo roster did not fill AI");
    LobbyPacket p;p.kind=LobbyKind::Created;p.request=7;LobbyRoom room;room.id=9;room.port=29620;room.options={2,2,4,1};room.security=NewRoomSecurity();room.security.invitation={};p.rooms.push_back(room);
    const auto bytes=EncodeLobbyPacket(p);LobbyPacket decoded;Require(DecodeLobbyPacket(bytes,decoded),"round trip failed");Require(!decoded.rooms[0].security.Hosting(),"host secret leaked");
    Require(decoded.rooms[0].options==room.options,"settings lost");
    for(uint8_t bots=0;bots<2;++bots)for(uint8_t level=0;level<3;++level){auto configured=p;configured.rooms[0].options.bots=bots;configured.rooms[0].options.botDifficulty=level;Require(DecodeLobbyPacket(EncodeLobbyPacket(configured),decoded)&&decoded.rooms[0].options==configured.rooms[0].options,"bot options round trip");}
    for(size_t n=0;n<bytes.size();++n)Require(!DecodeLobbyPacket(std::span(bytes).first(n),decoded),"truncated packet accepted");
    auto bad=bytes;bad.push_back(0);Require(!DecodeLobbyPacket(bad,decoded),"trailing byte accepted");bad=bytes;bad[192+11]=7;Require(!DecodeLobbyPacket(bad,decoded),"invalid players accepted");
    auto listing=p;listing.kind=LobbyKind::Rooms;listing.rooms[0].security.invitation.fill(1);bool leaked=false;try{EncodeLobbyPacket(listing);}catch(const std::invalid_argument&){leaked=true;}Require(leaked,"directory leaks admission capability");
    listing.rooms[0].security.invitation={};Require(DecodeLobbyPacket(EncodeLobbyPacket(listing),decoded),"safe directory rejected");
    auto security=NewRoomSecurity();security.invitation={};bool rejected=false;try{PrivateTransport privateRoom("127.0.0.1",0,1,security);}catch(const std::invalid_argument&){rejected=true;}Require(rejected,"private room accepted no invitation");

        PrivateTransport server("127.0.0.1",0,2,security,AccessPolicy::PublicDirectory);RoomInvitation endpoint{"127.0.0.1",server.BoundPort(),security.Public()};LobbyClient client(endpoint);
        auto deadline=Clock::now()+std::chrono::seconds(3);bool answered=false;
        while(client.Busy()&&Clock::now()<deadline){client.Poll();TransportEvent e;while(server.Poll(e))if(e.kind==TransportEvent::Kind::Message){LobbyPacket q;Require(DecodeLobbyPacket(e.bytes,q),"bad request");LobbyPacket reply;reply.kind=LobbyKind::Rooms;reply.request=q.request;server.Send(e.connection,Channel::Control,EncodeLobbyPacket(reply));server.Flush();answered=true;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        Require(answered&&!client.Busy()&&client.Error().empty(),"public directory handshake failed");
        // Simulate a render-thread stall longer than ENet's 10-second peer timeout.
        deadline=Clock::now()+std::chrono::seconds(12);
        while(Clock::now()<deadline){TransportEvent e;while(server.Poll(e))Require(e.kind!=TransportEvent::Kind::Disconnected,"asset loading disconnected directory transport");std::this_thread::sleep_for(std::chrono::milliseconds(2));}
        client.RequestList();answered=false;deadline=Clock::now()+std::chrono::seconds(3);
        while(client.Busy()&&Clock::now()<deadline){client.Poll();TransportEvent e;while(server.Poll(e))if(e.kind==TransportEvent::Kind::Message){LobbyPacket q;Require(DecodeLobbyPacket(e.bytes,q),"bad post-load request");LobbyPacket reply;reply.kind=LobbyKind::Rooms;reply.request=q.request;server.Send(e.connection,Channel::Control,EncodeLobbyPacket(reply));server.Flush();answered=true;}std::this_thread::sleep_for(std::chrono::milliseconds(2));}
        Require(answered&&!client.Busy()&&client.Error().empty(),"directory unavailable after asset loading");
        auto wrong=endpoint;wrong.security.hostPublic[0]^=1;LobbyClient impostor(wrong);deadline=Clock::now()+std::chrono::milliseconds(600);
        while(Clock::now()<deadline){impostor.Poll();TransportEvent e;while(server.Poll(e))Require(e.kind!=TransportEvent::Kind::Message,"wrong pinned server key admitted");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        Require(impostor.Busy()||!impostor.Error().empty(),"wrong key trusted");
    std::puts("lobby checks PASS");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"lobby check: %s\n",e.what());return 1;}}
