#include "platform/xr/vr_driving.h"
#include "vr_input_merge_checks.h"
#include "platform/xr/vr_controls.h"
#include "gt2formats/replay.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>
using namespace gt2::vr;
static int checks=0;
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);++checks;}
int main(){try{
    VrInputMergeChecks(Check);
    {
        DrivingSettings anchored;anchored.mode=1;
        anchored.cockpitWheelCenter=std::array<float,3>{.12f,-.55f,-.26f};
        TrackedControllers hand;hand.gripValid[0]=true;hand.grip[0]=1;
        hand.gripPose[0].position[0]=.12f-.18f;
        hand.gripPose[0].position[1]=-.55f;hand.gripPose[0].position[2]=-.26f;
        DrivingController controller;
        Check(controller.Update(hand,anchored,true)==0 && controller.Grabbed(0),"grip acquires the fixed dashboard rim after seat movement");
        hand.gripPose[0].position[0]=.12f-.18f*std::cos(.4f);
        hand.gripPose[0].position[1]=-.55f-.18f*std::sin(.4f);
        Check(controller.Update(hand,anchored,true)<-.2f,"single hand steering rotates around the dashboard anchor, including lateral calibration");
        anchored.cockpitWheelCenter.reset();
        Check(DrivingWheelCenter(anchored)==std::array<float,3>{0,-.28f,-.38f},"non-cockpit wheel retains its existing calibration");
    }
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
    // Latching must survive releasing the button, never repeat on a held
    // squeeze, and never reattach on tracking recovery or leaving a menu.
    drive.Reset();s={};s.mode=1;s.toggleGrip=true;in={};
    for(int h=0;h<2;++h){in.gripValid[h]=true;in.gripPose[h].position[0]=h? .18f:-.18f;in.gripPose[h].position[1]=-.28f;in.gripPose[h].position[2]=-.38f;}
    drive.Update(in,s,true);in.grip[0]=1;drive.Update(in,s,true);
    Check(drive.Grabbed(0),"toggle press grabs rim");
    in.grip[0]=0;drive.Update(in,s,true);rotate(0,.4f);one=drive.Update(in,s,true);
    Check(drive.Grabbed(0) && one<-.2f,"toggle retains steering with grip released");
    s.sensitivity=50;const float gentle=drive.Update(in,s,true);
    Check(std::abs(gentle)<std::abs(one) && std::abs(drive.Angle()-.4f)<.001f,"sensitivity softens output without moving visual wheel");
    s.sensitivity=200;Check(std::abs(drive.Update(in,s,true))>std::abs(one),"higher sensitivity increases response");
    s.sensitivity=100;in.grip[1]=1;drive.Update(in,s,true);in.grip[1]=0;
    Check(std::abs(drive.Update(in,s,true)-one)<.001f && drive.Grabbed(1),"second latched hand retains angle");
    in.grip[1]=1;Check(std::abs(drive.Update(in,s,true)-one)<.001f && !drive.Grabbed(1),"second press releases only that hand without jump");
    drive.Update(in,s,true);Check(!drive.Grabbed(1),"held toggle release does not regrab");
    in.grip[0]=1;Check(drive.Update(in,s,true)==0 && !drive.Grabbed(0),"last hand toggle releases steering");
    in.grip[0]=0;drive.Update(in,s,true);rotate(0,0);in.grip[0]=1;drive.Update(in,s,true);
    in.gripValid[0]=false;drive.Update(in,s,true);in.gripValid[0]=true;drive.Update(in,s,true);
    Check(!drive.Grabbed(0),"tracking recovery with held grip cannot latch");
    drive.Update(in,s,false);drive.Update(in,s,true);Check(!drive.Grabbed(0),"menu exit with held grip cannot latch");
    in.grip[0]=0;drive.Update(in,s,true);in.grip[0]=1;drive.Update(in,s,true);
    rotate(0,.01f);Check(drive.Update(in,s,true)==0,"default dead zone filters tiny movement");
    s.deadZonePercent=0;Check(drive.Update(in,s,true)<0,"zero dead zone permits micro corrections");
    for(float a:{.3f,.6f,.9f,1.2f,1.5f}){rotate(0,a);drive.Update(in,s,true);}
    s.sensitivity=50;Check(drive.Update(in,s,true)==-1,"low sensitivity still reaches full lock");
    // The same hand movement must now take time to reach the game axis,
    // independently of display rate and duplicate samples within one frame.
    for(int hz:{72,90,120}) {
        DrivingController timed;DrivingSettings setting;setting.mode=1;setting.sensitivity=50;setting.deadZonePercent=0;
        TrackedControllers hands;
        auto pose=[&](float angle){for(int h=0;h<2;++h){hands.gripValid[h]=true;hands.grip[h]=1;
            hands.gripPose[h].position[0]=(h?.18f:-.18f)*std::cos(angle);
            hands.gripPose[h].position[1]=-.28f+(h?.18f:-.18f)*std::sin(angle);
            hands.gripPose[h].position[2]=-.38f;}};
        pose(0);timed.Update(hands,setting,true,0);
        pose(DrivingController::maxWheelAngle);float output=0;
        for(int tick=1;tick<=hz/2;++tick){
            output=timed.Update(hands,setting,true,1.f/hz);
            Check(std::abs(output+(1.f-std::exp(-float(tick)/(hz*.18f))))<.00001f,"50 percent smooths actual steering independently of refresh rate");
            Check(timed.Update(hands,setting,true,0)==output,"duplicate tracking sample cannot accelerate response");
            Check(std::abs(timed.Angle()-DrivingController::maxWheelAngle)<.00001f,"visible rim follows hands immediately");
        }
        Check(output<-.93f && output>-.95f,"half a second reaches about 94 percent of the target");
        for (int tick=0;tick<hz*2;++tick) output=timed.Update(hands,setting,true,1.f/hz);
        Check(std::abs(output+1)<.00001f,"steady gentle steering still reaches full lock");
        pose(-DrivingController::maxWheelAngle);
        Check(timed.Update(hands,setting,true,1.f/hz)<-.8f,"sudden countersteer is smoothed too");
        hands.gripValid[0]=hands.gripValid[1]=false;
        Check(timed.Update(hands,setting,true,1.f/hz)==0,"tracking loss never retains filtered steering");
    }
    // Small corrections were the missing case: the old 4-units/s limiter
    // delivered a 0.2-radian hand turn immediately. Check the actual physics
    // pad after fine-steering encoding, not just the visible wheel angle.
    for (int sensitivity : {50,75,100}) for (int hz : {72,90,120}) {
        DrivingSettings setting;setting.mode=1;setting.sensitivity=sensitivity;setting.deadZonePercent=0;
        DrivingController filtered,reference;auto immediate=setting;immediate.sensitivity=100;
        TrackedControllers hands;
        auto pose=[&](float a){for(int h=0;h<2;++h){hands.gripValid[h]=true;hands.grip[h]=1;
            hands.gripPose[h].position[0]=(h?.18f:-.18f)*std::cos(a);
            hands.gripPose[h].position[1]=-.28f+(h?.18f:-.18f)*std::sin(a);
            hands.gripPose[h].position[2]=-.38f;}};
        auto physical=[&](float value) {
            gt2::LogicalPad p;p.analog=1;p.fineSteering=true;
            p.steerAxis=uint16_t(std::lround(2048+value*(value<0?2048:2047)));
            return gt2::PadOfFrame(gt2::FrameOfPad(p),pedals).steer;
        };
        pose(0);filtered.Update(hands,setting,true,0);reference.Update(hands,immediate,true,0);
        pose(.2f);
        const float first=filtered.Update(hands,setting,true,1.f/hz);
        const float direct=reference.Update(hands,immediate,true,1.f/hz);
        const float target=-std::pow(.2f/DrivingController::maxWheelAngle,100.f/sensitivity);
        if(sensitivity<100) {
            Check(std::abs(first)<std::abs(target)*.16f,"small correction must not bypass low-sensitivity smoothing");
            Check(std::abs(physical(first))<std::abs(physical(target))*.2f,"native physics receives the smoothed small correction");
        } else Check(first==direct,"100 percent keeps immediate steering");
        Check(std::abs(filtered.Angle()-.2f)<.00001f,"small-turn smoothing never delays the visible wheel");
        float settled=first;
        for(int tick=0;tick<hz*3;++tick) settled=filtered.Update(hands,setting,true,1.f/hz);
        Check(std::abs(settled-target)<.00001f,"smoothing retains the configured held steering angle");
        pose(0);const float returning=filtered.Update(hands,setting,true,1.f/hz);
        Check(sensitivity==100 ? returning==0 : returning<settled*.8f,"held return to centre is smooth, 100 percent remains direct");
        hands.gripValid[0]=hands.gripValid[1]=false;
        Check(filtered.Update(hands,setting,true,1.f/hz)==0,"tracking loss immediately clears the smoothed command");
    }
    // Every fine step must survive both RLE and the byte-only ghost cache.
    gt2::ReplayStream fineStream(60000);std::vector<gt2::ReplayFrame> fineFrames;
    pad={};pad.analog=13;pad.fineSteering=true;pad.throttle=192;pad.brake=64;pad.buttons=gt2::kPadReverse|gt2::kPadShiftUp;
    for(int i=0;i<4096;++i){
        pad.steerAxis=uint16_t(i);auto f=gt2::FrameOfPad(pad);fineFrames.push_back(f);
        fineStream.Record(f);fineStream=gt2::ReplayStream::FromBytes(fineStream.Bytes());
        auto p=gt2::PadOfFrame(f,pedals);
        Check(p.flags==7 && p.reverse==1 && p.shift==1,"fine steering retains original pad flags and transmission");
        if(i) Check(p.steer<gt2::PadOfFrame(fineFrames[size_t(i-1)],pedals).steer,"all 4096 fine steering steps are distinct and monotonic");
    }
    pad={};pad.analog=1;pad.steerAxis=192;fineFrames.push_back(gt2::FrameOfPad(pad));fineStream.Record(fineFrames.back());
    fineStream.Record({}); // The original stream reader consumes one final end tick.
    fineStream.End(false);fineStream.Init(true,fineStream.Capacity());
    for(const auto& expected:fineFrames){gt2::ReplayFrame f;fineStream.Read(f);Check(f==expected,"fine replay round trip including return to original pad");fineStream=gt2::ReplayStream::FromBytes(fineStream.Bytes());}
    Check(gt2::PadOfFrame(fineFrames.front(),pedals).steer==4096 && gt2::PadOfFrame(fineFrames[2048],pedals).steer==0 && gt2::PadOfFrame(fineFrames[4095],pedals).steer==-4096,"fine axis has exact centre and symmetric locks");
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
    reverse.Reset();p={};p.buttons=gt2::kPadBrake;p.throttle=200;
    reverse.Apply(p,8,true);Check(p.buttons==gt2::kPadBrake,"keyboard brakes at speed; inactive throttle axis ignored");
    reverse.Apply(p,0,true);Check((p.buttons & gt2::kPadReverse) && !p.brake,"Down selects reverse at rest without a controller");
    p={};p.buttons=gt2::kPadBrake;reverse.Apply(p,-3,true);Check((p.buttons & gt2::kPadReverse)!=0,"holding Down keeps reverse engaged");
    p={};reverse.Apply(p,-3,true);Check(!p.buttons && !p.throttle && !p.brake,"releasing Down coasts without unintended acceleration");
    p={};p.buttons=gt2::kPadThrottle;reverse.Apply(p,-3,true);Check(p.brake==255 && !p.throttle && (p.analog & 8),"Up brakes before returning from reverse");
    p={};p.buttons=gt2::kPadThrottle;reverse.Apply(p,0,true);Check(p.buttons==gt2::kPadThrottle,"Up restores forward at rest");
    p={};p.buttons=gt2::kPadBrake;reverse.Apply(p,0,false);Check(p.buttons==gt2::kPadBrake,"manual keyboard brake remains brake");
    std::printf("%d driving/menu checks passed\n",checks);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
