#include "gt2view/course_wall_visibility.h"
#include "gt2vfs/gtfs.h"
#include "gt2vfs/disc_image.h"
#include "gt2formats/sha1.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
int main(int argc,char** argv){try {
    if(argc<3)throw std::runtime_error("disc output-under-work [course]");
    const auto out=std::filesystem::absolute(argv[2]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
    std::ofstream csv(out/"walls.csv");csv<<"course,sha1,chunk,polygon,conflicting_chunks\n";
    size_t courses=0,polygons=0,masked=0;std::set<std::string> hashes;
    for(const auto& file:vol.Files())if(file.path.starts_with("crsobj/") && (file.path.ends_with(".tro") || file.path.ends_with(".tro.gz"))) {
        const auto name=file.path.substr(7,file.path.find(".tro")-7);if(argc>3 && name!=argv[3])continue;
        const auto bytes=vol.Read(file);const auto hash=gt2::Sha1Hex(bytes);if(!hashes.insert(hash).second)continue;
        const auto track=gt2::ParseTrack(bytes);if(track.chunks.empty())continue;
        const gt2view::CourseWallVisibility visibility(track);
        for(size_t ci=0;ci<track.chunks.size();++ci){
            const auto& chunk=track.chunks[ci];std::vector<size_t> conflicts;
            const auto mask=visibility.Classify(track,ci,&conflicts);polygons+=mask.size();
            std::set<size_t> physical;for(const auto& cell:chunk.surfaceGrid.cells)physical.insert(cell.begin(),cell.end());
            for(size_t pi=0;pi<mask.size();++pi)if(mask[pi]){
                if(physical.contains(pi) || conflicts.empty())throw std::runtime_error("invalid wall visibility classification "+name);
                ++masked;csv<<name<<','<<hash<<','<<ci<<','<<pi<<',';
                for(auto other:conflicts){
                    const auto& list=track.chunks.at(other).renderList;
                    if(other==ci || list.empty() || std::any_of(list.begin(),list.end(),[&](auto e){return (e&0x3fff)==ci && (e>>14)<=2;}))
                        throw std::runtime_error("authored visible geometry was masked "+name);
                    csv<<other<<';';
                }csv<<'\n';
            }
        }
        csv.flush();++courses;std::cout<<name<<" checked\n"<<std::flush;
    }
    if(!courses)throw std::runtime_error("no courses checked");
    std::ofstream(out/"gate.json")<<"{\"courses\":"<<courses<<",\"polygons\":"<<polygons<<",\"masked\":"<<masked<<",\"failures\":0}\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
