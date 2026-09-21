#include <android_native_app_glue.h>
#include <android/input.h>
#include <android/log.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include "android_host.h"
#include <stdexcept>
#include "game_main.h"
#include "pc_overlay.h"
#include "game_window.h"
#include "game/shell/title_options.h"
#include "game/shell/shared_vr_settings.h"
#include "platform/os/paths.h"

namespace {
inline constexpr const char* kInitialSettings = R"settings(# gt2game settings (written by the title's options; the career save keeps the original options too)
units=kmh
music_volume=240
sfx_volume=192
course_map=1
view_angle=1
chase_view=0
camera_position=0
replay_info=0
vibration=1
trigger_pedals=1
rumble_scale=100
frame_rate=display
frame_cap=72
vsync=1
render_scale=100
render_height=0
msaa=2
texture_filter=smooth
texture_mapping=perspective
scenery_detail=original
draw_distance=all
vr_stereo=1
vr_multiview=1
vr_horizon_lock=60
vr_world_scale=100
vr_render_scale=150
vr_near_mm=50
vr_seat_x=0
vr_seat_y=0
vr_seat_z=0
)settings";
inline constexpr const char* kInitialOverlay = R"settings(# Live overlay preferences; separate from earned progress.
frame_rate=display
frame_cap=72
vsync=1
render_scale=100
render_height=0
msaa=2
texture_filter=smooth
texture_mapping=perspective
scenery_detail=original
draw_distance=all
adaptive=60
rumble=100
unlock_courses=0
unlock_cars=0
vr_render_scale=150
vr_driving_mode=1
vr_motion_hand=1
vr_wheel_height=-28
vr_wheel_distance=38
vr_wheel_radius=18
vr_intro_lower_cm=200
vr_brake_reverse=1
vr_steering_stick=0
vr_binding_0=1
vr_binding_1=2
vr_binding_2=8
vr_binding_3=4
vr_binding_4=3
vr_binding_5=5
vr_binding_6=6
vr_binding_7=7
vr_hud_map=1
vr_hud_lap=1
vr_hud_records=1
vr_hud_gauges=1
vr_hud_turbo=1
vr_hud_tyres=1
vr_hud_mirror=1
vr_hud_countdown=1
vr_hud_warnings=1
vr_hud_messages=1
vr_hud_replay=1
profiler=0
vr_foveation=2
vr_refresh=72
unlock_sim_events=0
)settings";
void Command(android_app*, int32_t command) {
    if (command == APP_CMD_RESUME) gt2game::android::SetResumed(true);
    if (command == APP_CMD_PAUSE) gt2game::android::SetResumed(false);
    if (command == APP_CMD_DESTROY) gt2game::android::RequestQuit();
}
int32_t Input(android_app*, AInputEvent* event) {
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
        gt2game::android::KeyEvent(AKeyEvent_getKeyCode(event), AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN);
        return 1;
    }
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION &&
        (AInputEvent_getSource(event) & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        auto axis = [&](int id) { return AMotionEvent_getAxisValue(event, id, 0); };
        gt2game::android::MotionEvent(axis(AMOTION_EVENT_AXIS_X), axis(AMOTION_EVENT_AXIS_Y), axis(AMOTION_EVENT_AXIS_Z),
            axis(AMOTION_EVENT_AXIS_RZ), axis(AMOTION_EVENT_AXIS_LTRIGGER), axis(AMOTION_EVENT_AXIS_RTRIGGER),
            axis(AMOTION_EVENT_AXIS_HAT_X), axis(AMOTION_EVENT_AXIS_HAT_Y));
        return 1;
    }
    return 0;
}
}
void android_main(android_app* app) {
    app->onAppCmd = Command;
    app->onInputEvent = Input;
    gt2game::android::SetHost(app->activity->vm, app->activity->clazz);
    gt2::os::SetAndroidDirs(app->activity->externalDataPath, app->activity->internalDataPath);
    std::freopen(gt2::os::LogPath().string().c_str(), "w", stdout);
    std::freopen((gt2::os::SavesDir().parent_path() / "gt2game-error.log").string().c_str(), "w", stderr);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("android: new activity, lifecycle and controller state reset (0.2.0)\n");
    std::atomic<bool> finished{false};
    std::thread game;
    bool started = false;
    while (!app->destroyRequested) {
        int events = 0;
        android_poll_source* source = nullptr;
        const int result = ALooper_pollOnce(20, nullptr, &events, reinterpret_cast<void**>(&source));
        if (result >= 0 && source) source->process(app, source);
        if (!started && app->window && gt2game::android::Resumed()) {
            started = true;
            game = std::thread([&] {
                JNIEnv* env = nullptr;
                const bool attached = app->activity->vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
                try {
                    const auto root = gt2::os::DataRoot();
                    std::string mode = "arcade";
                    std::ifstream modeFile(root / "launch-mode.txt");
                    std::string requested; modeFile >> requested;
                    if (requested == "arcade" || requested == "simulation") mode = requested;
                    else if (!std::filesystem::exists(root / mode / "disc.raw2352")) mode = "simulation";
                    gt2::shell::InitializeSharedVrSettings(gt2::os::SavesDir(),kInitialSettings,kInitialOverlay);
                    gt2game::LoadOverlaySettings((gt2::os::SavesDir()/mode/"settings.txt").string());
                    bool firstLaunch = true;
                    for (;;) {
                        mode = gt2game::SelectGameDisc(root.string(), mode, true, false, {}, {}, true, firstLaunch);
                        if (mode.empty()) break;
                        firstLaunch = false;
                        { std::ofstream choice(root / "launch-mode.txt"); choice << mode; }
                        const auto saves = gt2::os::SavesDir() / mode;
                        std::filesystem::create_directories(saves);
                        // Seed only a new disc profile; updates keep the player's saved preferences.
                        if (!std::filesystem::exists(saves / "settings.txt") &&
                            !std::filesystem::exists(saves / "settings.txt.overlay")) {
                            const auto writeSettings = [&](const char* name, const char* text) {
                                std::ofstream out(saves / name);
                                out << text;
                                out.flush();
                                if (!out) throw std::runtime_error("Cannot write initial Quest settings");
                            };
                            writeSettings("settings.txt", kInitialSettings);
                            writeSettings("settings.txt.overlay", kInitialOverlay);
                        }
                        std::vector<std::string> args = {"gt2game", (root / mode).string(), "--vr",
                            "--settings", (saves / "settings.txt").string(), "--card", (saves / "card1.mcd").string()};
                        std::vector<char*> argv;
                        for (auto& arg : args) argv.push_back(arg.data());
                        const int code = gt2game::GameMain(int(argv.size()), argv.data());
                        __android_log_print(ANDROID_LOG_INFO, "GT2.Quest", "GameMain returned %d", code);
                        if (code != 0 || !gt2game::TakeGameChangeRequest()) break;
                    }
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "Startup failed: %s\n", e.what());
                    __android_log_print(ANDROID_LOG_ERROR, "GT2.Quest", "Startup failed: %s", e.what());
                }
                gt2game::ReleaseRetainedWindow();
                if (attached) app->activity->vm->DetachCurrentThread();
                finished = true;
                ALooper_wake(app->looper);
            });
        }
        if (finished) ANativeActivity_finish(app->activity);
    }
    gt2game::android::RequestQuit();
    if (game.joinable()) game.join();
}
