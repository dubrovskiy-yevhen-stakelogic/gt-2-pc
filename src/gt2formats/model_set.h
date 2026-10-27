#pragma once
#include "gt2formats/gltf_reader.h"
#include "gt2formats/json.h"
#include "gt2formats/sha1.h"
#include <fstream>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <map>
#include <stdexcept>

namespace gt2 {
inline std::filesystem::path gt4ModelSetRoot;
inline bool gt4ModelSetEnabled = false;
inline bool modernCarReflections = false;
inline void SetGt4ModelSetRoot(const std::string& disc, const std::filesystem::path& fallback) {
    if (const char* path = std::getenv("GT2_GT4_MODEL_SET")) { gt4ModelSetRoot = path; return; }
    auto path = std::filesystem::path(disc);
    if (!std::filesystem::is_directory(path)) path = path.parent_path();
    for (const auto& root : {path / "modelsets" / "gt4", path.parent_path() / "modelsets" / "gt4", fallback / "modelsets" / "gt4"})
        if (std::filesystem::is_regular_file(root / "manifest.json")) { gt4ModelSetRoot = root; return; }
    gt4ModelSetRoot = fallback / "modelsets" / "gt4";
}
inline bool Gt4ModelSetInstalled() { return std::filesystem::is_regular_file(gt4ModelSetRoot / "manifest.json"); }

inline std::filesystem::path ModelSetAsset(const std::filesystem::path& root, const std::string& name) {
    const auto relative = std::filesystem::path(name);
    if (name.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory() || name.find(':') != std::string::npos)
        throw std::runtime_error("model set: invalid asset path");
    for (const auto& part : relative) if (part == "..") throw std::runtime_error("model set: asset escapes pack");
    return root / relative;
}

struct ModelSetCar {
    GltfMesh exterior, cockpit, lamps, reflections, attachments;
    std::array<std::vector<std::string>,2> imageNames;
    std::vector<std::map<std::string,std::filesystem::path>> paints;
    std::vector<std::vector<std::array<float,17>>> nativePaintMaterials;
    std::vector<GltfMaterial> nativeMaterials;
    bool nativeInterior = false;
    bool openTop = false;
    float cabinHeightOffset = 0; // reviewed correction for a closed LOD's misleading roof profile
    bool nativeWheels = false;
    float nativeGround = 0;
    std::array<std::array<float,5>,4> wheelWells{};
    bool nativeWheelPose = false;
    float nativeBodyHeight = 0, nativeBodyPitchDegrees = 0;
    // +Y-up asset space. Apply the column-major rotation about nativeBodyPivot;
    // the existing nativeGround anchor supplies the static height translation.
    std::array<float,9> nativeBodyRotation{1,0,0,0,1,0,0,0,1};
    std::array<float,3> nativeBodyPivot{};
    std::array<std::array<float,3>,4> nativeWheelAngles{}; // spin, toe, camber degrees
    // Native body rotation * toe * signed camber * spin. Excludes the native
    // odd-wheel 180-degree rim flip: GT2 donor rims already face outwards.
    std::array<std::array<float,9>,4> nativeWheelBasis{};
    std::array<uint32_t,4> nativeSourceWheel{0,1,2,3};
    std::array<float,3> eye{}, mirror{};
    std::array<float,4> speedometer{}; // centre in source metres, display width
};
class ModelSet {
public:
    explicit ModelSet(std::filesystem::path root) : root_(std::move(root)) {}
    bool Has(const std::string& id) {
        ReadManifest();
        return entries_.contains(id);
    }
    // Catalogue metadata only: drawing a shop badge must not load a 3D model.
    bool HasNativeInterior(const std::string& id) {
        ReadManifest();
        const auto found=entries_.find(id);
        return found!=entries_.end() && found->second.BoolOr("nativeInterior",false);
    }
    ModelSetCar Read(const std::string& id,bool cockpit=true) {
        ReadManifest();
        const auto& entry = entries_.at(id);
        ModelSetCar car;
        const auto nativeRoot=std::getenv("GT2_GT4_NATIVE")?std::filesystem::path(std::getenv("GT2_GT4_NATIVE")):root_/"native-v16";
        const auto wheelFile=nativeRoot/"wheels.json";
        if(std::filesystem::is_regular_file(wheelFile)) {
            const auto wheels=json::ReadFile(wheelFile.string());
            const int wheelVersion=int(wheels.IntOr("version",0));
            if(wheels.StringOr("format","")!="gt4-wheel-geometry" || (wheelVersion!=1 && wheelVersion!=2))
                throw std::runtime_error("model set: unsupported wheel geometry");
            if(const auto* row=wheels.Require("cars").Get(id.c_str())) {
                car.nativeGround=float(row->Require("ground").AsDouble());
                const auto& wells=row->Require("wheels");
                if(wells.Size()!=4 || !std::isfinite(car.nativeGround))throw std::runtime_error("invalid wheel geometry");
                for(size_t w=0;w<4;++w) {
                    if(wells.At(w).Size()!=5)throw std::runtime_error("invalid wheel record");
                    for(size_t k=0;k<5;++k) {
                        const float value=float(wells.At(w).At(k).AsDouble());
                        if(!std::isfinite(value) || std::abs(value)>5.f)throw std::runtime_error("invalid wheel coordinate");
                        car.wheelWells[w][k]=value;
                    }
                    if(car.wheelWells[w][3]<.05f || car.wheelWells[w][4]<.005f)throw std::runtime_error("invalid wheel size");
                }
                if(wheelVersion==2) {
                    const auto readVector=[](const auto& value,auto& target) {
                        if(value.Size()!=target.size())throw std::runtime_error("invalid native wheel pose size");
                        for(size_t i=0;i<target.size();++i) {
                            target[i]=float(value.At(i).AsDouble());
                            if(!std::isfinite(target[i]) || std::abs(target[i])>360.f)
                                throw std::runtime_error("invalid native wheel pose value");
                        }
                    };
                    car.nativeBodyHeight=float(row->Require("bodyHeight").AsDouble());
                    car.nativeBodyPitchDegrees=float(row->Require("bodyPitchDegrees").AsDouble());
                    if(!std::isfinite(car.nativeBodyHeight) || std::abs(car.nativeBodyHeight)>5.f ||
                       !std::isfinite(car.nativeBodyPitchDegrees) || std::abs(car.nativeBodyPitchDegrees)>45.f)
                        throw std::runtime_error("invalid native body pose");
                    readVector(row->Require("bodyRotation"),car.nativeBodyRotation);
                    readVector(row->Require("bodyPivot"),car.nativeBodyPivot);
                    for(size_t c=0;c<3;++c)for(size_t d=0;d<3;++d) {
                        float dot=0;for(size_t r=0;r<3;++r)dot+=car.nativeBodyRotation[3*c+r]*car.nativeBodyRotation[3*d+r];
                        if(std::abs(dot-float(c==d))>2e-5f)throw std::runtime_error("invalid native body rotation");
                    }
                    if(std::abs(car.nativeGround+car.nativeBodyHeight-car.nativeBodyPivot[1])>1e-5f)
                        throw std::runtime_error("inconsistent native body ground");
                    const auto& angles=row->Require("wheelAngles");const auto& source=row->Require("sourceWheel");
                    const auto& basis=row->Require("wheelBasis");
                    if(angles.Size()!=4 || source.Size()!=4 || basis.Size()!=4)throw std::runtime_error("invalid native wheel pose count");
                    for(size_t w=0;w<4;++w) {
                        readVector(angles.At(w),car.nativeWheelAngles[w]);
                        readVector(basis.At(w),car.nativeWheelBasis[w]);
                        const auto index=source.At(w).AsInt();
                        if(index<0 || index>3)throw std::runtime_error("invalid native source wheel");
                        car.nativeSourceWheel[w]=uint32_t(index);
                    }
                    car.nativeWheelPose=true;
                }
                car.nativeWheels=true;
            }
        }
        bool native=false;
        if(std::filesystem::is_regular_file(nativeRoot/"manifest.json")) {
            if(!nativeRead_) {
                const auto doc=json::ReadFile((nativeRoot/"manifest.json").string());
                if(doc.StringOr("format","")!="gt4-native-overlay" || doc.IntOr("version",0)!=1)
                    throw std::runtime_error("model set: unsupported native overlay");
                for(const auto& [key,value]:doc.Require("cars").Members())nativeEntries_.emplace(key,value.AsString());
                nativeRead_=true;
            }
            if(const auto it=nativeEntries_.find(id);it!=nativeEntries_.end()) {
                native=true;const auto path=ModelSetAsset(nativeRoot,it->second);
                auto source=ReadGltf(path.string());car.nativeMaterials=source.materials;
                car.exterior=source;car.exterior.primitives.clear();car.lamps=car.exterior;car.reflections=car.exterior;car.attachments=car.exterior;
                if(cockpit)car.cockpit=car.exterior;
                for(auto& p:source.primitives) {
                    const auto& m=source.materials.at(size_t(p.material));
                    if(m.nativeGt4Type<0)throw std::runtime_error("model set: missing native material");
                    if(cockpit && !m.exteriorOnly && m.wheelAttachment<0 && !m.lampStates && !m.reflection)car.cockpit.primitives.push_back(p);
                    if(m.interiorOnly)continue;
                    if(m.wheelAttachment>=0)car.attachments.primitives.push_back(std::move(p));
                    else if(m.lampStates)car.lamps.primitives.push_back(std::move(p));
                    else if(m.reflection)car.reflections.primitives.push_back(std::move(p));
                    else car.exterior.primitives.push_back(std::move(p));
                }
                const auto mesh=json::ReadFile(path.string());
                if(const auto* images=mesh.Get("images"))for(size_t i=0;i<images->Size();++i) {
                    const auto& image=images->At(i);const auto* extras=image.Get("extras");
                    const auto name=extras?extras->StringOr("paintKey",""):image.StringOr("uri","");
                    car.imageNames[0].push_back(name);if(cockpit)car.imageNames[1].push_back(name);
                }
                const auto palette=json::ReadFile((path.parent_path()/"paints.json.gz").string());
                if(palette.StringOr("format","")!="gt4-native-paints")throw std::runtime_error("model set: invalid native palette");
                const auto& colors=palette.Require("colors");
                for(size_t i=0;i<colors.Size();++i) {
                    std::map<std::string,std::filesystem::path> images;
                    for(const auto& [name,file]:colors.At(i).Require("images").Members())images.emplace(name,ModelSetAsset(nativeRoot,file.AsString()));
                    car.paints.push_back(std::move(images));
                    const auto& values=colors.At(i).Require("materials");
                    if(values.Size()!=car.nativeMaterials.size())throw std::runtime_error("model set: native paint material count");
                    std::vector<std::array<float,17>> rows;
                    for(size_t m=0;m<values.Size();++m) {
                        if(values.At(m).Size()!=17)throw std::runtime_error("model set: native paint row");
                        std::array<float,17> row{};for(size_t k=0;k<17;++k)row[k]=float(values.At(m).At(k).AsDouble());rows.push_back(row);
                    }car.nativePaintMaterials.push_back(std::move(rows));
                }
            } else throw std::runtime_error("model set: native overlay missing car "+id);
        }
        if(!native) {
        car.exterior = ReadGltf(ModelSetAsset(root_, entry.Require("mesh").AsString()).string());
        if(cockpit)car.cockpit = ReadGltf(ModelSetAsset(root_, entry.Require("cockpitMesh").AsString()).string());
        const auto materialsRoot=std::getenv("GT2_GT4_MATERIALS")?std::filesystem::path(std::getenv("GT2_GT4_MATERIALS")):root_/"materials";
        if(std::filesystem::is_regular_file(materialsRoot/"manifest.json")) {
            if(!materialsRead_) {
                const auto doc=json::ReadFile((materialsRoot/"manifest.json").string());
                if(doc.StringOr("format","")!="gt4-material-overlay" || doc.IntOr("version",0)!=1)
                    throw std::runtime_error("model set: unsupported material overlay");
                for(const auto& [key,value]:doc.Require("cars").Members())materialEntries_.emplace(key,value.AsString());
                materialsRead_=true;
            }
            if(const auto it=materialEntries_.find(id);it!=materialEntries_.end()) {
                const auto doc=json::ReadFile(ModelSetAsset(materialsRoot,it->second).string());
                if(doc.StringOr("format","")!="gt4-materials" || doc.IntOr("version",0)!=1)
                    throw std::runtime_error("model set: unsupported materials");
                for(size_t i=0;i<(cockpit?2u:1u);++i) {
                    const char* name=i?"cockpitMesh":"mesh";const auto& overlay=doc.Require("meshes").Require(name);
                    const auto meshPath=ModelSetAsset(root_,entry.Require(name).AsString());
                    std::ifstream file(meshPath,std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
                    if(Sha1Hex(bytes)!=overlay.Require("sha1").AsString())throw std::runtime_error("model set: stale material overlay");
                    auto& mesh=i?car.cockpit:car.exterior;
                    for(size_t ci=0;ci<overlay.Require("primitives").Size();++ci) {
                        const auto& change=overlay.Require("primitives").At(ci);
                        const auto index=change.Require("index").AsInt();
                        if(index<0 || size_t(index)>=mesh.primitives.size())throw std::runtime_error("model set: invalid material primitive");
                        auto& p=mesh.primitives[size_t(index)];
                        if(p.material<0 || size_t(p.material)>=mesh.materials.size())throw std::runtime_error("model set: missing material");
                        auto material=mesh.materials[size_t(p.material)];material.alphaBlend=change.BoolOr("alphaBlend",false);
                        if(material.alphaBlend)material.explicitOpaque=false;
                        p.material=int(mesh.materials.size());mesh.materials.push_back(std::move(material));
                    }
                }
                if(const auto* lamp=doc.Get("lampMesh"))car.lamps=ReadGltf(ModelSetAsset(materialsRoot,lamp->AsString()).string());
                if(!car.lamps.warnings.empty())throw std::runtime_error("model set: incomplete lamp geometry");
            }
        }
        const auto paintRoot=root_/"paints";
        if (std::filesystem::is_regular_file(paintRoot/"manifest.json")) {
            if (!paintsRead_) {
                const auto doc=json::ReadFile((paintRoot/"manifest.json").string());
                if(doc.StringOr("format","")!="gt4-paint-overlay" || doc.IntOr("version",0)!=1)
                    throw std::runtime_error("model set: unsupported paint overlay");
                for(const auto& [key,value]:doc.Require("cars").Members()) paintEntries_.emplace(key,value.AsString());
                paintsRead_=true;
            }
            if (const auto it=paintEntries_.find(id);it!=paintEntries_.end()) {
                const auto doc=json::ReadFile(ModelSetAsset(paintRoot,it->second).string());
                if(doc.StringOr("format","")!="gt4-paints" || doc.IntOr("version",0)!=1)
                    throw std::runtime_error("model set: unsupported paint palette");
                const auto& colors=doc.Require("colors");
                if (!colors.Size() || colors.Size()>256) throw std::runtime_error("model set: invalid paint count");
                for(size_t i=0;i<colors.Size();++i) {
                    std::map<std::string,std::filesystem::path> images;
                    for(const auto& [name,file]:colors.At(i).Require("images").Members())
                        images.emplace(name,ModelSetAsset(paintRoot,file.AsString()));
                    car.paints.push_back(std::move(images));
                }
                for(size_t m=0;m<(cockpit?2u:1u);++m) {
                    const auto mesh=json::ReadFile(ModelSetAsset(root_,entry.Require(m?"cockpitMesh":"mesh").AsString()).string());
                    if(const auto* images=mesh.Get("images")) for(size_t i=0;i<images->Size();++i)
                        car.imageNames[m].push_back(images->At(i).StringOr("uri",""));
                }
            }
        }
        }
        car.nativeInterior = entry.BoolOr("nativeInterior", false);
        car.openTop = entry.BoolOr("openTop", car.nativeInterior);
        if(const auto* height=entry.Get("cabinHeightOffset")) {
            car.cabinHeightOffset=float(height->AsDouble());
            if(!std::isfinite(car.cabinHeightOffset) || std::abs(car.cabinHeightOffset)>1.f)
                throw std::runtime_error("model set: invalid cabin height correction");
        }
        if (car.nativeInterior) {
            const auto vector = [&](const char* key) {
                const auto& values = entry.Require(key);
                if (!values.IsArray() || values.Size()!=3) throw std::runtime_error("model set: invalid cabin position");
                std::array<float,3> result{};
                for (size_t i=0;i<3;++i) {
                    result[i]=float(values.At(i).AsDouble());
                    if (!std::isfinite(result[i]) || std::abs(result[i])>3.f) throw std::runtime_error("model set: invalid cabin coordinate");
                }
                return result;
            };
            car.eye=vector("eye"); car.mirror=vector("mirror");
            if (const auto* panel=entry.Get("speedometer")) {
                if (!panel->IsArray() || panel->Size()!=4) throw std::runtime_error("model set: invalid speedometer");
                for (size_t i=0;i<4;++i) {
                    car.speedometer[i]=float(panel->At(i).AsDouble());
                    if (!std::isfinite(car.speedometer[i]) || std::abs(car.speedometer[i])>3.f)
                        throw std::runtime_error("model set: invalid speedometer coordinate");
                }
                if (car.speedometer[3]<.04f || car.speedometer[3]>.3f) throw std::runtime_error("model set: invalid speedometer size");
            }
        }
        if (!car.exterior.TriangleCount() || (cockpit && !car.cockpit.TriangleCount())) throw std::runtime_error("model set: empty car");
        if (!car.exterior.warnings.empty() || !car.cockpit.warnings.empty())
            throw std::runtime_error("model set: incomplete mesh or texture: " +
                (!car.exterior.warnings.empty() ? car.exterior.warnings.front() : car.cockpit.warnings.front()));
        return car;
    }
private:
    void ReadManifest() {
        if (read_) return;
        read_ = true;
        if (!std::filesystem::is_regular_file(root_ / "manifest.json")) return;
        const auto doc = json::ReadFile((root_ / "manifest.json").string());
        if (doc.StringOr("format", "") != "gt2-model-set" || doc.IntOr("version", 0) != 1 || doc.StringOr("id", "") != "gt4")
            throw std::runtime_error("model set: unsupported manifest");
        if (!doc.Require("cars").IsObject()) throw std::runtime_error("model set: cars must be an object");
        std::map<std::string,json::Value> entries;
        for (const auto& [id, entry] : doc.Require("cars").Members()) {
            if (id.empty() || id.size() > 16 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos)
                throw std::runtime_error("model set: invalid GT2 id");
            entries.emplace(id, entry);
        }
        // Cabin metadata travels with the selected native geometry. An older
        // OBJ-only pack must not advertise an interior it does not contain.
        const auto nativeRoot=std::getenv("GT2_GT4_NATIVE")?std::filesystem::path(std::getenv("GT2_GT4_NATIVE")):root_/"native-v16";
        const auto cabins=nativeRoot/"interiors.json";
        if(std::filesystem::is_regular_file(cabins)) {
            const auto metadata=json::ReadFile(cabins.string());
            if(metadata.StringOr("format","")!="gt4-native-interiors" || metadata.IntOr("version",0)!=1)
                throw std::runtime_error("model set: unsupported cabin metadata");
            const auto sources=json::ReadFile((nativeRoot/"manifest.json").string());
            for(const auto& [id,cabin]:metadata.Require("cars").Members()) {
                auto it=entries.find(id);if(it==entries.end())throw std::runtime_error("model set: unknown cabin car");
                const auto mesh=cabin.Require("mesh").AsString();
                const auto* selected=sources.Require("cars").Get(id.c_str());
                if(!selected || selected->AsString()!=mesh || !std::filesystem::is_regular_file(ModelSetAsset(nativeRoot,mesh)))
                    throw std::runtime_error("model set: cabin metadata does not match native geometry");
                if(!cabin.Require("nativeInterior").IsBool() || !cabin.Require("openTop").IsBool())
                    throw std::runtime_error("model set: invalid cabin classification");
                for(const auto* field:{"nativeInterior","openTop","eye","mirror","speedometer"})
                    if(const auto* value=cabin.Get(field))it->second.Set(field,*value);
            }
        }
        entries_=std::move(entries);
    }
    std::filesystem::path root_;
    bool read_ = false;
    std::map<std::string, json::Value> entries_;
    bool materialsRead_=false;
    std::map<std::string,std::string> materialEntries_;
    bool paintsRead_=false;
    std::map<std::string,std::string> paintEntries_;
    bool nativeRead_=false;
    std::map<std::string,std::string> nativeEntries_;
};
}
