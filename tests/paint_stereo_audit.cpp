// GPU regression: one physical patch under deliberately different eye frusta.
#include "offscreen_car_audit.h"
#include "gt2view/paint_environment.h"
#include <filesystem>
#include <iostream>
int main(int argc,char** argv) {
    try {
        if(argc!=2)throw std::runtime_error("paint audit output-under-work");
        const auto output=std::filesystem::absolute(argv[1]).lexically_normal();
        if(!output.string().starts_with(std::filesystem::absolute("work").string()+std::string(1,std::filesystem::path::preferred_separator)))throw std::runtime_error("output must be under work");
        std::filesystem::create_directories(output);
        Device device;gt2view::VkContext context(device.existing);
        gt2view::VkSceneRenderer renderer(context,{256,256},VK_FORMAT_R8G8B8A8_UNORM);
        const uint32_t paint=0xff302018;renderer.UploadExternalTexture(0,1,&paint);
        std::vector<gt2view::SceneVertex> vertices;
        for(const auto& p:std::array<std::array<float,3>,6>{{{-1,-1,-3},{-1,1,-3},{1,-1,-3},{1,-1,-3},{-1,1,-3},{1,1,-3}}}) {
            gt2view::SceneVertex v{};std::copy(p.begin(),p.end(),v.pos);v.clut=65537;
            v.color[0]=.32f;v.color[1]=.45f;v.color[2]=.8336f;
            v.flags=gt2view::kTextured|gt2view::kExternalTexture|gt2view::kModelReflection|gt2view::kModelOpaque;vertices.push_back(v);
        }
        std::swap(vertices[1],vertices[2]);std::swap(vertices[4],vertices[5]);
        renderer.SetVertices(0,vertices);
        gt2view::DrawItem item;item.vertexCount=6;item.space=gt2view::kSpaceWorld;
        for(int k=0;k<4;++k)item.mvp[k*5]=1;
        gt2view::StereoViews views;
        for(int eye=0;eye<2;++eye) {
            auto* m=views.worldVP[eye];m[0]=eye?1.3f:.8f;m[5]=eye?-1.1f:-.9f;m[8]=eye?-.25f:.2f;m[9]=eye?.12f:-.1f;m[11]=-1;m[14]=.05f;
        }
        // Colourful probe geometry exercises the course cube rather than a constant.
        std::array<std::vector<gt2view::DrawItem>,6> faces;
        for(size_t face=0;face<6;++face) {
            auto m=gt2view::PaintCubeCamera(face,{0,0,0});auto draw=item;std::copy(m.begin(),m.end(),draw.mvp);draw.space=gt2view::kSpaceScreen;
            faces[face].push_back(draw);
        }
        // A studio descriptor is also valid before the first course load.
        renderer.BakePaintEnvironment(faces);
        for(bool multiview:{false,true}) {
            renderer.CreateStereoTarget({256,256},VK_FORMAT_R8G8B8A8_UNORM,multiview);
            for(int eye=0;eye<2;++eye) {
                auto current=views;
                std::copy(std::begin(views.worldVP[eye]),std::end(views.worldVP[eye]),current.worldVP[0]);
                renderer.SetStereoViews(current);
                renderer.DrawStereo({item},(output/((multiview?"single-":"two-")+std::to_string(eye)+".png")).string(),1);
                renderer.WaitFrame();
            }
        }
        std::cout<<"Captured shared surface under asymmetric frusta in both stereo modes\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
