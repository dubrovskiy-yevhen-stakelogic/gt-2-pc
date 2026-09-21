#include "platform/xr/vr_driving.h"
#include "platform/xr/vr_controls.h"
#include "gt2formats/replay.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>
using namespace gt2::vr;
static int checks=0;
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);++checks;}
int main(){try{
    MenuTriggers menu;menu.Begin(0,1);
    Check(menu.Update(0,1)==0,"held trigger must not change settings on menu entry");
    Check(menu.Update(0,.1f)==0 && menu.Update(0,.7f)==1,"right trigger advances once after release");
    Check(menu.Update(0,.8f)==0 && menu.Update(0,.5f)==0,"squeeze and mid-threshold noise do not repeat");
    Check(menu.Update(.8f,.5f)==-1,"left trigger decreases");
    menu.Update(0,0);Check(menu.Update(1,1)==0,"simultaneous triggers cancel");
    DrivingSettings s;s.mode=1;TrackedControllers in;
    in.gripValid[0]=in.gripValid[1]=true;
    for(int h=0;h<2;++h){in.gripPose[h].position[0]=(h==0?-.18f:.18f);in.gripPose[h].position[1]=-.28f;in.gripPose[h].position[2]=-.38f;}
    DrivingController drive;
    in.grip[0]=1;Check(drive.Update(in,s,true)==0 && drive.Grabbed(0),"left rim grabs without steering jump");
    auto rotate=[&](int hand,float a){float r=hand==0?-.18f:.18f;in.gripPose[hand].position[0]=r*std::cos(a);in.gripPose[hand].position[1]=-.28f+r*std::sin(a);};
    rotate(0,.4f);float one=drive.Update(in,s,true);Check(one<-.2f,"counter-clockwise wheel steers left");
    in.gripPose[0].position[0]=0;in.gripPose[0].position[1]=-.28f;
    Check(std::abs(drive.Update(in,s,true)-one)<.001f,"inward tracking holds the previous wheel angle");
    rotate(0,.9f);Check(std::abs(drive.Update(in,s,true)-one)<.001f,"returning to rim reseats without jumping");
    rotate(0,2.9f);Check(std::abs(drive.Update(in,s,true)-one)<.001f,"impossible single-hand jump is rejected");
    drive.Reset();rotate(0,0);rotate(1,0);in.grip[0]=in.grip[1]=1;
    drive.Update(in,s,true);rotate(0,-.4f);rotate(1,-.4f);
    float two=drive.Update(in,s,true);Check(two>.2f && drive.Grabbed(0) && drive.Grabbed(1),"two-hand chord steers right");
    in.grip[1]=0;Check(std::abs(drive.Update(in,s,true)-two)<.001f,"two hands to one retains steering");
    in.gripValid[0]=false;Check(drive.Update(in,s,true)==0 && !drive.Grabbed(0),"tracking loss releases wheel");
    s.mode=2;in={};in.gripValid[1]=in.aimValid[1]=true;
    Check(drive.Update(in,s,true)==0 && !drive.MotionCalibrated(),"motion waits for grip");
    in.grip[1]=.8f;Check(drive.Update(in,s,true)==0 && drive.MotionCalibrated(),"grip sets wrist centre");
    auto roll=[&](float a){in.gripPose[1]={};in.gripPose[1].orientation[2]=std::sin(a/2);in.gripPose[1].orientation[3]=std::cos(a/2);};
    constexpr float pi=3.14159265358979323846f;
    roll(-40*pi/180);float right=drive.Update(in,s,true);
    roll(40*pi/180);float left=drive.Update(in,s,true);
    Check(right>.45f && std::abs(left+right)<.0001f,"wrist rolls steer symmetrically left and right");
    roll(pi/2);Check(drive.Update(in,s,true)==-1,"left wrist lock clamps at wheel lock");
    roll(-pi/2);Check(drive.Update(in,s,true)==1,"right wrist lock clamps at wheel lock");
    in.trigger[1]=1;float withGas=drive.Update(in,s,true);in.trigger[1]=0;
    Check(drive.Update(in,s,true)==withGas,"coasting retains wrist steering");
    in.grip[1]=.4f;Check(drive.Update(in,s,true)==withGas,"grip hysteresis tolerates noise");
    roll(0);in.gripPose[1].orientation[1]=std::sin(pi/6);in.gripPose[1].orientation[3]=std::cos(pi/6);
    Check(std::abs(drive.Update(in,s,true))<.0001f,"yaw without wrist roll does not steer");
    in.grip[1]=0;Check(drive.Update(in,s,true)==0 && !drive.MotionCalibrated(),"grip release releases motion");
    in.grip[1]=1;Check(drive.Update(in,s,true)==0,"regrip establishes current wrist neutral");
    in.gripValid[1]=false;Check(drive.Update(in,s,true)==0 && !drive.MotionCalibrated(),"tracking loss releases motion");
    Check(drive.Update(in,s,false)==0 && !drive.MotionCalibrated(),"pause clears wrist reference");
    in={};s.motionHand=0;in.gripValid[0]=true;in.grip[0]=1;drive.Update(in,s,true);
    in.gripPose[0].orientation[2]=std::sin(-pi/9);in.gripPose[0].orientation[3]=std::cos(-pi/9);
    Check(std::abs(drive.Update(in,s,true)-right)<.0001f,"left controller wrist gives same steering as right");
    for(auto& q:in.gripPose[0].orientation)q=-q;
    Check(std::abs(drive.Update(in,s,true)-right)<.0001f,"equivalent quaternion sign does not reverse wrist steering");
    drive.Reset();in={};s.motionHand=1;in.gripValid[1]=in.aimValid[1]=true;in.grip[1]=1;
    const float sy=std::sin(pi/4),cy=std::cos(pi/4),sz=std::sin(-pi/9),cz=std::cos(-pi/9);
    in.gripPose[1].orientation[1]=in.aimPose[1].orientation[1]=sy;
    in.gripPose[1].orientation[3]=in.aimPose[1].orientation[3]=cy;
    drive.Update(in,s,true);
    in.gripPose[1].orientation[0]=sy*sz;in.gripPose[1].orientation[1]=sy*cz;
    in.gripPose[1].orientation[2]=cy*sz;in.gripPose[1].orientation[3]=cy*cz;
    Check(std::abs(drive.Update(in,s,true)-right)<.0001f,"wrist steering uses the grabbed forearm axis at a sideways heading");
    gt2::LogicalPad pad;pad.analog=1;pad.steerAxis=192;
    auto replay=gt2::FrameOfPad(pad);uint16_t pedals[16]{};
    Check(gt2::PadOfFrame(replay,pedals).steer==-2048,"physical steering sign reaches native physics and replay");
    ControlBindings bindings;gt2::input::Ps1PadFrame raw;raw.pressureR2=180;raw.pressureL2=80;
    uint32_t held=0;gt2::LogicalPad mapped;
    ApplyControlBindings(mapped,held,raw,bindings);
    Check(mapped.throttle==180 && mapped.brake==80 && (mapped.analog&12)==12,"mapped pedals preserve analog travel");
    bindings.source[0]=5;raw.pressureR2=255;raw.buttons=gt2::input::ps1::kSquare;
    ApplyControlBindings(mapped,held,raw,bindings);Check(mapped.throttle==255,"custom X accelerator works");
    raw.buttons=0;ApplyControlBindings(mapped,held,raw,bindings);Check(mapped.throttle==0,"old accelerator binding is inactive");
    auto desktop = DesktopBindings();
    raw.pressure = true; raw.pressureR2 = 143; raw.pressureL2 = 91;
    raw.buttons = gt2::input::ps1::kR1; raw.analog[2] = 200;
    ApplyDesktopControlBindings(mapped,held,raw,desktop);
    Check(mapped.throttle==143 && mapped.brake==91 && mapped.steerAxis==200 && (held & gt2::kPadShiftUp),"desktop analog pedals, steering and right bumper");
    desktop.source[0]=4; raw.buttons=gt2::input::ps1::kCircle;
    ApplyDesktopControlBindings(mapped,held,raw,desktop);
    Check(mapped.throttle==255,"desktop Circle binding uses Circle, not the Touch B mapping");
    desktop.steeringStick=1; raw.analog[0]=31; raw.buttons=0;
    ApplyDesktopControlBindings(mapped,held,raw,desktop);
    Check(mapped.throttle==0 && mapped.steerAxis==31,"desktop custom release and right steering stick");
    BrakeReverse reverse;gt2::LogicalPad p;p.analog=12;p.brake=170;p.buttons=gt2::kPadBrake;
    reverse.Apply(p,8,true);Check(!(p.buttons & gt2::kPadReverse) && p.brake==170,"L2 brakes while moving forward");
    reverse.Apply(p,.1f,true);Check((p.buttons & gt2::kPadReverse) && !p.brake,"L2 selects reverse only after stopping");
    p={};p.analog=12;p.brake=170;reverse.Apply(p,-3,true);Check((p.buttons & gt2::kPadReverse) && !p.brake,"L2 continues reversing");
    p={};p.analog=12;p.throttle=190;reverse.Apply(p,-3,true);Check(p.brake==190 && !p.throttle,"gas brakes reverse motion first");
    p={};p.analog=12;p.throttle=190;reverse.Apply(p,-.1f,true);Check(p.throttle==190 && !(p.buttons & gt2::kPadReverse),"gas selects forward near rest");
    p={};p.buttons=gt2::kPadReverse;reverse.Apply(p,-2,true);Check((p.buttons & gt2::kPadReverse)!=0,"explicit reverse binding remains available");
    p={};p.brake=100;reverse.Apply(p,0,false);Check(p.brake==100 && !(p.buttons & gt2::kPadReverse),"manual transmission retains original reverse behavior");
    std::printf("%d driving/menu checks passed\n",checks);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
