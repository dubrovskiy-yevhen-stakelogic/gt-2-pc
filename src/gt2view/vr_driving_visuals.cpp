#include "gt2view/vr_driving_visuals.h"
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
VrDrivingVisuals::VrDrivingVisuals(VkSceneRenderer& renderer):renderer_(renderer){
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
    if(texels>VkSceneRenderer::kExternalTexels-VkSceneRenderer::kVrHandTexelBase)throw std::runtime_error("hand texture too large");
    std::vector<uint32_t> rgba(texels);std::memcpy(rgba.data(),image.rgba.data(),texels*4);
    renderer_.UploadHandTexture(textureWidth_,textureHeight_,rgba.data());
    vertices_.reserve(32768);
}
void VrDrivingVisuals::Append(std::vector<DrawItem>& items,const gt2::vr::TrackedControllers& in,
        const gt2::vr::DrivingController& controls,const gt2::vr::DrivingSettings& settings,const float matrix[16]){
    if(settings.mode==0)return;
    auto draw=[&](uint32_t offset,uint32_t count,V right,V up,V back,V position){
        float local[16] = {right.x,right.y,right.z,0,up.x,up.y,up.z,0,back.x,back.y,back.z,0,position.x,position.y,position.z,1};
        DrawItem item;item.firstVertex=VkSceneRenderer::kVrDrivingVertexBase+offset;item.vertexCount=count;item.space=kSpaceWorld;
        for(int c=0;c<4;++c)for(int r=0;r<4;++r)for(int k=0;k<4;++k)item.mvp[c*4+r]+=matrix[k*4+r]*local[c*4+k];
        items.push_back(item);
    };
    constexpr float pi=3.14159265358979323846f;
    const V center{0,settings.wheelHeightCm*.01f,-settings.wheelDistanceCm*.01f};
    const float radius=settings.wheelRadiusCm*.01f,angle=controls.Angle();
    const V rimRight{std::cos(angle),std::sin(angle),0},rimUp{-std::sin(angle),std::cos(angle),0};
    if(settings.mode==1){
        if(wheelRadius_ != settings.wheelRadiusCm){
            wheelRadius_=settings.wheelRadiusCm; wheel_.clear();
            const V lamp=Normal({-.35f,.65f,1.f});
            auto vertex=[&](V p,V n,V color){
                const float diffuse=std::max(0.f,Dot(n,lamp));
                const float gloss=std::pow(std::max(0.f,Dot(n,Normal({-.15f,.25f,1.f}))),16.f)*.10f;
                return Colored(p,color*(.48f+.52f*diffuse)+V{gloss,gloss,gloss});
            };
            auto quad=[&](V a,V b,V c,V d,V color){
                V n=Normal(Cross(b-a,c-a));
                for(V p:{a,b,c,a,c,d})wheel_.push_back(vertex(p,n,color));
            };
            // Rounded leather rim with a centre marker. Lighting is baked once.
            for(int i=0;i<64;++i)for(int j=0;j<8;++j){
                SceneVertex q[4]; int at=0;
                for(auto ij:{std::pair{i,j},std::pair{i+1,j},std::pair{i+1,j+1},std::pair{i,j+1}}){
                    float a=2*pi*ij.first/64,b=2*pi*ij.second/8;
                    V radial{std::cos(a),std::sin(a),0};
                    V n=radial*std::cos(b)+V{0,0,std::sin(b)};
                    V p=radial*radius+n*.016f;
                    V color=(i==15 || i==16)?V{.85f,.58f,.10f}:V{.115f,.125f,.14f};
                    q[at++]=vertex(p,n,color);
                }
                for(int k:{0,1,2,0,2,3})wheel_.push_back(q[k]);
            }
            // Three tapered metal spokes with thickness, behind the padded hub.
            for(float a:{.05f,pi-.05f,1.5f*pi}){
                V d{std::cos(a),std::sin(a),0},side{-d.y,d.x,0};
                V q[4]={d*.035f-side*.026f,d*(radius-.014f)-side*.013f,
                         d*(radius-.014f)+side*.013f,d*.035f+side*.026f};
                V z{0,0,.008f};
                quad(q[0]+z,q[1]+z,q[2]+z,q[3]+z,{.48f,.51f,.56f});
                quad(q[3]-z,q[2]-z,q[1]-z,q[0]-z,{.22f,.24f,.28f});
                for(int k=0;k<4;++k)quad(q[k]-z,q[(k+1)%4]-z,q[(k+1)%4]+z,q[k]+z,{.25f,.27f,.30f});
            }
            auto cylinder=[&](V origin,float r,float depth,int sides,V color){
                for(int i=0;i<sides;++i){
                    float a=2*pi*i/sides,b=2*pi*(i+1)/sides;
                    V u{r*std::cos(a),r*std::sin(a),0},v{r*std::cos(b),r*std::sin(b),0},z{0,0,depth};
                    for(V p:{origin+z,origin+u+z,origin+v+z})wheel_.push_back(vertex(p,{0,0,1},color));
                    quad(origin+u,origin+v,origin+v+z,origin+u+z,color);
                }
            };
            cylinder({0,0,.008f},.052f,.020f,32,{.105f,.115f,.13f});
            cylinder({0,0,.029f},.026f,.002f,24,{.30f,.33f,.38f});
            for(int i=0;i<6;++i){float a=2*pi*i/6;cylinder({.039f*std::cos(a),.039f*std::sin(a),.029f},.003f,.001f,8,{.65f,.67f,.70f});}
            if(wheel_.size()>8192)throw std::runtime_error("wheel exceeds reserved range");
            renderer_.SetHandVertices(0,wheel_);
        }
        draw(0,uint32_t(wheel_.size()),rimRight,rimUp,{0,0,1},center);
    }
    const V light=Normal({-.35f,.9f,.25f});
    for(int h=0;h<2;++h){
        if(!in.gripValid[h])continue;
        const auto& pose=in.gripPose[h];
        V position{pose.position[0],pose.position[1],pose.position[2]};
        V gripRight=Rotate(pose.orientation,{1,0,0}), gripUp=Rotate(pose.orientation,{0,1,0}), gripForward=Rotate(pose.orientation,{0,0,-1});
        V forward=in.aimValid[h]?Rotate(in.aimPose[h].orientation,{0,0,-1}):gripForward;
        if(settings.mode==1 && controls.Grabbed(h)){
            position=center+rimRight*((h==0?-1.f:1.f)*radius)+V{0,0,.05f};
            gripRight=rimRight;gripUp=rimUp;gripForward=forward={0,0,-1};
        }
        forward=Normal(forward);
        V up=gripRight;up=up-forward*Dot(up,forward);
        if(Dot(up,up)<.0001f)up=gripUp;
        up=Normal(up);V right=Normal(Cross(up,forward));
        if(Dot(right,gripForward)<0)right=right*-1;

        const bool holdingWheel = settings.mode == 1 && controls.Grabbed(h);
        const int gripStep = holdingWheel ? 32 : int(std::lround(std::clamp(in.grip[h],0.f,1.f)*32));
        const int triggerStep = holdingWheel ? 32 : int(std::lround(std::clamp(in.trigger[h],0.f,1.f)*32));
        const uint32_t offset=8192+uint32_t(h)*14000;
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