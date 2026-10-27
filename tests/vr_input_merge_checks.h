#pragma once
#include "platform/xr/vr_input_merge.h"

template<class Check> void VrInputMergeChecks(Check check) {
    using namespace gt2;using namespace gt2::vr;
    input::Ps1PadFrame gamepad,touch;
    gamepad.type=touch.type=input::kTypeAnalog;
    gamepad.buttons=input::ps1::kCross;gamepad.analog[2]=20;
    touch.buttons=input::ps1::kStart;
    auto menu=MergeMenuPads(gamepad,touch);
    check(menu.buttons==(input::ps1::kCross|input::ps1::kStart) && menu.analog[2]==20,"Touch menu buttons coexist with a held gamepad stick and button");
    check(MergeMenuPads(gamepad,{}).buttons==gamepad.buttons,"disconnected Touch does not disconnect the desktop pad");
    check(MergeMenuPads({},touch).buttons==touch.buttons,"Touch menus work without a desktop pad");
    for (int axis=0;axis<=255;++axis) {
        LogicalPad desktop;desktop.analog=1|4|8;desktop.steerAxis=uint16_t(axis);desktop.throttle=170;
        LogicalPad xr;xr.analog=1;xr.steerAxis=128;
        uint32_t xrHeld=0,held=0;
        touch.pressureR2=0;touch.pressureL2=95;touch.buttons=input::ps1::kR1;
        ApplyControlBindings(xr,xrHeld,touch,{});
        MergeDrivingInput(desktop,held,xr,xrHeld);
        check(desktop.steerAxis==axis && desktop.throttle==170 && desktop.brake==95 && (held&0x100),
            "neutral Touch keeps every gamepad steering value and throttle, while Touch brake and camera remain usable");
    }
    LogicalPad desktop;desktop.buttons=kPadLeft|kPadThrottle;desktop.throttle=0;
    LogicalPad xr;xr.analog=1|4|8;xr.steerAxis=200;xr.throttle=90;
    uint32_t held=0;
    MergeDrivingInput(desktop,held,xr,0);
    check(!(desktop.analog&1) && (desktop.buttons&kPadLeft) && desktop.throttle==255,"digital steering and accelerator survive lower analog Touch input");
    desktop={};held=0;MergeDrivingInput(desktop,held,xr,0);
    check((desktop.analog&1) && desktop.steerAxis==200 && desktop.throttle==90,"Touch drives while the gamepad is neutral or disconnected");
    desktop={};held=0;MergeDrivingInput(desktop,held,{},0);
    check(desktop.throttle==0 && desktop.brake==0 && held==0 && desktop.buttons==0,"releasing both inputs leaves no latched controls");
    auto pcBindings=DesktopBindings();pcBindings.source[0]=3;pcBindings.source[6]=10;
    LogicalPad pc;uint32_t pcHeld=0;
    gamepad.buttons=input::ps1::kCross|input::ps1::kR1;
    ApplyDesktopControlBindings(pc,pcHeld,gamepad,pcBindings);
    touch.buttons=0;touch.pressureR2=0;touch.pressureL2=0;
    xr={};uint32_t xrHeld=0;ApplyControlBindings(xr,xrHeld,touch,{});
    MergeDrivingInput(pc,pcHeld,xr,xrHeld);
    check(pc.throttle==255 && (pcHeld&0x100),"PC custom accelerator and camera bindings are not overwritten by the Touch layout");
}
