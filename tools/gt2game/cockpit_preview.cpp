#include "cockpit_preview.h"
#include "game_window.h"

#if GT2_COCKPIT_PREVIEW
#include "cockpit_camera.h"
#include "frame_interp.h"
#include "pc_overlay.h"
#include "gt2formats/car_info.h"
#include "gt2view/menu_view.h"
#include "gt2view/procedural_cockpit.h"
#include "gt2view/scene_assets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace gt2game {
namespace {
constexpr float kIdentity[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

// Menu primitives keep these controls readable with every disc's font/VRAM layout.
void Label(std::vector<gt2::MenuPrim>& out, std::string_view text, int x, int y) {
    static constexpr uint8_t letters[26][5]={
        {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
        {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
        {0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
        {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
        {62,65,81,33,94},{127,9,25,41,70},{70,73,73,73,49},{1,1,127,1,1},
        {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
        {7,8,112,8,7},{97,81,73,69,67}};
    const auto tile=[&](int px,int py,int w,int h,uint32_t color) {
        gt2::MenuPrim p; p.kind=gt2::MenuPrim::kTile;
        p.x[0]=int16_t(px);p.y[0]=int16_t(py);p.w=int16_t(w);p.h=int16_t(h);
        p.colour[0]=color;out.push_back(p);
    };
    tile(x-5,y-5,int(text.size())*6+9,24,0x281b12);
    tile(x-5,y-5,2,24,0xf0ce70);
    for(char c:text) {
        if(c>='A' && c<='Z')for(int col=0;col<5;++col)for(int row=0;row<7;++row)
            if((letters[c-'A'][col]>>row)&1)tile(x+col,y+row*2,1,2,0xf5f5f5);
        x+=6;
    }
}

gt2::camera::CameraProjection Camera(float yaw,float pitch) {
    gt2::camera::CameraProjection p;
    const float sy=std::sin(yaw),cy=std::cos(yaw),sp=std::sin(pitch),cp=std::cos(pitch);
    p.right[0]=cy;p.right[2]=sy;
    p.up[0]=-sy*sp;p.up[1]=cp;p.up[2]=cy*sp;
    p.forward[0]=sy*cp;p.forward[1]=sp;p.forward[2]=-cy*cp;
    for(int axis=0;axis<3;++axis) {
        p.rows[0][axis]=p.right[axis];
        p.rows[1][axis]=-p.up[axis]*(240.f/264.f);
        p.rows[2][axis]=p.forward[axis];
    }
    return p;
}

struct RestorePreviewWindow {
    GameWindow& window;
    bool nativeMenu;
    float clear[3];
    explicit RestorePreviewWindow(GameWindow& w):window(w),nativeMenu(w.NativeMenuEnabled()) {
        std::copy_n(w.Renderer().clearColor,3,clear);
        window.EnableNativeMenu(false);window.SetDrivingActive(false);
        window.BlockMenuInputUntilRelease();
    }
    ~RestorePreviewWindow() {
        window.CancelStereoScene();window.SetDrivingActive(false);
        std::copy_n(clear,3,window.Renderer().clearColor);
        window.EnableNativeMenu(nativeMenu);window.BlockMenuInputUntilRelease();
        window.ResetPacing();
    }
};
}

bool CockpitPreviewRequested(const GameWindow& window,bool carShown) {
    return carShown && (window.KeyPressed('C') || window.PadPressed(gt2::input::ps1::kR1));
}

void AppendCockpitPreviewButton(std::vector<gt2::MenuPrim>& prims,bool quest,int x,int y) {
    Label(prims,quest?"Y  COCKPIT PREVIEW":"C  COCKPIT PREVIEW",x,y);
}

void RunCockpitPreview(GameWindow& window,const gt2::GtfsVolume& vol,uint32_t carId,int paint) {
    const auto started=std::chrono::steady_clock::now();
    RestorePreviewWindow restore(window);
    const std::string id=gt2::UnpackCarId(carId);
    window.SetTitle("gt2game - Cockpit Preview - "+id);
    auto& renderer=window.Renderer();
    renderer.clearColor[0]=.16f;renderer.clearColor[1]=.20f;renderer.clearColor[2]=.25f;
    renderer.SetNativeSceneLighting(gt2view::NativeSceneLighting{});
    renderer.ClearPaintProbes();
    // Exactly the car/body/furniture resources used by the race, without a course or race-data load.
    gt2view::SceneAssets assets(renderer,vol,true);
    assets.SetGt4Enabled(OverlayGt4ModelSet());
    gt2view::ProceduralCockpit cockpit(renderer);
    gt2view::MenuView ui(renderer);
    int slot=-1;
    std::string error;
    try {
        slot=assets.UseCar(id);
        if(slot<0 || !assets.SlotCockpitFit(slot).valid)throw std::runtime_error("no cockpit geometry");
    } catch(const std::exception& e) {
        error=e.what();
        std::printf("cockpit preview: %s unavailable: %s\n",id.c_str(),error.c_str());
    }
    float yaw=0,pitch=0;
    auto previous=std::chrono::steady_clock::now();
    std::printf("cockpit preview: enter %s paint %d, GT4 %d, no course or simulation\n",id.c_str(),paint,int(slot>=0 && assets.UsesGt4(slot)));
    bool logged=false;
    for(;;) {
        if(!window.BeginFrame())break;
        if(window.Pressed(gt2::keys::kEscape) || window.Pressed(gt2::keys::kBack) || window.MenuPressed())break;
        const auto now=std::chrono::steady_clock::now();
        const float dt=std::clamp(std::chrono::duration<float>(now-previous).count(),0.f,.05f);
        previous=now;
        if(!window.StereoAvailable()) {
            yaw+=float(int(window.Held(gt2::keys::kRight))-int(window.Held(gt2::keys::kLeft)))*dt;
            pitch=std::clamp(pitch+float(int(window.Held(gt2::keys::kUp))-int(window.Held(gt2::keys::kDown)))*dt,-1.2f,1.2f);
            if(window.KeyPressed('R'))yaw=pitch=0;
        }
        std::vector<gt2view::DrawItem> items;
        bool stereo=false;
        if(window.WillPresent())window.BeginPresent();
        if(error.empty()) {
            const auto fit=assets.SlotCockpitFit(slot);
            const auto settings=OverlayCockpitSettings(id,assets.UsesGt4(slot));
            auto p=Camera(0,0);
            PlaceCockpitEye(p,kIdentity,fit,settings);
            gt2::vr::Camera rig;
            std::copy_n(p.eye,3,rig.eye);std::copy_n(p.right,3,rig.right);
            std::copy_n(p.up,3,rig.up);std::copy_n(p.forward,3,rig.forward);
            ClipMatrixOf(p,window.StereoAspect(),.03f,rig.clip);
            gt2::vr::View tracked;
            if(window.StereoAvailable() && window.WillPresent())stereo=window.BeginStereoScene(rig,tracked);
            float mvp[16];
            if(stereo) {
                std::copy_n(kIdentity,16,mvp);
                for(int k=0;k<3;++k)mvp[12+k]=-tracked.refEye[k];
            } else {
                p=Camera(yaw,pitch);PlaceCockpitEye(p,kIdentity,fit,settings,true);FrameDesktopCockpit(p);
                ClipMatrixOf(p,renderer.AspectRatio(),.03f,mvp);
            }
            const std::array<std::array<float,3>,3> axes=stereo?
                std::array<std::array<float,3>,3>{{{1,0,0},{0,-1,0},{0,0,-1}}}:
                std::array<std::array<float,3>,3>{{{p.right[0],p.right[1],p.right[2]},
                    {-p.up[0],-p.up[1],-p.up[2]},{p.forward[0],p.forward[1],p.forward[2]}}};
            assets.UpdateCockpitReflection(slot,kIdentity,axes);
            assets.AppendCockpitBodyItems(items,slot,mvp,uint32_t(std::max(0,paint)),nullptr,settings.steeringWheel);
            cockpit.Append(items,fit,mvp,0,0,0,-1,settings.steeringWheel,
                CockpitSeatHeight(fit,settings),CockpitSeatBack(settings),nullptr,!assets.HasNativeGt4Interior(slot));
            if(settings.mirror)cockpit.AppendMirror(items,mvp,settings.mirrorScalePercent*.01f);
            if(settings.sideMirrors)assets.AppendSideMirrorItems(items,slot,mvp);
            gt2view::SortModelTransparency(items);
            for(auto& item:items)item.space=gt2view::kSpaceWorld;
            // Clear the shared mirror target to this room's background, never a previous race's view.
            gt2view::DrawItem mirrorClear;mirrorClear.space=gt2view::kSpaceMirrorSource;
            items.push_back(mirrorClear);
        }
        const size_t sceneItems=items.size();
        gt2::MenuFrame frame;
        if(!error.empty())Label(frame.prims,"COCKPIT UNAVAILABLE",24,30);
        Label(frame.prims,VrMode()?"B  BACK TO GARAGE":"ESC  BACK TO GARAGE",24,444);
        if(!VrMode())Label(frame.prims,"ARROWS  LOOK    R  RESET",24,414);
        ui.Build(frame,VrMode()?4.f/3.f:renderer.AspectRatio(),items);
        // Standalone MenuView quads live at the far depth. The inspection HUD
        // must start a new depth layer so the dashboard cannot hide Back/Look.
        if(items.size()>sceneItems)items[sceneItems].clearDepth=1;
        window.EndFrame(items,{},window.XrPaced()?std::chrono::nanoseconds(0):std::chrono::nanoseconds(16'666'667),sceneItems);
        if(!logged) {
            std::printf("cockpit preview: first frame %s stereo %d, %zu scene draws, load %.1f ms\n",id.c_str(),int(stereo),sceneItems,
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());
            logged=true;
        }
    }
    std::printf("cockpit preview: leave %s paint %d\n",id.c_str(),paint);
}
}
#else
namespace gt2game {
bool CockpitPreviewRequested(const GameWindow&,bool) { return false; }
void AppendCockpitPreviewButton(std::vector<gt2::MenuPrim>&,bool,int,int) {}
void RunCockpitPreview(GameWindow&,const gt2::GtfsVolume&,uint32_t,int) {}
}
#endif
