// Elevated/leaned views keep the production cockpit draw path active.
#include "offscreen_car_audit.h"
#include "car_audit_catalogue.h"
#include "gt2view/scene_assets.h"
#include "gt2export/png_writer.h"
#include "../tools/gt2game/cockpit_camera.h"
#include <iostream>
#include <fstream>
#include "hud_mirror_clip_check.h"
#include "cockpit_surface_checks.h"
using namespace gt2view;
int main(int argc,char** argv){try {
    if(argc<4)throw std::runtime_error("disc pack output-under-work [ids...]");
    if(!std::getenv("GT2_AUDIT_WHEEL_COLOR") || !std::getenv("GT2_AUDIT_COCKPIT"))throw std::runtime_error("enable wheel colour and cockpit audit instrumentation");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
    const auto ids=AuditCarCatalogue(vol,argc,argv);gt2::ModelSet pack(gt2::gt4ModelSetRoot);
    constexpr uint32_t size=256;Device device;VkContext context(device.existing);
    VkSceneRenderer renderer(context,{size,size},VK_FORMAT_R8G8B8A8_UNORM);
    renderer.EnableDecodedTextures(true);renderer.CreateStereoTarget({size,size},VK_FORMAT_R8G8B8A8_UNORM,true);
    CheckBonnetPenetrationFixture();
    CheckMirrorReferenceFixture();
    CheckBonnetVisibilityFixture(renderer);
    const size_t mirrorLeaks=CheckHudMirrorClip(renderer,size,out);
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::ofstream csv(out/"surfaces.csv");csv<<"car,path,pose,steer,holes,wheel_new_pixels,mirror_vertices,reflection_vertices,bonnet_changed,reference_pixels,shell_changed,bonnet_holes,geometry_violations\n";
    size_t paths=0,failures=mirrorLeaks?1:0;
    for(const auto& id:ids)for(bool native:{false,true}){
        if(native && !pack.Has(id))continue;
        if(!native && vol.IsExpansionAsset("carobj/"+id+".cdo"))continue;
        SceneAssets assets(renderer,vol,true);assets.SetGt4Enabled(native);const int slot=assets.UseCar(id);
        if(slot<0 || (native && !assets.UsesGt4(slot)))throw std::runtime_error("missing model "+id);
        const auto fit=assets.SlotCockpitFit(slot);
        bool hasReflections=false;
        if(native) {
            const auto imported=pack.Read(id,false);hasReflections=!imported.reflections.primitives.empty();
            for(uint32_t state=0;state<6;++state) {
                std::vector<DrawItem> lamps;assets.AppendGt4LampItems(lamps,slot,identity,state);
                size_t expected=0,actual=0;for(const auto& p:imported.lamps.primitives)
                    if(const auto& m=imported.lamps.materials.at(size_t(p.material)); !m.reflection && (m.lampStates & (1u<<state)))expected+=p.indices.size();
                // Environment carriers are clipped by the mirror silhouette.
                // Compare exact diffuse lamp geometry in all six callbacks.
                for(const auto& d:lamps)if(!NativeEnvironmentBlend(d.blend))actual+=d.vertexCount;
                if(expected!=actual)throw std::runtime_error("lamp callback lost geometry "+id+" state "+std::to_string(state)+" expected "+std::to_string(expected)+" actual "+std::to_string(actual));
            }
        }
        std::vector<DrawItem> bonnet;assets.AppendBonnetItemsForAudit(bonnet,slot,identity);
        std::vector<DrawItem> mirrors;assets.AppendSideMirrorItems(mirrors,slot,identity);
        ExcludeLiveMirrorReference(renderer,bonnet,mirrors);
        size_t mirrorCount=0;for(const auto& d:mirrors)mirrorCount+=d.vertexCount;
        assets.UpdateCarReflection(slot,identity,{{{1,0,0},{0,-1,0},{0,0,-1}}});
        ProceduralCockpit furniture(renderer);
        size_t pathReferencePixels=0;
        for(int steer:{0,-1,1}){
            size_t steeringReferencePixels=0;
            std::array<std::array<float,16>,4> wheels;
            for(size_t i=0;i<4;++i){auto centre=assets.SlotWheelCentres(slot)[i];if(steer)centre[1]+=.08f;
                gt2::WheelModelMatrix(int(i),centre.data(),int16_t(i<2?steer*384:0),0,512,wheels[i].data());}
            std::vector<DrawItem> body,cabin;
            assets.AppendCarItems(body,slot,identity,0,0,true,&wheels);
            assets.AppendCockpitBodyItems(cabin,slot,identity,0,&wheels);
            std::vector<DrawItem> wheelDraws;assets.AppendCarWheels(wheelDraws,slot,identity,0,&wheels);
            std::erase_if(body,[&](const auto& d){return std::any_of(wheelDraws.begin(),wheelDraws.end(),[&](const auto& w){return d.firstVertex==w.firstVertex;});});
            // Compare uncut and occupied bodies in the same production depth
            // layer. Different viewport depth ranges quantize near-coplanar
            // grille/decal surfaces differently and invent colour changes.
            for(auto& d:body)d.cabinDepth=1;
            if(std::getenv("GT2_AUDIT_INJECT_WHEEL_OVERLAP"))for(auto& d:cabin)
                if(std::any_of(wheelDraws.begin(),wheelDraws.end(),[&](const auto& w){return d.firstVertex==w.firstVertex;}))d.cabinDepth=1;
            size_t reflectionCount=0;for(const auto& d:cabin)if((d.blend==1 || d.blend==gt2view::kBlendStockSurfaceEnvironment) || NativeEnvironmentBlend(d.blend))reflectionCount+=d.vertexCount;
            // Production live mirror overlays also cover their static source
            // glass; do not classify those small panes as bonnet holes.
            body.insert(body.end(),mirrors.begin(),mirrors.end());
            cabin.insert(cabin.end(),mirrors.begin(),mirrors.end());
            // Physical skin includes authored blended panels; their opacity is
            // irrelevant to the trim-vs-body intersection test. The pixel gate
            // below isolates opaque coverage from translucent shadow decals.
            auto physicalBody=body;
            std::erase_if(physicalBody,[](const auto& d){return (d.blend==1 || d.blend==gt2view::kBlendStockSurfaceEnvironment) || d.blend==2 || NativeEnvironmentBlend(d.blend);});
            const auto cabinBody=cabin;
            furniture.Append(cabin,fit,identity,0,0,0,1,false,0,gt2game::CockpitSeatBack({}),nullptr,!assets.HasNativeGt4Interior(slot));
            for(auto* draws:{&body,&cabin}){std::erase_if(*draws,[](const auto& d){return d.blend==2 || (d.blend==1 || d.blend==gt2view::kBlendStockSurfaceEnvironment) || d.blend==kBlendAlpha || NativeEnvironmentBlend(d.blend);});for(auto& d:*draws)d.space=kSpaceWorld;SortModelTransparency(*draws);}
            auto raw=cabinBody;std::erase_if(raw,[](const auto& d){return (d.blend==1 || d.blend==gt2view::kBlendStockSurfaceEnvironment) || d.blend==kBlendAlpha || NativeEnvironmentBlend(d.blend);});
            for(auto& d:raw)d.space=kSpaceWorld;SortModelTransparency(raw);
            std::vector<gt2::CarMeshVertex> surface;std::vector<SceneVertex> trim;
            for(const auto& d:physicalBody)for(const auto& v:renderer.VerticesForAudit(d)) {
                gt2::CarMeshVertex p{};std::copy_n(v.pos,3,p.pos);surface.push_back(p);
            }
            // The vertical bonnet envelope applies to generated enclosure
            // furniture. Instrument glyphs are separate draws: in an open
            // prototype they legitimately sit above its exposed cockpit floor.
            // Their visible coverage is still checked by the image witnesses.
            for(const auto& d:cabin)if(d.firstVertex==SceneRenderer::kCockpitVertexBase) {
                const auto vertices=renderer.VerticesForAudit(d);trim.insert(trim.end(),vertices.begin(),vertices.end());
            }
            const size_t penetrations=BonnetPenetrations(surface,trim,fit);
            if(std::getenv("GT2_AUDIT_GEOMETRY") && !steer) {
                const auto prefix=out/(id+(native?"-gt4":"-gt2"));
                std::ofstream f(prefix.string()+"-shell.csv");f<<"x,y,z,clut,flags\n";
                for(const auto& d:raw)for(const auto& v:renderer.VerticesForAudit(d))f<<v.pos[0]<<','<<v.pos[1]<<','<<v.pos[2]<<','<<v.clut<<','<<v.flags<<'\n';
                std::ofstream t(prefix.string()+"-trim.csv");t<<"x,y,z,r,g,b\n";
                for(const auto& v:trim)t<<v.pos[0]<<','<<v.pos[1]<<','<<v.pos[2]<<','<<v.color[0]<<','<<v.color[1]<<','<<v.color[2]<<'\n';
                std::ofstream b(prefix.string()+"-source.csv");b<<"x,y,z\n";
                for(const auto& v:surface)b<<v.pos[0]<<','<<v.pos[1]<<','<<v.pos[2]<<'\n';
                std::printf("trim groups: cowl %zu liners %zu total %zu\n",BonnetPenetrations(surface,furniture.CowlSealForAudit(),fit),BonnetPenetrations(surface,furniture.WheelLinersForAudit(),fit),penetrations);
                std::ofstream g(prefix.string()+"-fit.txt");g<<fit.roofY+1.1f<<' '<<fit.windshieldZ+.6f<<'\n';
                for(const auto& v:fit.cowl)g<<v[0]<<' '<<v[1]<<' '<<v[2]<<'\n';
            }

            for(int pose=0;pose<4;++pose){
                // Low pointed noses can be edge-on in all three rearward
                // overhead views. Look back over the nose as well, keeping
                // precisely the same occupied cockpit draw submission.
                const bool nose=pose==3;
                const std::array<float,3> eye=nose ? std::array<float,3>{0.f,fit.roofY+.4f,fit.frontZ-1.f} :
                    std::array<float,3>{pose==1?-.75f:pose==2?.75f:0.f,fit.roofY+1.1f,fit.windshieldZ+.6f};
                StereoViews views;for(int e=0;e<2;++e){auto at=eye;at[0]+=e?.032f:-.032f;const auto vp=View(at,nose?180.f:0.f,nose?-35.f:-65.f);std::copy(vp.begin(),vp.end(),views.worldVP[e]);}renderer.SetStereoViews(views);
                renderer.clearColor[0]=0;renderer.clearColor[1]=1;renderer.clearColor[2]=1;
                const auto source=renderer.CaptureStereoForAudit(body,body.size());
                const auto seated=std::getenv("GT2_AUDIT_INJECT_BONNET_HOLE") ? CaptureMissingBonnet(renderer,cabin,fit) : renderer.CaptureStereoForAudit(cabin,cabin.size());
                // A matching colour alone is not a visibility witness: a black
                // wiper can hide a different black panel in the reference.
                // Tag bonnet fragments in place, retaining the full source
                // scene's depth, texture alpha and native material behaviour.
                const auto mask=CaptureBonnetWitness(renderer,body,bonnet);
                // Keep the complete occupied shell as the depth context, just
                // as the source witness keeps the complete exterior. Clipping
                // the comparison scene to whole triangles ahead of the cowl
                // removes cross-boundary occluders after re-triangulation.
                const auto shell=renderer.CaptureStereoForAudit(raw,raw.size());
                const auto nativeFurniture=assets.HasNativeGt4Interior(slot)?CaptureNativeFurnitureWitness(renderer,raw):std::vector<uint8_t>{};
                // Compare only intact original paint ahead of the rubber cowl
                // seal, eroded one pixel to exclude silhouette raster edges.
                auto bg=[](const auto& im,size_t p){return im[p]<3 && im[p+1]>250 && im[p+2]>250;};
                auto tyre=[](const auto& im,size_t p){return im[p]>250 && im[p+1]<3 && im[p+2]>250;};
                size_t holes=0,wheelNew=0;
                for(size_t p=0;p<seated.size();p+=4){holes+=bg(seated,p)&&!bg(source,p);}
                size_t changed=0,referencePixels=0,shellChanged=0,bonnetHoles=0;
                auto reference=[&](size_t p){return tyre(mask,p) || (mask[p]<3 && mask[p+1]>250 && mask[p+2]<3);};
                for(size_t e=0;e<2;++e)for(size_t y=2;y+2<size;++y)for(size_t x=2;x+2<size;++x){
                    const size_t p=((e*size+y)*size+x)*4;
                    if(!reference(p)||!reference(p-4)||!reference(p+4)||!reference(p-size*4)||!reference(p+size*4))continue;
                    ++referencePixels;wheelNew+=tyre(seated,p);int d=0;for(size_t k=0;k<3;++k)d+=std::abs(int(seated[p+k])-int(source[p+k]));changed+=d>40;
                    int shellDelta=0;for(size_t k=0;k<3;++k)shellDelta+=std::abs(int(shell[p+k])-int(source[p+k]));
                    const bool nativeFurniturePixel=!nativeFurniture.empty() && (tyre(nativeFurniture,p) ||
                        (nativeFurniture[p]<3 && nativeFurniture[p+1]>250 && nativeFurniture[p+2]<3));
                    shellChanged+=shellDelta>40 && !nativeFurniturePixel;bonnetHoles+=bg(seated,p);
                }
                csv<<id<<','<<(native?"gt4":"gt2")<<','<<pose<<','<<steer<<','<<holes<<','<<wheelNew<<','<<mirrorCount<<','<<reflectionCount<<','<<changed<<','<<referencePixels<<','<<shellChanged<<','<<bonnetHoles<<','<<penetrations<<'\n';
                pathReferencePixels+=referencePixels;
                steeringReferencePixels+=referencePixels;
                const bool failed=shellChanged>16 || bonnetHoles>2 || wheelNew>2 || penetrations>0 || (hasReflections && !reflectionCount);
                failures+=failed;
                if(failed || std::getenv("GT2_AUDIT_CAPTURE")){
                    const auto stem=id+(native?"-gt4":"-gt2")+"-"+std::to_string(pose)+"-"+std::to_string(steer);
                    gt2::WritePngRgba((out/(stem+"-cabin.png")).string(),size,size*2,seated);
                    gt2::WritePngRgba((out/(stem+"-mask.png")).string(),size,size*2,mask);
                    gt2::WritePngRgba((out/(stem+"-shell.png")).string(),size,size*2,shell);
                    gt2::WritePngRgba((out/(stem+"-body.png")).string(),size,size*2,source);
                }
            }
            if(steeringReferencePixels<32){++failures;std::cout<<id<<" insufficient surface coverage at steering "<<steer<<": "<<steeringReferencePixels<<'\n';}
        }
        ++paths;csv.flush();std::cout<<id<<(native?" gt4":" gt2")<<" surfaces captured, witness pixels "<<pathReferencePixels<<'\n'<<std::flush;
    }
    std::ofstream(out/"gate.json")<<"{\"paths\":"<<paths<<",\"failures\":"<<failures<<",\"mirror_leaks\":"<<mirrorLeaks<<"}\n";
    return failures?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}

