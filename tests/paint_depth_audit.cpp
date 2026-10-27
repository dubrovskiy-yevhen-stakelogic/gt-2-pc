// A hidden paint surface must not be lifted through an opaque foreground.
#include "offscreen_car_audit.h"
#include "gt2formats/png_reader.h"
#include <filesystem>
#include <iostream>
int main(int argc,char** argv){
    try{
        if(argc!=2)throw std::runtime_error("paint depth audit output");
        const auto output=std::filesystem::absolute(argv[1]).lexically_normal();
        if(!output.string().starts_with(std::filesystem::absolute("work").string()+std::string(1,std::filesystem::path::preferred_separator)))throw std::runtime_error("output must be under work");
        std::filesystem::create_directories(output);
        Device device;gt2view::VkContext context(device.existing);gt2view::VkSceneRenderer renderer(context,{256,256},VK_FORMAT_R8G8B8A8_UNORM);
        const uint32_t pixel=0xff553311;renderer.UploadExternalTexture(0,1,&pixel);
        auto surface=[](float z,bool paint){std::vector<gt2view::SceneVertex> vertices;
            for(const auto& p:std::array<std::array<float,2>,6>{{{-1,-1},{1,-1},{-1,1},{-1,1},{1,-1},{1,1}}}){gt2view::SceneVertex v{};v.pos[0]=p[0];v.pos[1]=p[1];v.pos[2]=z;
                v.color[0]=.08f;v.color[1]=.15f;v.color[2]=.3f;
                if(paint){v.flags=gt2view::kTextured|gt2view::kExternalTexture|gt2view::kModelReflection|gt2view::kModelOpaque;v.clut=65537;v.color[0]=0;v.color[1]=0;v.color[2]=1;}vertices.push_back(v);}
            return vertices;};
        renderer.SetVertices(0,surface(-3,false));renderer.SetVertices(6,surface(-3.001f,true));renderer.SetVertices(12,surface(-2.999f,true));
        const auto camera=View({0,0,0},0,0);gt2view::DrawItem base;base.vertexCount=6;std::copy(camera.begin(),camera.end(),base.mvp);
        auto hidden=base;hidden.firstVertex=6;hidden.blend=1;auto visible=hidden;visible.firstVertex=12;
        renderer.Draw({base},(output/"base.png").string(),0);
        renderer.Draw({base,hidden},(output/"hidden.png").string(),0);
        renderer.Draw({base,visible},(output/"visible.png").string(),0);
        const auto plain=gt2::ReadPngFile((output/"base.png").string()),covered=gt2::ReadPngFile((output/"hidden.png").string()),exposed=gt2::ReadPngFile((output/"visible.png").string());
        bool active=false;
        for(int y=124;y<=132;++y)for(int x=124;x<=132;++x)for(size_t c=0;c<3;++c){const size_t at=size_t(y*256+x)*4+c;if(plain.rgba[at]!=covered.rgba[at])throw std::runtime_error("hidden paint leaked through opaque surface");active|=plain.rgba[at]!=exposed.rgba[at];}
        if(!active)throw std::runtime_error("visible paint bypassed the clearcoat shader");
        std::cout<<"Hidden clearcoat occluded; visible clearcoat active: passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
