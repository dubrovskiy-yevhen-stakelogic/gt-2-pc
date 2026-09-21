#include "platform/xr/vr_rig.h"
#include "gt2view/billboard.h"
#include "gt2view/draw_culling.h"
#include "gt2view/decoded_texture_cache.h"
#include "gt2view/foveation.h"
#include "gt2view/scenery_visibility.h"
#include "../tools/gt2game/android_activity_state.h"
#include "../tools/gt2game/frame_profiler.h"
#include "../tools/gt2game/xr_field_pacing.h"
#include "game/audio/audio_resampler.h"
#include <vector>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace gt2::vr;
static void Check(bool condition, const char* label) { if (!condition) throw std::runtime_error(label); }
int main() {
    try {
        gt2game::android::ActivityState activity;
        activity.resumed = true; activity.quit = true; activity.keyDowns = {13}; activity.down[13] = true;
        activity.Reset();
        Check(!activity.quit && !activity.resumed && activity.keyDowns.empty() && !activity.down[13], "activity relaunch resets exit and input state");
        gt2view::DrawBoundsCache boundsCache;
        struct Vertex { float pos[3]; } vertices[] = {{{-0.2f, -0.2f, -2}}, {{0.2f, 0.2f, -1}}, {{10, 10, -2}}, {{11, 11, -1}}};
        const auto box = boundsCache.Get(vertices, 0, 2);
        float identity[16]{}; for (int i = 0; i < 4; ++i) identity[i * 5] = 1;
        float eyesVP[2][16];
        Projection({-0.7f, 0.7f, 0.7f, -0.7f}, 0.05f, eyesVP[0]);
        std::copy(eyesVP[0], eyesVP[0] + 16, eyesVP[1]);
        Check(gt2view::BoundsInStereo(box, eyesVP, identity), "culling keeps visible bounds");
        const auto side = boundsCache.Get(vertices, 2, 2);
        Check(!gt2view::BoundsInStereo(side, eyesVP, identity), "culling rejects offscreen bounds");
        float translate[16]; std::copy(identity, identity + 16, translate); translate[12] = -10; translate[13] = -10;
        Check(gt2view::BoundsInStereo(side, eyesVP, translate), "culling uses object transform");
        translate[12] = 0; translate[13] = 0; translate[14] = 4;
        Check(!gt2view::BoundsInStereo(box, eyesVP, translate), "culling rejects geometry behind camera");
        std::copy(identity, identity + 16, translate); translate[12] = 2;
        eyesVP[1][12] = -2 * eyesVP[1][0];
        Check(!gt2view::BoundsInView(box, eyesVP[0], translate) && gt2view::BoundsInStereo(box, eyesVP, translate), "keep geometry visible in only one eye");
        gt2view::DrawBounds spanning{{-100, -100, -100}, {100, 100, 100}, true};
        Check(gt2view::BoundsInStereo(spanning, eyesVP, identity), "keep bounds enclosing camera and near plane");
        vertices[1].pos[0] = 3;
        boundsCache.Invalidate(1, 1);
        Check(boundsCache.Get(vertices, 0, 2).hi[0] == 3, "partial vertex upload invalidates enclosing cached range");
        Check(boundsCache.Get(vertices, 2, 2).lo[0] == 10, "unrelated cached range is preserved");
        // Model a runtime whose predicted display time leads the CPU by two frames.
        // 600 fields must still yield ten seconds of physics and refresh-rate rendering.
        using Clock = std::chrono::steady_clock;
        for (int hz : {72, 80, 90, 120}) {
            int presented = 0, betweenFrames = 0, fieldEffects = 0, physicsSteps = 0;
            Clock::time_point cpuNow{};
            for (int field = 1; field <= 600; ++field) {
                if (field % 2 == 0) ++physicsSteps;
                const auto deadline = Clock::time_point(std::chrono::nanoseconds(int64_t(field) * 1'000'000'000 / 60));
                gt2game::PresentXrField(deadline, Clock::time_point{}, [&](bool between) {
                    if (between) ++betweenFrames; else ++fieldEffects;
                    const auto display = Clock::time_point(std::chrono::nanoseconds(int64_t(++presented) * 1'000'000'000 / hz));
                    cpuNow = display - std::chrono::nanoseconds(2'000'000'000 / hz);
                    return display;
                });
            }
            Check(presented == hz * 10 && betweenFrames == hz * 10 - 600, "XR renders every 72/80/90/120 Hz display slot");
            Check(fieldEffects == 600 && physicsSteps == 300, "XR preserves field effects and 30 Hz physics");
            Check(std::abs(std::chrono::duration<double>(cpuNow.time_since_epoch()).count() - 10) < 0.04, "predicted-time lead does not speed up game clock");
        }
        for (int hz : {24, 30, 45, 50, 72, 90, 120}) {
            using Clock = std::chrono::steady_clock;
            Clock::time_point last;
            int frames = 0, effects = 0, physics = 0;
            for (int field = 1; field <= 600; ++field) {
                if ((field & 1) == 0) ++physics;
                const auto deadline = Clock::time_point(std::chrono::nanoseconds(int64_t(field) * 1'000'000'000 / 60));
                const bool rendered = gt2game::PresentXrField(deadline, last, [&](bool between) {
                    if (!between) ++effects;
                    last = Clock::time_point(std::chrono::nanoseconds(int64_t(++frames) * 1'000'000'000 / hz));
                    return last;
                });
                if (!rendered) ++effects;
            }
            Check(frames == hz * 10 && physics == 300 && effects == 600, "slow GPU does not slow physics or skip field side effects");
        }
        for (int level = 0; level <= 3; ++level) {
            Check(gt2view::FoveationRate(50, 50, 100, 100, level) == 0, "foveation preserves central pixel rate");
            Check(gt2view::FoveationRate(0, 0, 100, 100, level) == (level ? 5 : 0), "foveation affects only enabled peripheral rate");
        }
        Check(gt2view::ExtendedScenery(400.0 * 400, 500, false), "500m scenery bypasses near-only PS1 masks and LOD cutoffs");
        Check(!gt2view::ExtendedScenery(600.0 * 600, 500, false), "outside extended range retains original scenery selection");
        Check(!gt2view::ExtendedScenery(100, 0, false), "original distance keeps PS1 visibility rules");
        Check(gt2view::ExtendedScenery(1e10, -1, false) && gt2view::ExtendedScenery(1e10, 0, true), "entire course and max detail keep all scenery");
        gt2view::DecodedTextureCache textures;
        std::vector<uint32_t> vram(1024 * 512);
        for (int y = 0; y < 256; ++y) for (int x = 0; x < 64; ++x) vram[size_t(y) * 1024 + x] = 0x1111;
        vram[513] = 0x7c1f;
        textures.Begin(); textures.Prepare(0, 512, vram.data(), 512);
        Check(textures.hits == 1 && textures.uploads.size() == 1 && textures.pixels[0] == 0xffff00ff, "cache expands 4-bit palette RGB while retaining recoverable 5-bit channels");
        textures.Begin(); textures.Prepare(0, 512, vram.data(), 512);
        Check(textures.uploads.empty(), "unchanged texture page is not decoded or uploaded again");
        vram[513] = 0x8001; textures.Invalidate(0, 1);
        textures.Begin(); textures.Prepare(0, 512, vram.data(), 512);
        bool stp = false; for (auto e : textures.table) if (e[3] && e[1] == 512) stp = (e[2] & 65536) != 0;
        Check(stp && textures.pixels[0] == 0xff000008, "palette edit invalidates cache and preserves STP class");
        vram[514] = 2; vram[0] = 0x1211; textures.Invalidate(0, 1);
        textures.Begin(); textures.Prepare(0, 512, vram.data(), 512);
        Check(textures.hits == 1 && textures.uploads.size() == 2, "mixed STP pages use two hardware-filtered layers");
        Check(textures.pixels[0] == 0 && textures.pixels[gt2view::DecodedTextureCache::kTexels] == 0xff000008,
              "STP pixels have zero coverage in the opaque filter layer");
        Check(textures.pixels[2] == 0xff000010 && textures.pixels[gt2view::DecodedTextureCache::kTexels + 2] == 0,
              "opaque pixels have zero coverage in the STP filter layer");
        textures.Invalidate(0, 512);
        for (int y = 0; y < 256; ++y) for (int x = 0; x < 128; ++x) vram[size_t(y) * 1024 + x] = 0x0101;
        vram[513] = 0x03e0;
        textures.Begin(); textures.Prepare(0, 512 | (1u << 28), vram.data(), 512);
        Check(textures.hits == 1 && textures.pixels[0] == 0xff00ff00, "cache decodes 8-bit palette pages");
        textures.Begin(); textures.Prepare(256u << 16, 2u << 28, vram.data(), 512);
        Check(textures.pixels[2 * gt2view::DecodedTextureCache::kTexels] == 0, "direct-colour zero texels retain transparent coverage");
        gt2game::FrameProfiler profiler;
        for (int i = 0; i <= 180; ++i) profiler.Record(double(i) / 90);
        Check(std::abs(profiler.fps - 90) < 0.001 && std::abs(profiler.frameMs - 1000.0 / 90) < 0.001, "profiler counts application frames");
        profiler.Record(10);
        Check(profiler.fps == 0, "profiler resets after suspension");
        for (int i = 1; i <= 20; ++i) profiler.Record(10 + double(i) * 0.025);
        Check(std::abs(profiler.fps - 40) < 0.001 && std::abs(profiler.maxMs - 25) < 0.001, "profiler frame time and maximum");
        profiler.Reset();
        double clock = 0; profiler.Record(clock);
        for (int i = 0; i < 255; ++i) { clock += i == 210 ? 0.04 : 1.0 / 90; profiler.Record(clock); }
        Check(profiler.fps > 80 && profiler.lowFps < 60 && profiler.recentMaxMs >= 39.9,
              "one 40ms hitch remains visible despite high average FPS");
        const auto board=gt2view::BillboardRight({0,0,0},{0,0,10},{1,0,0});
        Check(board[0]==-1 && board[2]==0,"VR tree faces viewer position");
        Check(board==gt2view::BillboardRight({0,2,0},{0,0,10},{0,0,1}),"tree ignores head yaw and viewer height");
        Check(gt2view::BillboardRight({0,0,0},{0,0,0},{1,0,0})==std::array<float,3>{1,0,0},"coincident billboard has a stable fallback");
        Camera orbit; orbit.eye[0] = 120; orbit.eye[1] = 32.8f;
        orbit.eye[2] = 17; const auto originalOrbit=orbit;
        LowerIntroCamera(orbit,0,2);
        Check(orbit.eye[0]==120 && orbit.eye[2]==17 && std::abs(orbit.eye[1]-30.8f)<.0001f &&
              std::equal(orbit.forward,orbit.forward+3,originalOrbit.forward) &&
              std::equal(orbit.up,orbit.up+3,originalOrbit.up) && std::equal(orbit.right,orbit.right+3,originalOrbit.right),
              "intro preserves original horizontal path and orientation, lowering only height");
        orbit.eye[0] = 0; orbit.eye[1] = 1.2f;
        LowerIntroCamera(orbit,0,2);
        Check(orbit.eye[1] == 1.2f, "intro transition leaves normal driving eye height unchanged");
        Camera camera; EyeView eyes[2]; Settings settings; settings.horizonLock = 0;
        eyes[0].pose.position[0] = -0.032f; eyes[1].pose.position[0] = 0.032f;
        auto view = Build(camera, eyes, settings);
        Check(view.valid, "valid stereo rig");
        Check(std::abs(view.eyeWorld[1][0] - view.eyeWorld[0][0] - 0.064f) < 1e-5f, "eye separation in metres");
        for (auto& eye : eyes) { eye.pose.position[0] += 0.4f; eye.pose.position[1] += 0.2f; }
        view = Build(camera, eyes, settings);
        Check(std::abs(view.refEye[0] - 0.4f) < 1e-5f && std::abs(view.refEye[1] - 0.2f) < 1e-5f, "lean moves camera");
        settings.worldScale = 2; view = Build(camera, eyes, settings);
        Check(std::abs(view.eyeWorld[1][0] - view.eyeWorld[0][0] - 0.128f) < 1e-5f, "world scale preserves stereo");
        Recenter recenter; Pose head; head.position[0] = 2; head.position[1] = 0.5f; head.position[2] = 3;
        recenter.Latch(head);
        auto centered = recenter.Apply(head);
        Check(std::abs(centered.position[0]) < 1e-5f && std::abs(centered.position[2]) < 1e-5f, "recenter horizontal origin");
        Check(std::abs(centered.position[1]) < 1e-5f, "recenter removes floor-relative headset height");
        head.position[1] -= 0.25f;
        Check(std::abs(recenter.Apply(head).position[1] + 0.25f) < 1e-5f, "crouching after recenter still moves the camera");
        camera.eye[1] = 0.8f; settings.worldScale = 1;
        for (auto& eye : eyes) eye.pose = recenter.Apply(head);
        view = Build(camera, eyes, settings);
        Check(std::abs(view.refEye[1] - 0.55f) < 1e-5f, "driver camera height is applied only once");
        float projection[16]; Projection(eyes[0].fov, 0.05f, projection);
        Check(std::abs(projection[14] - 0.05f) < 1e-5f && projection[11] == -1, "reversed depth near plane");
        // Both asymmetric eye images must reconstruct the same physical HUD plane.
        Pose hudHead; hudHead.position[0] = 0.4f; hudHead.position[1] = 0.2f;
        eyes[0].fov = {-0.93f, 0.71f, 0.88f, -0.81f};
        eyes[1].fov = {-0.71f, 0.93f, 0.85f, -0.83f};
        float hud[2][16]; HudProjection(hudHead, eyes, hud);
        for (float x : {-0.8f, 0.0f, 0.8f}) for (float y : {-0.8f, 0.0f, 0.8f}) {
            for (int eye = 0; eye < 2; ++eye) {
                const auto* m = hud[eye];
                const float w = m[3] * x + m[7] * y + m[15];
                const float nx = (m[0] * x + m[4] * y + m[12]) / w;
                const float ny = (m[1] * x + m[5] * y + m[13]) / w;
                float p[16]; Projection(eyes[eye].fov, 0.05f, p);
                const float physicalX = eyes[eye].pose.position[0] + (nx + p[8]) * 2 / p[0];
                const float physicalY = eyes[eye].pose.position[1] + (ny + p[9]) * 2 / p[5];
                Check(std::abs(physicalX - hudHead.position[0] - x * 1.28f) < 1e-5f, "HUD horizontal stereo convergence");
                Check(std::abs(physicalY - hudHead.position[1] + y * 0.96f) < 1e-5f, "HUD vertical stereo convergence");
            }
        }
        struct Source {
            size_t next = 0;
            void Mix(float* out, size_t frames) {
                for (size_t i = 0; i < frames; ++i, ++next)
                    out[i * 2] = out[i * 2 + 1] = float(std::sin(double(next) * 0.017));
            }
        };
        struct State { int streamRate; std::vector<float> mixed; double position = 0; };
        for (int rate : {24000, 44100, 48000}) {
            Source fullSource, blockSource; State full{rate, {}, 0}, blocks{rate, {}, 0};
            std::vector<int16_t> expected(4000), actual(4000);
            gt2::audio::ResampleAudio(fullSource, full, expected.data(), 2000);
            int frame = 0;
            while (frame < 2000) {
                const int count = std::min(2000 - frame, (frame % 7) * 43 + 1);
                gt2::audio::ResampleAudio(blockSource, blocks, actual.data() + frame * 2, count);
                frame += count;
            }
            for (size_t i = 0; i < actual.size(); ++i)
                Check(std::abs(int(actual[i]) - int(expected[i])) <= 1, "audio resampling differs across callback sizes");
        }
        std::cout << "VR rig, positional billboards, asymmetric-eye HUD and audio resampling checks passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
