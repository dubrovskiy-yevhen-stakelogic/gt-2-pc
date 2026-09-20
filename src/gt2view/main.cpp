// gt2view - car + course viewer. Reads everything straight from the user's disc image.
//   gt2view <disc.bin> [car-id] [--track NAME] [--grid N] [--paint N] [--lod N] [--night] [--brake]
//           [--yaw DEG] [--pitch DEG] [--dist M] [--screenshot out.png]
// Controls: LMB drag orbit, wheel zoom, WASD/QE move the orbit target (Shift = fast), R back to the car,
//           Left/Right car, PgUp/PgDn +-20 cars, P paint, L LOD, N day/night, B brake lights, Esc quit.
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "gt2export/car_mesh.h"
#include "gt2formats/car_info.h"
#include "gt2formats/car_model.h"
#include "gt2formats/car_texture.h"
#include "gt2formats/psx_vram.h"
#include "gt2formats/track.h"
#include "gt2view/vk_scene_renderer.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"

using namespace gt2;
using namespace gt2view;

namespace {

// Our half of the VRAM buffer (rows 512+): car texture at (0, 512), paint CLUT rows from 736.
constexpr uint32_t kCarPageY = 512, kCarClutY = 736;
constexpr uint32_t kCarFirstVertex = 0, kCarMaxVertices = 1 << 15, kTrackFirstVertex = kCarMaxVertices;

struct App {
    std::unique_ptr<GtfsVolume> vol;
    std::unique_ptr<VkContext> context; // the Vulkan instance / surface / device (gt2view/vk_context.h)
    std::unique_ptr<VkSceneRenderer> renderer;
    std::unique_ptr<CarInfo> carInfo;
    std::string carName, trackName;
    std::vector<std::string> carIds;
    size_t carIndex = 0;
    size_t paint = 0, paintCount = 1, lod = 0;
    std::vector<uint8_t> paintIds;
    bool night = false, brake = false;
    uint32_t carVertexCount = 0, trackVertexCount = 0;

    float carPos[3] = {0, 0, 0};     // world metres
    float carForward[2] = {0, -1};   // world X/Z unit vector (model front is -Z)
    float carLift = 0;               // wheel bottom -> body origin
    float target[3] = {0, 0.6f, 0};
    float yaw = 35, pitch = 18, distance = 6.0f;
    bool dragging = false;
    int lastX = 0, lastY = 0;
    HWND window = nullptr;
};

App g;

void Multiply(const float* a, const float* b, float* out) { // column-major out = a * b
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    std::copy(r, r + 16, out);
}

void BuildViewProjection(float aspect, float* vp) {
    const float kRad = 3.14159265f / 180.0f;
    float cp = std::cos(g.pitch * kRad), sp = std::sin(g.pitch * kRad);
    float eye[3] = {g.target[0] + g.distance * cp * std::sin(g.yaw * kRad), g.target[1] + g.distance * sp,
                    g.target[2] + g.distance * cp * std::cos(g.yaw * kRad)};
    float f[3] = {g.target[0] - eye[0], g.target[1] - eye[1], g.target[2] - eye[2]};
    float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& v : f) v /= fl;
    float s[3] = {-f[2], 0, f[0]}; // f x (0,1,0)
    float sl = std::sqrt(s[0] * s[0] + s[2] * s[2]);
    for (float& v : s) v /= sl;
    float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    float view[16] = {s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0,
                      -dot(s, eye), -dot(u, eye), dot(f, eye), 1};

    const float zn = 0.2f; // reversed Z, infinite far plane (vk_scene_renderer.h)
    float t = 1.0f / std::tan(45.0f * kRad / 2);
    float proj[16] = {t / aspect, 0, 0, 0, 0, -t, 0, 0, 0, 0, 0, -1, 0, 0, zn, 0};
    Multiply(proj, view, vp);
}

void CarModelMatrix(float* m) {
    // Model axes: +X right, +Y up, -Z front.
    float fx = g.carForward[0], fz = g.carForward[1];
    float r[16] = {-fz, 0, fx, 0, 0, 1, 0, 0, -fx, 0, -fz, 0, g.carPos[0], g.carPos[1] + g.carLift, g.carPos[2], 1};
    std::copy(r, r + 16, m);
}

void UpdateTitle() {
    char title[320];
    std::snprintf(title, sizeof(title), "gt2view - %s  [%s] (%zu/%zu)  %s  LOD%zu  paint %zu/%zu (id %02X)%s%s%s",
                  g.carName.c_str(), g.carIds[g.carIndex].c_str(), g.carIndex + 1, g.carIds.size(), g.night ? "night" : "day",
                  g.lod, g.paint + 1, g.paintCount, g.paintIds.empty() ? 0 : g.paintIds[g.paint], g.brake ? "  BRAKE" : "",
                  g.trackName.empty() ? "" : "  @ ", g.trackName.c_str());
    SetWindowTextA(g.window, title);
}

void LoadCar() {
    const std::string& id = g.carIds[g.carIndex];
    CarModel model = ParseCarModel(g.vol->Read("carobj/" + id + (g.night ? ".cno" : ".cdo")));
    CarTexture texture = ParseCarTexture(g.vol->Read("carobj/" + id + (g.night ? ".cnp" : ".cdp")));
    g.lod = std::min(g.lod, model.lods.size() - 1);
    g.paintCount = texture.paints.size();
    g.paint = std::min(g.paint, g.paintCount - 1);
    g.paintIds.clear();
    for (const auto& p : texture.paints) g.paintIds.push_back(p.id);
    auto info = g.carInfo->Lookup(id, g.paintIds);
    g.carName = info ? info->name : "<unlisted>";
    g.carLift = float((model.wheelRadiusFront - model.wheels[0].y) * kCarWheelMetresPerUnit);

    std::vector<SceneVertex> vertices;
    for (const CarMeshVertex& v : BuildCarMesh(model, {g.lod, true, false})) {
        SceneVertex o{};
        std::copy(v.pos, v.pos + 3, o.pos);
        std::copy(v.texel, v.texel + 2, o.texel);
        std::copy(v.color, v.color + 3, o.color);
        o.page = 0 | (kCarPageY << 16);
        o.clut = uint32_t(v.palette) * 16 | (kCarClutY << 16);
        o.flags = (v.textured ? kTextured : 0u) | (v.rawTexture ? kRawTexture : 0u) | kCarPaint | (v.cullBack ? kCullBack : 0u);
        vertices.push_back(o);
    }
    if (vertices.size() > kCarMaxVertices) throw std::runtime_error("car mesh exceeds its vertex budget");
    g.renderer->SetVertices(kCarFirstVertex, vertices);
    g.carVertexCount = uint32_t(vertices.size());

    // Car texture into our VRAM rows: 4bpp pixels packed 4 per word, then one CLUT row per paint.
    const uint32_t rows = (kCarClutY - kCarPageY) + 16;
    std::vector<uint16_t> words(size_t(rows) * 1024, 0);
    for (size_t i = 0; i < texture.indices.size(); i++) {
        size_t x = i % CarTexture::kWidth, y = i / CarTexture::kWidth;
        words[y * 1024 + x / 4] |= uint16_t(texture.indices[i] << ((x & 3) * 4));
    }
    for (size_t p = 0; p < texture.paints.size(); p++)
        for (size_t c = 0; c < 16; c++)
            for (size_t i = 0; i < 16; i++)
                words[(kCarClutY - kCarPageY + p) * 1024 + c * 16 + i] = texture.paints[p].cluts[c][i];
    g.renderer->UploadVram(kCarPageY, rows, words.data());
    if (g.window) UpdateTitle();
}

void LoadTrack(const std::string& name, size_t gridSlot) {
    Track track = ParseTrack(g.vol->Read("crsobj/" + name + ".tro"));
    PsxVram vram;
    vram.LoadTimPack(g.vol->Read("crsobj/" + name + ".trp"));
    g.renderer->UploadVram(0, PsxVram::kHeight, vram.Words().data());

    std::vector<SceneVertex> vertices;
    auto addShape = [&](const TrackChunk& chunk, const TrackShape& shape) {
        for (const TrackPolygon& p : shape.polygons) {
            const TrackUvSet* uv = p.IsTextured() ? &track.uvTable[p.uvIndex].nearSet : nullptr;
            auto corner = [&](size_t i) {
                SceneVertex o{};
                auto w = TrackVertexToWorld(chunk, shape.vertices[p.vertex[i]]);
                std::copy(w.begin(), w.end(), o.pos);
                for (int k = 0; k < 3; k++) o.color[k] = p.color[i][size_t(k)] / 255.0f;
                if (uv) {
                    o.texel[0] = uv->u[i] + 0.5f;
                    o.texel[1] = uv->v[i] + 0.5f;
                    o.page = uint32_t((uv->tpage & 0xF) * 64) | (uint32_t(((uv->tpage >> 4) & 1) * 256) << 16);
                    o.clut = uint32_t((uv->clut & 0x3F) * 16) | (uint32_t(uv->clut >> 6) << 16);
                    o.flags = kTextured | (uint32_t((uv->tpage >> 7) & 3) << 8);
                }
                return o;
            };
            static constexpr size_t kQuad[6] = {0, 1, 2, 0, 2, 3};
            for (size_t k = 0; k < (p.IsQuad() ? 6u : 3u); k++) vertices.push_back(corner(kQuad[k]));
        }
    };
    for (const TrackChunk& chunk : track.chunks) {
        addShape(chunk, chunk.road);
        addShape(chunk, chunk.surround);
    }
    g.renderer->SetVertices(kTrackFirstVertex, vertices);
    g.trackVertexCount = uint32_t(vertices.size());
    g.trackName = name;

    // Put the car on the requested grid slot, heading along the nearest chunk.
    const auto& slot = track.startGrid.at(gridSlot);
    for (int k = 0; k < 3; k++) g.carPos[k] = float(slot[size_t(k)] / 65536.0);
    double best = 1e30;
    for (const TrackChunk& c : track.chunks) {
        double dx = c.origin[0] / 65536.0 - g.carPos[0], dz = c.origin[2] / 65536.0 - g.carPos[2];
        if (dx * dx + dz * dz < best) {
            best = dx * dx + dz * dz;
            float fx = c.direction[0], fz = c.direction[2], fl = std::sqrt(fx * fx + fz * fz);
            g.carForward[0] = fx / fl;
            g.carForward[1] = fz / fl;
        }
    }
    std::printf("%s: %d m, %zu chunks, %zu vertices; car at %.1f %.1f %.1f\n", name.c_str(), track.lengthMetres,
                track.chunks.size(), vertices.size(), g.carPos[0], g.carPos[1], g.carPos[2]);
}

void TargetCar() {
    g.target[0] = g.carPos[0];
    g.target[1] = g.carPos[1] + 0.7f;
    g.target[2] = g.carPos[2];
}

void StepCar(int delta) {
    const int n = int(g.carIds.size());
    g.carIndex = size_t(((int(g.carIndex) + delta) % n + n) % n);
    LoadCar();
}

void MoveTarget(float seconds) {
    if (GetForegroundWindow() != g.window) return;
    auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    const float kRad = 3.14159265f / 180.0f;
    float speed = (down(VK_SHIFT) ? 120.0f : 20.0f) * seconds;
    float fx = -std::sin(g.yaw * kRad), fz = -std::cos(g.yaw * kRad); // view forward on the ground plane
    float move[3] = {0, 0, 0};
    if (down('W')) { move[0] += fx; move[2] += fz; }
    if (down('S')) { move[0] -= fx; move[2] -= fz; }
    if (down('D')) { move[0] -= fz; move[2] += fx; }
    if (down('A')) { move[0] += fz; move[2] -= fx; }
    if (down('E')) move[1] += 1;
    if (down('Q')) move[1] -= 1;
    for (int k = 0; k < 3; k++) g.target[k] += move[k] * speed;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    try {
        switch (msg) {
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_LBUTTONDOWN: g.dragging = true; g.lastX = GET_X_LPARAM(lp); g.lastY = GET_Y_LPARAM(lp); SetCapture(hwnd); return 0;
        case WM_LBUTTONUP: g.dragging = false; ReleaseCapture(); return 0;
        case WM_MOUSEMOVE:
            if (g.dragging) {
                g.yaw -= (GET_X_LPARAM(lp) - g.lastX) * 0.4f;
                g.pitch = std::clamp(g.pitch + (GET_Y_LPARAM(lp) - g.lastY) * 0.3f, -85.0f, 85.0f);
                g.lastX = GET_X_LPARAM(lp);
                g.lastY = GET_Y_LPARAM(lp);
            }
            return 0;
        case WM_MOUSEWHEEL:
            g.distance = std::clamp(g.distance * (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 0.9f : 1.1f), 0.5f, 2000.0f);
            return 0;
        case WM_KEYDOWN:
            switch (wp) {
            case VK_ESCAPE: DestroyWindow(hwnd); break;
            case VK_RIGHT: StepCar(1); break;
            case VK_LEFT: StepCar(-1); break;
            case VK_NEXT: StepCar(20); break;
            case VK_PRIOR: StepCar(-20); break;
            case 'P': g.paint = (g.paint + 1) % g.paintCount; UpdateTitle(); break;
            case 'L': g.lod = (g.lod + 1) % 3; LoadCar(); break;
            case 'N': g.night = !g.night; LoadCar(); break;
            case 'B': g.brake = !g.brake; UpdateTitle(); break;
            case 'R': TargetCar(); break;
            }
            return 0;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("usage: gt2view <disc.bin> [car-id] [--track NAME] [--grid N] [--paint N] [--lod N] [--night] [--brake]\n"
                  "               [--yaw DEG] [--pitch DEG] [--dist M] [--screenshot out.png]");
        return 2;
    }
    try {
        std::string startCar, screenshot, trackName;
        size_t gridSlot = 0;
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            auto next = [&] { return i + 1 < argc ? argv[++i] : ""; };
            if (a == "--paint") g.paint = size_t(std::atoi(next()));
            else if (a == "--lod") g.lod = size_t(std::atoi(next()));
            else if (a == "--night") g.night = true;
            else if (a == "--brake") g.brake = true;
            else if (a == "--yaw") g.yaw = float(std::atof(next()));
            else if (a == "--pitch") g.pitch = float(std::atof(next()));
            else if (a == "--dist") g.distance = float(std::atof(next()));
            else if (a == "--track") trackName = next();
            else if (a == "--grid") gridSlot = size_t(std::atoi(next()));
            else if (a == "--screenshot") screenshot = next();
            else startCar = a;
        }

        DiscImage disc(argv[1]);
        g.vol = std::make_unique<GtfsVolume>(disc);
        g.carInfo = std::make_unique<CarInfo>(g.vol->Read(".carinfoe"));
        for (const auto& f : g.vol->Files()) {
            const std::string prefix = "carobj/", suffix = ".cdo.gz";
            if (f.path.rfind(prefix, 0) == 0 && f.path.size() > suffix.size() &&
                f.path.compare(f.path.size() - suffix.size(), suffix.size(), suffix) == 0)
                g.carIds.push_back(f.path.substr(prefix.size(), f.path.size() - prefix.size() - suffix.size()));
        }
        if (g.carIds.empty()) throw std::runtime_error("no cars found in GT2.VOL");
        if (!startCar.empty()) {
            auto it = std::find(g.carIds.begin(), g.carIds.end(), startCar);
            if (it == g.carIds.end()) throw std::runtime_error("unknown car id: " + startCar);
            g.carIndex = size_t(it - g.carIds.begin());
        }

        HINSTANCE hinst = GetModuleHandleA(nullptr);
        WNDCLASSA wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = "gt2view";
        RegisterClassA(&wc);
        RECT rc{0, 0, 1280, 720};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        HWND hwnd = CreateWindowA("gt2view", "gt2view", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                                  rc.bottom - rc.top, nullptr, nullptr, hinst, nullptr);
        if (!hwnd) throw std::runtime_error("CreateWindow failed");

        g.context = std::make_unique<VkContext>(hinst, hwnd);
        g.renderer = std::make_unique<VkSceneRenderer>(*g.context);
        if (!trackName.empty()) LoadTrack(trackName, gridSlot);
        LoadCar();
        TargetCar();
        g.window = hwnd;
        UpdateTitle();
        // GT2_NO_FOCUS=1 (automated runs): show without taking the foreground.
        // Dev tool: never take the foreground (the user keeps working while runs happen); GT2_FOCUS=1 restores activation.
        ShowWindow(hwnd, std::getenv("GT2_FOCUS") ? SW_SHOW : SW_SHOWNOACTIVATE);

        int frame = 0;
        ULONGLONG lastTick = GetTickCount64();
        for (bool running = true; running;) {
            MSG msg;
            while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) running = false;
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            if (!running) break;
            ULONGLONG now = GetTickCount64();
            MoveTarget(float(now - lastTick) / 1000.0f);
            lastTick = now;

            float vp[16], model[16];
            BuildViewProjection(g.renderer->AspectRatio(), vp);
            CarModelMatrix(model);
            std::vector<DrawItem> items(2);
            items[0].firstVertex = kTrackFirstVertex;
            items[0].vertexCount = g.trackVertexCount;
            std::copy(vp, vp + 16, items[0].mvp);
            items[1].firstVertex = kCarFirstVertex;
            items[1].vertexCount = g.carVertexCount;
            Multiply(vp, model, items[1].mvp);
            items[1].paint = uint32_t(g.paint);
            items[1].brakeLit = g.brake ? 1u : 0u;

            bool capture = !screenshot.empty() && ++frame == 3;
            g.renderer->Draw(items, capture ? screenshot : std::string());
            if (capture) {
                std::printf("wrote %s\n", screenshot.c_str());
                break;
            }
        }
        g.renderer.reset();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
