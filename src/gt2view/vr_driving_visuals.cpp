#include "gt2view/vr_driving_visuals.h"
#include "gt2view/steering_wheel.h"
#include "gt2formats/png_reader.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
// Hand morphs and basis follow MiamiVR VRHandModel.cpp and vrhands_quest.cpp.
// Mesh/texture: UltimateXR (MIT). Both licenses are in third_party/vrhands.
namespace gt2view {
namespace {
struct V {
    float x=0,y=0,z=0;
    V operator+(V b) const { return {x+b.x,y+b.y,z+b.z}; }
    V operator-(V b) const { return {x-b.x,y-b.y,z-b.z}; }
    V operator*(float a) const { return {x*a,y*a,z*a}; }
};
float Dot(V a,V b){return a.x*b.x+a.y*b.y+a.z*b.z;}
V Cross(V a,V b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
V Normal(V a){float n=std::sqrt(Dot(a,a));return n>1e-6f?a*(1/n):V{};}
V Rotate(const float q[4],V p){V a{q[0],q[1],q[2]};V t=Cross(a,p)*2;return p+t*q[3]+Cross(a,t);}
V Blend(const float poses[4][3],float grip,float trigger){
    float w[4]={(1-grip)*(1-trigger),grip*(1-trigger),(1-grip)*trigger,grip*trigger}; V p;
    for(int i=0;i<4;++i)p=p+V{poses[i][0],poses[i][1],poses[i][2]}*w[i]; return p;
}
SceneVertex Colored(V p,V color){SceneVertex v{};v.pos[0]=p.x;v.pos[1]=p.y;v.pos[2]=p.z;v.color[0]=color.x;v.color[1]=color.y;v.color[2]=color.z;return v;}
}
VrDrivingVisuals::VrDrivingVisuals(SceneRenderer& renderer):renderer_(renderer){
    static_assert(sizeof(Vertex)==104);
    for(int h=0;h<2;++h){
        auto bytes=VrHandAsset(h);uint32_t header[4]{};
        if(bytes.size()<sizeof(header))throw std::runtime_error("truncated embedded hand");
        std::memcpy(header,bytes.data(),sizeof(header));
        const uint64_t expected=16+uint64_t(header[2])*sizeof(Vertex)+uint64_t(header[3])*2;
        if(header[0]!=0x48525855 || header[1]!=1 || !header[2] || header[2]>65535 || !header[3] || header[3]>1000000 || header[3]%3 || expected!=bytes.size())
            throw std::runtime_error("invalid embedded hand");
        auto& m=meshes_[h];m.vertices.resize(header[2]);m.indices.resize(header[3]);posed_[h].resize(header[2]);
        std::memcpy(m.vertices.data(),bytes.data()+16,m.vertices.size()*sizeof(Vertex));
        std::memcpy(m.indices.data(),bytes.data()+16+m.vertices.size()*sizeof(Vertex),m.indices.size()*2);
        for(auto index:m.indices)if(index>=m.vertices.size())throw std::runtime_error("invalid hand index");
        // Both baked source meshes have the same anatomical orientation. Reflect
        // the left mesh across its palm normal, including normals and winding.
        if(h==0){
            for(auto& v:m.vertices)for(int pose=0;pose<4;++pose){v.position[pose][1]*=-1;v.normal[pose][1]*=-1;}
            for(size_t i=0;i<m.indices.size();i+=3)std::swap(m.indices[i+1],m.indices[i+2]);
        }
    }
    auto image=gt2::DecodePng(VrHandAsset(2));textureWidth_=uint32_t(image.width);textureHeight_=uint32_t(image.height);
    const size_t texels=size_t(textureWidth_)*textureHeight_;
    if(texels>SceneRenderer::kExternalTexels-SceneRenderer::kVrHandTexelBase)throw std::runtime_error("hand texture too large");
    std::vector<uint32_t> rgba(texels);std::memcpy(rgba.data(),image.rgba.data(),texels*4);
    renderer_.UploadHandTexture(textureWidth_,textureHeight_,rgba.data());
    vertices_.reserve(32768);
}
void VrDrivingVisuals::Append(std::vector<DrawItem>& items,const gt2::vr::TrackedControllers& in,
        const gt2::vr::DrivingController& controls,const gt2::vr::DrivingSettings& settings,const float matrix[16]){
    if(settings.mode==0)return;
    auto draw=[&](uint32_t offset,uint32_t count,V right,V up,V back,V position){
        float local[16] = {right.x,right.y,right.z,0,up.x,up.y,up.z,0,back.x,back.y,back.z,0,position.x,position.y,position.z,1};
        DrawItem item;item.firstVertex=SceneRenderer::kVrDrivingVertexBase+offset;item.vertexCount=count;item.space=kSpaceWorld;
        // Hands and the interactive wheel share the seated geometry depth range
        // on every backend, including Quest's Android submission path.
        item.cabinDepth=2;
        for(int c=0;c<4;++c)for(int r=0;r<4;++r)for(int k=0;k<4;++k)item.mvp[c*4+r]+=matrix[k*4+r]*local[c*4+k];
        items.push_back(item);
    };
    const auto anchor=gt2::vr::DrivingWheelCenter(settings);
    const V center{anchor[0],anchor[1],anchor[2]};
    const float radius=settings.wheelRadiusCm*.01f,angle=controls.Angle();
    const auto basis=gt2::vr::DrivingWheelBasis(settings);
    const V wheelRight{basis[0][0],basis[0][1],basis[0][2]},wheelUp{basis[1][0],basis[1][1],basis[1][2]},wheelBack{basis[2][0],basis[2][1],basis[2][2]};
    const V rimRight=wheelRight*std::cos(angle)+wheelUp*std::sin(angle),rimUp=wheelRight*(-std::sin(angle))+wheelUp*std::cos(angle);
    if(settings.mode==1 && !settings.authoredWheel){
        if(wheelRadius_ != settings.wheelRadiusCm){
            wheelRadius_=settings.wheelRadiusCm;
            wheel_=BuildSteeringWheel(renderer_,radius);
            if(wheel_.size()>18432)throw std::runtime_error("steering wheel exceeds reserved range");
            renderer_.SetHandVertices(18432,wheel_);
        }
        draw(18432,uint32_t(wheel_.size()),rimRight,rimUp,wheelBack,center);
    }

    const V light=Normal({-.35f,.9f,.25f});
    for(int h=0;h<2;++h){
        if(!in.gripValid[h])continue;
        const auto& pose=in.gripPose[h];
        V position{pose.position[0],pose.position[1],pose.position[2]};
        V gripRight=Rotate(pose.orientation,{1,0,0}), gripUp=Rotate(pose.orientation,{0,1,0}), gripForward=Rotate(pose.orientation,{0,0,-1});
        V forward=in.aimValid[h]?Rotate(in.aimPose[h].orientation,{0,0,-1}):gripForward;
        if(settings.mode==1 && controls.Grabbed(h)){
            position=center+rimRight*((h==0?-1.f:1.f)*radius)+wheelBack*.05f;
            gripRight=rimRight;gripUp=rimUp;gripForward=forward=wheelBack*-1;
        }
        forward=Normal(forward);
        V up=gripRight;up=up-forward*Dot(up,forward);
        if(Dot(up,up)<.0001f)up=gripUp;
        up=Normal(up);V right=Normal(Cross(up,forward));
        // The cross product fixes handedness. Comparing this perpendicular
        // axis with gripForward tests numerical noise on a held wheel and can
        // reflect the hand on alternate frames, especially with authored tilt.

        const bool holdingWheel = settings.mode == 1 && controls.Grabbed(h);
        const int gripStep = holdingWheel ? 32 : int(std::lround(std::clamp(in.grip[h],0.f,1.f)*32));
        const int triggerStep = holdingWheel ? 32 : int(std::lround(std::clamp(in.trigger[h],0.f,1.f)*32));
        const uint32_t offset=36864+uint32_t(h)*14000;
        if(gripStep_[h]!=gripStep || triggerStep_[h]!=triggerStep){
            gripStep_[h]=gripStep;triggerStep_[h]=triggerStep;
            const float grip=gripStep/32.f,trigger=triggerStep/32.f;
            for(size_t index=0;index<meshes_[h].vertices.size();++index){
                const auto& source=meshes_[h].vertices[index];
                V p=Blend(source.position,grip,trigger),n=Normal(Blend(source.normal,grip,trigger));
                const float shade=.72f+.28f*std::max(0.f,Dot(n,light));
                auto v=Colored(p,{shade,shade,shade});
                v.texel[0]=source.u;v.texel[1]=source.v;v.flags=kTextured|kHandTexture;
                posed_[h][index]=v;
            }
            vertices_.clear();
            for(uint16_t index:meshes_[h].indices)vertices_.push_back(posed_[h][index]);
            if(vertices_.size()>14000)throw std::runtime_error("hand exceeds reserved range");
            renderer_.SetHandVertices(offset,vertices_);
        }
        draw(offset,uint32_t(meshes_[h].indices.size()),forward,up*-1,right,position-forward*.055f);
    }
}
}
