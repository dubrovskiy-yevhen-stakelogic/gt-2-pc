#include "platform/net/lobby.h"
#include "platform/net/lobby_endpoint.h"
#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <mutex>
#include <atomic>
#include <thread>
#include <deque>
#include <exception>
namespace gt2::net {
namespace {
constexpr size_t headerBytes=192,roomBytes=456;
bool Identifier(const std::string& s){return !s.empty()&&s.size()<16&&std::all_of(s.begin(),s.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';});}
void Put(std::vector<uint8_t>& b,size_t at,uint64_t n,size_t count){for(size_t i=0;i<count;++i)b.at(at+i)=uint8_t(n>>(8*i));}
uint64_t Get(std::span<const uint8_t> b,size_t at,size_t count){uint64_t n=0;for(size_t i=0;i<count;++i)n|=uint64_t(b[at+i])<<(8*i);return n;}
void PutText(std::vector<uint8_t>& b,size_t at,size_t count,const std::string& s){if(s.size()>=count)throw std::invalid_argument("room text too long");std::copy(s.begin(),s.end(),b.begin()+at);}
bool Text(std::span<const uint8_t> b,size_t at,size_t count,std::string& out){auto v=b.subspan(at,count);auto z=std::find(v.begin(),v.end(),0);if(z==v.end())return false;for(auto c:v.first(z-v.begin()))if(c<32||c>126)return false;if(std::any_of(z,v.end(),[](auto c){return c!=0;}))return false;out.assign(v.begin(),z);return true;}
void PutOptions(std::vector<uint8_t>& b,size_t at,const LobbyOptions& o){b[at]=o.track;b[at+1]=o.players;b[at+2]=o.laps;b[at+3]=o.manual;b[at+4]=o.carRule;b[at+5]=o.carClass;PutText(b,at+6,16,o.fixedCar);b[at+22]=o.bots;b[at+23]=o.botDifficulty;}
bool Options(std::span<const uint8_t> b,size_t at,LobbyOptions& o){o.track=b[at];o.players=b[at+1];o.laps=b[at+2];o.manual=b[at+3];o.carRule=b[at+4];o.carClass=b[at+5];if(!Text(b,at+6,16,o.fixedCar))return false;o.bots=b[at+22];o.botDifficulty=b[at+23];for(size_t i=24;i<32;++i)if(b[at+i])return false;return ValidLobbyOptions(o);}
}
bool ValidLobbyOptions(const LobbyOptions& o){return o.track<kLobbyTracks.size()&&o.players>=2&&o.players<=6&&o.laps>=1&&o.laps<=10&&o.bots<=1&&o.botDifficulty<=2&&o.manual<=1&&o.carRule<=1&&(o.carClass<=3||o.carClass==6)&&Identifier(o.fixedCar);}
std::vector<uint8_t> EncodeLobbyPacket(const LobbyPacket& p){
    if(p.rooms.size()>4)throw std::invalid_argument("too many rooms");
    std::vector<uint8_t> b(headerBytes+roomBytes*p.rooms.size());b[0]='G';b[1]='T';b[2]='2';b[3]='L';b[4]=6;b[5]=uint8_t(p.kind);b[6]=p.error;b[7]=uint8_t(p.rooms.size());Put(b,8,p.request,4);PutOptions(b,12,p.options);Put(b,44,p.roomId,8);
    PutText(b,52,32,p.name);PutText(b,84,32,p.password);PutText(b,116,16,p.car);PutText(b,132,24,p.nickname);b[156]=p.paint;b[157]=p.ready;b[158]=p.target;Put(b,159,p.revision,4);
    for(size_t i=0;i<p.rooms.size();++i){const auto& r=p.rooms[i];size_t at=headerBytes+roomBytes*i;Put(b,at,r.id,8);Put(b,at+8,r.port,2);PutOptions(b,at+10,r.options);b[at+42]=r.connected;b[at+43]=r.phase;b[at+44]=r.passwordRequired;b[at+46]=r.localSlot;b[at+45]=r.hostSlot;b[at+47]=r.raceSlot;Put(b,at+448,r.revision,4);
        std::copy(r.security.hostPublic.begin(),r.security.hostPublic.end(),b.begin()+at+48);std::copy(r.security.invitation.begin(),r.security.invitation.end(),b.begin()+at+80);PutText(b,at+112,32,r.name);std::copy(r.admission.begin(),r.admission.end(),b.begin()+at+144);
        for(size_t j=0;j<6;++j){const auto& m=r.members[j];size_t a=at+160+j*48;PutText(b,a,16,m.car);PutText(b,a+16,24,m.name);b[a+40]=m.paint;b[a+41]=m.occupied;b[a+42]=m.ready;b[a+43]=m.returned;}
    }
    LobbyPacket checked;if(!DecodeLobbyPacket(b,checked))throw std::invalid_argument("invalid lobby packet");return b;
}
bool DecodeLobbyPacket(std::span<const uint8_t> b,LobbyPacket& output){
    if(b.size()<headerBytes||b[0]!='G'||b[1]!='T'||b[2]!='2'||b[3]!='L'||b[4]!=6||b[5]<1||b[5]>14||b[7]>4||b.size()!=headerBytes+size_t(b[7])*roomBytes)return false;
    LobbyPacket p;p.kind=LobbyKind(b[5]);p.request=uint32_t(Get(b,8,4));p.error=b[6];p.roomId=Get(b,44,8);
    if(!p.request||!Options(b,12,p.options)||!Text(b,52,32,p.name)||!Text(b,84,32,p.password)||!Text(b,116,16,p.car)||!Text(b,132,24,p.nickname)||b[157]>1)return false;
    p.paint=b[156];p.ready=b[157];p.target=b[158];p.revision=uint32_t(Get(b,159,4));if((p.kind==LobbyKind::Kick)?p.target>=6:p.target!=255)return false;for(size_t i=163;i<headerBytes;++i)if(b[i])return false;
    const bool addressed=p.kind==LobbyKind::Join||p.kind==LobbyKind::Update||p.kind==LobbyKind::Leave||p.kind==LobbyKind::Start||p.kind==LobbyKind::Kick||p.kind==LobbyKind::Configure||p.kind==LobbyKind::Return;
    if(addressed!=bool(p.roomId))return false;
    if(p.kind!=LobbyKind::Create&&!p.name.empty())return false;
    if(p.kind!=LobbyKind::Create&&p.kind!=LobbyKind::Join&&!p.password.empty())return false;
    if(p.kind==LobbyKind::Update){if(!Identifier(p.car)||p.nickname.empty()||p.paint>=32)return false;}
    else if(!p.car.empty()||!p.nickname.empty()||p.paint||p.ready)return false;
    if(p.kind==LobbyKind::Error){if(!p.error||p.error>9||b[7])return false;}else if(p.error)return false;
    if((p.kind==LobbyKind::List||p.kind==LobbyKind::Create||addressed)&&b[7])return false;
    if((p.kind==LobbyKind::Created||p.kind==LobbyKind::Joined||p.kind==LobbyKind::Waiting)&&b[7]!=1)return false;
    for(size_t i=0;i<b[7];++i){const size_t at=headerBytes+roomBytes*i;LobbyRoom r;r.id=Get(b,at,8);r.port=uint16_t(Get(b,at+8,2));r.connected=b[at+42];r.phase=b[at+43];r.passwordRequired=b[at+44];r.localSlot=b[at+46];r.hostSlot=b[at+45];r.raceSlot=b[at+47];r.revision=uint32_t(Get(b,at+448,4));
        if(!r.id||!r.port||!Options(b,at+10,r.options)||r.connected>r.options.players||r.phase>2||b[at+44]>1||(r.hostSlot!=255&&r.hostSlot>=r.options.players)||(r.raceSlot!=255&&r.raceSlot>=6)||!r.revision||(r.localSlot!=255&&r.localSlot>=r.options.players))return false;
        std::copy_n(b.begin()+at+48,32,r.security.hostPublic.begin());std::copy_n(b.begin()+at+80,32,r.security.invitation.begin());std::copy_n(b.begin()+at+144,16,r.admission.begin());if(!Text(b,at+112,32,r.name))return false;
        if(r.security.hostPublic==std::array<uint8_t,32>{})return false;
        const bool privateOffer=p.kind!=LobbyKind::Rooms&&r.phase==1;
        if(privateOffer){if(r.security.invitation==std::array<uint8_t,32>{}||r.admission==ResumeToken{}||r.localSlot==255)return false;}
        else if(r.security.invitation!=std::array<uint8_t,32>{}||r.admission!=ResumeToken{})return false;
        if(p.kind==LobbyKind::Rooms&&r.localSlot!=255)return false;
        unsigned occupied=0;
        for(size_t j=0;j<6;++j){auto& m=r.members[j];size_t a=at+160+j*48;if(!Text(b,a,16,m.car)||!Text(b,a+16,24,m.name)||b[a+41]>1||b[a+42]>1||b[a+43]>1)return false;m.paint=b[a+40];m.occupied=b[a+41];m.ready=b[a+42];m.returned=b[a+43];for(size_t k=44;k<48;++k)if(b[a+k])return false;
            if(m.occupied){if(j>=r.options.players||!Identifier(m.car)||m.name.empty()||m.paint>=32)return false;++occupied;}else if(!m.car.empty()||!m.name.empty()||m.paint||m.ready||m.returned)return false;
        }
        for(size_t k=452;k<456;++k)if(b[at+k])return false;
        if(occupied!=r.connected)return false;
        if(r.hostSlot!=255&&!r.members[r.hostSlot].occupied)return false;
        if(privateOffer&&(r.raceSlot>=r.options.players||!r.members[r.localSlot].occupied))return false;
        if(!privateOffer&&r.raceSlot!=255)return false;
        if(std::any_of(p.rooms.begin(),p.rooms.end(),[&](const auto& x){return x.id==r.id;}))return false;p.rooms.push_back(r);
    }output=std::move(p);return true;
}
RoomInvitation DefaultLobby(){RoomInvitation e;e.address="62.238.36.19";e.port=29610;e.security.hostPublic=kLobbyPublic;
    if(const auto* address=std::getenv("GT2_LOBBY_ADDRESS"))e.address=address;return e;}
struct LobbyClient::Impl {
    using Clock=std::chrono::steady_clock;
    RoomInvitation endpoint;PrivateTransport transport;ConnectionId peer;bool connected=false,sent=false;uint32_t next=0;
    std::optional<LobbyPacket> pending;Clock::time_point started=Clock::now();std::vector<LobbyRoom> rooms;std::optional<LobbyRoom> created;std::string error;
    std::mutex transportMutex;
    std::deque<TransportEvent> incoming;
    std::exception_ptr transportError;
    std::atomic<bool> stopping{false};
    std::thread worker;
    ~Impl(){stopping=true;if(worker.joinable())worker.join();}
    bool NextEvent(TransportEvent& event) {
        std::lock_guard lock(transportMutex);
        if(transportError)std::rethrow_exception(transportError);
        if(incoming.empty())return false;
        event=std::move(incoming.front());incoming.pop_front();return true;
    }
    void Send(const LobbyPacket& packet) {
        std::lock_guard lock(transportMutex);
        transport.Send(peer,Channel::Control,EncodeLobbyPacket(packet));transport.Flush();
    }
    explicit Impl(RoomInvitation e):endpoint(std::move(e)),transport("0.0.0.0",0,1,endpoint.security,AccessPolicy::PublicDirectory),peer(transport.Connect(endpoint.address,endpoint.port)) {
        // Asset uploads can block presentation for seconds; transport acknowledgements cannot wait.
        worker=std::thread([this]{
            while(!stopping.load()) {
                {std::lock_guard lock(transportMutex);
                    try {TransportEvent event;for(unsigned n=0;n<64&&transport.Poll(event);++n)incoming.push_back(std::move(event));}
                    catch(...){transportError=std::current_exception();return;}
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }
    void Request(LobbyKind kind,LobbyOptions options={}){if(pending)return;if(++next==0)++next;pending=LobbyPacket{};pending->kind=kind;pending->request=next;pending->options=options;sent=false;error.clear();started=Clock::now();}
};
LobbyClient::LobbyClient(RoomInvitation e):impl_(std::make_unique<Impl>(std::move(e))){RequestList();}
LobbyClient::~LobbyClient()=default;
void LobbyClient::RequestList(){impl_->Request(LobbyKind::List);}
void LobbyClient::Create(const LobbyOptions& o,const std::string& name,const std::string& password){if(!ValidLobbyOptions(o))throw std::invalid_argument("invalid room options");if(Busy())return;impl_->Request(LobbyKind::Create,o);impl_->pending->name=name;impl_->pending->password=password;}
void LobbyClient::Join(uint64_t id,const std::string& password){if(Busy()){if(impl_->pending->kind!=LobbyKind::List)return;impl_->pending.reset();}impl_->Request(LobbyKind::Join);impl_->pending->roomId=id;impl_->pending->password=password;}
void LobbyClient::Update(uint64_t id,const std::string& nickname,const std::string& car,uint8_t paint,bool ready,uint32_t revision){if(Busy())return;impl_->Request(LobbyKind::Update);auto& p=*impl_->pending;p.roomId=id;p.nickname=nickname;p.car=car;p.paint=paint;p.ready=ready;p.revision=revision;}
void LobbyClient::Command(LobbyKind kind,uint64_t id,uint32_t revision,uint8_t target,LobbyOptions options){
    if(Busy())return;impl_->Request(kind,options);auto& p=*impl_->pending;p.roomId=id;p.revision=revision;p.target=target;
}
void LobbyClient::Leave(uint64_t id){if(Busy())return;impl_->Request(LobbyKind::Leave);impl_->pending->roomId=id;}
void LobbyClient::Poll(){auto& q=*impl_;try{TransportEvent e;
    for(unsigned n=0;n<64&&q.NextEvent(e);++n){if(e.kind==TransportEvent::Kind::Connected)q.connected=true;
        else if(e.kind==TransportEvent::Kind::Disconnected){q.connected=false;q.pending.reset();q.error="Server disconnected. Select Refresh to reconnect.";}
        else if(e.channel==Channel::Control&&q.pending){LobbyPacket p;if(!DecodeLobbyPacket(e.bytes,p)||p.request!=q.pending->request)continue;
            if(p.kind==LobbyKind::Error){q.error=p.error==1?"All rooms are occupied. Try again later.":p.error==2?"Please wait before trying again.":p.error==5?"Incorrect room password.":p.error==6?"This room is no longer available.":p.error==7?"Choose a car allowed by this room.":p.error==8?"Only the room host can do that.":p.error==9?"Need 2 real players, all returned and ready.":"The server could not create this room.";q.pending.reset();}
            else if(p.kind==LobbyKind::Rooms&&(q.pending->kind==LobbyKind::List||q.pending->kind==LobbyKind::Leave)){q.rooms=std::move(p.rooms);q.pending.reset();}
            else if(((p.kind==LobbyKind::Created&&q.pending->kind==LobbyKind::Create)||(p.kind==LobbyKind::Joined&&q.pending->kind==LobbyKind::Join)||(p.kind==LobbyKind::Waiting&&(q.pending->kind==LobbyKind::Update||q.pending->kind==LobbyKind::Start||q.pending->kind==LobbyKind::Kick||q.pending->kind==LobbyKind::Configure||q.pending->kind==LobbyKind::Return)))){q.created=p.rooms.front();q.pending.reset();}}
    }
    if(q.pending&&q.connected&&!q.sent){q.Send(*q.pending);q.sent=true;}
    if(q.pending&&Impl::Clock::now()-q.started>std::chrono::seconds(10)){q.pending.reset();q.error="Server is unavailable. Select Refresh to retry.";}
}catch(const std::exception&){q.pending.reset();q.error="Connection failed. Select Refresh to retry.";}}
bool LobbyClient::Busy()const{return impl_->pending.has_value();}
const std::vector<LobbyRoom>& LobbyClient::Rooms()const{return impl_->rooms;}
const std::string& LobbyClient::Error()const{return impl_->error;}
std::optional<LobbyRoom> LobbyClient::TakeCreated(){auto r=impl_->created;impl_->created.reset();return r;}
const std::string& LobbyClient::Address()const{return impl_->endpoint.address;}
}
