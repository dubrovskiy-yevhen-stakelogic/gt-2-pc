#pragma once
#include "gt2view/cockpit_fit.h"
#include <set>

// Live mirror glass is a separately tested replacement surface, not bonnet
// paint. Match its actual triangles in 3D; a nearby body panel must remain
// eligible. The production overlay sits up to 2 mm off its source pane.
inline bool IsLiveMirrorSurface(std::span<const gt2view::SceneVertex> triangle,
        std::span<const gt2view::SceneVertex> mirrors) {
    for(const auto& v:triangle) {
        bool onPane=false;
        for(size_t i=0;i+2<mirrors.size() && !onPane;i+=3) {
            std::array<float,3> u{},w{},p{},n{};
            for(size_t k=0;k<3;++k){u[k]=mirrors[i+1].pos[k]-mirrors[i].pos[k];w[k]=mirrors[i+2].pos[k]-mirrors[i].pos[k];p[k]=v.pos[k]-mirrors[i].pos[k];}
            n={u[1]*w[2]-u[2]*w[1],u[2]*w[0]-u[0]*w[2],u[0]*w[1]-u[1]*w[0]};
            auto dot=[](const auto& a,const auto& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
            const float length=std::sqrt(dot(n,n));if(length<1e-8f || std::abs(dot(n,p))/length>.004f)continue;
            const float uu=dot(u,u),uw=dot(u,w),ww=dot(w,w),pu=dot(p,u),pw=dot(p,w),den=uu*ww-uw*uw;
            if(den<1e-12f)continue;
            const float a=(ww*pu-uw*pw)/den,b=(uu*pw-uw*pu)/den;
            onPane=a>=-.01f && b>=-.01f && a+b<=1.01f;
        }
        if(!onPane)return false;
    }
    return !triangle.empty();
}
inline void ExcludeLiveMirrorReference(gt2view::VkSceneRenderer& renderer,
        std::vector<gt2view::DrawItem>& bonnet,const std::vector<gt2view::DrawItem>& mirrors) {
    std::vector<gt2view::SceneVertex> panes;
    for(const auto& d:mirrors){auto vertices=renderer.VerticesForAudit(d);panes.insert(panes.end(),vertices.begin(),vertices.end());}
    if(panes.empty())return;
    std::erase_if(bonnet,[&](const auto& d){return IsLiveMirrorSurface(renderer.VerticesForAudit(d),panes);});
}
inline void CheckMirrorReferenceFixture() {
    std::array<gt2view::SceneVertex,3> pane{};
    pane[0].pos[0]=-.1f;pane[1].pos[0]=.1f;pane[2].pos[1]=.1f;
    auto source=pane;for(auto& v:source)v.pos[2]=-.002f;
    if(!IsLiveMirrorSurface(source,pane))throw std::runtime_error("mirror source not recognized");
    for(auto& v:source)v.pos[2]=-.02f;
    if(IsLiveMirrorSurface(source,pane))throw std::runtime_error("nearby body incorrectly excluded as mirror");
    source=pane;source[0].pos[0]-=.04f;
    if(IsLiveMirrorSurface(source,pane))throw std::runtime_error("panel spanning mirror incorrectly excluded");
}
// Projected trim can legitimately occlude paint. Test its physical position
// against the original surface instead of equating all colour changes to cuts.
inline size_t BonnetPenetrations(std::span<const gt2::CarMeshVertex> body,
        std::span<const gt2view::SceneVertex> trim,const gt2view::CockpitFit& fit) {
    std::set<std::array<float,3>> points;
    for(size_t i=0;i+2<trim.size();i+=3) {
        for(size_t j=0;j<3;++j) {
            std::array<float,3> v{},mid{};
            for(size_t k=0;k<3;++k){v[k]=trim[i+j].pos[k];mid[k]=(v[k]+trim[i+(j+1)%3].pos[k])*.5f;}
            points.insert(v);points.insert(mid);
        }
        std::array<float,3> centre{};for(size_t j=0;j<3;++j)for(size_t k=0;k<3;++k)centre[k]+=trim[i+j].pos[k]/3.f;points.insert(centre);
    }
    size_t violations=0;
    for(const auto& p:points) {
        if(p[2]>=gt2view::cockpit_detail::CowlZAt(fit,p[0])-.06f)continue;
        const float y=gt2view::cockpit_detail::SurfaceHeight(body,p[2],p[0]);
        if(std::isfinite(y) && p[1]>y+.015f) {
            ++violations;
            if(std::getenv("GT2_AUDIT_GEOMETRY") && violations<=12)std::printf("bonnet penetration: %.4f %.4f %.4f surface %.4f\n",p[0],p[1],p[2],y);
        }
    }
    return violations;
}
inline void CheckBonnetPenetrationFixture() {
    gt2view::CockpitFit fit;for(size_t i=0;i<17;++i)fit.cowl[i]={float(i)/8-1,0,-.5f};
    std::vector<gt2::CarMeshVertex> body;std::vector<gt2view::SceneVertex> trim;
    for(const auto& p:std::array<std::array<float,3>,6>{{{-1,0,-2},{1,0,-2},{1,0,-1},{-1,0,-2},{1,0,-1},{-1,0,-1}}}) {
        gt2::CarMeshVertex v{};std::copy(p.begin(),p.end(),v.pos);body.push_back(v);
        gt2view::SceneVertex t{};std::copy(p.begin(),p.end(),t.pos);t.pos[1]=-.02f;trim.push_back(t);
    }
    if(BonnetPenetrations(body,trim,fit))throw std::runtime_error("under-bonnet fixture rejected");
    for(auto& v:trim)v.pos[1]=.15f;
    if(BonnetPenetrations(body,trim,fit)<6)throw std::runtime_error("bonnet penetration negative control missed");
}

// Render the candidate triangles inside the full source depth/alpha scene.
// In-place flags preserve native attributes and already-promoted atlas UVs.
inline std::vector<uint8_t> CaptureBonnetWitness(gt2view::VkSceneRenderer& renderer,
        const std::vector<gt2view::DrawItem>& scene,const std::vector<gt2view::DrawItem>& bonnet) {
    using namespace gt2view;
    std::vector<std::pair<uint32_t,std::vector<SceneVertex>>> restore;
    for(const auto& d:bonnet) {
        auto vertices=renderer.VerticesForAudit(d);
        if(!restore.empty() && restore.back().first+restore.back().second.size()==d.firstVertex)
            restore.back().second.insert(restore.back().second.end(),vertices.begin(),vertices.end());
        else restore.emplace_back(d.firstVertex,std::move(vertices));
    }
    for(const auto& [first,original]:restore) {
        auto tagged=original;for(auto& v:tagged)v.flags|=kDebugBackfaces|kAuditWheelColor;
        renderer.SetVerticesForAudit(first,tagged);
    }
    auto image=renderer.CaptureStereoForAudit(scene,scene.size());
    for(const auto& [first,vertices]:restore)renderer.SetVerticesForAudit(first,vertices);
    return image;
}

// Identify authored native furniture in the occupied scene. It can lie ahead
// of a GT2 donor cowl on an open car and deliberately has different exposure.
// Keep missing geometry and wheel-overlap checks active at these pixels;
// only the exterior-paint colour comparison excludes this witnessed furniture.
inline std::vector<uint8_t> CaptureNativeFurnitureWitness(gt2view::VkSceneRenderer& renderer,
        const std::vector<gt2view::DrawItem>& scene) {
    using namespace gt2view;
    std::vector<std::pair<uint32_t,std::vector<SceneVertex>>> restore;
    for(const auto& d:scene){
        auto original=renderer.VerticesForAudit(d),tagged=original;
        for(auto& v:tagged){
            v.flags&=~(kAuditWheelColor|kDebugBackfaces);
            if(v.flags&kNativeCabin){v.flags|=kAuditWheelColor|kDebugBackfaces;v.flags&=~kNativeCabin;}
        }
        restore.emplace_back(d.firstVertex,std::move(original));renderer.SetVerticesForAudit(d.firstVertex,tagged);
    }
    auto image=renderer.CaptureStereoForAudit(scene,scene.size());
    for(auto it=restore.rbegin();it!=restore.rend();++it)renderer.SetVerticesForAudit(it->first,it->second);
    return image;
}

// Negative control removes actual submitted front-panel triangles for one
// capture, then restores them. This exercises holes on every real model,
// including models whose useful witness appears only from the nose view.
inline std::vector<uint8_t> CaptureMissingBonnet(gt2view::VkSceneRenderer& renderer,
        const std::vector<gt2view::DrawItem>& scene,const gt2view::CockpitFit& fit) {
    using namespace gt2view;
    std::vector<std::pair<uint32_t,std::vector<SceneVertex>>> restore;
    for(const auto& d:scene) {
        auto original=renderer.VerticesForAudit(d),broken=original;bool changed=false;
        for(size_t i=0;i+2<broken.size();i+=3) {
            std::array<std::array<float,3>,3> triangle{};
            for(size_t j=0;j<3;++j)std::copy_n(broken[i+j].pos,3,triangle[j].begin());
            if(!cockpit_detail::TriangleAheadOfCowl(triangle,fit.cowl,-.06f))continue;
            for(size_t j=1;j<3;++j)std::copy_n(broken[i].pos,3,broken[i+j].pos);
            changed=true;
        }
        if(changed){restore.emplace_back(d.firstVertex,std::move(original));renderer.SetVerticesForAudit(d.firstVertex,broken);}
    }
    auto image=renderer.CaptureStereoForAudit(scene,scene.size());
    for(auto it=restore.rbegin();it!=restore.rend();++it)renderer.SetVerticesForAudit(it->first,it->second);
    return image;
}
inline void CheckBonnetVisibilityFixture(gt2view::VkSceneRenderer& renderer) {
    using namespace gt2view;
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    StereoViews views;for(int e=0;e<2;++e)std::copy_n(identity,16,views.worldVP[e]);renderer.SetStereoViews(views);
    for(bool textured:{false,true}) {
    std::vector<SceneVertex> vertices;
    for(float z:{.25f,.75f})for(auto xy:std::array<std::array<float,2>,6>{{{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}}}) {
        SceneVertex v{};v.pos[0]=xy[0];v.pos[1]=xy[1];v.pos[2]=z;if(textured)v.flags=kTextured|kRawTexture|(2u<<8);vertices.push_back(v);
    }
    if(textured){std::vector<uint16_t> row(1024,0x8000);renderer.UploadVram(0,1,row.data());}
    const uint32_t base=textured?0u:SceneRenderer::kModelSetVertexBase;renderer.SetVertices(base,vertices);
    DrawItem far;far.firstVertex=base;far.vertexCount=6;far.space=kSpaceWorld;std::copy_n(identity,16,far.mvp);
    auto near=far;near.firstVertex+=6;
    const std::vector<DrawItem> hidden{far,near},visible{far};
    const auto black=renderer.CaptureStereoForAudit(hidden,hidden.size());
    if(black!=renderer.CaptureStereoForAudit(visible,visible.size()))throw std::runtime_error("visibility fixture must have identical black colours");
    auto tagged=[](const auto& image){size_t n=0;for(size_t p=0;p<image.size();p+=4)n+=image[p]>250 || image[p+1]>250;return n;};
    if(tagged(CaptureBonnetWitness(renderer,hidden,visible)) || !tagged(CaptureBonnetWitness(renderer,visible,visible)))
        throw std::runtime_error("black occluder mistaken for visible bonnet");
    if(black!=renderer.CaptureStereoForAudit(hidden,hidden.size()))throw std::runtime_error("visibility witness did not restore source vertices");
    }
}
