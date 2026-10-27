#include "offscreen_car_audit.h"
#include "gt2export/png_writer.h"
#include "gt2formats/png_reader.h"
#include "gt2view/paint_environment.h"
#include "gt2view/paint_probe_coverage.h"
#include <iostream>
#include <filesystem>
using namespace gt2view;
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("output-under-work");
    const auto out=std::filesystem::absolute(argv[1]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    PaintProbeCoverage coverage;
    coverage.Add({-10,2,-10},{10,2,-10},{10,2,10});
    coverage.Add({-10,2,-10},{10,2,10},{-10,2,10});
    // Adjacent frames at a short canopy's actual boundaries, including reverse
    // travel and negative grid coordinates: a nearest chunk cannot delay this.
    for(float x:{-10.1f,-9.9f,0.f,9.9f,10.1f,9.9f,-9.9f,-10.1f})
        if(coverage.Covered({x,1.2f,0})!=(std::abs(x)<10.f))throw std::runtime_error("canopy boundary ray failed");
    if(coverage.Covered({0,3,0}))throw std::runtime_error("roof below eye hides sky");
    Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{128,128},VK_FORMAT_R8G8B8A8_UNORM);
    renderer.clearColor[0]=.2f;renderer.clearColor[1]=.6f;renderer.clearColor[2]=1;
    std::array<std::vector<DrawItem>,6> faces;
    renderer.CachePaintProbe(0,faces);
    const size_t centre=(2*kPaintEnvironmentSize*kPaintEnvironmentSize+64*kPaintEnvironmentSize+64)*4;
    const auto sky=renderer.PaintProbeForAudit(0);
    if(sky[centre+2]<240 || sky[centre]>70)throw std::runtime_error("open sky probe failed");
    std::vector<SceneVertex> roof;
    for(const auto& p:std::array<std::array<float,3>,6>{{{-10,2,-10},{10,2,-10},{10,2,10},{-10,2,-10},{10,2,10},{-10,2,10}}}) {
        SceneVertex v{};std::copy(p.begin(),p.end(),v.pos);v.color[0]=1;roof.push_back(v);
    }
    renderer.SetVertices(0,roof);
    DrawItem item;item.vertexCount=6;const auto vp=PaintCubeCamera(2,{0,0,0});std::copy(vp.begin(),vp.end(),item.mvp);
    faces[2].push_back(item);renderer.CachePaintProbe(1,faces);
    const auto& covered=renderer.PaintProbeForAudit(1);
    if(covered[centre]<240 || covered[centre+1]>10 || covered[centre+2]>10)throw std::runtime_error("roof failed to occlude reflected sky");
    std::vector<uint8_t> image(covered.begin()+2*128*128*4,covered.begin()+3*128*128*4);
    gt2::WritePngRgba((out/"covered-up.png").string(),128,128,image);
    renderer.SelectPaintProbe(0);renderer.Draw({});renderer.WaitFrame();
    renderer.SelectPaintProbe(1);renderer.Draw({});renderer.WaitFrame();
    const uint32_t pixel=0x400000ffu; // red, 25% coverage
    renderer.UploadExternalTexture(SceneRendererState::kModelSetTexelBase,1,&pixel);
    std::vector<SceneVertex> glass;
    for(const auto& p:std::array<std::array<float,2>,6>{{{-.7f,-.7f},{.7f,-.7f},{.7f,.7f},{-.7f,-.7f},{.7f,.7f},{-.7f,.7f}}}) {
        SceneVertex v{};v.pos[0]=p[0];v.pos[1]=p[1];v.pos[2]=.5f;
        v.texel[0]=v.texel[1]=.5f;v.color[0]=v.color[1]=v.color[2]=1;
        v.flags=kTextured|kExternalTexture|kModelBlend;v.page=SceneRendererState::kModelSetTexelBase;v.clut=1|(1<<16);glass.push_back(v);
    }
    renderer.SetVertices(SceneRendererState::kModelSetVertexBase,glass);
    DrawItem window;window.firstVertex=SceneRendererState::kModelSetVertexBase;window.vertexCount=6;
    window.blend=kBlendAlpha;window.stpPass=3;for(int i=0;i<4;++i)window.mvp[i*5]=1;
    renderer.Draw({window},(out/"glass.png").string(),1);renderer.WaitFrame();
    const auto imageGlass=gt2::ReadPngFile((out/"glass.png").string());
    const auto* center=imageGlass.rgba.data()+(64*128+64)*4;
    // Output = alpha * red + (1-alpha) * the blue background, not an opaque
    // black/red window and not a discarded mask (tolerance: UNORM rounding).
    if(std::abs(int(center[0])-102)>2 || std::abs(int(center[1])-114)>2 || std::abs(int(center[2])-191)>2)
        throw std::runtime_error("native glass alpha blend failed");
    std::cout<<"Paint probes passed: open sky, roof occlusion, cached probe switching\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
