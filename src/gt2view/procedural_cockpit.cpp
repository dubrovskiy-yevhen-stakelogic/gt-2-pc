#include "gt2view/cockpit_backing_fit.h"
#include "gt2view/procedural_cockpit.h"
#include "gt2view/cockpit_materials.h"
#include "gt2view/cockpit_lining_clip.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

namespace gt2view {
namespace {
constexpr float kPi = 3.14159265358979323846f;
// The renderer already reserves 49152 vertices for the complete cockpit.
// Give the cabin the unused 8192 slots without growing the GPU allocation.
constexpr uint32_t kCabinLimit = SceneRenderer::kCockpitVertexLimit - 4096 - 4000 - 96;
constexpr uint32_t kWheelOffset = kCabinLimit, kWheelLimit = 4096;
constexpr uint32_t kInstrumentOffset = kWheelOffset + kWheelLimit, kInstrumentLimit = 4000;
constexpr uint32_t kMirrorOffset = kInstrumentOffset + kInstrumentLimit, kMirrorLimit = 96;
static_assert(kMirrorOffset + kMirrorLimit <= SceneRenderer::kCockpitVertexLimit);

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
            if (std::getenv("GT2_DEBUG_CABIN")) {
                v.color[0]=.1f;v.color[1]=.25f;v.color[2]=1.f;
            }
            vertices.push_back(v);
        }
    }
    void Quad(V a, V b, V c, V d, V color, bool shade = true) {
        Triangle(a,b,c,color,shade); Triangle(a,c,d,color,shade);
    }
    void ClipToWindows(size_t start,const CockpitFit& fit) {
        if(fit.nativeInterior || fit.liningApertures.empty())return;
        auto seatedEye=fit.eye;seatedEye[2]+=kCockpitDefaultSeatBack-(fit.nativeInterior?kNativeCockpitEyeForward:kGeneratedCockpitEyeForward);
        const std::array<std::array<float,3>,1> eyes{seatedEye};
        auto clipped=ClipCockpitLining(std::span<const SceneVertex>(vertices).subspan(start),fit.liningApertures,eyes);
        vertices.resize(start);vertices.insert(vertices.end(),clipped.begin(),clipped.end());
    }
    void ClipToCowl(size_t start,const CockpitFit& fit,bool boundWidth=true) {
        const std::vector<SceneVertex> original(vertices.begin()+start,vertices.end());
        vertices.resize(start);
        auto reverseCowl=fit.cowl;for(auto& p:reverseCowl)p[2]=-p[2];
        if(!boundWidth) {
            float lo=reverseCowl.front()[0],hi=reverseCowl.back()[0];
            for(const auto& v:original){lo=std::min(lo,v.pos[0]);hi=std::max(hi,v.pos[0]);}
            // Match the outer clipping planes' linear continuation. Roof and
            // side trim outside the cowl width can still be wholly behind it.
            auto extend=[](auto& a,const auto& b,float x){
                a[2]+=(x-a[0])*(b[2]-a[2])/std::max(.001f,std::abs(b[0]-a[0]))*(b[0]>=a[0]?1.f:-1.f);a[0]=x;
            };
            extend(reverseCowl.front(),reverseCowl[1],lo);
            extend(reverseCowl.back(),reverseCowl[reverseCowl.size()-2],hi);
        }
        std::vector<SceneVertex> crossing;
        for(size_t i=0;i+2<original.size();i+=3) {
            std::array<std::array<float,3>,3> reversed{};bool within=true;
            for(size_t j=0;j<3;++j) {
                const auto* p=original[i+j].pos;reversed[j]={p[0],p[1],-p[2]};
                within=within && (!boundWidth || (p[0]>=fit.cowl.front()[0] && p[0]<=fit.cowl.back()[0]));
            }
            // Preserve intact triangles, including long roof/door surfaces.
            // Partitioning every one into 16 strips wastes the cabin budget.
            auto& target=within && cockpit_detail::TriangleAheadOfCowl(reversed,reverseCowl,-.010f)?vertices:crossing;
            target.insert(target.end(),original.begin()+i,original.begin()+i+3);
        }
        for(size_t strip=1;strip<fit.cowl.size();++strip) {
            const auto& left=fit.cowl[strip-1];const auto& right=fit.cowl[strip];
            const float slope=(right[2]-left[2])/std::max(.001f,right[0]-left[0]);
            const std::array<std::array<float,4>,3> planes{{{1,0,0,right[0]}, {-1,0,0,-left[0]},
                {slope,0,-1,slope*left[0]-left[2]-.010f}}};
            for(size_t i=0;i<crossing.size();i+=3) {
                std::vector<SceneVertex> polygon(crossing.begin()+i,crossing.begin()+i+3);
                for(size_t planeIndex=0;planeIndex<planes.size();++planeIndex) {
                    if(!boundWidth && ((planeIndex==0 && strip+1==fit.cowl.size()) || (planeIndex==1 && strip==1)))continue;
                    const auto& plane=planes[planeIndex];
                    std::vector<SceneVertex> clipped;
                    for(size_t k=0;k<polygon.size();++k) {
                        const auto& a=polygon[k];const auto& b=polygon[(k+1)%polygon.size()];
                        auto distance=[&](const auto& v){return plane[0]*v.pos[0]+plane[1]*v.pos[1]+plane[2]*v.pos[2]-plane[3];};
                        const float da=distance(a),db=distance(b);
                        if(da<=0)clipped.push_back(a);
                        if((da<=0)!=(db<=0)) {
                            const float t=da/(da-db);auto v=a;
                            for(size_t c=0;c<3;++c){v.pos[c]+=t*(b.pos[c]-a.pos[c]);v.color[c]+=t*(b.color[c]-a.color[c]);}
                            clipped.push_back(v);
                        }
                    }
                    polygon=std::move(clipped);
                }
                for(size_t k=1;k+1<polygon.size();++k) {
                    const auto* a=polygon[0].pos;const auto* b=polygon[k].pos;const auto* c=polygon[k+1].pos;
                    const V cross=Cross({b[0]-a[0],b[1]-a[1],b[2]-a[2]},{c[0]-a[0],c[1]-a[1],c[2]-a[2]});
                    if(Dot(cross,cross)<1e-16f)continue;
                    for(size_t j:{size_t(0),k,k+1})vertices.push_back(polygon[j]);
                }
            }
        }
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
    mesh.Box({x-width*.5f,y-.075f,z-.30f},{x+width*.5f,y+.025f,z+.24f},kBlack);
    mesh.Box({x-width*.36f,y+.025f,z-.275f},{x+width*.36f,y+.075f,z+.19f},kFabric);
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

ProceduralCockpit::ProceduralCockpit(SceneRenderer& renderer) : renderer_(renderer) {}

void ProceduralCockpit::Build(const CockpitFit& fit) {
    if (std::getenv("GT2_DEBUG_CABIN")) std::printf("Cabin shell: smooth %d open %d front %.3f %.3f %.3f rear %.3f %.3f %.3f\n",
        int(fit.smoothShell),int(fit.openTop),fit.frontHeader[0][0],fit.frontHeader[0][1],fit.frontHeader[0][2],
        fit.rearHeader[0][0],fit.rearHeader[0][1],fit.rearHeader[0][2]);
    Mesh cabin;
    cabin.vertices.reserve(18000);
    const float eyeX=fit.eye[0], eyeY=fit.eye[1], eyeZ=fit.eye[2];
    // Seating adjustments cannot move the cabin floor through the body.
    // Low sports bodies require shorter furniture, not a deeper exterior hull.
    const float floor=fit.floorY+.005f;
    const float dashFront=fit.windshieldZ+.020f;
    const float cowlY=fit.windshieldY-.025f;
    const float fasciaTop=std::min(cowlY-.035f,eyeY-.255f);
    const float dashboard=fasciaTop+.045f;
    // Closed backing below the window perimeter. Dashboard/door details sit
    // inside it; every wall shares its bottom edge with the floor, so curved
    // cowl corners cannot expose the road through a gap between trim pieces.
    const std::array backingSides{FinalCockpitBackingSide(fit,0),FinalCockpitBackingSide(fit,1)};
    std::vector<V> perimeter;
    for (const auto& p:fit.cowl) perimeter.push_back({p[0],p[1],p[2]});
    if(!backingSides[1].empty()) {
        for(size_t i=0;i<backingSides[1].size();++i) {
            const auto& q=backingSides[1][i];perimeter.push_back({q[0],q[1],q[2]});
        }
    } else for (size_t i=1;i<17;++i) {
        const float z=fit.frontCowl[1][2]+(fit.rearCowl[1][2]-fit.frontCowl[1][2])*float(i)/16.f;
        const auto p=CockpitSideSillAt(fit,1,z); perimeter.push_back({p[0],p[1],p[2]});
    }
    // The closed backing must use the same authored lower rear-glass curve
    // as the visible trim; a straight chord between high corners blocks it.
    if(fit.rearCowlProfileValid)for(size_t i=fit.rearCowlProfile.size()-1;i>0;--i){
        const auto& p=fit.rearCowlProfile[i-1];perimeter.push_back({p[0],p[1],p[2]});
    } else perimeter.push_back({fit.rearCowl[0][0],fit.rearCowl[0][1],fit.rearCowl[0][2]});
    if(!backingSides[0].empty()) {
        for(size_t i=backingSides[0].size();i>0;--i) {
            const auto& q=backingSides[0][i-1];perimeter.push_back({q[0],q[1],q[2]});
        }
    } else for (int i=15;i>=0;--i) {
        const float z=fit.frontCowl[0][2]+(fit.rearCowl[0][2]-fit.frontCowl[0][2])*float(i)/16.f;
        const auto p=CockpitSideSillAt(fit,0,z); perimeter.push_back({p[0],p[1],p[2]});
    }
    std::vector<std::array<float,3>> backingOutline;for(const auto& q:perimeter)backingOutline.push_back({q.x,q.y,q.z});
    backingOutline=FitBackingGlazingFootprint(fit,backingOutline);
    perimeter.clear();for(const auto& q:backingOutline)perimeter.push_back({q[0],q[1],q[2]});
    const V floorCenter{0,floor-.005f,(fit.windshieldZ+fit.rearWindowZ)*.5f};
    for (size_t i=0;i<perimeter.size();++i) {
        const V a=perimeter[i],b=perimeter[(i+1)%perimeter.size()];
        const V lowA{a.x,floorCenter.y,a.z},lowB{b.x,floorCenter.y,b.z};
        cabin.Quad(lowA,lowB,b,a,kDash);
        cabin.Triangle(floorCenter,lowB,lowA,kBlack);
    }
    // The enclosure remains watertight; trim clipping must not perforate the backing.
    backingCount_=uint32_t(cabin.vertices.size());
    // Closed, rounded wheel housings. Tyres remain visible outside the car,
    // while their inboard halves are enclosed instead of intersecting the cabin.
    wheelWellFirst_=uint32_t(cabin.vertices.size());
    for (size_t wheel=0;wheel<fit.wheelWells.size();++wheel) {
        const size_t liningFirst=cabin.vertices.size();
        const auto& well=fit.wheelWells[wheel];
        if (well[3]<.1f || well[4]<=0) continue;
        const float sign=well[0]<0?-1.f:1.f;
        // Allow suspension travel and the race half-track's small difference
        // from the showroom wheel centres. Close the lower half too: a low
        // seat can otherwise see through underneath the wheel's centre line.
        // Bound the rotating tyre cylinder, including steering and camber.
        // Half-width alone misses the tread sweeping inward during a turn.
        // Rear axles do not steer. Giving them the front axle's full steering
        // envelope can put the seat inside the housing on narrow open cars.
        const float swept=wheel<2?std::hypot(well[3],well[4]*.5f):well[4]*.5f;
        // Replacement tyres are tucked under the outer wing when steered.
        // Their inner edge moves by twice the growth in projected half-width.
        const float inner=well[0]-sign*(2.f*swept-well[4]*.5f+.04f);
        const float outer=well[0]+sign*(well[4]*.5f+.025f);
        const float radius=well[3]+.11f;
        constexpr int segments=24;
        for (int i=0;i<segments;++i) {
            const float a=2*kPi*float(i)/segments,b=2*kPi*float(i+1)/segments;
            const V p{inner,well[1]+radius*std::sin(a),well[2]+radius*std::cos(a)};
            const V q{inner,well[1]+radius*std::sin(b),well[2]+radius*std::cos(b)};
            cabin.Triangle({inner,well[1],well[2]},q,p,kDash);
            cabin.Quad(p,q,{outer,q.y,q.z},{outer,p.y,p.z},kDash);
        }
        // Suspension envelopes can be taller than the bonnet on low/short
        // bodies. Keep the closed liner below the window sill instead of
        // extruding a tyre-shaped hump into the driver's forward view.
        const float ceiling=(wheel<2?std::min({fit.windshieldY,fit.frontCowl[0][1],fit.frontCowl[1][1]}):
            std::min({fit.rearWindowY,fit.rearCowl[0][1],fit.rearCowl[1][1]}))-.04f;
        for(size_t v=liningFirst;v<cabin.vertices.size();++v)
            cabin.vertices[v].pos[1]=std::min(cabin.vertices[v].pos[1],ceiling);
    }
    // The generated firewall closes the front of the seating volume. Do not
    // extend its lining over the visible bonnet of low sports prototypes.
    cabin.ClipToCowl(wheelWellFirst_,fit);
    // Wheel housings are closed safety geometry, not window trim.
    wheelWellCount_=uint32_t(cabin.vertices.size())-wheelWellFirst_;
    if(std::getenv("GT2_AUDIT_COCKPIT"))auditWheelLiners_.assign(cabin.vertices.begin()+wheelWellFirst_,cabin.vertices.end());
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
    // Fit the instrument cluster into the actual swept cowl footprint at
    // the existing working distance; do not push the whole cluster past the driver.
    // Recover width using available depth as well as scale. Keep the steering
    // hub (panel + 0.29 m) at least 0.10 m in front of the eye plane.
    const float panelZ=eyeZ-.39f, boundaryZ=panelZ-.08f;
    float spanLeft=eyeX,spanRight=eyeX;
    for(size_t i=1;i<fit.cowl.size();++i){
        const auto& a=fit.cowl[i-1];const auto& b=fit.cowl[i];
        float lo=a[0],hi=b[0];
        if(a[2]>boundaryZ && b[2]>boundaryZ)continue;
        if((a[2]>boundaryZ)!=(b[2]>boundaryZ)){
            const float x=a[0]+(b[0]-a[0])*(boundaryZ-a[2])/(b[2]-a[2]);
            if(a[2]>boundaryZ)lo=x;else hi=x;
        }
        spanLeft=std::min(spanLeft,lo);spanRight=std::max(spanRight,hi);
    }
    const float originalDashBack=std::max({dashFront+.12f,eyeZ-.52f,
        cowlAt(eyeX-.196f).z+.06f,cowlAt(eyeX+.196f).z+.06f});
    // Preserve a working full-size layout. Only recover a swept cowl that
    // would place the steering hub at or behind the driver's eye plane.
    const bool recoverControls=originalDashBack+.29f>=eyeZ;
    float controlX=eyeX;
    if(recoverControls) {
        // Keep at least half-size round gauges where the cowl permits it,
        // using at most 10 cm lateral offset instead of shrinking to pinheads.
        const float left=std::max(spanLeft+.098f,eyeX-.10f);
        const float right=std::min(spanRight-.098f,eyeX+.10f);
        if(left<=right)controlX=std::clamp(eyeX,left,right);
    }
    instrumentScale_=recoverControls?std::clamp(std::min(controlX-spanLeft,spanRight-controlX)/.196f,.25f,1.f):1.f;
    const float podLeft=controlX-.181f*instrumentScale_,podRight=controlX+.181f*instrumentScale_;
    const auto scaleInstruments=[&](auto& vertices,size_t first,float centreY){
        for(size_t i=first;i<vertices.size();++i){auto& p=vertices[i].pos;
            p[0]=controlX+(p[0]-controlX)*instrumentScale_;p[1]=centreY+(p[1]-centreY)*instrumentScale_;}
    };
    const float dashBack=std::max({dashFront+.12f,eyeZ-.52f,
                                  cowlAt(controlX-.196f*instrumentScale_).z+.06f,cowlAt(controlX+.196f*instrumentScale_).z+.06f});
    const float cabinBack=std::min(rear[0].z,rear[1].z);
    const float width=std::max(.32f,std::min(-sidePoint(0,dashBack).x,sidePoint(1,dashBack).x));
    const size_t cowlFirst=cabin.vertices.size();
    auto closedBand=[&](V a,V b,V c,V d,float thickness){
        const V down{0,thickness,0};
        cabin.Quad(a,b,c,d,kDash);
        cabin.Quad(d-down,c-down,b-down,a-down,kDash);
        cabin.Quad(a-down,b-down,b,a,kDash);
        cabin.Quad(b-down,c-down,c,b,kDash);
        cabin.Quad(c-down,d-down,d,c,kDash);
        cabin.Quad(d-down,a-down,a,d,kDash);
    };
    // Join the inset dashboard and door cards to the authored body boundary.
    for (size_t i=1;i<fit.cowl.size();++i) {
        const auto& a=fit.cowl[i-1]; const auto& b=fit.cowl[i];
        cabin.Quad({a[0],a[1],a[2]},{b[0],b[1],b[2]},cowlAt(b[0]),cowlAt(a[0]),kDashTop);
        // Rubber cowl seal overlaps both meshes. Merely sharing a sampled
        // edge leaves sub-texel openings on curved/quantised source panels.
        // Flat overlap stays underneath the authored edge. A 3 cm round beam
        // here introduced a scalloped ridge on otherwise smooth bonnets.
        // Close the underside too: a floating top quad hides the seam from
        // above but leaves a slit when the player lowers their seat.
        closedBand({a[0],a[1]+.006f,a[2]-.025f},{b[0],b[1]+.006f,b[2]-.025f},
                   {b[0],b[1]+.006f,b[2]+.025f},{a[0],a[1]+.006f,a[2]+.025f},.031f);
        if(fit.bonnetSealValid)for(size_t row=1;row<fit.bonnetSeal.size();++row) {
            const auto& p=fit.bonnetSeal[row-1][i-1];const auto& q=fit.bonnetSeal[row-1][i];
            const auto& r=fit.bonnetSeal[row][i];const auto& s=fit.bonnetSeal[row][i-1];
            closedBand({p[0],p[1],p[2]},{q[0],q[1],q[2]},
                       {r[0],r[1],r[2]},{s[0],s[1],s[2]},.025f);
        }
    }
    cabin.ClipToWindows(cowlFirst,fit);
    if(std::getenv("GT2_AUDIT_COCKPIT"))auditCowl_.assign(cabin.vertices.begin()+cowlFirst,cabin.vertices.end());
    const size_t deckFirst=cabin.vertices.size();
    const V floorFront{0,floor,dashFront},floorRear{0,floor,cabinBack};
    const V floorLeft{front[0].x,floor,front[0].z},floorRight{front[1].x,floor,front[1].z};
    const V rearLeft{rear[0].x,floor,rear[0].z},rearRight{rear[1].x,floor,rear[1].z};
    cabin.Quad(floorLeft,floorFront,floorRear,rearLeft,kBlack);
    cabin.Quad(floorFront,floorRight,rearRight,floorRear,kBlack);
    const std::array<float,5> deckX{-width,podLeft-.015f,0.f,podRight+.015f,width};
    // The cowl follows the authored windscreen, while the fascia is a usable
    // interior surface. A deeply swept screen must not produce two long wings
    // beside a recessed central panel. Bring that central surface back enough
    // to limit the recess, keeping the cowl, seat and wheel anchors unchanged.
    float fasciaBase=dashBack;
    if(!(std::getenv("GT2_AUDIT_COCKPIT") && std::getenv("GT2_AUDIT_LEGACY_COWL"))){
        float backmost=std::max(cowlAt(-width).z,cowlAt(width).z)+.06f;
        for(const auto& p:fit.cowl)if(p[0]>-width && p[0]<width)backmost=std::max(backmost,cowlAt(p[0]).z+.06f);
        fasciaBase=std::max(fasciaBase,(recoverControls?std::min(backmost-.18f,eyeZ-.52f):backmost-.18f));
    }
    // The full 154 mm instrument face must clear the physical floor.
    const float originalDialY=std::max(floor+.090f,std::min(cowlY-.085f,fasciaTop-.045f));
    float fittedDialY=originalDialY;
    if(!fit.nativeInterior) {
        const V view{eyeX,eyeY,eyeZ+.05f};
        auto linerBlocks=[&](float base,float y) {
            for(float eyeSide:{-.032f,.032f})for(float dialSide:{-.086f,.086f})
            for(int iy=-4;iy<=4;++iy)for(int ix=-4;ix<=4;++ix) {
                if(ix*ix+iy*iy>16)continue;
                const float x=controlX+(dialSide+float(ix)*.018f)*instrumentScale_;
                const V origin=view+V{eyeSide,0,0};
                const V direction=V{x,y+float(iy)*.018f*instrumentScale_,std::max(base,cowlAt(x).z+.06f)+.018f}-origin;
                for(size_t first=wheelWellFirst_;first+2<wheelWellFirst_+wheelWellCount_;first+=3){
                    const auto point=[&](size_t i){const auto& p=cabin.vertices[first+i].pos;return V{p[0],p[1],p[2]};};
                    const V a=point(0),e1=point(1)-a,e2=point(2)-a;
                    const V p=Cross(direction,e2);const float det=Dot(e1,p);if(std::abs(det)<1e-9f)continue;
                    const V t=origin-a;const float u=Dot(t,p)/det;if(u<0 || u>1)continue;
                    const V q=Cross(t,e1);const float v=Dot(direction,q)/det;if(v<0 || u+v>1)continue;
                    const float distance=Dot(e2,q)/det;if(distance>.001f && distance<1.01f)return true;
                }
            }
            return false;
        };
        if(linerBlocks(fasciaBase,originalDialY)) {
            // Preserve the panel's viewing elevation when bringing it out of
            // a tyre enclosure. Otherwise a nearer panel drops behind the rim.
            // Neither the wheel anchor nor the camera is moved by this fit.
            const float originalBase=fasciaBase;
            const float distance=std::max(.10f,view.z-(originalBase+.018f));
            for(int step=1;step<=30;++step){
                const float base=originalBase+float(step)*.01f;
                if(base>view.z-.20f)break;
                const float y=std::min(cowlY-.085f,originalDialY+(base-originalBase)*(view.y-originalDialY)/distance);
                if(!linerBlocks(base,y)){fasciaBase=base;fittedDialY=y;break;}
            }
        }
    }
    auto fasciaAt=[&](float x) {
        const float z=std::max(fasciaBase,cowlAt(x).z+.06f);
        // Keep the instrument pod and centre controls at their established
        // height. Outside that span, meet the measured door belt instead of
        // extending a flat, windscreen-height ledge into low side windows.
        const float flatLeft=std::min(0.f,podLeft-.015f),flatRight=std::max(0.f,podRight+.015f);
        const size_t side=x<flatLeft?0:1;
        const float amount=x<flatLeft?(flatLeft-x)/std::max(.001f,width+flatLeft):
                           (x>flatRight?(x-flatRight)/std::max(.001f,width-flatRight):0.f);
        const float belt=sidePoint(side,z).y-.022f;
        const float y=fasciaTop+std::min(0.f,belt-fasciaTop)*std::clamp(amount,0.f,1.f);
        return V{x,y,z};
    };
    // Sort because the binnacle may straddle the vehicle centre line.
    std::vector<float> stations(deckX.begin(),deckX.end());
    for (const auto& point : fit.cowl) if (point[0]>-width && point[0]<width) stations.push_back(point[0]);
    std::sort(stations.begin(),stations.end());
    stations.erase(std::unique(stations.begin(),stations.end()),stations.end());
    auditFascia_.clear();
    for (size_t i=1;i<stations.size();++i) {
        const float a=stations[i-1],b=stations[i];
        const V backA=fasciaAt(a),backB=fasciaAt(b);
        auto deckAt=[&](float x) {
            V point=cowlAt(x);
            // The front edge of the decorative deck shares the measured
            // side-window belt constraint. Tapering only its rear edge leaves
            // a diagonal wedge at the swept outer windscreen corner.
            const float flatLeft=std::min(0.f,podLeft-.015f),flatRight=std::max(0.f,podRight+.015f);
            const size_t side=x<flatLeft?0:1;
            const float amount=x<flatLeft?(flatLeft-x)/std::max(.001f,width+flatLeft):
                               (x>flatRight?(x-flatRight)/std::max(.001f,width-flatRight):0.f);
            point.y+=std::min(0.f,sidePoint(side,point.z).y-.022f-point.y)*std::clamp(amount,0.f,1.f);
            return point;
        };
        const V topA=deckAt(a),topB=deckAt(b);
        cabin.Quad(topA,topB,backB,backA,kDashTop);
        cabin.Quad({a,floor,topA.z},{b,floor,topB.z},topB,topA,kBlack);
        cabin.Triangle({a,floor,topA.z},{b,floor,topB.z},floorRear,kBlack);
        const size_t fasciaFirst=cabin.vertices.size();
        cabin.Quad(backA-V{0,.195f,0},backB-V{0,.195f,0},backB,backA,kDash);
        if(std::getenv("GT2_AUDIT_COCKPIT"))auditFascia_.insert(auditFascia_.end(),cabin.vertices.begin()+fasciaFirst,cabin.vertices.end());
        cabin.Quad({a,floor,topA.z},{b,floor,topB.z},backB-V{0,.195f,0},backA-V{0,.195f,0},kBlack);
    }
    for (size_t i=0;i<2;++i) {
        const float x=i==0?-width:width;
        const auto& outer=fit.frontCowl[i];
        const V cowl=cowlAt(outer[0]);
        cabin.Quad({cowl.x,floor,cowl.z},{front[i].x,floor,front[i].z},front[i],cowl,kDash);
        cabin.Quad({0,floor,cowl.z},{cowl.x,floor,cowl.z},
                   {front[i].x,floor,front[i].z},{0,floor,front[i].z},kBlack);
        cabin.Quad({front[i].x,floor,front[i].z},front[i],
                   fasciaAt(x),{x,floor,fasciaAt(x).z},kDash);
        const V edge=fasciaAt(x);
        V door=sidePoint(i,edge.z); door.y-=.022f; door.z=edge.z;
        cabin.Quad({edge.x,floor,edge.z},{door.x,floor,door.z},door,edge,kDash);
    }
    cabin.ClipToWindows(deckFirst,fit);
    const size_t dashboardDetails=cabin.vertices.size();
    // Every vent shares the same instrument exclusion, including the centre
    // pair on narrow dashboards and the opposite side in right-hand cabins.
    const auto safeVent=[&](float x,float y,float z,float w,float h){
        const float left=std::max(-width+.02f,x-w*.5f),right=std::min(width-.02f,x+w*.5f);
        for(const auto span:{std::array<float,2>{left,std::min(right,podLeft-.012f)},
                             std::array<float,2>{std::max(left,podRight+.012f),right}})
            if(span[1]-span[0]>=.04f)Vent(cabin,(span[0]+span[1])*.5f,y,z,span[1]-span[0],h);
    };

    for (const auto& span : {std::array<float,2>{-width+.04f,controlX-.20f},
                             std::array<float,2>{controlX+.20f,width-.04f}})
        if (span[1]>span[0]) cabin.Box({span[0],dashboard-.078f,dashBack+.001f},
                                     {span[1],dashboard-.065f,dashBack+.008f},kTrim*.72f);

    Mesh binnacle;
    // The instrument face sits behind a shallow, open-bottom binnacle.
    dialX_[0]=controlX-.086f; dialX_[1]=controlX+.086f;
    dialZ_=dashBack+.014f;
    // Recess the instruments into the fascia. The hood/cowl sets the upper
    // edge; moving the driver's eye must never raise a pod into the windscreen.
    dialY_=fittedDialY;
    const float podTop=dialY_+.080f;
    const float podFront=std::max(dashFront+.025f,dashBack-.18f);
    const float podBase=std::min(podTop-.04f,fasciaTop+(cowlY-fasciaTop)*
        std::clamp((dashBack-podFront)/std::max(.10f,dashBack-dashFront),0.f,1.f)-.006f);
    binnacle.Box({podLeft, dashboard-.087f,dashBack-.035f},
              {podRight,podTop-.017f,dashBack+.004f},kBlack);
    binnacle.Quad({podLeft-.012f,dashboard-.06f,dashBack+.02f},
               {podLeft+.026f,podTop,dashBack+.02f},
               {podLeft+.026f,podTop-.025f,podFront},
               {podLeft-.012f,podBase,podFront},kDashTop);
    binnacle.Quad({podRight-.026f,podTop,dashBack+.02f},
               {podRight+.012f,dashboard-.06f,dashBack+.02f},
               {podRight+.012f,podBase,podFront},
               {podRight-.026f,podTop-.025f,podFront},kDashTop);
    binnacle.Quad({podLeft+.026f,podTop,dashBack+.025f},{podRight-.026f,podTop,dashBack+.025f},
               {podRight-.026f,podTop-.025f,podFront},{podLeft+.026f,podTop-.025f,podFront},kDashTop);
    binnacle.Quad({podLeft+.026f,podBase,podFront},{podLeft+.026f,podTop-.025f,podFront},
               {podRight-.026f,podTop-.025f,podFront},{podRight-.026f,podBase,podFront},kDashTop);
    const size_t instrumentFacesFirst=cabin.vertices.size();
    Gauge(cabin,{dialX_[0],dialY_,dialZ_},false);
    Gauge(cabin,{dialX_[1],dialY_,dialZ_},true);
    scaleInstruments(cabin.vertices,instrumentFacesFirst,dialY_);
    if(std::getenv("GT2_AUDIT_COCKPIT")) {
        Mesh faces;faces.vertices.assign(cabin.vertices.begin()+instrumentFacesFirst,cabin.vertices.end());
        faces.BendAlongX(0,stations,[&](float x){return fasciaAt(x).z-dashBack;});
        auditInstrumentFaces_=std::move(faces.vertices);
    }
    const size_t displayFirst=cabin.vertices.size();
    cabin.Box({controlX-.016f,dialY_+.028f,dialZ_+.003f},
              {controlX+.016f,dialY_+.052f,dialZ_+.006f},kTrim);
    cabin.Box({controlX-.013f,dialY_+.030f,dialZ_+.0065f},
              {controlX+.013f,dialY_+.050f,dialZ_+.007f},kBlack);

    scaleInstruments(cabin.vertices,displayFirst,dialY_);
    if(fit.smoothShell) {
        // Narrow imported dashboards have no room for a full-width outboard
        // vent beside the driver's instrument pod. Fit the available span
        // instead of drawing the slats through the speedometer.
        const float room=controlX-.181f-(-width+.04f);
        if(room>.06f) {
            const float ventWidth=std::min(.13f,room-.02f);
            safeVent(-width+.04f+ventWidth*.5f,dashboard-.126f,dashBack+.011f,ventWidth,.052f);
        }
    } else safeVent(-width+.125f,dashboard-.126f,dashBack+.011f,.13f,.052f);
    safeVent(width-.14f,dashboard-.126f,dashBack+.011f,.16f,.052f);
    safeVent(.02f,dashboard-.116f,dashBack+.016f,.102f,.058f);
    safeVent(.135f,dashboard-.116f,dashBack+.016f,.102f,.058f);
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
    // The attachment mesh is already split at the same fascia stations.
    // Carry vents and glovebox with the tapered face; the pod is unchanged.
    for(size_t i=dashboardDetails;i<cabin.vertices.size();++i) {
        auto& point=cabin.vertices[i].pos;point[1]+=fasciaAt(point[0]).y-fasciaTop;
    }
    // Fit only the projecting hood. Gauges, controls and their positions stay
    // unchanged; clipping thousands of tiny glyph triangles is unnecessary.
    binnacle.BendAlongX(0,stations,[&](float x) { return fasciaAt(x).z-dashBack; });
    binnacle.ClipToCowl(0,fit,false);
    cabin.vertices.insert(cabin.vertices.end(),binnacle.vertices.begin(),binnacle.vertices.end());

    // Dashboard attachments share the authored opening boundary with the deck.
    cabin.ClipToCowl(dashboardDetails,fit,false);
    cabin.ClipToWindows(dashboardDetails,fit);

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

    const size_t doorTrimFirst=cabin.vertices.size();
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
        // Dense measured corners belong to the backing; clip bounded decoration below.
        std::sort(doorStations.begin(),doorStations.end());
        doorStations.erase(std::unique(doorStations.begin(),doorStations.end()),doorStations.end());
        for (size_t s=1;s<doorStations.size();++s) {
            const float za=doorStations[s-1],zb=doorStations[s];
            const V a=sidePoint(i,za)-V{0,.022f,0},b=sidePoint(i,zb)-V{0,.022f,0};
            cabin.Quad({a.x,floor,a.z},{b.x,floor,b.z},b,a,kDash);
            // Follow the same segmented boundary as the door, including its inset.
            // A single front/rear trapezoid leaves wedges below curved door cards.
            cabin.Quad({0,floor,a.z},{a.x,floor,a.z},{b.x,floor,b.z},{0,floor,b.z},kBlack);
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
    cabin.ClipToCowl(doorTrimFirst,fit,false);
    cabin.ClipToWindows(doorTrimFirst,fit);
    const size_t rearTrimFirst=cabin.vertices.size();
    const V backLeft=rear[0]-V{0,.022f,0},backRight=rear[1]-V{0,.022f,0};
    if(fit.rearCowlProfileValid) {
        auto boundary=[&](float x){
            for(size_t i=1;i<fit.rearCowlProfile.size();++i){
                const auto& a=fit.rearCowlProfile[i-1];const auto& b=fit.rearCowlProfile[i];
                if(x>b[0] && i+1<fit.rearCowlProfile.size())continue;
                const float t=std::clamp((x-a[0])/std::max(.0001f,b[0]-a[0]),0.f,1.f);
                return V{x,a[1]+t*(b[1]-a[1])-.057f,a[2]+t*(b[2]-a[2])-.035f};
            }
            return backRight;
        };
        std::vector<float> rearStations{backLeft.x,backRight.x};
        for(const auto& point:fit.rearCowlProfile)if(point[0]>backLeft.x && point[0]<backRight.x)rearStations.push_back(point[0]);
        std::sort(rearStations.begin(),rearStations.end());
        for(size_t i=1;i<rearStations.size();++i){const V a=boundary(rearStations[i-1]),b=boundary(rearStations[i]);
            cabin.Quad({a.x,floor,a.z},{b.x,floor,b.z},b,a,kBlack);
            cabin.Quad(a,b,b-V{0,0,.09f},a-V{0,0,.09f},kDash);
        }
    } else {
        cabin.Quad({backLeft.x,floor,backLeft.z},{backRight.x,floor,backRight.z},backRight,backLeft,kBlack);
        cabin.Quad(backLeft,backRight,backRight-V{0,0,.09f},backLeft-V{0,0,.09f},kDash);
    }

    cabin.ClipToWindows(rearTrimFirst,fit);
    const size_t windowTrimFirst=cabin.vertices.size();
    // Rounded inner window reveals hide the paper-thin, faceted edge of the
    // exterior shell. Their centres follow the measured aperture, not a box.
    if (!fit.openTop && !fit.authoredUpperShell) for (size_t side=0;side<2;++side) {
        const float sign=side==0?-1.f:1.f;
        std::array<V,4> border;
        const std::array<std::array<float,3>,4> source{
            fit.frontCowl[side],fit.frontHeader[side],fit.rearHeader[side],fit.rearCowl[side]};
        // Inset by at least the reveal radius in every outward direction.
        // Otherwise its outer half protrudes through paint when the head leans
        // above the bonnet, although the driving view hides the intersection.
        for (size_t j=0;j<4;++j) border[j]={source[j][0]-sign*.030f,source[j][1]-.025f,source[j][2]+(j<2?.025f:-.025f)};
        // Round each corner within 6 cm without changing the aperture's size.
        for (size_t j=0;j<4;++j) {
            const V corner=border[j],before=border[(j+3)%4],after=border[(j+1)%4];
            const V a=corner+Unit(before-corner)*.06f,b=corner+Unit(after-corner)*.06f;
            V previous=a;
            for (int k=1;k<=6;++k) {
                const float t=float(k)/6.f;
                const V next=a*((1-t)*(1-t))+corner*(2*t*(1-t))+b*(t*t);
                cabin.Beam(previous,next,.022f,kDash,8);previous=next;
            }
            cabin.Beam(b,after+Unit(corner-after)*.06f,.022f,kDash,8);
        }
    }
    if (fit.smoothShell && !fit.openTop) {
        auto point=[](const std::array<float,3>& p) { return V{p[0],p[1],p[2]}; };
        auto roofPoint=[&](float across,float along) {
            if(fit.headlinerValid)return point(fit.headliner[size_t(std::lround(along*8))][size_t(std::lround(across*12))]);
            const V front=point(fit.frontHeader[0])*(1-across)+point(fit.frontHeader[1])*across;
            const V rear=point(fit.rearHeader[0])*(1-across)+point(fit.rearHeader[1])*across;
            return front*(1-along)+rear*along+V{0,.035f*std::sin(kPi*across)*std::sin(kPi*along),0};
        };
        if(!fit.headlinerSurface.empty()) {
            for(const auto& t:fit.headlinerSurface)cabin.Triangle(point(t[0]),point(t[1]),point(t[2]),kDash);
        } else for (int z=0;z<8;++z) for (int x=0;x<12;++x)
            cabin.Quad(roofPoint(float(x)/12,float(z)/8),roofPoint(float(x+1)/12,float(z)/8),
                       roofPoint(float(x+1)/12,float(z+1)/8),roofPoint(float(x)/12,float(z+1)/8),kDash);
        if(!fit.authoredUpperShell)for (const auto* pair:{&fit.frontHeader,&fit.rearHeader,&fit.rearCowl}) {
            const V a=point((*pair)[0]),b=point((*pair)[1]);
            cabin.Beam(a,b,.032f,kDash,12);
        }
    }
    // On curved racing canopies a rounded reveal/roof beam can cross the
    // cowl between its endpoints. Keep inner trim on the cabin side; the
    // original exterior frame remains responsible for the exposed part.
    cabin.ClipToCowl(windowTrimFirst,fit,false);
    cabin.ClipToWindows(windowTrimFirst,fit);
    auto roofAt=[&](float z) {
        if (fit.openTop) return eyeY+.25f;
        if(z>fit.roofRearZ && fit.rearWindowZ>fit.roofRearZ+.001f) {
            const float rear=std::clamp((z-fit.roofRearZ)/(fit.rearWindowZ-fit.roofRearZ),0.f,1.f);
            return fit.roofRearY+(fit.rearWindowY-fit.roofRearY)*rear;
        }
        const float t=std::clamp((z-fit.roofFrontZ)/std::max(.10f,fit.roofRearZ-fit.roofFrontZ),0.f,1.f);
        return fit.roofFrontY+(fit.roofRearY-fit.roofFrontY)*t;
    };
    // Keep the seat base on the floor and offset generated-cabin furniture
    // another 10 cm rearward. This adjusts the seat without moving the eye
    // or steering wheel to compensate for its dimensions.
    const float seatY=floor+.075f;
    const float seatZ=eyeZ+.10f+(fit.nativeInterior ? 0.f : .10f);
    const float seatWidth=std::clamp(width-.12f,.52f,.60f);
    // Seat() adds 18 cm of headrest above backTop and reaches z+.395.
    // Fit both ends of that top against the descending rear enclosure.
    const float seatCeiling=std::min(roofAt(seatZ+seatOffset_[1]+.275f),roofAt(seatZ+seatOffset_[1]+.395f));
    const float seatTop=fit.openTop?seatY+.72f:std::min(seatY+.72f,seatCeiling-.20f);
    // A high driving position needs a raised seat base. Keep the cushion
    // within seated reach as the user raises the eye, without moving the
    // eye, wheel, passenger seat or the surrounding floor to fit the chair.
    const float driverSeatY=std::max(seatY,eyeY+seatOffset_[0]-.825f);
    if(driverSeatY>seatY+.001f)
        cabin.Box({eyeX-seatWidth*.32f,floor,seatZ+seatOffset_[1]-.19f},
                  {eyeX+seatWidth*.32f,driverSeatY-.075f,seatZ+seatOffset_[1]+.14f},kBlack);
    const size_t driverSeatFirst=cabin.vertices.size();
    auditSeatRange_[0]=uint32_t(driverSeatFirst);
    Seat(cabin,eyeX,driverSeatY,seatZ+seatOffset_[1],seatWidth,fit.openTop?seatTop+driverSeatY-seatY:std::min(seatTop+driverSeatY-seatY,seatCeiling-.20f));
    if(std::getenv("GT2_AUDIT_COCKPIT"))auditSeat_.assign(cabin.vertices.begin()+driverSeatFirst,cabin.vertices.end());
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

    auditSeatRange_[1]=uint32_t(cabin.vertices.size());
    // Generated cabins extend the fixed wheel mount 15 cm toward the driver (+Z).
    wheelCenter_={eyeX,std::min(eyeY-.28f,originalDialY-.105f),std::max(eyeZ-.38f,dashBack+.14f)+(fit.nativeInterior ? 0.f : .15f)};
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
        throw std::runtime_error("procedural cockpit exceeds its vertex range: "+std::to_string(cabin.vertices.size()));
    cabinCount_=uint32_t(cabin.vertices.size()); wheelCount_=uint32_t(wheel.vertices.size());
    mirrorCenter_=fit.mirror; mirrorScale_=0;
    speedometer_=fit.speedometer;
    renderer_.SetVertices(SceneRenderer::kCockpitVertexBase,cabin.vertices);
    renderer_.SetVertices(SceneRenderer::kCockpitVertexBase+kWheelOffset,wheel.vertices);
    fitKey_=fit; built_=true;
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
    renderer_.SetVertices(SceneRenderer::kCockpitVertexBase+kMirrorOffset,mirror.vertices);
    mirrorScale_=scale;
}

void ProceduralCockpit::UpdateInstruments(float speedKph, float rpm, int gear) {
    const bool digital=speedometer_[3]>0;
    const float speed=std::clamp(std::isfinite(speedKph)?speedKph:0.f,0.f,999.f);
    const int speedStep=digital?int(std::lround(speed))*10:int(std::lround(speed*10.f));
    const int rpmStep=digital?0:int(std::lround(std::clamp(std::isfinite(rpm)?rpm:0.f,0.f,20000.f)/20.f));
    gear=std::clamp(gear,-1,9);
    if (speedStep==speedStep_ && rpmStep==rpmStep_ && gear==gear_) return;
    Mesh mesh;
    if (speedometer_[3]>0) {
        // A small physical display inside the imported instrument binnacle.
        // Only the integer readout changes; no render target or texture upload.
        const float w=speedometer_[3];
        const V p{speedometer_[0],speedometer_[1],speedometer_[2]};
        mesh.Box(p+V{-w*.52f,-w*.25f,-.004f},p+V{w*.52f,w*.25f,0},kTrim);
        mesh.Quad(p+V{-w*.49f,-w*.22f,.001f},p+V{w*.49f,-w*.22f,.001f},
                  p+V{w*.49f,w*.22f,.001f},p+V{-w*.49f,w*.22f,.001f},kBlack,false);
        mesh.Text(std::to_string((speedStep+5)/10),p+V{-w*.10f,w*.025f,.002f},w*.24f,kAmber);
        mesh.Text("KM/H",p+V{-w*.10f,-w*.14f,.002f},w*.08f,kIvory);
        mesh.Text(gear<0?"N":gear==0?"R":std::to_string(gear),p+V{w*.33f,0,.002f},w*.16f,kIvory);
    } else {
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
    }
    if(!digital) {
        const float centreX=(dialX_[0]+dialX_[1])*.5f;
        for(auto& v:mesh.vertices){v.pos[0]=centreX+(v.pos[0]-centreX)*instrumentScale_;
            v.pos[1]=dialY_+(v.pos[1]-dialY_)*instrumentScale_;}
    }
    if (mesh.vertices.size()>kInstrumentLimit) throw std::runtime_error("cockpit instruments exceed their vertex range");
    renderer_.SetVertices(SceneRenderer::kCockpitVertexBase+kInstrumentOffset,mesh.vertices);
    instrumentCount_=uint32_t(mesh.vertices.size());
    speedStep_=speedStep; rpmStep_=rpmStep; gear_=gear;
}

void ProceduralCockpit::Append(std::vector<DrawItem>& items, const CockpitFit& fit, const float* carMvp,
                               float steeringRadians, float speedKph, float rpm, int gear, bool showWheel,
                               float seatHeight, float seatBack, const std::array<float,3>* steeringHub, bool showCabin) {
    if (!fit.valid) return;
    const std::array<float,2> seatOffset{seatHeight,seatBack};
    if (!built_ || fit!=fitKey_ || seatOffset!=seatOffset_ || bool(steeringHub)!=interactiveWheel_ ||
        (steeringHub && *steeringHub!=steeringHub_)) {
        if (std::getenv("GT2_DEBUG_CABIN")) std::printf("Cabin rebuild: first %d fit %d self %d seat %d wheel %d hub %d\n",
            int(!built_),int(fit!=fitKey_),int(fit==fit),int(seatOffset!=seatOffset_),int(bool(steeringHub)!=interactiveWheel_),int(steeringHub && *steeringHub!=steeringHub_));
        seatOffset_=seatOffset;
        interactiveWheel_=steeringHub!=nullptr;
        if (steeringHub) steeringHub_=*steeringHub;
        Build(fit);
    }
    UpdateInstruments(speedKph,rpm,gear);
    if (!showCabin) wheelCenter_=fit.nativeWheel[3]>0 ?
        std::array<float,3>{fit.nativeWheel[0],fit.nativeWheel[1],fit.nativeWheel[2]} :
        std::array<float,3>{fit.eye[0],fit.eye[1]-.37f,fit.eye[2]-.22f};
    auto append=[&](uint32_t offset,uint32_t count) {
        DrawItem item; item.firstVertex=SceneRenderer::kCockpitVertexBase+offset; item.vertexCount=count;
        item.cabinDepth=true;
        std::memcpy(item.mvp,carMvp,sizeof(item.mvp)); items.push_back(item);
    };
    if (showCabin) append(0,cabinCount_);
    else if(std::getenv("GT2_AUDIT_COCKPIT") && std::getenv("GT2_AUDIT_LEGACY_NATIVE_ENCLOSURE")) {
        // Negative regression witness. Authored interiors retain their own
        // floor/firewall: a generic enclosure fills intentional open chassis
        // gaps and covers exposed wheels on early cars and concepts.
        append(0,backingCount_);append(wheelWellFirst_,wheelWellCount_);
    }
    if (showCabin || speedometer_[3]>0) append(kInstrumentOffset,instrumentCount_);
    if (!showWheel) return;
    const float angle=std::isfinite(steeringRadians)?steeringRadians:0.f;
    constexpr float tilt=18.f*kPi/180.f;
    const float c=std::cos(angle),s=std::sin(angle),ct=std::cos(tilt),st=std::sin(tilt);
    const float local[16]={c,s*ct,-s*st,0, -s,c*ct,-c*st,0, 0,st,ct,0,
                           wheelCenter_[0],wheelCenter_[1],wheelCenter_[2],1};
    DrawItem wheel; wheel.firstVertex=SceneRenderer::kCockpitVertexBase+kWheelOffset; wheel.vertexCount=wheelCount_;
    wheel.cabinDepth=true;
    for (int col=0;col<4;++col) for (int row=0;row<4;++row) for (int k=0;k<4;++k)
        wheel.mvp[col*4+row]+=carMvp[k*4+row]*local[col*4+k];
    items.push_back(wheel);
}

void ProceduralCockpit::AppendMirror(std::vector<DrawItem>& items, const float* carMvp, float scale) {
    if (!built_) return;
    scale=std::clamp(scale,.25f,1.f);
    if (scale!=mirrorScale_) BuildMirror(scale);
    DrawItem item;
    item.cabinDepth=true;
    item.firstVertex=SceneRenderer::kCockpitVertexBase+kMirrorOffset;
    item.vertexCount=mirrorCount_;
    std::memcpy(item.mvp,carMvp,sizeof(item.mvp));
    items.push_back(item);
}

} // namespace gt2view
