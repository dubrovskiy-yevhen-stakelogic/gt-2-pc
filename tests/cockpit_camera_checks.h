#pragma once
#include "../tools/gt2game/cockpit_camera.h"
#include "game/shell/shared_vr_settings.h"
#include <cmath>

template<class Check>
void CockpitCameraChecks(Check check) {
    check(gt2::shell::VrPreferences("race_view=3\nunlock_cars=1\n")=="race_view=3\n", "last camera view is shared between discs without earned progress");
    gt2::camera::RaceCamera native{};
    native.hideTarget = 1;
    check(gt2game::CockpitShown(native, true, false, false, 0), "driver camera selects the cockpit");
    check(!gt2game::CockpitShown(native, false, false, false, 0), "original driver view remains selectable");
    check(gt2game::CockpitShown(native, true, false, false, 1), "last countdown field already shows the selected driver cabin");
    check(!gt2game::CockpitShown(native, true, false, false, 60), "intro orbit does not attach the cabin before the driver camera cut");
    check(!gt2game::CockpitShown(native, true, true, false, 0), "trackside replays retain their native camera");
    check(!gt2game::CockpitShown(native, true, false, true, 0), "legacy debug camera keeps its original presentation");
    native.position = 1;
    check(!gt2game::CockpitShown(native, true, false, false, 0), "chase camera does not draw an interior over the scene");
    native.position = 0; native.lookBack = 1;
    check(gt2game::CockpitShown(native, true, false, false, 0), "look-back turns inside the same cockpit");
    // Exercise the actual intro state transitions, including the boundary where
    // StartIntro chooses RaceView while the race is still held for the countdown.
    gt2::camera::CameraConstants cameraConstants{};
    cameraConstants.viewAngle = {277,216,190};
    cameraConstants.chase = {117964,117964,353894,445644,117964,117964,353894,445644};
    gt2::camera::CameraCar introCar{};
    introCar.pose.m[0][0] = introCar.pose.m[1][1] = introCar.pose.m[2][2] = 4096;
    gt2::camera::CameraWorld introWorld{};
    introWorld.cars = std::span<const gt2::camera::CameraCar>(&introCar, 1);
    introWorld.constants = &cameraConstants;
    introWorld.holdInitial = 180;
    for (uint8_t position : {uint8_t(0),uint8_t(1),uint8_t(2)}) {
        gt2::camera::RaceCamera introCamera{};
        introCamera.position = position;
        for (uint16_t hold : {uint16_t(180),uint16_t(120),uint16_t(119),uint16_t(60),
                              uint16_t(59),uint16_t(30),uint16_t(1),uint16_t(0)}) {
            introWorld.hold = hold;
            gt2::camera::UpdateCamera(introCamera, {}, introWorld);
            const bool selectedDriver = hold < 60 && position == 0;
            check(gt2game::CockpitShown(introCamera, true, false, false, hold) == selectedDriver,
                  "cockpit follows the real intro-to-driver transition, including the final countdown second");
            check(bool(introCamera.hideTarget) == selectedDriver,
                  "the cabin appears on exactly the frame that the original driver view hides the player car");
        }
    }
    gt2::camera::CameraProjection camera{};
    camera.rows[0][0] = 1; camera.rows[1][1] = -1; camera.rows[2][2] = -1;
    const float model[] = {0,0,-1,0, 0,1,0,0, 1,0,0,0, 10,2,20,1};
    gt2view::CockpitFit fit;
    fit.eye = {-.3f, 1.0f, .2f}; fit.roofY=1.4f;
    gt2game::CockpitSettings settings;
    const std::string perCar="race_view=3\ncockpit_car_height_gt2_t2vzn=-4\ncockpit_car_height_gt4_t2vzn=8\n";
    check(gt2::shell::VrPreferences(perCar+"unlock_cars=1\n")==perCar,"both model-family seat heights survive shared disc preferences");
    fit.nativeInterior=true;
    check(std::abs(gt2game::CockpitEyeBack(fit,settings)+.15f)<1e-5f, "authored interiors move the player another 10 cm forward");
    fit.nativeInterior=false;
    check(std::abs(gt2game::CockpitEyeBack(fit,settings)-.05f)<1e-5f, "generated interiors move the player 10 cm back from the previous release");
    check(std::abs(gt2game::CockpitSeatBack(settings)-.15f)<1e-5f, "eye tuning leaves furniture at its original anchor");
    settings.seatHeightCm = 20;
    fit.roofY=1.16f;
    check(std::abs(gt2game::CockpitSeatHeight(fit,settings)-.20f)<1e-5f,
          "restored seating preserves saved height without moving the player to fit trim");
    fit.openTop=true;
    check(std::abs(gt2game::CockpitSeatHeight(fit,settings)-.20f)<1e-5f,
          "open cockpit retains full upward seat travel");
    settings.seatHeightCm=-20;fit.openTop=false;
    check(std::abs(gt2game::CockpitSeatHeight(fit,settings)+.20f)<1e-5f,
          "roof clearance does not restrict downward seat travel");
    fit.roofY=1.4f;
    settings.seatHeightCm = 10; settings.seatBackCm = 20;
    gt2game::PlaceCockpitEye(camera, model, fit, settings);
    const float rigSeat[] = {.02f,-.03f,.04f};
    const auto controls = gt2game::CockpitControlsLocal(fit, settings, 1.2f, rigSeat);
    const std::array<float,3> dashboardHub{-.3f,.55f,-.18f};
    for (int back : {-20,0,40}) for (int height : {-20,0,20}) for (float scale : {.8f,1.f,1.2f}) {
        auto adjusted=settings;adjusted.seatBackCm=back;adjusted.seatHeightCm=height;
        const auto parent=gt2game::CockpitControlsLocal(fit,adjusted,scale,rigSeat);
        const auto center=gt2game::CockpitWheelTrackingCenter(dashboardHub,parent);
        for(size_t axis=0;axis<3;++axis)
            check(std::abs(parent[12+axis]+parent[axis*5]*center[axis]-dashboardHub[axis])<1e-6f,
                  "dashboard wheel stays fixed through seat travel, tracking scale and calibration");
    }
    check(std::abs(controls[12]+.28f)<1e-5f && std::abs(controls[13]-1.07f)<1e-5f &&
          std::abs(controls[14]-.49f)<1e-5f && controls[0]==1.2f && controls[5]==1.2f && controls[10]==1.2f,
          "car-local controls preserve seat calibration and tracking scale");
    // A 90-degree suspension roll exchanges local vertical/lateral motion.
    const float rolled[] = {0,1,0,0, -1,0,0,0, 0,0,1,0, 10,2,20,1};
    const float hand[] = {.2f,-.28f,-.38f};
    float moved[3]{};
    for (int row=0;row<3;++row) {
        moved[row]=rolled[12+row];
        for (int axis=0;axis<3;++axis)
            moved[row]+=rolled[axis*4+row]*(controls[12+axis]+controls[axis*5]*hand[axis]);
    }
    check(std::abs(moved[0]-9.266f)<1e-5f && std::abs(moved[1]-1.96f)<1e-5f &&
          std::abs(moved[2]-20.034f)<1e-5f,
          "wheel and tracked hand points inherit car roll and impact translation");
    check(std::abs(camera.eye[0] - 10.45f) < 1e-5f && std::abs(camera.eye[1] - 3.1f) < 1e-5f &&
          std::abs(camera.eye[2] - 20.3f) < 1e-5f, "seat offset follows the car rotation and height");
    for (int row = 0; row < 3; ++row) {
        float origin = camera.rows[row][3];
        for (int axis = 0; axis < 3; ++axis) origin += camera.rows[row][axis] * camera.eye[axis];
        check(std::abs(origin) < 1e-5f, "desktop clip projection uses the same seated origin as stereo");
    }
    camera.right[0] = 1; camera.up[1] = 1; camera.forward[2] = -1;
    gt2game::FrameDesktopCockpit(camera);
    check(camera.forward[1] < -.13f && std::abs(camera.H - 180.f) < 1e-5f, "monitor framing includes the dashboard below the road");
    for (int row = 0; row < 3; ++row) {
        float origin = camera.rows[row][3];
        for (int axis = 0; axis < 3; ++axis) origin += camera.rows[row][axis] * camera.eye[axis];
        check(std::abs(origin) < 1e-5f, "monitor framing rotates about the seated eye");
    }
    const auto shared = gt2::shell::VrPreferences("cockpit=0\ncockpit_wheel=0\ncockpit_mirror=0\ncockpit_side_mirrors=0\ncockpit_mirror_scale=35\ncockpit_seat_height=4\ncockpit_seat_back=-2\nunlock_cars=1\n");
    check(shared == "cockpit=0\ncockpit_wheel=0\ncockpit_mirror=0\ncockpit_side_mirrors=0\ncockpit_mirror_scale=35\ncockpit_seat_height=4\ncockpit_seat_back=-2\n", "cockpit preferences are shared without copying disc cheats");
}
