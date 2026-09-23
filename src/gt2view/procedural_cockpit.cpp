#include "gt2view/procedural_cockpit.h"
#include "gt2view/cockpit_materials.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace gt2view {
namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr uint32_t kCabinLimit = 24576;
constexpr uint32_t kWheelOffset = kCabinLimit, kWheelLimit = 4096;
constexpr uint32_t kInstrumentOffset = kWheelOffset + kWheelLimit, kInstrumentLimit = 4000;
constexpr uint32_t kMirrorOffset = kInstrumentOffset + kInstrumentLimit, kMirrorLimit = 96;
static_assert(kMirrorOffset + kMirrorLimit <= VkSceneRenderer::kCockpitVertexLimit);

struct V {
    float x, y, z;
    V operator+(V b) const { return {x+b.x, y+b.y, z+b.z}; }
    V operator-(V b) const { return {x-b.x, y-b.y, z-b.z}; }
    V operator*(float s) const { return {x*s, y*s, z*s}; }
    bool operator==(const V&) const = default;
};
float Dot(V a, V b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
V Cross(V a, V b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
V Unit(V v) { const float length = std::sqrt(Dot(v,v)); return length > .00001f ? v*(1.f/length) : V{0,1,0}; }
V Polar(float a, float radius) { return {std::cos(a)*radius, std::sin(a)*radius, 0}; }
float GaugeAngle(float amount) { return (225.f - 270.f*std::clamp(amount, 0.f, 1.f))*kPi/180.f; }

constexpr V kDash{kCockpitInteriorRgb[0],kCockpitInteriorRgb[1],kCockpitInteriorRgb[2]}, kDashTop=kDash;
constexpr V kTrim{.30f,.33f,.36f}, kBlack{.045f,.052f,.06f};
constexpr V kFabric{.115f,.115f,.115f};
constexpr V kIvory{.83f,.85f,.79f}, kAmber{1.f,.43f,.11f};

class Mesh {
public:
    std::vector<SceneVertex> vertices;
    void Triangle(V a, V b, V c, V color, bool shade = true) {
        // Source body lining is unlit; keep the adjoining shell the same colour.
        if (shade && color!=kDash) {
            const V normal = Unit(Cross(b-a,c-a));
            color = color*(.72f + .20f*std::abs(normal.y) + .10f*std::abs(normal.x) + .04f*normal.z);
        }
        for (V p : {a,b,c}) {
            SceneVertex v{};
            v.pos[0]=p.x; v.pos[1]=p.y; v.pos[2]=p.z;
            v.color[0]=color.x; v.color[1]=color.y; v.color[2]=color.z;
            vertices.push_back(v);
        }
    }
    void Quad(V a, V b, V c, V d, V color, bool shade = true) {
        Triangle(a,b,c,color,shade); Triangle(a,c,d,color,shade);
    }
    void Box(V a, V b, V color) {
        Quad({a.x,a.y,b.z},{b.x,a.y,b.z},{b.x,b.y,b.z},{a.x,b.y,b.z},color);
        Quad({b.x,a.y,a.z},{a.x,a.y,a.z},{a.x,b.y,a.z},{b.x,b.y,a.z},color);
        Quad({a.x,b.y,a.z},{a.x,b.y,b.z},{b.x,b.y,b.z},{b.x,b.y,a.z},color);
        Quad({a.x,a.y,b.z},{a.x,a.y,a.z},{b.x,a.y,a.z},{b.x,a.y,b.z},color);
        Quad({a.x,a.y,a.z},{a.x,a.y,b.z},{a.x,b.y,b.z},{a.x,b.y,a.z},color);
        Quad({b.x,a.y,b.z},{b.x,a.y,a.z},{b.x,b.y,a.z},{b.x,b.y,b.z},color);
    }
    template<class Offset>
    void BendAlongX(size_t start, const std::vector<float>& stations, Offset offset) {
        const std::vector<SceneVertex> original(vertices.begin()+start,vertices.end());
        vertices.resize(start);
        for (size_t at=0;at<original.size();at+=3) {
            std::vector<std::vector<SceneVertex>> pieces{{original[at],original[at+1],original[at+2]}};
            for (float x:stations) {
                std::vector<std::vector<SceneVertex>> next;
                for (const auto& piece:pieces) {
                    float lo=piece[0].pos[0],hi=lo;
                    for (const auto& v:piece) { lo=std::min(lo,v.pos[0]); hi=std::max(hi,v.pos[0]); }
                    if (x<=lo+.00001f || x>=hi-.00001f) { next.push_back(piece); continue; }
                    for (bool left:{true,false}) {
                        std::vector<SceneVertex> half;
                        for (size_t i=0;i<piece.size();++i) {
                            const auto& a=piece[i]; const auto& b=piece[(i+1)%piece.size()];
                            const bool inA=left?a.pos[0]<=x:a.pos[0]>=x;
                            const bool inB=left?b.pos[0]<=x:b.pos[0]>=x;
                            if (inA) half.push_back(a);
                            if (inA!=inB) {
                                const float t=(x-a.pos[0])/(b.pos[0]-a.pos[0]);
                                auto v=a;
                                for (int axis=0;axis<3;++axis) v.pos[axis]+=t*(b.pos[axis]-a.pos[axis]);
                                half.push_back(v);
                            }
                        }
                        if (half.size()>=3) next.push_back(std::move(half));
                    }
                }
                pieces=std::move(next);
            }
            for (auto& piece:pieces) {
                for (auto& v:piece) v.pos[2]+=offset(v.pos[0]);
                for (size_t i=1;i+1<piece.size();++i)
                    vertices.insert(vertices.end(),{piece[0],piece[i],piece[i+1]});
            }
        }
    }
    void Beam(V a, V b, float radius, V color, int sides = 6) {
        const V direction = Unit(b-a);
        const V u = Unit(Cross(direction, std::abs(direction.y) > .95f ? V{1,0,0} : V{0,1,0}));
        const V v = Cross(direction,u);
        for (int i=0; i<sides; ++i) {
            const float aa=2*kPi*float(i)/float(sides), ab=2*kPi*float(i+1)/float(sides);
            const V p=(u*std::cos(aa)+v*std::sin(aa))*radius;
            const V q=(u*std::cos(ab)+v*std::sin(ab))*radius;
            Quad(a+p,a+q,b+q,b+p,color);
            Triangle(a,a+q,a+p,color); Triangle(b,b+p,b+q,color);
        }
    }
    void Disc(V center, float radius, V color, int sides = 32) {
        for (int i=0; i<sides; ++i)
            Triangle(center,center+Polar(2*kPi*float(i)/float(sides),radius),
                     center+Polar(2*kPi*float(i+1)/float(sides),radius),color,false);
    }
    void Ring(V center, float inner, float outer, V color, int sides = 32) {
        for (int i=0; i<sides; ++i) {
            const float a=2*kPi*float(i)/float(sides), b=2*kPi*float(i+1)/float(sides);
            Quad(center+Polar(a,inner),center+Polar(a,outer),center+Polar(b,outer),
                 center+Polar(b,inner),color,false);
        }
    }
    void Line(V a, V b, float width, V color) {
        const V d=Unit(b-a), side{-d.y*width*.5f,d.x*width*.5f,0};
        Quad(a-side,b-side,b+side,a+side,color,false);
    }
    void Text(const std::string& text, V center, float height, V color) {
        const float pixel=height/7.f;
        const float x0=center.x-(float(text.size())*6.f-1.f)*pixel*.5f;
        for (size_t i=0; i<text.size(); ++i) {
            const auto rows=Glyph(text[i]);
            for (int y=0;y<7;++y) for (int x=0;x<5;++x) {
                if (!(rows[size_t(y)] & (1u<<(4-x)))) continue;
                const float left=x0+(float(i)*6.f+float(x))*pixel;
                const float top=center.y+(3.5f-float(y))*pixel;
                Quad({left,top-pixel,center.z},{left+pixel,top-pixel,center.z},
                     {left+pixel,top,center.z},{left,top,center.z},color,false);
            }
        }
    }
private:
    static std::array<uint8_t,7> Glyph(char c) {
        switch (c) {
        case '0': return {14,17,19,21,25,17,14};
        case '1': return {4,12,4,4,4,4,14};
        case '2': return {14,17,1,2,4,8,31};
        case '3': return {30,1,1,14,1,1,30};
        case '4': return {2,6,10,18,31,2,2};
        case '5': return {31,16,16,30,1,1,30};
        case '6': return {14,16,16,30,17,17,14};
        case '7': return {31,1,2,4,8,8,8};
        case '8': return {14,17,17,14,17,17,14};
        case '9': return {14,17,17,15,1,1,14};
        case 'R': return {30,17,17,30,20,18,17};
        case 'N': return {17,25,25,21,19,19,17};
        case 'K': return {17,18,20,24,20,18,17};
        case 'M': return {17,27,21,21,17,17,17};
        case 'H': return {17,17,17,31,17,17,17};
        case 'P': return {30,17,17,30,16,16,16};
        case '/': return {1,1,2,4,8,16,16};
        case '-': return {0,0,0,31,0,0,0};
        default: return {};
        }
    }
};

std::array<float,203> FitKey(const CockpitFit& f) {
    std::array<float,203> key{f.halfWidth,f.frontZ,f.rearZ,f.floorY,f.roofY,f.windshieldZ,f.dashboardY,
        f.eye[0],f.eye[1],f.eye[2],f.windshieldY,f.roofFrontZ,f.roofFrontY,f.roofRearZ,f.roofRearY,
        f.rearWindowZ,f.rearWindowY,f.windshieldHalfWidth,f.roofFrontHalfWidth,f.roofRearHalfWidth,
        f.rearWindowHalfWidth,f.openTop?1.f:0.f};
    size_t at=22;
    for (const auto* pair : {&f.frontCowl,&f.frontHeader,&f.rearHeader,&f.rearCowl})
        for (const auto& point : *pair) for (float component : point) key[at++]=component;
    for (float component : f.mirror) key[at++]=component;
    for (const auto& point : f.cowl) for (float component : point) key[at++]=component;
    for (const auto& side : f.sideSill) for (const auto& point : side)
        for (float component : point) key[at++]=component;
    key[at]=f.sideSillValid?1.f:0.f;
    return key;
}

void Vent(Mesh& mesh, float x, float y, float z, float width, float height) {
    mesh.Box({x-width*.5f-.006f,y-height*.5f-.006f,z-.009f},
             {x+width*.5f+.006f,y+height*.5f+.006f,z},kTrim);
    mesh.Box({x-width*.5f,y-height*.5f,z},{x+width*.5f,y+height*.5f,z+.001f},kBlack);
    for (int i=1;i<5;++i) {
        const float yy=y-height*.5f+height*float(i)/5.f;
        mesh.Box({x-width*.47f,yy-.0015f,z+.0015f},{x+width*.47f,yy+.0015f,z+.006f},kDash);
    }
}

void Seat(Mesh& mesh, float x, float y, float z, float width, float backTop) {
    mesh.Box({x-width*.5f,y-.075f,z-.26f},{x+width*.5f,y+.025f,z+.22f},kBlack);
    mesh.Box({x-width*.36f,y+.025f,z-.235f},{x+width*.36f,y+.075f,z+.19f},kFabric);
    for (float sign : {-1.f,1.f}) {
        const float sx=x+sign*width*.43f;
        mesh.Beam({sx,y+.04f,z-.20f},{sx,y+.055f,z+.18f},width*.085f,kDash,6);
        mesh.Beam({sx,y+.08f,z+.18f},{sx,backTop-.06f,z+.31f},width*.08f,kDash,6);
    }
    const float front=z+.20f, back=z+.31f;
    mesh.Quad({x-width*.37f,y+.06f,front},{x+width*.37f,y+.06f,front},
              {x+width*.34f,backTop,back},{x-width*.34f,backTop,back},kFabric);
    mesh.Quad({x-width*.45f,y,front+.10f},{x-width*.40f,backTop,back+.07f},
              {x+width*.40f,backTop,back+.07f},{x+width*.45f,y,front+.10f},kBlack);
    mesh.Box({x-width*.40f,backTop-.04f,back},{x+width*.40f,backTop+.01f,back+.075f},kDash);
    for (float sign : {-1.f,1.f})
        mesh.Beam({x+sign*.06f,backTop,back+.02f},{x+sign*.06f,backTop+.065f,back+.02f},.008f,kTrim,6);
    mesh.Box({x-width*.26f,backTop+.055f,back-.035f},{x+width*.26f,backTop+.18f,back+.085f},kDash);
    for (int i=0;i<4;++i) {
        const float yy=y+.15f+float(i)*.065f;
        if (yy>backTop-.02f) break;
        const float zz=front+(back-front)*(yy-y-.06f)/(backTop-y-.06f)-.001f;
        mesh.Line({x-width*.30f,yy,zz},{x+width*.30f,yy,zz},.003f,kTrim*.70f);
    }
}

void Gauge(Mesh& mesh, V center, bool tachometer) {
    mesh.Disc(center,.077f,kBlack,40);
    mesh.Ring(center+V{0,0,.001f},.0695f,.074f,kTrim,40);
    mesh.Disc(center+V{0,0,.0015f},.069f,{.025f,.032f,.040f},40);
    constexpr int ticks=20;
    for (int i=0;i<=ticks;++i) {
        const float amount=float(i)/ticks, a=GaugeAngle(amount);
        const bool major=(i%2)==0;
        const V color=tachometer && i>=16 ? V{.85f,.21f,.14f} : kIvory;
        const V c=center+V{0,0,.0025f};
        mesh.Line(c+Polar(a,major?.056f:.059f),c+Polar(a,.064f),major?.0023f:.0012f,color);
        if (major && (tachometer || i%4==0)) {
            const int value=tachometer?i/2:i*15;
            mesh.Text(std::to_string(value),c+Polar(a,.046f),.0085f,kIvory*.92f);
        }
    }
    mesh.Text(tachometer?"RPM":"KM/H",center+V{0,-.027f,.003f},.0075f,kIvory*.65f);
    if (tachometer) mesh.Text("1000",center+V{0,-.040f,.003f},.0065f,kIvory*.65f);
}
} // namespace

ProceduralCockpit::ProceduralCockpit(VkSceneRenderer& renderer) : renderer_(renderer) {}

void ProceduralCockpit::Build(const CockpitFit& fit) {
    Mesh cabin;
    cabin.vertices.reserve(18000);
    const float eyeX=fit.eye[0], eyeY=fit.eye[1], eyeZ=fit.eye[2];
    const float floor=fit.floorY+.035f;
    const float dashFront=fit.windshieldZ+.020f;
    const float cowlY=fit.windshieldY-.025f;
    const float fasciaTop=std::min(cowlY-.035f,eyeY-.255f);
    const float dashboard=fasciaTop+.045f;
    auto outerSide=[&](size_t side,float z) {
        const auto p=CockpitSideSillAt(fit,side,z);
        return V{p[0],p[1],p[2]};
    };
    auto sidePoint=[&](size_t side,float z) {
        const float sign=side==0?-1.f:1.f;
        const float t=std::clamp((z-fit.frontCowl[side][2])/
            std::max(.15f,fit.rearCowl[side][2]-fit.frontCowl[side][2]),0.f,1.f);
        return outerSide(side,z)-V{sign*(.030f+.010f*t),.025f+.010f*t,0};
    };
    V front[2]{},rear[2]{};
    for (size_t i=0;i<2;++i) {
        front[i]=sidePoint(i,fit.frontCowl[i][2]+.020f);
        rear[i]=sidePoint(i,fit.rearCowl[i][2]-.035f);
    }
    const V frontMiddle{0,cowlY,dashFront};
    auto cowlAt=[&](float x) {
        for (size_t i=1;i<fit.cowl.size();++i) {
            const auto& a=fit.cowl[i-1]; const auto& b=fit.cowl[i];
            if (x>b[0] && i+1<fit.cowl.size()) continue;
            const float t=std::clamp((x-a[0])/std::max(.001f,b[0]-a[0]),0.f,1.f);
            return V{x,a[1]+(b[1]-a[1])*t-.025f,a[2]+(b[2]-a[2])*t+.020f};
        }
        return frontMiddle;
    };
    const float podLeft=eyeX-.181f,podRight=eyeX+.181f;
    const float dashBack=std::max({dashFront+.12f,eyeZ-.52f,
                                  cowlAt(podLeft-.015f).z+.06f,cowlAt(podRight+.015f).z+.06f});
    const float cabinBack=std::min(rear[0].z,rear[1].z);
    const float width=std::max(.32f,std::min(-sidePoint(0,dashBack).x,sidePoint(1,dashBack).x));
    // Join the inset dashboard and door cards to the authored body boundary.
    for (size_t i=1;i<fit.cowl.size();++i) {
        const auto& a=fit.cowl[i-1]; const auto& b=fit.cowl[i];
        cabin.Quad({a[0],a[1],a[2]},{b[0],b[1],b[2]},cowlAt(b[0]),cowlAt(a[0]),kDashTop);
    }
    const V floorFront{0,floor,dashFront},floorRear{0,floor,cabinBack};
    const V floorLeft{front[0].x,floor,front[0].z},floorRight{front[1].x,floor,front[1].z};
    const V rearLeft{rear[0].x,floor,rear[0].z},rearRight{rear[1].x,floor,rear[1].z};
    cabin.Quad(floorLeft,floorFront,floorRear,rearLeft,kBlack);
    cabin.Quad(floorFront,floorRight,rearRight,floorRear,kBlack);
    const std::array<float,5> deckX{-width,podLeft-.015f,0.f,podRight+.015f,width};
    // Side cowls may sweep behind the instruments. Keep those wings outside
    // the binnacle instead of spanning it with one folded full-width quad.
    auto fasciaAt=[&](float x) {
        return V{x,fasciaTop,std::max(dashBack,cowlAt(x).z+.06f)};
    };
    // Sort because the binnacle may straddle the vehicle centre line.
    std::vector<float> stations(deckX.begin(),deckX.end());
    for (const auto& point : fit.cowl) if (point[0]>-width && point[0]<width) stations.push_back(point[0]);
    std::sort(stations.begin(),stations.end());
    stations.erase(std::unique(stations.begin(),stations.end()),stations.end());
    for (size_t i=1;i<stations.size();++i) {
        const float a=stations[i-1],b=stations[i];
        const V backA=fasciaAt(a),backB=fasciaAt(b);
        const V topA=cowlAt(a),topB=cowlAt(b);
        cabin.Quad(topA,topB,backB,backA,kDashTop);
        cabin.Quad({a,floor,topA.z},{b,floor,topB.z},topB,topA,kBlack);
        cabin.Triangle({a,floor,topA.z},{b,floor,topB.z},floorRear,kBlack);
        cabin.Quad(backA-V{0,.195f,0},backB-V{0,.195f,0},backB,backA,kDash);
        cabin.Quad({a,floor,topA.z},{b,floor,topB.z},backB-V{0,.195f,0},backA-V{0,.195f,0},kBlack);
    }
    for (size_t i=0;i<2;++i) {
        const float x=i==0?-width:width;
        cabin.Quad({front[i].x,floor,front[i].z},front[i],
                   fasciaAt(x),{x,floor,fasciaAt(x).z},kDash);
        const V edge=fasciaAt(x);
        V door=sidePoint(i,edge.z); door.y-=.022f; door.z=edge.z;
        cabin.Quad({edge.x,floor,edge.z},{door.x,floor,door.z},door,edge,kDash);
    }
    const size_t dashboardDetails=cabin.vertices.size();
    for (const auto& span : {std::array<float,2>{-width+.04f,eyeX-.20f},
                             std::array<float,2>{eyeX+.20f,width-.04f}})
        if (span[1]>span[0]) cabin.Box({span[0],dashboard-.078f,dashBack+.001f},
                                     {span[1],dashboard-.065f,dashBack+.008f},kTrim*.72f);

    // The instrument face sits behind a shallow, open-bottom binnacle.
    dialX_[0]=eyeX-.086f; dialX_[1]=eyeX+.086f;
    dialZ_=dashBack+.014f;
    // Recess the instruments into the fascia. The hood/cowl sets the upper
    // edge; moving the driver's eye must never raise a pod into the windscreen.
    dialY_=std::min(cowlY-.085f,fasciaTop-.045f);
    const float podTop=dialY_+.080f;
    const float podFront=std::max(dashFront+.025f,dashBack-.18f);
    const float podBase=std::min(podTop-.04f,fasciaTop+(cowlY-fasciaTop)*
        std::clamp((dashBack-podFront)/std::max(.10f,dashBack-dashFront),0.f,1.f)-.006f);
    cabin.Box({podLeft, dashboard-.087f,dashBack-.035f},
              {podRight,podTop-.017f,dashBack+.004f},kBlack);
    cabin.Quad({podLeft-.012f,dashboard-.06f,dashBack+.02f},
               {podLeft+.026f,podTop,dashBack+.02f},
               {podLeft+.026f,podTop-.025f,podFront},
               {podLeft-.012f,podBase,podFront},kDashTop);
    cabin.Quad({podRight-.026f,podTop,dashBack+.02f},
               {podRight+.012f,dashboard-.06f,dashBack+.02f},
               {podRight+.012f,podBase,podFront},
               {podRight-.026f,podTop-.025f,podFront},kDashTop);
    cabin.Quad({podLeft+.026f,podTop,dashBack+.025f},{podRight-.026f,podTop,dashBack+.025f},
               {podRight-.026f,podTop-.025f,podFront},{podLeft+.026f,podTop-.025f,podFront},kDashTop);
    cabin.Quad({podLeft+.026f,podBase,podFront},{podLeft+.026f,podTop-.025f,podFront},
               {podRight-.026f,podTop-.025f,podFront},{podRight-.026f,podBase,podFront},kDashTop);
    Gauge(cabin,{dialX_[0],dialY_,dialZ_},false);
    Gauge(cabin,{dialX_[1],dialY_,dialZ_},true);
    cabin.Box({eyeX-.016f,dialY_+.028f,dialZ_+.003f},
              {eyeX+.016f,dialY_+.052f,dialZ_+.006f},kTrim);
    cabin.Box({eyeX-.013f,dialY_+.030f,dialZ_+.0065f},
              {eyeX+.013f,dialY_+.050f,dialZ_+.007f},kBlack);

    Vent(cabin,-width+.125f,dashboard-.126f,dashBack+.011f,.13f,.052f);
    Vent(cabin,width-.14f,dashboard-.126f,dashBack+.011f,.16f,.052f);
    Vent(cabin,.02f,dashboard-.116f,dashBack+.016f,.102f,.058f);
    Vent(cabin,.135f,dashboard-.116f,dashBack+.016f,.102f,.058f);
    cabin.Box({.01f,dashboard-.23f,dashBack+.008f},{.19f,dashboard-.163f,dashBack+.018f},kBlack);
    for (int i=0;i<3;++i) {
        const float x=.047f+float(i)*.052f;
        cabin.Disc({x,dashboard-.195f,dashBack+.021f},.015f,kTrim,16);
        cabin.Disc({x,dashboard-.195f,dashBack+.023f},.0115f,kDash,16);
        cabin.Line({x,dashboard-.191f,dashBack+.025f},{x,dashboard-.184f,dashBack+.025f},.002f,kIvory);
    }
    // Passenger glovebox seam and latch.
    const float gloveLeft=std::min(.26f,width*.45f),gloveRight=width-.06f;
    const float gloveCenter=(gloveLeft+gloveRight)*.5f;
    cabin.Box({gloveLeft,dashboard-.205f,dashBack+.002f},{gloveRight,dashboard-.095f,dashBack+.005f},kBlack);
    cabin.Box({gloveLeft+.004f,dashboard-.201f,dashBack+.0055f},{gloveRight-.004f,dashboard-.099f,dashBack+.007f},kDash);
    cabin.Box({gloveCenter-.035f,dashboard-.132f,dashBack+.008f},
              {gloveCenter+.035f,dashboard-.12f,dashBack+.014f},kTrim);
    cabin.BendAlongX(dashboardDetails,stations,[&](float x) { return fasciaAt(x).z-dashBack; });

    const float tunnelTop=std::min(floor+.25f,eyeY-.44f);
    const float cabinSeatZ=eyeZ+seatOffset_[1];
    const float tunnelEnd=std::min(cabinSeatZ+.74f,cabinBack-.06f);
    cabin.Box({-.105f,floor, dashFront},{.105f,tunnelTop,tunnelEnd},kDash);
    cabin.Box({-.079f,tunnelTop,cabinSeatZ-.28f},{.079f,tunnelTop+.014f,cabinSeatZ+.20f},kBlack);
    const float shifterZ=cabinSeatZ-.14f;
    cabin.Beam({0,tunnelTop+.014f,shifterZ},{0,tunnelTop+.15f,shifterZ-.025f},.024f,kBlack,8);
    cabin.Beam({0,tunnelTop+.07f,shifterZ-.01f},{0,tunnelTop+.18f,shifterZ-.034f},.011f,kTrim,8);
    cabin.Beam({0,tunnelTop+.175f,shifterZ-.034f},{0,tunnelTop+.207f,shifterZ-.039f},.029f,kBlack,8);
    cabin.Beam({.055f,tunnelTop+.025f,cabinSeatZ+.20f},{.055f,tunnelTop+.058f,cabinSeatZ+.025f},.018f,kBlack,6);
    if (tunnelEnd>cabinSeatZ+.38f)
        cabin.Box({-.082f,tunnelTop+.009f,cabinSeatZ+.32f},
                  {.082f,tunnelTop+.075f,std::min(cabinSeatZ+.66f,tunnelEnd)},kFabric);

    // Door trim remains below the original windows; their frames belong to the car body.
    for (size_t i=0;i<2;++i) {
        const float sign=i==0?-1.f:1.f;
        auto inside=[&](float z,float below) {
            V p=sidePoint(i,z); p.x-=sign*.007f; p.y-=below; p.z=z; return p;
        };
        const float panelFront=std::max(front[i].z+.09f,eyeZ-.28f);
        const float panelBack=std::min(rear[i].z-.08f,eyeZ+.58f);
        std::vector<float> doorStations{front[i].z,rear[i].z};
        if (fit.sideSillValid) for (const auto& p:fit.sideSill[i])
            if (p[2]>front[i].z && p[2]<rear[i].z) doorStations.push_back(p[2]);
        if (panelFront>front[i].z && panelFront<rear[i].z) doorStations.push_back(panelFront);
        if (panelBack>front[i].z && panelBack<rear[i].z) doorStations.push_back(panelBack);
        std::sort(doorStations.begin(),doorStations.end());
        for (size_t s=1;s<doorStations.size();++s) {
            const float za=doorStations[s-1],zb=doorStations[s];
            const V a=sidePoint(i,za)-V{0,.022f,0},b=sidePoint(i,zb)-V{0,.022f,0};
            cabin.Quad({a.x,floor,a.z},{b.x,floor,b.z},b,a,kDash);
            cabin.Quad(a,b,outerSide(i,zb),outerSide(i,za),kDashTop);
            if (za>=panelFront && zb<=panelBack) {
                const V topA=inside(za,.105f),topB=inside(zb,.105f);
                cabin.Quad({topA.x,floor+.15f,topA.z},{topB.x,floor+.15f,topB.z},topB,topA,kFabric);
            }
        }
        if (panelBack>panelFront+.08f) {
            cabin.Beam(inside(panelFront+.06f,.135f),inside(std::min(panelBack,panelFront+.38f),.135f),.024f,kBlack,6);
            cabin.Beam(inside(panelFront+.025f,.075f)-V{sign*.023f,0,0},
                       inside(panelFront+.115f,.075f)-V{sign*.023f,0,0},.010f,kTrim,6);
        }
    }
    const V backLeft=rear[0]-V{0,.022f,0},backRight=rear[1]-V{0,.022f,0};
    cabin.Quad({backLeft.x,floor,backLeft.z},{backRight.x,floor,backRight.z},backRight,backLeft,kBlack);
    cabin.Quad(backLeft,backRight,backRight-V{0,0,.09f},backLeft-V{0,0,.09f},kDash);

    auto roofAt=[&](float z) {
        if (fit.openTop) return eyeY+.25f;
        const float t=std::clamp((z-fit.roofFrontZ)/std::max(.10f,fit.roofRearZ-fit.roofFrontZ),0.f,1.f);
        return fit.roofFrontY+(fit.roofRearY-fit.roofFrontY)*t;
    };
    // Leave adult seated eye-to-cushion room instead of raising the cushion
    // toward the wheel. The base remains above the physical cabin floor.
    const float seatY=std::max(floor+.08f,eyeY-.78f);
    const float seatZ=std::min(eyeZ-.08f,cabinBack-.40f);
    const float seatWidth=std::clamp(width-.17f,.36f,.56f);
    const float seatTop=std::min(eyeY-.40f,roofAt(seatZ+.31f)-.34f);
    // Keep the driver's seat attached to the adjusted seated origin; otherwise
    // moving the eye backward puts it behind the old headrest.
    const float driverSeatY=std::max(floor+.08f,seatY+seatOffset_[0]);
    Seat(cabin,eyeX,driverSeatY,seatZ+seatOffset_[1],seatWidth,seatTop+driverSeatY-seatY);
    Seat(cabin,-eyeX,seatY,seatZ+seatOffset_[1],seatWidth,seatTop);
    if (cabinBack-eyeZ>1.04f && fit.roofRearZ-eyeZ>.78f) {
        const float backZ=cabinBack-.31f;
        const float backWidth=std::max(.30f,std::min(-sidePoint(0,backZ).x,sidePoint(1,backZ).x)-.06f);
        const float backTop=std::min(eyeY-.36f,roofAt(backZ+.12f)-.34f);
        cabin.Box({-backWidth,seatY-.025f,backZ-.28f},{backWidth,seatY+.08f,backZ+.06f},kFabric);
        cabin.Box({-backWidth+.025f,seatY+.05f,backZ+.025f},{backWidth-.025f,backTop,backZ+.16f},kDash);
        for (float sign : {-1.f,1.f})
            cabin.Box({sign*backWidth*.50f-.10f,backTop,backZ+.02f},
                      {sign*backWidth*.50f+.10f,backTop+.16f,backZ+.13f},kDash);
    }

    wheelCenter_={eyeX,std::min(eyeY-.28f,dialY_-.105f),std::max(eyeZ-.38f,dashBack+.14f)};
    const auto& hub=interactiveWheel_ ? steeringHub_ : wheelCenter_;
    cabin.Beam({eyeX,dashboard-.14f,dashBack-.025f},
               {hub[0],hub[1],hub[2]},.035f,kBlack,8);
    Mesh wheel;
    constexpr int rimSides=48, tubeSides=6;
    constexpr float radius=.18f, tube=.015f;
    for (int i=0;i<rimSides;++i) for (int j=0;j<tubeSides;++j) {
        V points[4]{}; int at=0;
        for (auto ij : {std::pair{i,j},std::pair{i+1,j},std::pair{i+1,j+1},std::pair{i,j+1}}) {
            const float a=2*kPi*float(ij.first)/rimSides, b=2*kPi*float(ij.second)/tubeSides;
            points[at++]=Polar(a,radius+tube*std::cos(b))+V{0,0,tube*std::sin(b)};
        }
        const V color=i==11||i==12 ? kTrim : V{.095f,.108f,.121f};
        wheel.Quad(points[0],points[1],points[2],points[3],color);
    }
    for (float angle : {0.f,kPi,1.5f*kPi}) {
        const V d=Polar(angle,1), side{-d.y,d.x,0}, z{0,0,.007f};
        const V a=d*.035f-side*.025f,b=d*.167f-side*.014f,c=d*.167f+side*.014f,e=d*.035f+side*.025f;
        wheel.Quad(a+z,b+z,c+z,e+z,kTrim);
        wheel.Quad(e-z,c-z,b-z,a-z,kBlack);
        wheel.Quad(a-z,b-z,b+z,a+z,kDash); wheel.Quad(c-z,e-z,e+z,c+z,kDash);
    }
    wheel.Beam({0,0,-.015f},{0,0,.024f},.048f,kBlack,12);
    wheel.Disc({0,0,.025f},.027f,kDash,24);
    wheel.Ring({0,0,.026f},.025f,.027f,kTrim*.70f,24);

    if (cabin.vertices.size()>kCabinLimit || wheel.vertices.size()>kWheelLimit)
        throw std::runtime_error("procedural cockpit exceeds its vertex range");
    cabinCount_=uint32_t(cabin.vertices.size()); wheelCount_=uint32_t(wheel.vertices.size());
    mirrorCenter_=fit.mirror; mirrorScale_=0;
    renderer_.SetVertices(VkSceneRenderer::kCockpitVertexBase,cabin.vertices);
    renderer_.SetVertices(VkSceneRenderer::kCockpitVertexBase+kWheelOffset,wheel.vertices);
    fitKey_=FitKey(fit); built_=true;
    speedStep_=rpmStep_=-1; gear_=-100;
}

void ProceduralCockpit::BuildMirror(float scale) {
    // One physical surface samples the mono rear-view pass in both eyes.
    Mesh mirror;
    const V center{mirrorCenter_[0],mirrorCenter_[1],mirrorCenter_[2]};
    // Resize about the fitted centre so a smaller mirror cannot rise into the
    // headliner. Keep its roof mounting point fixed and reconnect the stalk.
    mirror.Beam(center+V{0,.035f*scale,0},center+V{0,.065f,-.035f},.009f*scale,kBlack,4);
    mirror.Box(center+V{-.125f,-.035f,-.018f}*scale,center+V{.125f,.035f,.008f}*scale,kBlack);
    const size_t faceFirst=mirror.vertices.size();
    mirror.Quad(center+V{-.117f,-.027f,.009f}*scale,center+V{.117f,-.027f,.009f}*scale,
                center+V{.117f,.027f,.009f}*scale,center+V{-.117f,.027f,.009f}*scale,{1,1,1},false);
    for (size_t i=faceFirst;i<mirror.vertices.size();++i) {
        auto& v=mirror.vertices[i];
        v.flags=kMirrorTexture;
        v.texel[0]=((v.pos[0]-center.x)/scale+.117f)/.234f;
        v.texel[1]=(.027f-(v.pos[1]-center.y)/scale)/.054f;
    }
    if (mirror.vertices.size()>kMirrorLimit) throw std::runtime_error("cockpit mirror exceeds its vertex range");
    mirrorCount_=uint32_t(mirror.vertices.size());
    renderer_.SetVertices(VkSceneRenderer::kCockpitVertexBase+kMirrorOffset,mirror.vertices);
    mirrorScale_=scale;
}

void ProceduralCockpit::UpdateInstruments(float speedKph, float rpm, int gear) {
    const int speedStep=int(std::lround(std::clamp(std::isfinite(speedKph)?speedKph:0.f,0.f,999.f)*10.f));
    const int rpmStep=int(std::lround(std::clamp(std::isfinite(rpm)?rpm:0.f,0.f,20000.f)/20.f));
    gear=std::clamp(gear,-1,9);
    if (speedStep==speedStep_ && rpmStep==rpmStep_ && gear==gear_) return;
    Mesh mesh;
    for (int i=0;i<2;++i) {
        const float amount=i==0?float(speedStep)/3000.f:float(rpmStep)*20.f/10000.f;
        const float angle=GaugeAngle(amount);
        const V center{dialX_[i],dialY_,dialZ_+.005f};
        const V d=Polar(angle,1), side{-d.y,d.x,0};
        mesh.Triangle(center-d*.012f-side*.0025f,center+d*.055f,center-d*.012f+side*.0025f,kAmber,false);
        mesh.Disc(center+V{0,0,.001f},.0055f,kBlack,16);
        mesh.Disc(center+V{0,0,.0015f},.0025f,kTrim,12);
    }
    const V display{(dialX_[0]+dialX_[1])*.5f,dialY_+.040f,dialZ_+.009f};
    mesh.Text(gear<0?"N":gear==0?"R":std::to_string(gear),display,.017f,kAmber);
    mesh.Text(std::to_string((speedStep+5)/10),{dialX_[0],dialY_-.042f,dialZ_+.004f},.010f,kIvory);
    if (mesh.vertices.size()>kInstrumentLimit) throw std::runtime_error("cockpit instruments exceed their vertex range");
    renderer_.SetVertices(VkSceneRenderer::kCockpitVertexBase+kInstrumentOffset,mesh.vertices);
    instrumentCount_=uint32_t(mesh.vertices.size());
    speedStep_=speedStep; rpmStep_=rpmStep; gear_=gear;
}

void ProceduralCockpit::Append(std::vector<DrawItem>& items, const CockpitFit& fit, const float* carMvp,
                               float steeringRadians, float speedKph, float rpm, int gear, bool showWheel,
                               float seatHeight, float seatBack, const std::array<float,3>* steeringHub) {
    if (!fit.valid) return;
    const std::array<float,2> seatOffset{seatHeight,seatBack};
    if (!built_ || FitKey(fit)!=fitKey_ || seatOffset!=seatOffset_ || bool(steeringHub)!=interactiveWheel_ ||
        (steeringHub && *steeringHub!=steeringHub_)) {
        seatOffset_=seatOffset;
        interactiveWheel_=steeringHub!=nullptr;
        if (steeringHub) steeringHub_=*steeringHub;
        Build(fit);
    }
    UpdateInstruments(speedKph,rpm,gear);
    auto append=[&](uint32_t offset,uint32_t count) {
        DrawItem item; item.firstVertex=VkSceneRenderer::kCockpitVertexBase+offset; item.vertexCount=count;
        std::memcpy(item.mvp,carMvp,sizeof(item.mvp)); items.push_back(item);
    };
    append(0,cabinCount_);
    append(kInstrumentOffset,instrumentCount_);
    if (!showWheel) return;
    const float angle=std::isfinite(steeringRadians)?steeringRadians:0.f;
    constexpr float tilt=18.f*kPi/180.f;
    const float c=std::cos(angle),s=std::sin(angle),ct=std::cos(tilt),st=std::sin(tilt);
    const float local[16]={c,s*ct,-s*st,0, -s,c*ct,-c*st,0, 0,st,ct,0,
                           wheelCenter_[0],wheelCenter_[1],wheelCenter_[2],1};
    DrawItem wheel; wheel.firstVertex=VkSceneRenderer::kCockpitVertexBase+kWheelOffset; wheel.vertexCount=wheelCount_;
    for (int col=0;col<4;++col) for (int row=0;row<4;++row) for (int k=0;k<4;++k)
        wheel.mvp[col*4+row]+=carMvp[k*4+row]*local[col*4+k];
    items.push_back(wheel);
}

void ProceduralCockpit::AppendMirror(std::vector<DrawItem>& items, const float* carMvp, float scale) {
    if (!built_) return;
    scale=std::clamp(scale,.25f,1.f);
    if (scale!=mirrorScale_) BuildMirror(scale);
    DrawItem item;
    item.firstVertex=VkSceneRenderer::kCockpitVertexBase+kMirrorOffset;
    item.vertexCount=mirrorCount_;
    std::memcpy(item.mvp,carMvp,sizeof(item.mvp));
    items.push_back(item);
}

} // namespace gt2view
