#pragma once
#include "gt2formats/model_set.h"
#include "gt2view/cockpit_fit.h"
#include "gt2view/scene_types.h"
#include <set>

namespace gt2view {
struct AuthoredControl {
    std::vector<std::string> nodes;
    std::array<float,3> center{},axis{0,0,1};
    float maximum=1, sweep=0;
};
struct AuthoredCar {
    gt2::GltfMesh mesh;
    CockpitFit fit;
    std::array<AuthoredControl,3> controls; // steering, speedometer, tachometer
    std::vector<int> primitiveControls; // -1 = fixed geometry
    std::map<std::string,std::array<float,4>> mirrors; // reviewed glass nodes and rear-target crops
};
inline std::filesystem::path AuthoredCarManifest(const std::string& id) {
    const auto root=std::getenv("GT2_AUTHORED_CAR_SET")?
        std::filesystem::path(std::getenv("GT2_AUTHORED_CAR_SET")):gt2::gt4ModelSetRoot/"authored";
    return gt2::ModelSetAsset(root,id)/"manifest.json";
}
inline std::array<float,3> AuthoredVector(const gt2::json::Value& value) {
    if(value.Size()!=3)throw std::runtime_error("authored car: expected xyz vector");
    std::array<float,3> result{};
    for(size_t k=0;k<3;++k) {
        result[k]=float(value.At(k).AsDouble());
        if(!std::isfinite(result[k]) || std::abs(result[k])>20)throw std::runtime_error("authored car: invalid coordinate");
    }
    return result;
}
inline AuthoredCar ReadAuthoredCar(const std::filesystem::path& manifest,const std::string& id, std::unique_ptr<gt2::GltfMesh> prepared = {}) {
    const auto doc=gt2::json::ReadFile(manifest.string());
    if(doc.StringOr("format","")!="gt2-authored-car" || doc.IntOr("version",0)!=1 || doc.StringOr("car","")!=id)
        throw std::runtime_error("authored car: incompatible manifest");
    AuthoredCar car;
    car.mesh=prepared?std::move(*prepared):gt2::ReadGltf(gt2::ModelSetAsset(manifest.parent_path(),doc.Require("mesh").AsString()).string());
    if(car.mesh.primitives.empty() || !car.mesh.warnings.empty())throw std::runtime_error("authored car: empty mesh or import warnings");
    car.fit.valid=true;car.fit.nativeInterior=true;car.fit.frozenInterior=true;car.fit.authoredUpperShell=true;
    car.fit.eye=AuthoredVector(doc.Require("eye"));car.fit.mirror=AuthoredVector(doc.Require("mirror"));
    car.fit.floorY=car.mesh.boundsMin[1];car.fit.roofY=car.mesh.boundsMax[1];
    car.fit.frontZ=car.mesh.boundsMin[2];car.fit.rearZ=car.mesh.boundsMax[2];
    car.fit.halfWidth=std::max(std::abs(car.mesh.boundsMin[0]),std::abs(car.mesh.boundsMax[0]));
    if(car.fit.rearZ-car.fit.frontZ<1 || car.fit.rearZ-car.fit.frontZ>12)throw std::runtime_error("authored car: invalid dimensions");
    for(size_t k=0;k<3;++k)if(car.fit.eye[k]<car.mesh.boundsMin[k] || car.fit.eye[k]>car.mesh.boundsMax[k])
        throw std::runtime_error("authored car: eye outside body");
    const auto& records=doc.Require("controls");
    // Some authored donors have fixed instrument faces. Do not force fake
    // animated needles onto them; steering-only manifests retain those meshes.
    if(records.Size()!=1 && records.Size()!=3)
        throw std::runtime_error("authored car: steering or steering and two instrument controls required");
    std::map<std::string,int> roles;
    for(size_t i=0;i<records.Size();++i) {
        auto& c=car.controls[i];const auto& row=records.At(i);
        c.center=AuthoredVector(row.Require("center"));c.axis=AuthoredVector(row.Require("axis"));
        float length=0;for(float x:c.axis)length+=x*x;
        if(std::abs(length-1)>1e-4f)throw std::runtime_error("authored car: control axis must be unit length");
        if(i) {
            c.maximum=float(row.Require("maximum").AsDouble());c.sweep=float(row.Require("sweepRadians").AsDouble());
            if(!std::isfinite(c.maximum) || c.maximum<=0 || !std::isfinite(c.sweep) || std::abs(c.sweep)>7)
                throw std::runtime_error("authored car: invalid instrument range");
        }
        const auto& nodes=row.Require("nodes");if(!nodes.Size())throw std::runtime_error("authored car: empty control");
        for(size_t n=0;n<nodes.Size();++n) {
            auto name=nodes.At(n).AsString();
            if(name.empty() || !roles.emplace(name,int(i)).second)throw std::runtime_error("authored car: repeated control node");
            c.nodes.push_back(name);
        }
    }
    std::set<std::string> found;
    if(const auto* mirrors=doc.Get("mirrors"))for(size_t i=0;i<mirrors->Size();++i){
        const auto& row=mirrors->At(i);const auto& uv=row.Require("uvRect");
        if(uv.Size()!=4)throw std::runtime_error("authored mirror: expected UV rectangle");
        std::array<float,4> rect{};for(size_t k=0;k<4;++k){rect[k]=float(uv.At(k).AsDouble());if(!std::isfinite(rect[k]) || rect[k]<0 || rect[k]>1)throw std::runtime_error("authored mirror: invalid UV crop");}
        if(rect[0]>=rect[2] || rect[1]>=rect[3] || !car.mirrors.emplace(row.Require("node").AsString(),rect).second)throw std::runtime_error("authored mirror: duplicate or empty surface");
    }
    std::set<std::string> mirrorNodes;
    for(const auto& p:car.mesh.primitives) {
        if(car.mirrors.contains(p.nodeName))mirrorNodes.insert(p.nodeName);
        const auto role=roles.find(p.nodeName);car.primitiveControls.push_back(role==roles.end()?-1:role->second);
        if(role!=roles.end())found.insert(p.nodeName);
        for(const auto& v:p.vertices)for(float x:v.position)if(!std::isfinite(x))throw std::runtime_error("authored car: non-finite vertex");
    }
    if(found.size()!=roles.size())throw std::runtime_error("authored car: control node missing from glTF");
    if(mirrorNodes.size()!=car.mirrors.size())throw std::runtime_error("authored mirror: glass node missing from glTF");
    for(size_t k=0;k<3;++k)car.fit.nativeWheel[k]=car.controls[0].center[k];
    car.fit.nativeWheel[3]=float(doc.Require("wheelRadius").AsDouble());
    if(!std::isfinite(car.fit.nativeWheel[3]) || car.fit.nativeWheel[3]<.1f || car.fit.nativeWheel[3]>.4f)
        throw std::runtime_error("authored car: invalid steering radius");
    // The renderer stores alpha in texture texels, not in SceneVertex colour.
    // Preserve scalar glTF transparency explicitly, including untextured glass.
    for(auto& m:car.mesh.materials)if(m.alphaBlend && m.baseColorFactor[3]<1) {
        gt2::PngImage image;
        if(m.baseColorImage>=0)image=car.mesh.images.at(size_t(m.baseColorImage));
        else {image.width=image.height=1;image.rgba={255,255,255,255};}
        for(size_t p=3;p<image.rgba.size();p+=4)image.rgba[p]=uint8_t(std::lround(image.rgba[p]*std::clamp(m.baseColorFactor[3],0.f,1.f)));
        m.baseColorImage=int(car.mesh.images.size());car.mesh.images.push_back(std::move(image));
    }
    return car;
}
struct AuthoredRange { uint32_t first=0,count=0;int control=-1;bool alpha=false;uint32_t lampStates=0;int mirror=-1;uint32_t fallbackFirst=0; bool glazing=false; uint32_t exteriorFirst=0; };
struct AuthoredGeometry { std::vector<SceneVertex> vertices;std::vector<AuthoredRange> ranges;uint32_t opaqueCount=0; };
inline float AuthoredDisplayColour(float linear) {
    linear=std::clamp(linear,0.f,1.f);
    return linear<=.0031308f?12.92f*linear:1.055f*std::pow(linear,1.f/2.4f)-.055f;
}
inline AuthoredGeometry BuildAuthoredGeometry(const AuthoredCar& car,const std::vector<uint32_t>& pages) {
    AuthoredGeometry result;
    std::vector<std::array<float,3>> normals;
    for(int alpha=0;alpha<2;++alpha)for(size_t pi=0;pi<car.mesh.primitives.size();++pi) {
        const auto& p=car.mesh.primitives[pi];
        const auto* m=p.material>=0?&car.mesh.materials.at(size_t(p.material)):nullptr;
        if(bool(alpha)!=(m && m->alphaBlend))continue;
        AuthoredRange range{uint32_t(result.vertices.size()),0,car.primitiveControls.at(pi),bool(alpha)};
        range.lampStates=m?m->lampStates:0;
        // Explicit restored glass materials only; never lamps, mirrors or trim.
        range.glazing=m && m->alphaBlend && p.hasNormals && m->name.starts_with("R32_glass_source_");
        if(car.mirrors.contains(p.nodeName))range.mirror=p.nodeName=="LiveMirror_center"?0:1;
        for(auto index:p.indices) {
            const auto& v=p.vertices.at(index);SceneVertex out{};
            const float light=m && m->emissive?1.f:p.hasNormals?std::clamp(.72f+.22f*v.normal[1]-.12f*v.normal[0],.45f,1.f):1.f;
            // glTF factors are linear; this legacy renderer modulates sRGB
            // bytes directly. Encode the factor before sending vertex colour.
            for(size_t k=0;k<3;++k) {out.pos[k]=v.position[k];out.color[k]=AuthoredDisplayColour(v.color[k]*(m?m->baseColorFactor[k]:1.f)*light);}
            if(m && m->baseColorImage>=0) {
                const auto image=size_t(m->baseColorImage);const auto& im=car.mesh.images.at(image);
                out.texel[0]=v.uv[0]*im.width;out.texel[1]=v.uv[1]*im.height;out.page=pages.at(image);out.clut=uint32_t(im.width)|uint32_t(im.height)<<16;
                out.flags=kTextured|kExternalTexture;
                if(m->explicitOpaque)out.flags|=kModelOpaque;
                if(m->clampU)out.flags|=kModelClampU;if(m->clampV)out.flags|=kModelClampV;
            }
            if(m && !m->doubleSided)out.flags|=kCullBack;
            if(alpha)out.flags|=kModelBlend;
            if(const auto mirror=car.mirrors.find(p.nodeName);mirror!=car.mirrors.end()) {
                const auto& rect=mirror->second;out.flags=kMirrorTexture;
                out.texel[0]=rect[0]+v.uv[0]*(rect[2]-rect[0]);out.texel[1]=rect[1]+v.uv[1]*(rect[3]-rect[1]);
                out.color[0]=out.color[1]=out.color[2]=1;
            }
            result.vertices.push_back(out);normals.push_back(v.normal);++range.count;
        }
        if(!alpha)result.opaqueCount+=range.count;
        // Textures/material flags are vertex attributes. Adjacent fixed parts
        // can share a draw without losing their alpha or lamp-state semantics.
        if(!result.ranges.empty() && result.ranges.back().control==range.control &&
           range.mirror<0 && result.ranges.back().mirror<0 && result.ranges.back().glazing==range.glazing && result.ranges.back().alpha==range.alpha && result.ranges.back().lampStates==range.lampStates)
            result.ranges.back().count+=range.count;
        else result.ranges.push_back(range);
    }
    // Separate attributes for the same window triangles, not a second overlay.
    // The cockpit continues to use the original transparent material unchanged.
    for(auto& range:result.ranges)if(range.glazing) {
        range.exteriorFirst=uint32_t(result.vertices.size());
        for(uint32_t i=0;i<range.count;++i) {
            auto v=result.vertices.at(range.first+i);
            std::copy_n(normals.at(range.first+i).data(),3,v.color);
            v.flags|=kModelReflection;
            result.vertices.push_back(v);
        }
    }
    // Neutral glass for exterior/AI cars and disabled mirrors. Never sample the
    // rear attachment from a car being rendered into that same attachment.
    for(auto& range:result.ranges)if(range.mirror>=0){
        range.fallbackFirst=uint32_t(result.vertices.size());
        for(uint32_t i=0;i<range.count;++i){auto v=result.vertices.at(range.first+i);v.flags=0;
            v.color[0]=.045f;v.color[1]=.065f;v.color[2]=.07f;result.vertices.push_back(v);}
    }
    return result;
}
inline std::array<float,16> AuthoredControlMatrix(const AuthoredControl& control,float angle) {
    if(!std::isfinite(angle))angle=0;
    const auto& a=control.axis;const float c=std::cos(angle),s=std::sin(angle),t=1-c;
    std::array<float,16> m{};m[15]=1;
    for(size_t col=0;col<3;++col)for(size_t row=0;row<3;++row)m[col*4+row]=t*a[col]*a[row]+(col==row?c:0);
    m[4]-=s*a[2];m[8]+=s*a[1];m[1]+=s*a[2];m[9]-=s*a[0];m[2]-=s*a[1];m[6]+=s*a[0];
    for(size_t r=0;r<3;++r){m[12+r]=control.center[r];for(size_t k=0;k<3;++k)m[12+r]-=m[k*4+r]*control.center[k];}
    return m;
}
} // namespace gt2view
