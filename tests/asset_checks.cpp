#include "gt2formats/car_model.h"
#include "gt2export/car_mesh.h"
#include "gt2view/scenery_visibility.h"
#include "gt2view/course_texture_seams.h"
#include "gt2formats/psx_vram.h"
#include "gt2view/scenery_replacements.h"
#include "gt2vfs/gtfs.h"
#include "game/audio/music_player.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {
size_t checks = 0;
void Require(bool ok, const std::string& message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
void Fixtures() {
    {
        gt2::Track t; t.chunks.resize(1); t.uvTable.resize(1);
        auto& uv=t.uvTable[0].nearSet;
        uv.u={0,31,31,0}; uv.v={0,0,31,31}; uv.tpage=256;
        auto& shape=t.chunks[0].road;
        shape.vertices={{0,0,0},{64,0,0},{64,64,0},{0,64,0},{0,-64,0},{64,-64,0}};
        gt2::TrackPolygon a; a.primCode=0x2c; a.vertex={0,1,2,3}; shape.polygons.push_back(a);
        std::vector<uint16_t> words(1024*512,0x1234); words[5]=0; words[1024+5]=0;
        Require(gt2view::CourseTextureSeams(t,words)[0][0].v==uv.v,"free texture silhouettes remain transparent");
        gt2::TrackPolygon b; b.primCode=0x28; b.vertex={1,0,4,5}; shape.polygons.push_back(b);
        const auto fixed=gt2view::CourseTextureSeams(t,words);
        Require(fixed[0][0].v==std::array<uint8_t,4>{2,2,31,31},"shared solid surface trims two-row transparent fringe");
        Require(fixed[0][0].u==uv.u,"seam correction preserves the other texture axis");
        words[10*1024+10]=0;
        Require(gt2view::CourseTextureSeams(t,words)[0][0].v==uv.v,"interior transparency prevents seam cropping");
        words[10*1024+10]=0x1234; shape.polygons[0].primCode|=2;
        Require(gt2view::CourseTextureSeams(t,words)[0][0].v==uv.v,"translucent materials are never cropped");
    }

    gt2::CarModel car;
    car.lods.resize(1);
    car.lods[0].scale = 17;
    car.shadow.scale = 16;
    car.shadow.vertices = {{{-4096,-8192}}, {{4096,-8192}}, {{0,8192}}};
    gt2::CarShadowPolygon p;
    p.vertex = {0,1,2,0}; p.fullyShaded = true;
    car.shadow.polygons.push_back(p);
    auto mesh = gt2::BuildCarShadowMesh(car, 0.04f);
    Require(mesh.size() == 3 && mesh[0].pos[0] == -1 && mesh[0].pos[2] == -2 &&
        mesh[1].pos[0] == 1 && mesh[2].pos[2] == 2, "shadow uses its own scale, not the body scale");
    car.lods[0].scale = 18;
    Require(gt2::BuildCarShadowMesh(car, 0.04f)[0].pos[0] == -1, "body LOD changes cannot resize shadow");
    car.shadow.scale = 17;
    Require(gt2::BuildCarShadowMesh(car, 0.04f)[0].pos[0] == -2, "shadow exponent is applied");
    // 30-degree bank; a shadow stored 0.4 m below the body must land 4 cm
    // above the ground, independent of the suspension's body-space height.
    float ground[16] = {0.8660254f,0.5f,0,0, -0.5f,0.8660254f,0,0, 0,0,1,0, 10,20,30,1};
    gt2::GroundShadowMatrix(ground,-0.4f);
    Require(std::abs((ground[13]-0.4f*ground[5])- (20+0.04f*0.8660254f))<1e-5f &&
        std::abs((ground[12]-0.4f*ground[4])-9.98f)<1e-5f, "shadow follows banked ground instead of body height");
    gt2::Track track;
    track.sceneryModels.resize(1);
    track.sceneryLods = {{{150u * 65536u, 0}}};
    auto& model = track.sceneryModels[0];
    model.scaleExponent = 25;
    model.polygons.resize(1);
    model.boundsMin = {-80,-10,-80}; model.boundsMax = {7456,108,2680};
    gt2::TrackSceneryInstance instance;
    instance.scale = {4096,4096,4096};
    for (bool maxDetail : {false,true}) {
        Require(gt2view::SceneryEntry(track,instance,0,UINT32_MAX,1e9,-1,maxDetail)==0,
            "full distance reveals large scenery outside original masks and LOD cutoff");
        Require(gt2view::SceneryEntry(track,instance,0,UINT32_MAX,10000,250,maxDetail)==0,
            "extended radius includes large scenery outside original masks");
    }
    Require(gt2view::SceneryEntry(track,instance,0,0,0,0,false)==-1,
        "original distance retains authored masks");
    model.boundsMax = {80,108,80};
    Require(gt2view::SceneryEntry(track,instance,0,UINT32_MAX,1e9,-1,false)==0,
        "full distance reveals small scenery outside original masks");
    // Full-course detail must respect the authored empty near representation.
    track.sceneryModels.resize(2);
    track.sceneryModels[0].polygons.clear();
    track.sceneryModels[1].polygons.resize(1);
    track.sceneryLods[0]={{50u*65536u,0},{300u*65536u,1}};
    Require(gt2view::SceneryEntry(track,instance,0,UINT32_MAX,1e9,-1,false)==0,
        "full detailed course never resurrects a distant-only copy past an empty near LOD");
    Require(gt2view::SceneryEntry(track,instance,0,UINT32_MAX,100,500,false)==1,
        "partial course retains its distant fallback representation");
    Require(gt2view::SceneryEntry(track,instance,1,0,0,0,false)==0,
        "original drawing still respects the empty near LOD");

    gt2::Track surface;
    surface.chunks.resize(1);
    surface.chunks[0].road.vertices={{0,0,0},{640,0,0},{0,640,0}};
    gt2::TrackPolygon face;face.primCode=0x20;face.vertex={0,1,2,0};
    surface.chunks[0].road.polygons.push_back(face);
    gt2::TrackSceneryModel patch;
    patch.scaleExponent=22;
    patch.vertices={{64,0,-64},{320,0,-64},{64,0,-320}};
    gt2::TrackSceneryPolygon coarse;coarse.primCode=0x20;coarse.vertex={0,1,2,0};coarse.sortFarthest=true;
    patch.polygons.push_back(coarse);
    gt2::TrackSceneryInstance placement;placement.scale={4096,4096,4096};
    const gt2view::SceneryDetailIndex detail(surface);
    auto match=detail.Match(placement,patch);
    Require(match[0]==std::vector<uint16_t>{0},"coarse corners inside a subdivided surface identify its detailed replacement");
    Require(gt2view::DetailedReplacementVisible(match[0],{true}),"detailed surface replaces duplicate coarse surface");
    Require(!gt2view::DetailedReplacementVisible(match[0],{false}),"missing detailed surface keeps coarse fallback");
    placement.position[1]=65536;
    Require(detail.Match(placement,patch)[0].empty(),"independent geometry above the road is retained");
    placement.position[1]=1024;
    Require(!detail.Match(placement,patch)[0].empty(),"format quantisation does not break surface matching");

    gt2::Track rounded;
    rounded.chunks.resize(1);
    rounded.chunks[0].road.vertices={{0,0,0},{640,0,0},{640,640,0},{0,640,0},
                                   {0,0,320},{640,0,320},{640,640,320},{0,640,320}};
    rounded.sceneryModels.resize(2);
    auto& copy=rounded.sceneryModels[1];copy.scaleExponent=22;
    for(const auto& v:rounded.chunks[0].road.vertices) copy.vertices.push_back({v.x,v.z,int16_t(-v.y)});
    for(const std::array<uint16_t,4> corners : {std::array<uint16_t,4>{0,1,2,3},{4,5,6,7},{0,4,5,1},{3,2,6,7}}) {
        gt2::TrackPolygon detailed;detailed.primCode=0x28;detailed.vertex=corners;
        rounded.chunks[0].road.polygons.push_back(detailed);
        gt2::TrackSceneryPolygon reduced;reduced.primCode=0x28;reduced.vertex=corners;reduced.sortFarthest=true;
        copy.polygons.push_back(reduced);
    }
    rounded.sceneryLods={{{80u*65536u,0},{500u*65536u,1}}};
    placement.position={12288,33792,22528};
    const gt2view::SceneryDetailIndex roundedIndex(rounded);
    std::vector<std::array<float,3>> joined;
    const auto registered=roundedIndex.Match(placement,copy,&joined);
    for(const auto& polygon:registered) Require(polygon==std::vector<uint16_t>{0},"rounded distant origin matches the same detailed structure");
    const auto joinedMatrix=gt2::SceneryInstanceMatrix(placement,copy);
    for(size_t vi=0;vi<joined.size();++vi) {
        const auto expected=gt2::TrackVertexToWorld(rounded.chunks[0],rounded.chunks[0].road.vertices[vi]);
        for(size_t axis=0;axis<3;++axis) {
            const float actual=joinedMatrix[axis]*joined[vi][0]+joinedMatrix[4+axis]*joined[vi][1]+joinedMatrix[8+axis]*joined[vi][2]+joinedMatrix[12+axis];
            Require(std::abs(actual-expected[axis])<1e-5f,"surviving coarse neighbours share exact detailed boundary positions");
        }
    }
    rounded.sceneryLods[0].erase(rounded.sceneryLods[0].begin());
    for(const auto& polygon:roundedIndex.Match(placement,copy))
        Require(polygon.empty(),"independent near-visible structure is not registered away");

    gt2::Track visibility;visibility.chunks.resize(3);
    visibility.chunks[1].centre[0]=501*65536;visibility.chunks[2].centre[0]=100*65536;
    const std::vector<uint16_t> retailEntries={0xc000,0xc001};
    auto extended=retailEntries;
    gt2view::ExtendTrackEntries(extended,visibility,{0,0,0},500);
    Require(extended==std::vector<uint16_t>({0,0xc001,2}),"radius promotes existing glow-only entries and adds missing geometry");
    extended=retailEntries;gt2view::ExtendTrackEntries(extended,visibility,{0,0,0},0);
    Require(extended==retailEntries,"original range preserves original entry flags");
    gt2view::ExtendTrackEntries(extended,visibility,{0,0,0},-1);
    Require(extended==std::vector<uint16_t>({0,1,2}),"entire course promotes all geometry without duplicates");

}

// Read the independent shadow header by walking the packed body sections, then
// compare every emitted corner to its raw coordinate and the shadow exponent.
void CheckCar(const std::vector<uint8_t>& bytes, const std::string& path, size_t& affected) {
    auto u16 = [&](size_t p) { return uint16_t(bytes.at(p) | unsigned(bytes.at(p+1)) << 8); };
    auto u32 = [&](size_t p) { return uint32_t(u16(p)) | uint32_t(u16(p+2)) << 16; };
    const auto car = gt2::ParseCarModel(bytes);
    if (car.shadow.polygons.empty()) return;
    size_t header = 0x884;
    for (uint32_t lod = 0; lod < u32(0x868); ++lod) {
        size_t length = 0x50;
        const size_t stride[] = {8,4,16,16,0,0,28,28,0,0};
        for (size_t i=0; i<10; ++i) length += u16(header+i*2) * stride[i];
        header += length;
    }
    const int exponent = int16_t(u16(header+24));
    Require(car.shadow.scale == exponent, path + ": shadow header scale");
    auto mesh = gt2::BuildCarShadowMesh(car, gt2::CarShadowHeight(car));
    size_t corner = 0;
    const size_t order[] = {0,1,2,0,2,3};
    for (const auto& polygon : car.shadow.polygons) {
        for (size_t k=0; k<(polygon.quad ? 6u : 3u); ++k) {
            const size_t offset = header+28+size_t(polygon.vertex[order[k]])*4;
            const float x = float(int16_t(u16(offset))) * float(std::pow(2.0, exponent-16)) / 4096;
            const float z = float(int16_t(u16(offset+2))) * float(std::pow(2.0, exponent-16)) / 4096;
            const auto& v = mesh.at(corner++);
            Require(std::abs(v.pos[0]-x)<1e-6f && std::abs(v.pos[2]-z)<1e-6f && std::isfinite(v.pos[1]), path+": emitted shadow coordinate");
        }
    }
    Require(corner == mesh.size(), path+": complete shadow topology");
    if (exponent != car.lods[0].scale) {
        ++affected;
        std::cout << "SHADOW_SCALE " << path << " body=" << car.lods[0].scale << " shadow=" << exponent
            << " old_linear_error=" << std::pow(2.0, car.lods[0].scale-exponent) << '\n';
    }
}

void Scan(const char* path) {
    gt2::DiscImage disc(path);
    std::vector<uint8_t> block(32*gt2::DiscImage::kRawSectorSize), single(gt2::DiscImage::kRawSectorSize);
    disc.ReadRawSectors(0,32,block.data());
    for(uint32_t sector=0;sector<32;++sector) {
        disc.ReadRawSector(sector,single.data());
        Require(std::equal(single.begin(),single.end(),block.begin()+sector*single.size()), "bulk disc reads preserve sector boundaries");
    }
    disc.ReadRawSectors(disc.SectorCount(),0,nullptr);
    bool rejected=false;
    try { disc.ReadRawSectors(disc.SectorCount()-1,2,block.data()); }
    catch(const std::runtime_error&) { rejected=true; }
    Require(rejected,"bulk read cannot cross image end");
    gt2::GtfsVolume vol(disc);
    size_t cars=0, affected=0, tracks=0, fullRangeInstances=0, cameraSamples=0, replacements=0;
    for (const auto& file : vol.Files()) {
        const std::string& p = file.path;
        if (p.ends_with(".cdo.gz") || p.ends_with(".cno.gz") || p.ends_with(".cdo") || p.ends_with(".cno")) {
            CheckCar(vol.Read(file), p, affected); ++cars;
        }
        if (!p.ends_with(".tro.gz") && !p.ends_with(".tro")) continue;
        const auto track = gt2::ParseTrack(vol.Read(file));
        ++tracks;
        gt2::PsxVram texture;
        const auto suffix=p.find(".tro");
        texture.LoadTimPack(vol.Read(p.substr(0,suffix)+".trp"));
        const auto seamUvs=gt2view::CourseTextureSeams(track,texture.Words());
        size_t seamCount=0;
        for(size_t ci=0;ci<track.chunks.size();++ci) for(size_t pi=0;pi<track.chunks[ci].road.polygons.size();++pi) {
            const auto& poly=track.chunks[ci].road.polygons[pi];
            if(!poly.IsTextured()) continue;
            const auto& original=track.uvTable[poly.uvIndex].nearSet;
            const auto& fixed=seamUvs[ci][pi];
            if(fixed.u==original.u && fixed.v==original.v) continue;
            ++seamCount;
            Require(!(poly.primCode&2),p+": seam cropping excludes translucent polygons");
            for(size_t k=0;k<(poly.IsQuad()?4u:3u);++k)
                Require(std::abs(int(fixed.u[k])-int(original.u[k]))<=2 && std::abs(int(fixed.v[k])-int(original.v[k]))<=2,
                    p+": seam correction is at most two source texels");
        }
        if(p=="crsobj/parma.tro.gz" || p=="crsobj/parma.tro") Require(seamCount>0,"Midfield real cliff cutout seam is detected");
        std::cout<<"SEAMS "<<p<<" joined_polygons="<<seamCount<<'\n';
        const gt2view::SceneryDetailIndex detailIndex(track);
        for(const auto& cameraChunk:track.chunks) {
            const std::array<float,3> eye={cameraChunk.centre[0]/65536.0f,cameraChunk.centre[1]/65536.0f,cameraChunk.centre[2]/65536.0f};
            auto entries=cameraChunk.renderList;
            gt2view::ExtendTrackEntries(entries,track,eye,500);
            std::vector<bool> drawn(track.chunks.size(),false);
            for(auto entry:entries) if((entry&0x3fff)<drawn.size() && (entry>>14)<=2) drawn[entry&0x3fff]=true;
            for(size_t ci=0;ci<track.chunks.size();++ci) {
                double squared=0;
                for(size_t axis=0;axis<3;++axis) {const double d=track.chunks[ci].centre[axis]/65536.0-eye[axis];squared+=d*d;}
                if(squared<=500*500) Require(drawn[ci],p+": every in-range chunk draws geometry, including former lights-only entries");
            }
        }
        size_t replacedHere=0;
        for (const auto& instance : track.sceneryInstances) {
            const auto& lods=track.sceneryLods.at(instance.lodList);
            const int highest=gt2view::HighestSceneryLod(track,lods);
            if (highest<0) continue;
            ++fullRangeInstances;
            const auto matched=detailIndex.Match(instance,track.sceneryModels[lods[highest].model]);
            std::vector<bool> allDrawn(track.chunks.size(),true),noneDrawn(track.chunks.size(),false);
            for(const auto& patch:matched) if(!patch.empty()) {
                ++replacedHere;
                Require(gt2view::DetailedReplacementVisible(patch,allDrawn),p+": full course supplies the replacement patch");
                Require(!gt2view::DetailedReplacementVisible(patch,noneDrawn),p+": coarse fallback remains without detailed geometry");
                for(auto chunk:patch) Require(chunk<track.chunks.size(),p+": replacement references a real course chunk");
            }
            for (const auto& chunk : track.chunks) {
                ++cameraSamples;
                const std::array<double,3> eye = {chunk.centre[0]/65536.0, chunk.centre[1]/65536.0, chunk.centre[2]/65536.0};
                const std::array<std::array<double,3>,3> axes = {{{1,0,0},{0,-1,0},{0,0,1}}};
                const auto distance=gt2::SceneryLodMeasure(gt2::SceneryCameraSpace(instance, eye, axes), gt2::SceneryLodK(256,instance.lodDivisor));
                Require(gt2view::SceneryEntry(track,instance,0,distance,1e12,-1,false)==0,
                    p+": full course selects authored near detail, including empty distant-copy sentinels");
            }
        }
        replacements+=replacedHere;
        std::cout << "TRACK " << p << " chunks=" << track.chunks.size() << " full_range_instances=" << track.sceneryInstances.size() << " replacements=" << replacedHere << '\n';
        if (p == "crsobj/parma.tro.gz" || p == "crsobj/parma.tro") {
            Require(track.sceneryInstances.size()>83, "Midfield regression instances exist");
            for(size_t i : {42u,70u,83u}) {
                const auto& instance=track.sceneryInstances[i];
                Require(gt2view::SceneryEntry(track,instance,0,UINT32_MAX,1e12,-1,false)==0,
                    "Midfield mountain, road and tunnel remain drawn at full range");
            }
            gt2view::SceneryDetailIndex detail(track);
            for(size_t i : {127u,128u,134u,135u}) {
                const auto& instance=track.sceneryInstances[i];
                const auto& lods=track.sceneryLods[instance.lodList];
                const int entry=gt2view::SceneryEntry(track,instance,0,UINT32_MAX,1e12,-1,false);
                Require(entry==0 && track.sceneryModels[lods[entry].model].polygons.empty(),
                    "Midfield full course excludes lifted straight and hillside fallback meshes at every distance");
                Require(!track.sceneryModels[lods.back().model].polygons.empty(),
                    "Midfield regression exercises a real distant fallback");
            }
            const auto tunnel=detail.Match(track.sceneryInstances[83],track.sceneryModels[130]);
            const auto road=detail.Match(track.sceneryInstances[70],track.sceneryModels[133]);
            const auto mountain=detail.Match(track.sceneryInstances[42],track.sceneryModels[47]);
            const auto grid=detail.Match(track.sceneryInstances[127],track.sceneryModels[124]);
            Require(!grid[11].empty(),"Midfield shifted start-straight asphalt matches detailed road");
            const auto straight=detail.Match(track.sceneryInstances[128],track.sceneryModels[125]);
            Require(!straight[21].empty(),"Midfield second shifted straight copy cannot cover cars");
            Require(!tunnel.back().empty(),"Midfield tunnel cap matches detailed tunnel boundary vertices");
            Require(std::all_of(road.begin(),road.end(),[](const auto& match){return !match.empty();}),
                "Midfield duplicate road polygons match detailed road boundary vertices");
            Require(std::all_of(mountain.begin(),mountain.end(),[](const auto& match){return match.empty();}),
                "Midfield distant mountain is independent scenery, never a road replacement");
            std::vector<bool> drawn(track.chunks.size(),true);
            Require(gt2view::DetailedReplacementVisible(tunnel.back(),drawn),"detailed tunnel replaces its coarse cap");
            drawn[tunnel.back().front()]=false;
            Require(!gt2view::DetailedReplacementVisible(tunnel.back(),drawn),"coarse polygon remains when its detailed replacement is absent");
            Require(track.sceneryModels.at(130).polygons.back().sortFarthest,
                "Midfield tunnel cap carries the original scenery depth bias");
            for(const auto& polygon : track.sceneryModels.at(133).polygons)
                Require(polygon.sortFarthest,"Midfield distant road carries the original scenery depth bias");
        }
    }
    Require(cars>0 && tracks>0, "disc scan must visit cars and tracks");
    std::cout << "CORPUS cars=" << cars << " scale_mismatches=" << affected << " tracks=" << tracks
        << " full_range_instances=" << fullRangeInstances << " replacement_polygons=" << replacements << " camera_samples=" << cameraSamples << '\n';
}
void Music(const char* discPath, const char* outDir) {
    gt2::DiscImage disc(discPath);
    const auto exe=gt2::LoadExeImage(disc);
    std::filesystem::create_directories(outDir);
    for (int id=0; id<21; ++id) {
        gt2::audio::MusicPlayer player;
        player.Open(discPath,exe); player.SetVolume(240); player.Play(id,false);
        std::vector<float> mix(44100*15*2,0);
        for (size_t at=0; at<mix.size()/2; at+=441) player.MixStream(mix.data()+at*2,441);
        double squared=0, peak=0;
        std::vector<int16_t> pcm(mix.size());
        for(size_t i=0;i<mix.size();++i) {
            Require(std::isfinite(mix[i]),"music output finite");
            squared+=double(mix[i])*mix[i]; peak=std::max(peak,double(std::abs(mix[i])));
            pcm[i]=int16_t(std::clamp(mix[i]*32767,-32768.0f,32767.0f));
        }
        std::ofstream file(std::filesystem::path(outDir)/(std::to_string(id)+".wav"),std::ios::binary);
        auto word=[&](uint32_t n,int bytes){ for(int b=0;b<bytes;++b) file.put(char(n>>(b*8))); };
        file.write("RIFF",4); word(36+uint32_t(pcm.size()*2),4); file.write("WAVEfmt ",8);
        word(16,4);word(1,2);word(2,2);word(44100,4);word(176400,4);word(4,2);word(16,2);
        file.write("data",4);word(uint32_t(pcm.size()*2),4);file.write(reinterpret_cast<const char*>(pcm.data()),pcm.size()*2);
        std::cout<<"MUSIC "<<exe.fileName<<" track="<<id<<" rms="<<std::sqrt(squared/mix.size())<<" peak="<<peak<<'\n';
    }
}
}
int main(int argc, char** argv) {
    try {
        Fixtures();
        if (argc==4 && std::string(argv[1])=="--music") Music(argv[2],argv[3]);
        else for (int i=1; i<argc; ++i) Scan(argv[i]);
        std::cout << "PASS " << checks << " checks\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n'; return 1;
    }
}

