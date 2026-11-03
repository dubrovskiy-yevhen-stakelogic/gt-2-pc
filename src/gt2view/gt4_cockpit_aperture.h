#pragma once
#include "gt2view/model_glazing.h"
#include "gt2view/native_cockpit_fit.h"
#include <map>

namespace gt2view {
// Fit the lower edge of the authored windscreen, rather than a height
// envelope of the whole car (which also contains wipers, dashboard impostors
// and roll cages). All coordinates returned here are in aligned car metres.
inline bool FitGt4Windscreen(CockpitFit& fit,const gt2::GltfMesh& mesh,
                            float scale,const std::array<float,3>& offset,bool recoverBootstrap=false,
                            bool rearScreen=false) {
    // Rear panes may taper sharply toward the boot. Keep the complete-pane
    // width test, but permit a narrower lower edge only for rear recovery.
    // Body width includes wheel arches; a narrow cockpit can have a much
    // narrower real screen. Require the centre/driver span instead.
    const float minimumBottomWidth=rearScreen?fit.halfWidth*.5f:std::min(fit.halfWidth,2.f*std::abs(fit.eye[0])+.30f);
    std::vector<std::array<std::array<float,3>,3>> best;
    float bestArea=0;
    std::vector<std::array<std::array<float,3>,3>> companionPanes;
    for(const auto& p:mesh.primitives) {
        if(!p.hasUvs || p.material<0)continue;
        const auto& m=mesh.materials.at(size_t(p.material));
        if(m.reflection || m.wheelAttachment>=0 || m.lampStates || m.baseColorImage<0)continue;
        const auto& img=mesh.images.at(size_t(m.baseColorImage));
        std::vector<uint8_t> mask(img.rgba.size()/4);
        for(size_t i=0;i<mask.size();++i){const auto* c=&img.rgba[i*4];
            // Native opaque packets can sample alpha zero without being
            // transparent. Match the source material's alpha test and retain
            // the existing native-paint exclusion used by window extraction.
            const bool clear=c[3]==0 && (m.nativeGt4Type<0 || m.alphaBlend || NativeTextureAlphaDiscard(m,0));
            const bool tinted=m.alphaBlend?ModelBlendGlassPixel(c):
                (ModelGlassPixel(c) && (m.nativeGt4Type<0 || (c[3]>0 && c[3]<250)));
            mask[i]=uint8_t(clear || tinted);}
        const auto holes=ModelGlassHoles(img,mask);if(holes.empty())continue;
        std::vector<std::array<std::array<float,3>,3>> pane;
        std::array<float,3> lo{1e9f,1e9f,1e9f},hi{-1e9f,-1e9f,-1e9f};
        float area=0,glassArea=0;
        for(size_t i=0;i+2<p.indices.size();i+=3) {
            const std::array<gt2::GltfVertex,3> t{p.vertices.at(p.indices[i]),p.vertices.at(p.indices[i+1]),p.vertices.at(p.indices[i+2])};
            const auto plane=GlazingOpening(t);
            if(std::abs(plane.normal[2])<(rearScreen?.05f:.20f) || std::abs(plane.normal[0])>.85f || std::abs(plane.normal[1])>(recoverBootstrap?.995f:.95f))continue;
            // A front screen rises toward the rear of the car. This excludes
            // the rear glass when an invalid body profile cannot guide us.
            if(recoverBootstrap && !rearScreen && plane.normal[1]*plane.normal[2]>=-.02f)continue;
            auto triangleArea=[](const auto& a,const auto& b,const auto& c){
                float sum=0;for(size_t k=0;k<3;++k){const size_t j=(k+1)%3,l=(k+2)%3;
                    const float v=(b[j]-a[j])*(c[l]-a[l])-(b[l]-a[l])*(c[j]-a[j]);sum+=v*v;}
                return std::sqrt(sum)*.5f;};
            const auto inside=GlazingInside(t,holes,m.clampU,m.clampV);
            float glass=0;for(size_t j=0;j+2<inside.size();j+=3)glass+=triangleArea(inside[j].position,inside[j+1].position,inside[j+2].position);
            if(glass<triangleArea(t[0].position,t[1].position,t[2].position)*.25f) {
                // Sparse border draws cannot qualify a main pane, but their
                // actual glass can extend a physically connected main pane.
                // Never recover the opaque carrier triangle in this case.
                if(plane.normal[1]*plane.normal[2]<(rearScreen?.02f:-.02f))
                    for(size_t j=0;j+2<inside.size();j+=3) {
                        std::array<std::array<float,3>,3> world;
                        for(size_t a=0;a<3;++a)for(size_t k=0;k<3;++k)
                            world[a][k]=inside[j+a].position[k]*scale+offset[k];
                        companionPanes.push_back(world);
                    }
                continue;
            }
            // A hatch can have a lower, nearly vertical pane that folds
            // the other way from the main rear screen. It cannot select the
            // main screen, but an actually adjoining carrier may extend it.
            if(rearScreen && plane.normal[1]*plane.normal[2]>=.02f) {
                std::array<std::array<float,3>,3> world;
                for(size_t a=0;a<3;++a)for(size_t k=0;k<3;++k)world[a][k]=t[a].position[k]*scale+offset[k];
                companionPanes.push_back(world);continue;
            }
            // During recovery use the authored pane boundary. Alpha-mask
            // islands (wipers and borders) can leave disconnected transparent
            // strips and must not create missing dashboard cross-sections.
            // Native no-depth passes are overlays/interior coverage, not the
            // physical screen carrier. They may extend an already selected
            // pane through real topology, but must not select the rear shell.
            if(rearScreen && m.nativeGt4Type>=0 && !m.nativeDepthWrite) {
                if(plane.normal[1]*plane.normal[2]<.02f)for(size_t j=0;j+2<inside.size();j+=3) {
                    std::array<std::array<float,3>,3> world;
                    for(size_t a=0;a<3;++a)for(size_t k=0;k<3;++k)world[a][k]=inside[j+a].position[k]*scale+offset[k];
                    companionPanes.push_back(world);
                }
                continue;
            }
            const auto fitVertices=recoverBootstrap?std::vector<gt2::GltfVertex>(t.begin(),t.end()):inside;
            for(size_t j=0;j+2<fitVertices.size();j+=3) {
                std::array<std::array<float,3>,3> world;
                for(size_t v=0;v<3;++v)for(size_t k=0;k<3;++k){world[v][k]=fitVertices[j+v].position[k]*scale+offset[k];lo[k]=std::min(lo[k],world[v][k]);hi[k]=std::max(hi[k],world[v][k]);}
                pane.push_back(world);
                if(plane.normal[1]*plane.normal[2]<(rearScreen?.02f:-.02f))companionPanes.push_back(world);
            }
            area+=triangleArea(t[0].position,t[1].position,t[2].position);glassArea+=glass;
        }
        if(pane.empty() || glassArea<area*.25f || hi[0]-lo[0]<(rearScreen?fit.halfWidth:minimumBottomWidth) ||
           lo[0]>-.15f || hi[0]<.15f || (rearScreen?std::hypot(hi[1]-lo[1],hi[2]-lo[2]):hi[1]-lo[1])<.16f || hi[2]-lo[2]>1.5f ||
           (!recoverBootstrap && (lo[1]<fit.windshieldY-.35f || lo[1]>fit.windshieldY+.12f || hi[1]<fit.roofFrontY-.15f ||
           hi[2]>fit.eye[2]+.20f || hi[2]<fit.windshieldZ || lo[2]<fit.windshieldZ-.45f)))continue;
        // The screen's lower edge is ahead of the axle midpoint. A rear
        // cabin partition may be larger and lean in the same direction.
        if(recoverBootstrap && (glassArea*scale*scale<.12f || lo[1]<std::min(fit.wheelWells[0][1],fit.wheelWells[2][1])+.08f ||
           lo[2]>(fit.wheelWells[0][2]+fit.wheelWells[2][2])*.5f))continue;
        if(recoverBootstrap){
            float bottomLeft=1e9f,bottomRight=-1e9f;
            for(const auto& t:pane)for(const auto& v:t)if(v[1]<lo[1]+.08f){bottomLeft=std::min(bottomLeft,v[0]);bottomRight=std::max(bottomRight,v[0]);}
            if(bottomRight-bottomLeft<minimumBottomWidth)continue;
            bool complete=true;
            for(size_t section=0;section<fit.cowl.size();++section){
                const float x=bottomLeft+(bottomRight-bottomLeft)*float(section)/float(fit.cowl.size()-1);
                bool covered=false;
                for(const auto& t:pane)if(x>=std::min({t[0][0],t[1][0],t[2][0]})-1e-5f && x<=std::max({t[0][0],t[1][0],t[2][0]})+1e-5f){covered=true;break;}
                complete&=covered;
            }
            if(!complete)continue;
        }
        if(glassArea>bestArea){bestArea=glassArea;best=std::move(pane);}
    }
    if(best.empty())return false;
    // A windscreen can be split into a main pane and a lower tint strip in
    // separate native draws. Fit their connected physical surface together.
    // Disconnected lamps, aprons and other windows cannot extend this pane.
    const auto asOpening=[](const auto& t){std::array<gt2::GltfVertex,3> vertices{};for(size_t k=0;k<3;++k)vertices[k].position=t[k];return GlazingOpening(vertices);};
    const auto touches=[&](const auto& a,const auto& b){
        const auto pa=asOpening(a),pb=asOpening(b);float dot=0;for(size_t k=0;k<3;++k)dot+=pa.normal[k]*pb.normal[k];
        if(!rearScreen && std::abs(dot)<.75f)return false;
        const auto close=[](const auto& point,const auto& plane){
            float distance=0;for(size_t k=0;k<3;++k)distance+=(point[k]-plane.points[0][k])*plane.normal[k];
            if(std::abs(distance)>.001f)return false;
            for(const auto& e:plane.edges){const float length=std::sqrt(e[0]*e[0]+e[1]*e[1]+e[2]*e[2]);
                if(e[0]*point[0]+e[1]*point[1]+e[2]*point[2]>e[3]+.001f*length)return false;}
            return true;
        };
        for(const auto& point:a)if(close(point,pb))return true;
        for(const auto& point:b)if(close(point,pa))return true;
        return false;
    };
    std::array<float,3> selectedLo{1e9f,1e9f,1e9f},selectedHi{-1e9f,-1e9f,-1e9f};
    for(const auto& t:best)for(const auto& point:t)for(size_t k=0;k<3;++k){selectedLo[k]=std::min(selectedLo[k],point[k]);selectedHi[k]=std::max(selectedHi[k],point[k]);}
    std::erase_if(companionPanes,[&](const auto& t){
        const auto q=asOpening(t);
        return q.hi[1]<selectedLo[1]-.20f || q.lo[1]>selectedHi[1]+.05f || q.lo[2]<selectedLo[2]-.45f || q.hi[2]>selectedHi[2]+.05f || q.lo[0]<selectedLo[0]-.10f || q.hi[0]>selectedHi[0]+.10f;
    });
    // A split hatch window has a real opaque crossbar between its panes.
    // Joining only glass-to-glass vertices loses the lower pane. Require a
    // source-opaque triangle touching both panes, rather than a proximity gap.
    std::vector<uint8_t> framed(companionPanes.size());
    if(rearScreen && !companionPanes.empty()) {
        // Topological frame support is not an exterior-visibility witness:
        // a hatch crossbar may sit under a spoiler yet still connect its panes.
        // Retain exact original opaque UV coverage, without axis-visibility
        // rejection of such sheltered source frame faces.
        std::vector<gt2::GltfVertex> opaque;
        std::map<int,ModelWindowHoles> opaqueMasks;
        for(const auto& part:mesh.primitives) {
            if(part.material<0)continue;const auto& m=mesh.materials.at(size_t(part.material));
            if(m.reflection || m.wheelAttachment>=0 || m.lampStates || !m.nativeDepthWrite)continue;
            const bool textured=part.hasUvs && m.baseColorImage>=0;
            if(textured && !opaqueMasks.contains(part.material)) {
                const auto& image=mesh.images.at(size_t(m.baseColorImage));std::vector<uint8_t> mask(image.rgba.size()/4);
                for(size_t i=0;i<mask.size();++i){const auto alpha=image.rgba[i*4+3];mask[i]=uint8_t(alpha>=254 && !NativeTextureAlphaDiscard(m,alpha));}
                opaqueMasks.emplace(part.material,ExactModelMask(image,std::move(mask)));
            }
            for(size_t at=0;at+2<part.indices.size();at+=3) {
                const std::array<gt2::GltfVertex,3> triangle{part.vertices.at(part.indices[at]),part.vertices.at(part.indices[at+1]),part.vertices.at(part.indices[at+2])};
                if(std::any_of(triangle.begin(),triangle.end(),[&](const auto& q){return q.color[3]*m.baseColorFactor[3]<.999f;}))continue;
                const auto covered=textured?GlazingInside(triangle,opaqueMasks.at(part.material),m.clampU,m.clampV):std::vector<gt2::GltfVertex>(triangle.begin(),triangle.end());
                opaque.insert(opaque.end(),covered.begin(),covered.end());
            }
        }
        std::vector<std::array<std::array<float,3>,3>> bridges;
        for(size_t i=0;i+2<opaque.size();i+=3) {
            std::array<std::array<float,3>,3> world;
            for(size_t j=0;j<3;++j)for(size_t k=0;k<3;++k)world[j][k]=opaque[i+j].position[k]*scale+offset[k];
            const auto q=asOpening(world);
            if(q.hi[1]<selectedLo[1]-.20f || q.lo[1]>selectedHi[1]+.05f || q.hi[2]<selectedLo[2]-.45f || q.lo[2]>selectedHi[2]+.05f || q.hi[0]<selectedLo[0]-.10f || q.lo[0]>selectedHi[0]+.10f)continue;
            if(std::any_of(best.begin(),best.end(),[&](const auto& pane){return touches(pane,world);}))bridges.push_back(world);
        }
        for(size_t i=0;i<companionPanes.size();++i) {
            // A hatch crossbar joins a lower pane behind the main screen.
            // Roof/side frame contacts must not pull neighbouring glazing
            // into the rear cowl footprint.
            const auto pane=asOpening(companionPanes[i]);
            if(pane.hi[1]>selectedLo[1]+.001f || pane.hi[2]>selectedLo[2]+.001f)continue;
            framed[i]=uint8_t(std::any_of(bridges.begin(),bridges.end(),[&](const auto& frame){return touches(frame,companionPanes[i]);}));
        }
    }
    // Transparent lettering/badges below a hatch also touch opaque body
    // faces. Require a connected lower glass pane spanning the rear cabin,
    // using the same authored width/centre evidence as a primary screen.
    std::vector<uint8_t> qualified(framed.size()),visited(framed.size());
    for(size_t seed=0;seed<framed.size();++seed)if(framed[seed] && !visited[seed]) {
        std::vector<size_t> group{seed};visited[seed]=1;
        float left=1e9f,right=-1e9f;
        for(size_t at=0;at<group.size();++at) {
            const auto& pane=companionPanes[group[at]];
            for(const auto& q:pane){left=std::min(left,q[0]);right=std::max(right,q[0]);}
            for(size_t j=0;j<companionPanes.size();++j)if(!visited[j]) {
                const auto q=asOpening(companionPanes[j]);
                if(q.hi[1]>selectedLo[1]+.001f || q.hi[2]>selectedLo[2]+.001f)continue;
                if(touches(pane,companionPanes[j])){visited[j]=1;group.push_back(j);}
            }
        }
        if(right-left>=fit.halfWidth && left<=-.15f && right>=.15f)
            for(size_t j:group)qualified[j]=framed[j];
    }
    framed=std::move(qualified);
    std::vector<uint8_t> joined(companionPanes.size());
    for(size_t at=0;at<best.size();++at)for(size_t i=0;i<companionPanes.size();++i)if(!joined[i] && (framed[i] || touches(best[at],companionPanes[i]))){
        joined[i]=1;
        if(std::find(best.begin(),best.end(),companionPanes[i])==best.end())best.push_back(companionPanes[i]);
    }
    float bottom=1e9f;for(const auto& t:best)for(const auto& p:t)bottom=std::min(bottom,p[1]);
    float left=1e9f,right=-1e9f;for(const auto& t:best)for(const auto& p:t)if(p[1]<bottom+.08f){left=std::min(left,p[0]);right=std::max(right,p[0]);}
    if(right-left<minimumBottomWidth)return false;
    auto cowl=fit.cowl;
    for(size_t i=0;i<cowl.size();++i) {
        const float x=left+(right-left)*float(i)/float(cowl.size()-1);
        std::array<float,3> lowest{x,1e9f,0};
        for(const auto& t:best)for(size_t e=0;e<3;++e){const auto& a=t[e];const auto& b=t[(e+1)%3];
            if(x<std::min(a[0],b[0])-1e-5f || x>std::max(a[0],b[0])+1e-5f)continue;
            const float u=std::abs(b[0]-a[0])<1e-6f?(a[1]<b[1]?0.f:1.f):std::clamp((x-a[0])/(b[0]-a[0]),0.f,1.f);
            const float y=a[1]+(b[1]-a[1])*u;
            if(y<lowest[1])lowest={x,y,a[2]+(b[2]-a[2])*u};
        }
        if(lowest[1]>1e8f)return false;
        // Overlap the opaque lower frame from underneath, not the clear pane.
        lowest[1]-=.018f;lowest[2]-=.012f;cowl[i]=lowest;
    }
    // An opaque sponsor/wiper island can interrupt a single sampled section.
    // Bridge an isolated inward spike with the neighbouring lower boundary;
    // do not lift the whole dashboard to the top of that island.
    for(size_t pass=0;pass<cowl.size();++pass)for(size_t i=1;i+1<cowl.size();++i)
        if(cowl[i][2]>std::max(cowl[i-1][2],cowl[i+1][2])+.06f &&
           cowl[i][1]>std::min(cowl[i-1][1],cowl[i+1][1])+.025f)
            for(size_t k=1;k<3;++k)cowl[i][k]=(cowl[i-1][k]+cowl[i+1][k])*.5f;
    float oldLow=1e9f,oldHigh=-1e9f;
    for(const auto& p:fit.cowl){oldLow=std::min(oldLow,p[1]);oldHigh=std::max(oldHigh,p[1]);}
    // Some LOD masks include an invisible apron below the actual windscreen.
    // A smooth existing body boundary is stronger evidence than such a large
    // downward jump. A visibly corrugated fit still needs the glass correction.
    if(!rearScreen && !recoverBootstrap && fit.windshieldY<fit.roofFrontY-.12f && oldHigh-oldLow<.12f && cowl[8][1]<fit.windshieldY-.12f)return false;
    if(recoverBootstrap) {
        float top=-1e9f;for(const auto& t:best)for(const auto& p:t)top=std::max(top,p[1]);
        std::array<std::array<float,3>,2> upper{{{1e9f,0,0},{-1e9f,0,0}}};
        for(const auto& t:best)for(const auto& p:t)if(p[1]>=top-.08f){
            if(p[0]<upper[0][0])upper[0]=p;if(p[0]>upper[1][0])upper[1]=p;
        }
        if(upper[1][0]-upper[0][0]<(rearScreen?fit.halfWidth:minimumBottomWidth)*.6f)return false;
        for(auto& p:upper)p[1]+=.012f;
        fit.frontHeader=upper;fit.roofFrontY=top+.012f;
        fit.roofFrontZ=(upper[0][2]+upper[1][2])*.5f;
        fit.roofFrontHalfWidth=(upper[1][0]-upper[0][0])*.5f;
        fit.roofY=std::max(fit.roofY,fit.roofFrontY);
    }
    fit.cowl=cowl;fit.frontCowl={cowl.front(),cowl.back()};
    fit.windshieldY=cowl[8][1];fit.windshieldZ=cowl[8][2];fit.dashboardY=fit.windshieldY+.025f;
    fit.windshieldHalfWidth=(right-left)*.5f;
    return true;
}
// A rear body-profile maximum may be a roof/wing rather than the lower
// rear-glass edge. Recover an impossible inverted aperture from authored
// glass using the same spatial and texture evidence as the front screen.
inline bool RecoverGt4RearScreen(CockpitFit& fit,const gt2::GltfMesh& mesh,
                                float scale,const std::array<float,3>& offset) {
    if(fit.nativeInterior || fit.openTop)return false;

    auto mirrored=mesh;
    for(auto& p:mirrored.primitives)for(auto& v:p.vertices)v.position[2]=-v.position[2];
    auto reverse=fit;
    reverse.frontHeader=fit.rearHeader;reverse.frontCowl=fit.rearCowl;
    for(auto* pair:{&reverse.frontHeader,&reverse.frontCowl})for(auto& point:*pair)point[2]=-point[2];
    reverse.windshieldHalfWidth=fit.rearWindowHalfWidth;reverse.roofFrontHalfWidth=fit.roofRearHalfWidth;
    reverse.windshieldY=fit.rearWindowY;reverse.windshieldZ=-fit.rearWindowZ;
    reverse.roofFrontY=fit.roofRearY;reverse.roofFrontZ=-fit.roofRearZ;
    reverse.eye[2]=-reverse.eye[2];
    for(auto& wheel:reverse.wheelWells)wheel[2]=-wheel[2];
    for(auto& p:reverse.cowl)p={0,fit.rearWindowY,-fit.rearWindowZ};
    if(!FitGt4Windscreen(reverse,mirrored,scale,{offset[0],offset[1],-offset[2]},true,true))return false;
    fit.rearCowlProfile=reverse.cowl;for(auto& point:fit.rearCowlProfile)point[2]=-point[2];
    fit.rearCowlProfileValid=true;
    fit.rearCowl=reverse.frontCowl;fit.rearHeader=reverse.frontHeader;
    for(auto* pair:{&fit.rearCowl,&fit.rearHeader})for(auto& p:*pair)p[2]=-p[2];
    fit.rearWindowY=reverse.windshieldY;fit.rearWindowZ=-reverse.windshieldZ;
    fit.rearWindowHalfWidth=reverse.windshieldHalfWidth;
    fit.roofRearY=reverse.roofFrontY;fit.roofRearZ=-reverse.roofFrontZ;
    fit.roofRearHalfWidth=reverse.roofFrontHalfWidth;
    return true;
}
} // namespace gt2view
