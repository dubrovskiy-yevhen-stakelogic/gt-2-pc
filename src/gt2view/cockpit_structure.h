#pragma once
#include "gt2view/side_mirror_fit.h"
#include <numeric>

namespace gt2view::cockpit_structure {
using Point=std::array<float,3>;
inline Point Position(const gt2::CarMeshVertex& v){return {v.pos[0],v.pos[1],v.pos[2]};}
inline Point Sub(Point a,Point b){for(size_t i=0;i<3;++i)a[i]-=b[i];return a;}
inline float Dot(Point a,Point b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline float Length(Point a){return std::sqrt(Dot(a,a));}
inline Point Mix(Point a,Point b,float t){for(size_t i=0;i<3;++i)a[i]+=(b[i]-a[i])*t;return a;}
inline Point Cross(Point a,Point b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline Point Closest(Point p,Point a,Point b,Point c){
    const auto ab=Sub(b,a),ac=Sub(c,a),n=Cross(ab,ac);const float nn=Dot(n,n);
    Point best=a;float distance=Length(Sub(p,a));
    if(nn>1e-12f){auto q=p;const float t=Dot(Sub(p,a),n)/nn;for(size_t k=0;k<3;++k)q[k]-=n[k]*t;
        const auto aq=Sub(q,a);const float aa=Dot(ab,ab),bb=Dot(ac,ac),d=Dot(ab,ac),den=aa*bb-d*d;
        if(den>1e-12f){const float u=(Dot(aq,ab)*bb-Dot(aq,ac)*d)/den,v=(Dot(aq,ac)*aa-Dot(aq,ab)*d)/den;
            if(u>=0 && v>=0 && u+v<=1){best=q;distance=Length(Sub(p,q));}}}
    for(const auto& edge:std::array<std::array<Point,2>,3>{{{a,b},{b,c},{c,a}}}){
        const auto delta=Sub(edge[1],edge[0]);const auto q=Mix(edge[0],edge[1],std::clamp(Dot(Sub(p,edge[0]),delta)/std::max(1e-12f,Dot(delta,delta)),0.f,1.f));
        const float d=Length(Sub(p,q));if(d<distance){best=q;distance=d;}}
    return best;
}
inline bool Opaque(const gt2::CarMeshVertex& a,const gt2::CarMeshVertex& b,float t,const gt2::CarTexture& texture){
    if(!a.textured || texture.paints.empty())return true;
    const int u=std::clamp(int(a.texel[0]+(b.texel[0]-a.texel[0])*t),0,255),v=std::clamp(int(a.texel[1]+(b.texel[1]-a.texel[1])*t),0,223);
    return texture.paints[0].cluts[a.palette][texture.indices[size_t(v)*256+size_t(u)]]!=0;
}
struct Support{Point root,tip;float radius;};
inline void AppendSupport(const Support& s,std::vector<gt2::CarMeshVertex>& output){
    auto axis=Sub(s.tip,s.root);const float length=Length(axis);if(length<.001f)return;
    for(auto& x:axis)x/=length;auto right=Cross(axis,std::abs(axis[1])<.9f?Point{0,1,0}:Point{1,0,0});
    const float norm=Length(right);for(auto& x:right)x/=norm;const auto up=Cross(axis,right);
    std::array<std::array<gt2::CarMeshVertex,8>,2> ring{};
    for(size_t end=0;end<2;++end)for(size_t j=0;j<8;++j){auto& v=ring[end][j];const float angle=float(j)*.785398163f;
        for(size_t k=0;k<3;++k){v.pos[k]=(end?s.tip[k]:s.root[k])+s.radius*(right[k]*std::cos(angle)+up[k]*std::sin(angle));v.color[k]=.0967742f;}}
    for(size_t j=0;j<8;++j){const size_t next=(j+1)%8;
        for(const auto& v:std::array<gt2::CarMeshVertex,6>{ring[0][j],ring[1][j],ring[1][next],ring[0][j],ring[1][next],ring[0][next]})output.push_back(v);}
    // Close both ends as well: a grazing view through an uncapped tube can
    // expose another tiny gap even though its centre line reaches the housing.
    for(size_t j=1;j+1<8;++j)for(const auto& v:std::array<gt2::CarMeshVertex,6>{ring[0][0],ring[0][j+1],ring[0][j],ring[1][0],ring[1][j],ring[1][j+1]})output.push_back(v);
}
// The retail mirror silhouette is an alpha-cut plane. A low-resolution alpha
// break in its narrow stalk is conspicuous at life size. Bridge only a measured
// break between a body-seated root and an opaque part of the same mirror shell.
inline std::vector<Support> MirrorSupports(const gt2::CarModel& model,const gt2::CarTexture& texture,const CockpitFit& fit){
    if(texture.paints.empty())return {};
    gt2::CarMeshOptions options;options.wheels=false;const auto mesh=gt2::BuildCarMesh(model,options);
    const size_t count=mesh.size()/3;std::vector<size_t> parent(count);std::iota(parent.begin(),parent.end(),0);
    auto root=[&](size_t i){while(parent[i]!=i){parent[i]=parent[parent[i]];i=parent[i];}return i;};
    std::map<Point,size_t> owners;
    for(size_t i=0;i<count;++i)for(size_t j=0;j<3;++j){const auto [it,added]=owners.emplace(Position(mesh[i*3+j]),i);if(!added)parent[root(i)]=root(it->second);}
    std::map<size_t,std::vector<size_t>> groups;for(size_t i=0;i<count;++i)groups[root(i)].push_back(i);
    std::vector<Support> result;
    for(const auto& [group,triangles]:groups){
        if(triangles.size()>60 || triangles.size()<2)continue;
        const bool diagnostic=std::getenv("GT2_AUDIT_STRUCTURE_DEBUG")!=nullptr;
        Point lo=Position(mesh[triangles[0]*3]),hi=lo;
        for(size_t i:triangles)for(size_t j=0;j<3;++j)for(size_t k=0;k<3;++k){lo[k]=std::min(lo[k],mesh[i*3+j].pos[k]);hi[k]=std::max(hi[k],mesh[i*3+j].pos[k]);}
        if(diagnostic)std::printf("Small component %zu count %zu lo %.3f %.3f %.3f hi %.3f %.3f %.3f fit %.3f %.3f %.3f %.3f\n",group,triangles.size(),lo[0],lo[1],lo[2],hi[0],hi[1],hi[2],fit.halfWidth,fit.windshieldY,fit.windshieldZ,fit.eye[2]);
        if(hi[0]-lo[0]>.36f || hi[1]-lo[1]>.30f || hi[2]-lo[2]>.40f ||
            std::min(std::abs(lo[0]),std::abs(hi[0]))<fit.halfWidth*.65f || lo[0]*hi[0]<=0 ||
            lo[1]<fit.windshieldY-.18f || hi[1]>fit.windshieldY+.32f ||
            lo[2]<fit.windshieldZ-.85f || hi[2]>fit.eye[2]+.35f)continue;
        bool mirror=false;
        for(size_t i:triangles){const auto& a=mesh[i*3];const auto& b=mesh[i*3+1];const auto& c=mesh[i*3+2];
            if(!a.textured || !RearFacingMirrorTriangle(Position(a),Position(b),Position(c)))continue;
            size_t covered=0,neutral=0;bool fixed=true;
            for(int u=0;u<=6;++u)for(int v=0;v<=6-u;++v){const float wa=float(u)/6,wb=float(v)/6,wc=1-wa-wb;
                const int x=std::clamp(int(a.texel[0]*wa+b.texel[0]*wb+c.texel[0]*wc),0,255),y=std::clamp(int(a.texel[1]*wa+b.texel[1]*wb+c.texel[1]*wc),0,223);
                const auto index=texture.indices[size_t(y)*256+size_t(x)];const auto color=texture.paints[0].cluts[a.palette][index];
                if(!color)continue;++covered;const int r=color&31,g=(color>>5)&31,blue=(color>>10)&31;
                neutral+=std::max({r,g,blue})-std::min({r,g,blue})<=3;
                for(const auto& paint:texture.paints)fixed &= paint.cluts[a.palette][index]==color;
            }
            if(diagnostic)std::printf("  rear %zu covered %zu neutral %zu fixed %d\n",i,covered,neutral,fixed);
            // The glass and its painted rim can share one PS1 polygon. Require
            // a neutral majority, not an entirely neutral material rectangle.
            mirror |= covered>=7 && neutral*2>covered && fixed;
        }
        if(!mirror)continue;
        if(diagnostic)std::printf("Mirror component %zu triangles %zu bounds %.3f %.3f %.3f / %.3f %.3f %.3f\n",group,triangles.size(),lo[0],lo[1],lo[2],hi[0],hi[1],hi[2]);
        float nearest=.025f;Point seated{},mount{};bool found=false;
        for(size_t i:triangles)for(size_t j=0;j<3;++j){const auto p=Position(mesh[i*3+j]);
            for(size_t other=0;other<count;++other){if(root(other)==group)continue;
                const auto q=Closest(p,Position(mesh[other*3]),Position(mesh[other*3+1]),Position(mesh[other*3+2]));
                const float distance=Length(Sub(q,p));if(distance<nearest){nearest=distance;seated=q;mount=p;found=true;}}}
        if(!found)continue;
        if(diagnostic)std::printf("  mount %.6f %.6f %.6f distance %.6f\n",mount[0],mount[1],mount[2],nearest);
        float shortest=.12f;Point tip{};bool broken=false;
        for(size_t i:triangles)for(size_t j=0;j<3;++j){const auto& a=mesh[i*3+j];if(Position(a)!=mount)continue;
            for(size_t k=0;k<3;++k){if(k==j)continue;const auto& b=mesh[i*3+k];
                const float length=Length(Sub(Position(b),mount));if(length<.01f)continue;
                bool gap=false;int opaqueRun=0;
                for(int sample=1;sample<=64;++sample){const float t=float(sample)/64.f;
                    if(!Opaque(a,b,t,texture)){gap=true;opaqueRun=0;}else if(gap && ++opaqueRun>=5){
                        const float distance=length*t;if(distance<shortest){shortest=distance;tip=Mix(mount,Position(b),t);broken=true;}break;}}
            }}
        if(diagnostic)std::printf("  broken %d shortest %.6f\n",broken,shortest);
        if(broken){auto delta=Sub(tip,seated);const float length=Length(delta);if(length>.005f && length<.12f){
            for(size_t k=0;k<3;++k)seated[k]-=delta[k]/length*.003f;
            result.push_back({seated,tip,.006f});}}
    }
    return result;
}
}
