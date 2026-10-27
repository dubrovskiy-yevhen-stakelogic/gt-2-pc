// Native showroom geometry from owned US 1.01 CORE and SpecDB inputs.
// Uses the same garage-car builder, EE arithmetic and geometry getters as the
// validated showroom export; it does not execute the game's instructions.
#include "adhoc/showroom_pose.h"
#include "game/menu/garage_car.h"
#include "game/physics/carparams_specdb.h"
#include <bit>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using Bytes=std::vector<uint8_t>;
Bytes Read(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in)throw std::runtime_error("Cannot read "+path.string());
    return {(std::istreambuf_iterator<char>(in)),{}};
}
uint32_t Word(const uint8_t* p) {uint32_t v;std::memcpy(&v,p,4);return v;}
uint16_t Half(const uint8_t* p) {uint16_t v;std::memcpy(&v,p,2);return v;}
float Float(uint32_t bits) {return std::bit_cast<float>(bits);}

Bytes LoadElf(const std::filesystem::path& path) {
    const auto input=Read(path);
    if(input.size()<52 || std::memcmp(input.data(),"\177ELF",4) || input[4]!=1 || input[5]!=1 || Half(input.data()+18)!=8)
        throw std::runtime_error("Expected decompressed little-endian MIPS ELF32 CORE image");
    const size_t phoff=Word(input.data()+28),phsize=Half(input.data()+42),count=Half(input.data()+44);
    if(phsize<32 || phoff>input.size() || count>(input.size()-phoff)/phsize)
        throw std::runtime_error("Invalid CORE ELF program headers");
    Bytes ram(32u<<20,0);size_t loaded=0;
    for(size_t i=0;i<count;++i){
        const auto* p=input.data()+phoff+i*phsize;
        if(Word(p)!=1)continue;
        const size_t offset=Word(p+4),address=Word(p+8),files=Word(p+16),memory=Word(p+20);
        if(files>memory || offset>input.size() || files>input.size()-offset || address>ram.size() || memory>ram.size()-address)
            throw std::runtime_error("CORE segment outside file or EE RAM");
        std::memcpy(ram.data()+address,input.data()+offset,files);++loaded;
    }
    if(!loaded)throw std::runtime_error("CORE has no loadable segments");
    return ram;
}

// 00366100, the body-condition static initializer called before showroom setup.
// Kept here to avoid linking the unrelated race-start orchestration.
void InitializeSteerTable(Bytes& ram) {
    using namespace gt4::ee;
    constexpr size_t s=0x620af0,d=0x620b00;
    auto f=[](uint32_t v){return FpuCvtSW(v);};
    auto mul=[](uint32_t a,uint32_t b){return FpuMul(a,b,nullptr);};
    auto div=[](uint32_t a,uint32_t b){return FpuDiv(a,b,nullptr);};
    auto put=[&](size_t o,uint32_t v){std::memcpy(ram.data()+d+o,&v,4);};
    auto curve=[&](size_t o,uint32_t x,uint32_t y){put(o,2);put(o+4,uint32_t(d)+x);put(o+8,uint32_t(d)+y);};
    constexpr uint32_t one=0x3f800000,hundred=0x42c80000,degree=0x3c8efa34;
    put(4,mul(mul(f(ram[s+1]),0x3c23d70a),degree));
    put(8,mul(f(ram[s+2]),hundred));put(12,f(ram[s+3]));put(28,0);
    put(20,mul(f(uint32_t(int32_t(int16_t(Half(ram.data()+s+4))))),0x447a0000));
    put(32,f(2));put(24,mul(f(uint32_t(int32_t(int16_t(Half(ram.data()+s+6))))),0x447a0000));
    curve(36,20,28);put(48,0);put(52,f(6));put(16,div(one,f(2)));
    put(56,div(f(ram[s+8]),hundred));put(60,div(f(ram[s+9]),hundred));curve(64,48,56);
    put(76,div(mul(f(ram[s+10]),degree),hundred));put(80,div(mul(f(ram[s+11]),degree),hundred));curve(84,48,76);
}

std::array<uint32_t,16> DonorBasis(const gt4::adhoc::ShowroomGeometry& geometry,size_t slot) {
    gt4::game::MotionRenderTransform transform;
    const auto body=gt4::adhoc::ShowroomBodyMatrix(geometry);
    gt4::game::Vu0MatLoad(transform.vu,transform.scratch.data(),body.data());
    gt4::game::Vu0MatRotateY(transform.vu,transform.scratch.data(),geometry.angles[slot][1]);
    gt4::game::Vu0MatRotateZ(transform.vu,transform.scratch.data(),slot%2 ? gt4::ee::FpuNeg(geometry.angles[slot][2]):geometry.angles[slot][2]);
    gt4::game::Vu0MatRotateX(transform.vu,transform.scratch.data(),geometry.angles[slot][0]);
    return transform.Current();
}

void WritePose(std::ostream& out,const std::string& label,const uint8_t* work,const uint8_t* entry) {
    const auto geometry=gt4::adhoc::ShowroomGeometryFromWork(work,entry);
    out<<"{\"label\":"<<std::quoted(label)<<",\"body\":["<<Float(geometry.body[0])<<','<<Float(geometry.body[1])
       <<"],\"style\":"<<int(entry[0])<<",\"bodyMatrix\":[";
    const auto body=gt4::adhoc::ShowroomBodyMatrix(geometry);
    for(size_t i=0;i<16;++i){if(i)out<<',';out<<Float(body[i]);}
    out<<"],\"sourceWheel\":[";
    for(size_t i=0;i<4;++i){if(i)out<<',';out<<geometry.sourceWheel[i];}
    out<<"],\"axles\":[";
    for(size_t a=0;a<2;++a){
        if(a)out<<',';
        out<<"{\"radius\":"<<Float(Word(work+0x8c4+4*a))<<",\"rideHeight\":"<<Float(Word(work+0x640+4*a))
           <<",\"work914\":"<<Float(Word(work+0x914+0x58*a))<<",\"work918\":"<<Float(Word(work+0x918+0x58*a))
           <<",\"raw914\":"<<Word(work+0x914+0x58*a)<<",\"raw918\":"<<Word(work+0x918+0x58*a)<<'}';
    }
    out<<"],\"wheels\":[";
    for(size_t w=0;w<4;++w){
        if(w)out<<',';
        out<<"{\"position\":["<<Float(geometry.wheels[w][0])<<','<<Float(geometry.wheels[w][1])<<','<<Float(geometry.wheels[w][2])
           <<"],\"dimensions\":["<<Float(geometry.dimensions[w][0])<<','<<Float(geometry.dimensions[w][1])<<','<<Float(geometry.dimensions[w][2])
           <<"],\"angles\":["<<Float(geometry.angles[w][0])<<','<<Float(geometry.angles[w][1])<<','<<Float(geometry.angles[w][2])
           <<"],\"yBits\":"<<geometry.wheels[w][1]<<",\"donorBasis\":[";
        const auto basis=DonorBasis(geometry,w);
        for(size_t i=0;i<9;++i){if(i)out<<',';out<<Float(basis[4*(i/3)+i%3]);}
        out<<"]}";
    }
    out<<"]}";
}
}

int main(int argc,char** argv) {try{
    if(argc<5)throw std::runtime_error("Usage: gt4showroomexport CORE.elf SPECDB_DIRECTORY OUTPUT.json LABEL|@LABELS.txt [...]");
    auto ram=LoadElf(argv[1]);InitializeSteerTable(ram);
    gt4::game::carparams::SpecDbSource db;std::string error;
    if(!db.Load(argv[2],"american",&error))throw std::runtime_error(error);
    std::map<uint32_t,std::string> labels;
    if(!gt4::specdb::LoadLabels(Read(std::filesystem::path(argv[2])/"GENERIC_CAR.idi"),&labels,&error))throw std::runtime_error(error);
    std::map<std::string,uint32_t> ids;for(const auto& [id,label]:labels)ids.emplace(label,id);
    std::vector<std::string> requested;
    for(int i=4;i<argc;++i){
        if(argv[i][0]=='@'){
            std::ifstream list(argv[i]+1);if(!list)throw std::runtime_error("Cannot read label list");
            std::string label;while(std::getline(list,label)){if(!label.empty()&&label.back()=='\r')label.pop_back();if(!label.empty())requested.push_back(label);}
        }else requested.emplace_back(argv[i]);
    }
    if(requested.empty())throw std::runtime_error("No car labels requested");
    std::ofstream out(argv[3]);out.exceptions(std::ios::badbit|std::ios::failbit);out<<std::setprecision(10)<<'[';
    constexpr uint32_t car=0x01e80000,work=0x01fa0000;
    for(size_t i=0;i<requested.size();++i){
        const auto& label=requested[i];auto found=ids.find(label);
        if(found==ids.end())throw std::runtime_error("Unknown GENERIC_CAR label: "+label);
        const gt4::game::carparams::Guest guest{ram.data()};
        gt4::game::menu::GarageCarConstruct(guest,car);
        if(!gt4::game::menu::GarageCarFromKey(guest,car,found->second,car,db))throw std::runtime_error("Cannot build car: "+label);
        uint64_t variation;std::memcpy(&variation,ram.data()+car+0x10,8);
        if(db.Count(variation)>0)ram[car+0x18]=0;
        gt4::game::carparams::BuildFrame frame;constexpr uint32_t bsp=car-0x1b0;
        std::memcpy(frame.frame,ram.data()+bsp,sizeof frame.frame);
        std::memcpy(frame.sub,ram.data()+bsp-sizeof frame.sub,sizeof frame.sub);
        gt4::game::menu::ShowRoomCarMetric(guest,work,car,db,frame);
        const size_t entry=0x620478+size_t(ram[work+0x630])*0x54;
        if(entry+0x54>ram.size())throw std::runtime_error("Invalid wheel layout");
        if(i)out<<',';WritePose(out,label,ram.data()+work,ram.data()+entry);
    }
    out<<"]\n";std::cout<<"Exported "<<requested.size()<<" showroom poses\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
