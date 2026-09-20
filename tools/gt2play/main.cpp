// gt2play - the game in our runtime, drawn by OUR renderer.
//   gt2play <disc.bin> [--original] [--script "field:button,..."] [--shot <field> <out.png>] [--prims <field> <out.txt>]
// --prims: dev aid - lists every GP0 primitive of the draw lists in fields <field>..<field>+7 with its texture
// words, plus the GTE transforms (with input vertices) reported at each display flip and the console VRAM
// (<out.txt>.vram.png / .vram.bin). The draw list between two flips belongs to the transforms of the LATER flip.
// Glow records (gt2view/glow.h): "# glow-pass" (our chunk order per chunk pass), "# glow-guest-chunk" (the original's
// 0x80026BB4 calls), "# glow-model" (a model's glow call), and at each flip "# glow-frame" + "# glow-ours" = our glow
// primitives of the frame in the GPU's order, to compare with the GP0 primitives with CLUT 7F57 that follow.
// Native mode (default, F1 toggles): while a race is on screen the 3D scene is NOT the game's picture.
// The running game only provides the camera and the car transforms (recovered from its GTE traffic by
// SceneExtractor); the course and the cars are drawn from the parsed asset files by the Vulkan scene
// renderer at window resolution, with the game's own 2D layer (HUD) composited on top. Everything else
// (menus, loading screens) shows the original picture.
// --card <mcd>: memory card 1 image (default work/memcards/card1.mcd; the file is written when the game saves - point
// it at a copy); --card2 <mcd>: memory card 2 (slot 2 is empty unless given). --ai-player: DEV CAPTURE AID - the
// original's AI drives the player's car (tools/gt2run/ai_player.h;
// changes guest state, so such a run is an oracle capture aid only). With --prims the RAM after field <field> + 6 is
// written to <out.txt>.ram.bin (the state the screen comparisons read).
// --auto <from> <script.txt>: DEV CAPTURE AID - from field <from> the race autopilot (tools/gt2run/race_autopilot.h)
// presses through the screens between races; the run's presses are kept in <script.txt> as a --script.
// --prims-on-call <hexaddr> <off1+off2+...> <prefix>: at the first call of <hexaddr> (field F) queue --prims captures
// at F + off (prefix_<field>.txt), e.g. the championship end view 0x80059800, whose field is not known in advance.
// Keys: arrows = d-pad, Z = Cross, X = Circle, A = Square, S = Triangle, Q/W = L1/R1, 1/2 = L2/R2,
//       Enter = Start, Backspace = Select, Tab (hold) = unthrottled, F1 = native/original, F12 = screenshot, Esc = quit.
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "gt2export/car_mesh.h"
#include "gt2export/png_writer.h"
#include "gt2formats/car_model.h"
#include "gt2formats/car_texture.h"
#include "gt2formats/course_data.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/psx_vram.h"
#include "gt2formats/sponsor_boards.h"
#include "gt2formats/track.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "gt2view/hud.h"
#include "gt2formats/exe_profile.h"
#include "gt2view/glow.h"
#include "gt2view/particles.h"
#include "gt2view/race_overlay_screens.h"
#include "gt2view/scene_assets.h"
#include "gt2view/vk_scene_renderer.h"
#include "machine/machine.h"
#include "scene/scene_extractor.h"
#include "../gt2run/ai_player.h"
#include "../gt2run/ai_player_arcade.h"
#include "../gt2run/race_autopilot.h"

using namespace gt2;
using namespace gt2view;

namespace {

constexpr uint32_t kStackTop = 0x801FFF00;
bool g_running = true;
bool g_toggleNative = false;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { g_running = false; PostQuitMessage(0); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_F1 && !(lp & (1 << 30))) { g_toggleNative = true; return 0; }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

const std::map<std::string, int> kButtonBits = {{"select", 0}, {"start", 3}, {"up", 4}, {"right", 5}, {"down", 6}, {"left", 7},
                                                {"l2", 8}, {"r2", 9}, {"l1", 10}, {"r1", 11}, {"triangle", 12}, {"circle", 13},
                                                {"cross", 14}, {"square", 15}};

uint16_t ReadPad(HWND hwnd) {
    if (GetForegroundWindow() != hwnd) return 0;
    struct Map { int key; int bit; };
    static constexpr Map kMap[] = {{VK_BACK, 0}, {VK_RETURN, 3}, {VK_UP, 4}, {VK_RIGHT, 5}, {VK_DOWN, 6}, {VK_LEFT, 7},
                                   {'1', 8}, {'2', 9}, {'Q', 10}, {'W', 11}, {'S', 12}, {'X', 13}, {'Z', 14}, {'A', 15}};
    uint16_t buttons = 0;
    for (const Map& m : kMap)
        if (GetAsyncKeyState(m.key) & 0x8000) buttons |= uint16_t(1u << m.bit);
    return buttons;
}

std::vector<uint8_t> ReadRootFile(const DiscImage& disc, const std::string& name) {
    auto f = disc.FindRootFile(name);
    if (!f) throw std::runtime_error(name + " not found in disc root");
    std::vector<uint8_t> data(f->size);
    disc.ReadForm1(f->lba, 0, data.data(), data.size());
    return data;
}

using NativeScene = gt2view::SceneAssets;

// Dev aid (--prims): decodes the GP0 stream of one field into a text listing of every primitive with its
// texture words, so the original's draw list can be compared with what the native renderer draws.
struct PrimitiveLogger {
    std::FILE* out = nullptr;
    std::vector<uint32_t> words;
    size_t expected = 0;
    bool polyline = false;
    int64_t uploadRemaining = 0;
    uint32_t index = 0;

    static size_t Length(uint32_t first) { // mirrors Gpu::CommandLength
        const uint32_t cmd = first >> 24;
        switch (cmd >> 5) {
        case 0: return cmd == 0x02 ? 3 : 1;
        case 1: { const size_t n = (cmd & 0x08) ? 4 : 3; return 1 + n + ((cmd & 0x04) ? n : 0) + ((cmd & 0x10) ? n - 1 : 0); }
        case 2: return (cmd & 0x08) ? SIZE_MAX : ((cmd & 0x10) ? 4 : 3);
        case 3: return 2 + ((cmd & 0x04) ? 1 : 0) + (((cmd >> 3) & 3) == 0 ? 1 : 0);
        case 4: return 4;
        case 5: case 6: return 3;
        default: return 1;
        }
    }

    void Word(bool gp1, uint32_t w) {
        if (!out) return;
        if (gp1) { std::fprintf(out, "GP1 %08X\n", w); return; }
        if (uploadRemaining > 0) { uploadRemaining -= 2; return; }
        if (words.empty()) { expected = Length(w); polyline = expected == SIZE_MAX; }
        words.push_back(w);
        const bool terminator = polyline && words.size() >= 3 && (w & 0xF000F000u) == 0x50005000u;
        if (terminator || (!polyline && words.size() >= expected) || words.size() > 4096) {
            Emit();
            words.clear();
        }
    }

    void Emit() {
        const uint32_t cmd = words[0] >> 24;
        auto xy = [&](uint32_t v) { return std::string("(") + std::to_string(int(int16_t(v & 0xFFFF))) + "," + std::to_string(int(int16_t(v >> 16))) + ")"; };
        if ((cmd >> 5) == 1) {
            const bool gouraud = cmd & 0x10, quad = cmd & 0x08, textured = cmd & 0x04, semi = cmd & 0x02, raw = cmd & 0x01;
            std::fprintf(out, "%u POLY %02X %s%s%s%s%s rgb=%06X", index++, cmd, quad ? "quad" : "tri", textured ? " tex" : "",
                         gouraud ? " gouraud" : "", semi ? " semi" : "", raw ? " raw" : "", words[0] & 0xFFFFFF);
            size_t i = 1;
            for (size_t v = 0; v < (quad ? 4u : 3u); v++) {
                if (v > 0 && gouraud) std::fprintf(out, " rgb%zu=%06X", v, words[i++] & 0xFFFFFF);
                std::fprintf(out, " v%zu=%s", v, xy(words[i++]).c_str());
                if (textured) {
                    const uint32_t t = words[i++];
                    std::fprintf(out, " uv%zu=(%u,%u)", v, t & 0xFF, (t >> 8) & 0xFF);
                    if (v == 0) std::fprintf(out, " clut=%04X", t >> 16);
                    if (v == 1) std::fprintf(out, " tpage=%04X", t >> 16);
                }
            }
            std::fprintf(out, "\n");
        } else if ((cmd >> 5) == 3) {
            const bool textured = cmd & 0x04, semi = cmd & 0x02, raw = cmd & 0x01;
            std::fprintf(out, "%u RECT %02X rgb=%06X xy=%s", index++, cmd, words[0] & 0xFFFFFF, xy(words[1]).c_str());
            size_t i = 2;
            if (textured) { std::fprintf(out, " uv=(%u,%u) clut=%04X", words[i] & 0xFF, (words[i] >> 8) & 0xFF, words[i] >> 16); i++; }
            if (((cmd >> 3) & 3) == 0) std::fprintf(out, " size=%s", xy(words[i]).c_str());
            std::fprintf(out, "%s%s\n", semi ? " semi" : "", raw ? " raw" : "");
        } else if (cmd == 0x02) {
            std::fprintf(out, "%u FILL rgb=%06X xy=%s size=%s\n", index++, words[0] & 0xFFFFFF, xy(words[1]).c_str(), xy(words[2]).c_str());
        } else if ((cmd >> 5) == 5) {
            const uint32_t w = words[2] & 0xFFFF, h = words[2] >> 16;
            std::fprintf(out, "%u UPLOAD xy=%s size=%s\n", index++, xy(words[1]).c_str(), xy(words[2]).c_str());
            uploadRemaining = int64_t(w ? w : 1024) * int64_t(h ? h : 512);
            if (uploadRemaining & 1) uploadRemaining++;
        } else if ((cmd >> 5) == 2) {
            std::fprintf(out, "%u LINE %02X rgb=%06X", index++, cmd, words[0] & 0xFFFFFF);
            // the vertices (gouraud: colour words between them; a polyline ends at the 0x5xxx5xxx terminator)
            const bool shaded = cmd & 0x10;
            for (size_t i = 1; i < words.size(); i++) {
                if (cmd & 0x08 && (words[i] & 0xF000F000u) == 0x50005000u && i >= 3) break;
                if (shaded && (i & 1) == 0) { std::fprintf(out, " rgb=%06X", words[i] & 0xFFFFFF); continue; }
                std::fprintf(out, " v=%s", xy(words[i]).c_str());
            }
            std::fprintf(out, "\n");
        } else if (cmd == 0xE1) {
            const uint32_t v = words[0];
            std::fprintf(out, "%u E1 tpage=%03X blend=%u depth=%u dither=%u\n", index++, v & 0x7FF, (v >> 5) & 3, (v >> 7) & 3, (v >> 9) & 1);
        } else if (cmd == 0xE2) {
            const uint32_t v = words[0];
            std::fprintf(out, "%u E2 window mask=(%u,%u) offset=(%u,%u)\n", index++, v & 0x1F, (v >> 5) & 0x1F, (v >> 10) & 0x1F, (v >> 15) & 0x1F);
        } else if (cmd >= 0xE3 && cmd <= 0xE6) {
            std::fprintf(out, "%u E%X %06X\n", index++, cmd & 0xF, words[0] & 0xFFFFFF);
        } else if (cmd != 0x00 && cmd != 0x01 && cmd != 0x03) {
            std::fprintf(out, "%u CMD %02X (%zu words)\n", index++, cmd, words.size());
        }
    }
};

std::array<double, 3> CameraEye(const SceneExtractor::Scene& scene) {
    return {scene.cameraPosition[0], scene.cameraPosition[1], scene.cameraPosition[2]};
}
std::array<std::array<double, 3>, 3> CameraAxes(const SceneExtractor::Scene& scene) {
    std::array<std::array<double, 3>, 3> axes{};
    for (size_t r = 0; r < 3; r++)
        for (size_t c = 0; c < 3; c++) axes[r][c] = scene.cameraAxes[r][c];
    return axes;
}

// One call of the original's scenery LOD choice (0x8001F7F8 -> 0x8007AE38 -> 0x8007AEF4), traced by --prims.
struct LodCall {
    uint32_t from = 0;                   // 0x8001F960 = scenery; 0x80067580 = cars
    std::array<int32_t, 3> instance{};   // scratchpad 0x1F8000B0: the record position (16.16)
    std::array<int32_t, 3> t{};          // scratchpad 0x1F800014: camera-space translation (16.16)
    int32_t k = 0;
    uint32_t measure = 0;
    int choice = -1;
};

// Dev aid (--prims): the LOD choice of every scenery instance the original measured in the frame of `scene`
// against track.h's rule: (1) the measure from the original's own camera-space translation and k (the formula),
// (2) the measure and entry from our camera-space translation (the camera the renderer would use).
void CheckLodTrace(const Track& track, const SceneExtractor::Scene& scene, const std::vector<LodCall>& calls, std::FILE* out) {
    size_t total = 0, formulaSame = 0, choiceSame = 0, choiceSameOriginalK = 0, found = 0;
    for (const LodCall& c : calls) {
        if (c.from != 0x8001F960u) continue;
        total++;
        const TrackSceneryInstance* inst = nullptr;
        for (const TrackSceneryInstance& i : track.sceneryInstances)
            if (i.position == c.instance) { inst = &i; break; }
        const bool formulaOk = SceneryLodMeasure(c.t, c.k) == c.measure;
        if (formulaOk) formulaSame++;
        if (!inst) { std::fprintf(out, "# scenery-lod: instance at (%d %d %d) not in the course file\n", c.instance[0], c.instance[1], c.instance[2]); continue; }
        found++;
        const int32_t k = SceneryLodK(int32_t(std::lround(scene.projectionDistance)), inst->lodDivisor);
        const std::array<int32_t, 3> t = SceneryCameraSpace(*inst, CameraEye(scene), CameraAxes(scene));
        const uint32_t measure = SceneryLodMeasure(t, k);
        const int ours = SceneryLodIndex(track.sceneryLods[inst->lodList], measure);
        if (ours == c.choice) choiceSame++;
        // The same with the original's k: separates the camera-space translation from the projection distance (the
        // replay cameras zoom every frame, and the extracted h can belong to the neighbouring frame).
        if (SceneryLodIndex(track.sceneryLods[inst->lodList], SceneryLodMeasure(t, c.k)) == c.choice) choiceSameOriginalK++;
        std::fprintf(out, "# scenery-lod instance %zu: original k %d measure %u entry %d; formula %s; ours k %d measure %u entry %d%s; file thresholds",
                     size_t(inst - track.sceneryInstances.data()), c.k, c.measure, c.choice, formulaOk ? "exact" : "DIFFERS", k, measure, ours,
                     ours == c.choice ? "" : "  <-- ENTRY DIFFERS");
        for (const TrackLodEntry& e : track.sceneryLods[inst->lodList]) std::fprintf(out, " %u", e.threshold);
        std::fprintf(out, "\n");
    }
    std::fprintf(out, "# scenery-lod: %zu calls, formula exact for %zu, %zu instances found, our entry = original's for %zu (%zu with the original's k)\n", total, formulaSame,
                 found, choiceSame, choiceSameOriginalK);
    std::printf("scenery-lod: %zu calls in the frame, formula exact for %zu, our camera gives the original's entry for %zu of %zu (%zu with the original's k)\n", total,
                formulaSame, choiceSame, found, choiceSameOriginalK);
}

// Dev aid (--prims): checks our scenery placement against the original's GTE traffic of the frame. Every
// transform whose first 8 input vertices are the corner set of a scenery model's bounds (the original's view
// test, 0x8007B8F8) is attributed to the instance whose placement (track.h SceneryInstanceMatrix) puts those
// corners where the original's matrices put them: world = camera + axes^T * ((rt * v) / 4096 + tr) * (4096 / |row|) * metresPerUnit.
// Drawn objects (more vertices follow) also have their vertex list compared with the model file.
void CheckScenery(const Track& track, const SceneExtractor::Scene& scene, const std::vector<Gte::CapturedTransform>& transforms,
                  const std::vector<Gte::CapturedVertex>& vertices, std::FILE* out) {
    size_t tested = 0, matched = 0, unmatched = 0, drawn = 0, drawnVerticesOk = 0, drawnVerticesTotal = 0, lodChecked = 0, lodSame = 0;
    double worstMatched = 0;
    for (const auto& t : transforms) {
        if (t.vertexCount < 9 || t.firstVertex + 9 > vertices.size()) continue;
        std::array<int16_t, 3> lo{}, hi{};
        bool box = true;
        for (size_t a = 0; a < 3 && box; a++) {
            std::vector<int16_t> seen;
            for (uint32_t k = 0; k < 8; k++) {
                const auto& v = vertices[t.firstVertex + k];
                const int16_t c = a == 0 ? v.x : a == 1 ? v.y : v.z;
                if (std::find(seen.begin(), seen.end(), c) == seen.end()) seen.push_back(c);
            }
            if (seen.size() > 2) box = false;
            else { lo[a] = *std::min_element(seen.begin(), seen.end()); hi[a] = *std::max_element(seen.begin(), seen.end()); }
        }
        if (!box) continue;
        std::vector<size_t> models;
        for (size_t m = 0; m < track.sceneryModels.size(); m++)
            if (track.sceneryModels[m].boundsMin == lo && track.sceneryModels[m].boundsMax == hi) models.push_back(m);
        if (models.empty()) continue;
        tested++;
        // The rows are the camera rotation scaled by a power of two (the original keeps the object inside 16 bits,
        // 0x8007B8A0) and rounded to integers; the y row additionally carries the screen aspect factor 3723.5/4096
        // seen on every chunk and car transform. Using the exact scales instead of the rounded row norms keeps
        // the translation of far objects (a 0.5 % norm error at scale 128 would move a 400 m translation by 2.5 m).
        double rowNorm[3];
        {
            const double n0 = std::sqrt(double(t.rt[0][0]) * t.rt[0][0] + double(t.rt[0][1]) * t.rt[0][1] + double(t.rt[0][2]) * t.rt[0][2]);
            const double n2 = std::sqrt(double(t.rt[2][0]) * t.rt[2][0] + double(t.rt[2][1]) * t.rt[2][1] + double(t.rt[2][2]) * t.rt[2][2]);
            const double scale = std::ldexp(1.0, int(std::lround(std::log2((n0 + n2) / 2))));
            rowNorm[0] = rowNorm[2] = scale;
            rowNorm[1] = scale * 3723.5 / 4096.0;
        }
        // Original: world position of an input vertex in the units of the matched model.
        auto originalWorld = [&](const Gte::CapturedVertex& v, double mu) {
            std::array<double, 3> w = {scene.cameraPosition[0], scene.cameraPosition[1], scene.cameraPosition[2]};
            for (int r = 0; r < 3; r++) {
                const double cam = ((double(t.rt[r][0]) * v.x + double(t.rt[r][1]) * v.y + double(t.rt[r][2]) * v.z) / 4096.0 + t.tr[r]) * 4096.0 / rowNorm[r] * mu;
                for (size_t c = 0; c < 3; c++) w[c] += scene.cameraAxes[size_t(r)][c] * cam;
            }
            return w;
        };
        double best = 1e9;
        size_t bestInstance = 0, bestModel = 0;
        for (size_t i = 0; i < track.sceneryInstances.size(); i++) {
            const auto& inst = track.sceneryInstances[i];
            for (const TrackLodEntry& lod : track.sceneryLods[inst.lodList]) {
                if (std::find(models.begin(), models.end(), size_t(lod.model)) == models.end()) continue;
                const TrackSceneryModel& model = track.sceneryModels[lod.model];
                const std::array<float, 16> m = SceneryInstanceMatrix(inst, model);
                const double mu = SceneryMetresPerUnit(model);
                double worst = 0;
                for (uint32_t k = 0; k < 8; k++) {
                    const auto& v = vertices[t.firstVertex + k];
                    const std::array<double, 3> o = originalWorld(v, mu);
                    double d = 0;
                    for (size_t r = 0; r < 3; r++) {
                        const double ours = double(m[r]) * v.x + double(m[4 + r]) * v.y + double(m[8 + r]) * v.z + m[12 + r];
                        d = std::max(d, std::abs(ours - o[r]));
                    }
                    worst = std::max(worst, d);
                }
                if (worst < best) { best = worst; bestInstance = i; bestModel = lod.model; }
            }
        }
        const bool ok = best < 1.0; // metres; the GTE rounds rotation rows to 1/4096 and translations to the object's unit
        if (ok) { matched++; worstMatched = std::max(worstMatched, best); } else unmatched++;
        std::string vertexNote;
        if (t.vertexCount > 9) { // drawn: the model's vertices follow the corners and one more vector
            const TrackSceneryModel& model = track.sceneryModels[bestModel];
            size_t same = 0;
            for (size_t k = 0; k < model.vertices.size() && 9 + k < t.vertexCount && t.firstVertex + 9 + k < vertices.size(); k++) {
                const auto& v = vertices[t.firstVertex + 9 + k];
                const TrackVertex& f = model.vertices[k];
                if (v.x == f.x && v.y == f.y && v.z == f.z) same++;
            }
            drawn++;
            drawnVerticesOk += same;
            drawnVerticesTotal += model.vertices.size();
            vertexNote = " drawn, " + std::to_string(same) + "/" + std::to_string(model.vertices.size()) + " vertices identical";
        }
        // The level of detail our rule (track.h SceneryLodIndex) picks at the captured camera vs the model whose
        // bounds the original tested (its chosen entry).
        std::string lodNote;
        if (ok) {
            const TrackSceneryInstance& inst = track.sceneryInstances[bestInstance];
            const auto& lods = track.sceneryLods[inst.lodList];
            const int ours = SceneryLodIndex(lods, SceneryLodMeasure(SceneryCameraSpace(inst, CameraEye(scene), CameraAxes(scene)),
                                                                     SceneryLodK(int32_t(std::lround(scene.projectionDistance)), inst.lodDivisor)));
            // Bounds shared by several entries of the list make the attribution ambiguous: any of them counts.
            const bool same = ours >= 0 && (lods[size_t(ours)].model == bestModel ||
                                            (track.sceneryModels[lods[size_t(ours)].model].boundsMin == track.sceneryModels[bestModel].boundsMin &&
                                             track.sceneryModels[lods[size_t(ours)].model].boundsMax == track.sceneryModels[bestModel].boundsMax));
            lodChecked++;
            if (same) lodSame++;
            lodNote = same ? ", lod ok" : ", LOD DIFFERS (ours " + std::to_string(ours) + ")";
        }
        std::fprintf(out, "# scenery-check transform %zu: bounds of model %zu -> instance %zu (list %u, lod list %u) %s, corner error %.3f m%s%s\n",
                     size_t(&t - transforms.data()), bestModel, bestInstance, track.sceneryInstances[bestInstance].list,
                     track.sceneryInstances[bestInstance].lodList, ok ? "matched" : "UNMATCHED", best, vertexNote.c_str(), lodNote.c_str());
    }
    std::fprintf(out, "# scenery-check: %zu bounds transforms, %zu matched (worst corner error %.3f m), %zu unmatched; %zu drawn objects, %zu/%zu vertices identical\n",
                 tested, matched, worstMatched, unmatched, drawn, drawnVerticesOk, drawnVerticesTotal);
    std::printf("scenery-check: %zu bounds transforms, %zu matched (worst %.3f m), %zu unmatched; %zu drawn, %zu/%zu vertices identical\n",
                tested, matched, worstMatched, unmatched, drawn, drawnVerticesOk, drawnVerticesTotal);
    std::printf("scenery-check: level of detail as the original chose it for %zu of %zu matched instances\n", lodSame, lodChecked);
    std::fprintf(out, "# scenery-check: level of detail as the original chose it for %zu of %zu matched instances\n", lodSame, lodChecked);
}

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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("usage: gt2play <disc.bin> [--original] [--script \"field:button[:fields],...\"] [--shot <field> <out.png>] [--prims <field> <out.txt>]\n"
                  "            [--card <mcd>] [--card2 <mcd>] [--ai-player]   (--ai-player = DEV CAPTURE AID: the original's AI drives the player's car)\n"
                  "            [--auto <from> <script.txt>] [--prims-on-call <hexaddr> <off1+off2+...> <prefix>]   (capture aids: race autopilot, captures after a call)\n"
                  "            [--poke <field> <hexaddr> <hexword>]   (DEV CAPTURE AID: writes a guest RAM word at a field; oracle captures only)\n"
                  "            [--hud2p-check <ram.bin> <out.txt> [<capture.txt>]]   (dev aid: our 2 player HUD from a capture's RAM, '# hud2p-ours' lines; pixels)");
        return 2;
    }
    try {
        bool native = true, aiPlayer = false;
        std::string cardPath = "work/memcards/card1.mcd", card2Path;
        std::vector<std::pair<uint64_t, std::string>> primsQueue;
        uint64_t shotField = 0, primsField = 0;
        std::string shotPath, script, primsPath, spuLogPath, cdLogPath, smokeLogPath;
        std::string hud2pRamPath, hud2pOutPath, hud2pCapturePath; // --hud2p-check
        uint64_t autoFrom = UINT64_MAX;          // --auto
        std::string autoScriptPath, callPrefix;   // --auto, --prims-on-call
        uint32_t callTrigger = 0;
        std::vector<uint64_t> callOffsets;
        struct Poke { uint64_t at; uint32_t address, value; };
        std::vector<Poke> pokes;
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--original") native = false;
            else if (a == "--card" && i + 1 < argc) cardPath = argv[++i];
            else if (a == "--card2" && i + 1 < argc) card2Path = argv[++i];
            else if (a == "--ai-player") aiPlayer = true; // dev capture aid (ai_player.h)
            else if (a == "--spu-log" && i + 1 < argc) spuLogPath = argv[++i]; // dev aid: voice programming + car 0's sound object per field (docs/formats/sound.md)
            else if (a == "--cd-log" && i + 1 < argc) cdLogPath = argv[++i]; // dev aid: stream commands + XA channel changes per field (docs/formats/sound.md)
            else if (a == "--smoke-log" && i + 1 < argc) smokeLogPath = argv[++i]; // dev aid: the tyre smoke pool mirrored natively, per frame (docs/formats/particles.md)
            else if (a == "--script" && i + 1 < argc) script = argv[++i];
            else if (a == "--hud2p-check" && i + 2 < argc) { // dev aid (offline) [+ <capture.txt>: the pixel check]
                hud2pRamPath = argv[i + 1];
                hud2pOutPath = argv[i + 2];
                i += 2;
                if (i + 1 < argc && argv[i + 1][0] != '-') hud2pCapturePath = argv[++i];
            }
            else if (a == "--poke" && i + 3 < argc) { // DEV CAPTURE AID: write a 32-bit word into guest RAM at a field (e.g. enable a HUD panel for its oracle)
                pokes.push_back({std::strtoull(argv[i + 1], nullptr, 10), uint32_t(std::strtoul(argv[i + 2], nullptr, 16)), uint32_t(std::strtoul(argv[i + 3], nullptr, 16))});
                i += 3;
            }
            else if (a == "--shot" && i + 2 < argc) { shotField = std::strtoull(argv[i + 1], nullptr, 10); shotPath = argv[i + 2]; i += 2; }
            else if (a == "--prims" && i + 2 < argc) { primsQueue.emplace_back(std::strtoull(argv[i + 1], nullptr, 10), argv[i + 2]); i += 2; } // repeatable, fields 8+ apart
            else if (a == "--auto" && i + 2 < argc) { autoFrom = std::strtoull(argv[i + 1], nullptr, 10); autoScriptPath = argv[i + 2]; i += 2; } // dev capture aid
            else if (a == "--prims-on-call" && i + 3 < argc) { // dev capture aid: captures relative to the first call of a routine
                callTrigger = uint32_t(std::strtoul(argv[i + 1], nullptr, 16));
                for (const char* p = argv[i + 2]; *p;) {
                    char* end = nullptr;
                    callOffsets.push_back(std::strtoull(p, &end, 10));
                    p = (end && *end == '+') ? end + 1 : (end ? end : p + std::strlen(p));
                }
                callPrefix = argv[i + 3];
                i += 3;
            }
        }
        // "field:button[:fields]", held for 6 fields unless given; "p2.<button>" presses the pad in port 2 (connected then).
        struct Press { uint64_t at, length; uint16_t mask; int port; };
        std::vector<Press> presses;
        bool pad2 = false;
        for (size_t pos = 0; pos < script.size();) {
            size_t comma = script.find(',', pos), colon = script.find(':', pos);
            if (comma == std::string::npos) comma = script.size();
            if (colon == std::string::npos || colon > comma) throw std::runtime_error("bad --script");
            size_t colon2 = script.find(':', colon + 1);
            if (colon2 > comma) colon2 = comma;
            std::string name = script.substr(colon + 1, colon2 - colon - 1);
            int port = 0;
            if (name.rfind("p2.", 0) == 0) { port = 1; name = name.substr(3); pad2 = true; }
            presses.push_back({std::strtoull(script.c_str() + pos, nullptr, 10), colon2 < comma ? std::strtoull(script.c_str() + colon2 + 1, nullptr, 10) : 6,
                               uint16_t(1u << kButtonBits.at(name)), port});
            pos = comma + 1;
        }

        std::sort(primsQueue.begin(), primsQueue.end());
        size_t primsNext = 0;
        auto nextPrims = [&] { // the next --prims capture
            if (primsNext < primsQueue.size()) { primsField = primsQueue[primsNext].first; primsPath = primsQueue[primsNext].second; primsNext++; }
            else primsPath.clear();
        };
        nextPrims();
        DiscImage disc(argv[1]);
        GtfsVolume vol(disc);
        std::string exeName;
        for (const auto& f : disc.RootFiles())
            if (f.name.rfind("SCUS_", 0) == 0 || f.name.rfind("SCPS_", 0) == 0 || f.name.rfind("SCES_", 0) == 0) exeName = f.name;
        if (exeName.empty()) throw std::runtime_error("no PS-X EXE found in disc root");

        std::puts("indexing car models...");
        SceneExtractor extractor(vol);

        Machine machine;
        machine.AttachDisc(&disc);
        machine.AttachMemoryCard(cardPath); // relative to the working directory; created when missing
        if (!card2Path.empty()) machine.AttachMemoryCard(card2Path, 1);
        machine.EnableSceneCapture();
        machine.LoadExe(ReadRootFile(disc, exeName), kStackTop);
        if (!spuLogPath.empty()) {
            machine.spu.log = std::fopen(spuLogPath.c_str(), "w");
            if (!machine.spu.log) throw std::runtime_error("cannot write " + spuLogPath);
        }
        std::FILE* cdLog = nullptr;
        if (!cdLogPath.empty()) {
            cdLog = std::fopen(cdLogPath.c_str(), "w");
            if (!cdLog || !machine.Disc()) throw std::runtime_error("cannot write " + cdLogPath);
            machine.Disc()->log = cdLog;
        }

        SceneExtractor::Scene scene; // latest extracted frame
        std::vector<GlowPrimitive> glowSubmitted; // --prims: this frame's main-view glow packets in submission order (listed at the flip)
        PrimitiveLogger primLogger;
        std::vector<uint8_t> hudRgba;
        int hudWidth = 0, hudHeight = 0;
        Machine::SceneFrame lastFrame; // --prims: the raw GTE traffic of the latest flip, for the scenery check
        LodCall pendingLod;            // --prims: the original's scenery LOD calls, those of the latest frame
        std::vector<LodCall> lodTrace, lastFrameLod, previousFrameLod;
        std::vector<uint8_t> flipRam;  // --prims: guest RAM at the latest flip
        machine.onSceneFrame = [&](const Machine::SceneFrame& frame) {
            scene = extractor.Extract(frame.transforms, frame.vertices);
            if (primLogger.out) flipRam.assign(machine.bus.Ram(), machine.bus.Ram() + 0x200000); // the HUD check's inputs of this frame
            previousFrameLod.swap(lastFrameLod);
            lastFrameLod.swap(lodTrace);
            lodTrace.clear();
            hudRgba = machine.hudGpu.DisplayRgbaTouched(hudWidth, hudHeight);
            if (primLogger.out) { // --prims: the frame's glow primitives in the GPU's order (gt2view/glow.h), then the GTE transforms
                std::fprintf(primLogger.out, "# glow-frame: %zu primitives\n", glowSubmitted.size());
                for (const GlowPrimitive& p : SortGlowPrimitives(glowSubmitted))
                    std::fprintf(primLogger.out, "# glow-ours: %s ot=%d chunk=%d\n", p.ToString().c_str(), p.otEntry, p.chunk == 0xFFFF ? -1 : int(p.chunk));
            }
            glowSubmitted.clear();
            if (primLogger.out) { // --prims: the GTE transforms of the frame, with their input vertices
                lastFrame = frame;
                std::fprintf(primLogger.out, "# flip: %zu transforms\n", frame.transforms.size());
                for (size_t i = 0; i < frame.transforms.size(); i++) {
                    const auto& t = frame.transforms[i];
                    std::fprintf(primLogger.out, "# transform %zu rt=[%d %d %d; %d %d %d; %d %d %d] tr=(%d,%d,%d) h=%u of=(%d,%d) vertices=%u:",
                                 i, t.rt[0][0], t.rt[0][1], t.rt[0][2], t.rt[1][0], t.rt[1][1], t.rt[1][2], t.rt[2][0], t.rt[2][1], t.rt[2][2],
                                 t.tr[0], t.tr[1], t.tr[2], t.h, t.ofx, t.ofy, t.vertexCount);
                    for (uint32_t k = 0; k < t.vertexCount && k < 4096; k++) {
                        const auto& v = frame.vertices[t.firstVertex + k];
                        std::fprintf(primLogger.out, " (%d,%d,%d)", v.x, v.y, v.z);
                    }
                    std::fprintf(primLogger.out, "\n");
                }
            }
        };

        if (autoFrom != UINT64_MAX || callTrigger != 0 || !primsQueue.empty()) {
            // Long unattended capture runs: opt out of Windows' power throttling (EcoQoS) of background / minimized
            // processes, which otherwise gives the run a small fraction of a core.
            PROCESS_POWER_THROTTLING_STATE throttling{};
            throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            throttling.StateMask = 0;
            SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
            SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
        }
        HINSTANCE hinst = GetModuleHandleA(nullptr);
        WNDCLASSA wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = "gt2play";
        RegisterClassA(&wc);
        RECT rc{0, 0, 1280, 720};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        HWND hwnd = CreateWindowA("gt2play", "gt2play", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hinst, nullptr);
        if (!hwnd) throw std::runtime_error("CreateWindow failed");
        // GT2_NO_FOCUS=1 (automated runs): show the window without taking the foreground, so the machine stays usable.
        // Dev tool: never take the foreground (the user keeps working while runs happen); GT2_FOCUS=1 restores activation.
        ShowWindow(hwnd, std::getenv("GT2_FOCUS") ? SW_SHOW : SW_SHOWNOACTIVATE);
        VkContext vulkan(hinst, hwnd);
        VkSceneRenderer renderer(vulkan);
        NativeScene nativeScene(renderer, vol);
        SponsorTable sponsorTable;              // .crstims.tsd, parsed when a course appears
        std::vector<SponsorUpload> sponsors;    // the boards of the current course (pointing into sponsorTable)
        std::string sponsorTrack;
        int mapCourse2P = -1;        // the 2 player HUD check: the course index whose map is loaded, and its file name
        std::string mapCourse2PName;
        std::unique_ptr<Hud> hudCheck;          // --prims: our HUD from the original's inputs
        bool tyrePanelSeen = false;             // --prims: the original drew its tyre panel (0x8002DE8C) in a captured frame
        // --prims: the chunk pass 0x80020110 (its call of 0x80020E38) and the chunk GTE setup 0x80026BB4 at this build's
        // addresses (glow check).
        const ExeProfile& buildProfile = ProfileOf(disc);
        const uint32_t glowPassAddress = buildProfile.Code(0x80020E38u), glowPassCallAddress = buildProfile.Code(0x80020194u),
                       glowChunkSetupAddress = buildProfile.Code(0x80026BB4u);
        const uint32_t glowModelDraw[2] = {buildProfile.Code(0x80019B58u), buildProfile.Code(0x8001C17Cu)},
                       glowModelCall[2] = {buildProfile.Code(0x8001FFC0u), buildProfile.Code(0x8001FFD0u)};
        uint32_t glowChunkTable = 0;

        // Audio: one waveOut buffer per field from a small pool; when the pool is busy the field is dropped.
        HWAVEOUT waveOut = nullptr;
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = Spu::kSampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = format.nSamplesPerSec * 4;
        if (shotPath.empty() && primsPath.empty() && waveOutOpen(&waveOut, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) waveOut = nullptr;
        struct AudioBuffer { WAVEHDR header{}; std::vector<int16_t> samples; bool queued = false; };
        std::vector<AudioBuffer> audioPool(12);

        timeBeginPeriod(1); // 1 ms sleep granularity: the default ~15.6 ms makes 60 Hz pacing stutter
        using Clock = std::chrono::steady_clock;
        const auto fieldTime = std::chrono::nanoseconds(16'683'350); // NTSC field
        auto nextField = Clock::now();
        auto statStart = Clock::now();
        uint64_t statFields = 0, fieldNumber = 0;
        // --smoke-log: the tyre smoke pool (particles.h) mirrored natively from the original's update call
        // (0x80016978 from 0x80015D14) and compared with the guest's pool at its draw call (0x8002EB60 from 0x800154E0).
        std::FILE* smokeLog = smokeLogPath.empty() ? nullptr : std::fopen(smokeLogPath.c_str(), "w");
        if (!smokeLogPath.empty() && !smokeLog) throw std::runtime_error("cannot write " + smokeLogPath);
        SmokeGuestMirror smokeMirror;
        std::function<void(uint32_t, uint32_t)> smokeHook;
        if (smokeLog)
            smokeHook = [&](uint32_t from, uint32_t to) {
                if (to == 0x80016978u && from == 0x80015D14u) smokeMirror.OnUpdate();
                if (to == 0x8002EB60u && from == 0x800154E0u)
                    std::fprintf(smokeLog, "field %llu %s\n", static_cast<unsigned long long>(fieldNumber), smokeMirror.OnDraw(machine.bus.Ram()).c_str());
            };
        if (aiPlayer) // dev capture aid: chained in front of the other call hooks (ai_player.h)
            smokeHook = [&machine, inner = smokeHook](uint32_t from, uint32_t to) {
                if (AiPlayerSwitch(machine, from, to) || ArcadeAiPlayerSwitch(machine, from, to)) std::puts("ai-player: the player's car starts with control class 2 (dev capture aid)");
                if (inner) inner(from, to);
            };
        RaceAutopilot autopilot;
        uint16_t autoPad = 0;
        bool callTriggered = false;
        if (autoFrom != UINT64_MAX || callTrigger != 0) // dev capture aids: the autopilot's view counters, the capture trigger
            smokeHook = [&, inner = smokeHook](uint32_t from, uint32_t to) {
                if (autoFrom != UINT64_MAX) autopilot.OnCall(to);
                if (callTrigger != 0 && to == callTrigger && !callTriggered) {
                    callTriggered = true;
                    std::vector<std::pair<uint64_t, std::string>> pending;
                    if (!primsPath.empty()) pending.emplace_back(primsField, primsPath);
                    pending.insert(pending.end(), primsQueue.begin() + std::ptrdiff_t(primsNext), primsQueue.end());
                    for (uint64_t off : callOffsets)
                        pending.emplace_back(fieldNumber + off, callPrefix + "_" + std::to_string(fieldNumber + off) + ".txt");
                    std::sort(pending.begin(), pending.end());
                    std::printf("prims-on-call: %08X called in field %llu (from %08X), %zu captures queued\n", to,
                                static_cast<unsigned long long>(fieldNumber), from, callOffsets.size());
                    std::fflush(stdout);
                    // Only captures that are not running yet are replaced (a running one keeps its hook).
                    if (!primLogger.out) {
                        primsQueue = pending;
                        primsNext = 0;
                        nextPrims();
                    } else {
                        primsQueue.assign(pending.begin() + (primsPath.empty() ? 0 : 1), pending.end());
                        primsNext = 0;
                    }
                }
                if (inner) inner(from, to);
            };
        // The 2 player Battle (game mode 0, docs/research/arcade_disc.md section 19): our split-screen HUD (Hud::Build2P,
        // 0x8002E908 for car 0 and car 1) or, with the view's split flag off (view + 0x2EA, a replay's full view), the full
        // view's (Hud::Build2PFull, 0x8002E63C for the car camera 1 follows) from the original's inputs of this frame, at the
        // build's addresses (profile).
        // Also offline: --hud2p-check <ram.bin> <out.txt> (a capture's RAM) writes the lines and exits.
        auto hud2pCheck = [&](const uint8_t* ram, std::FILE* out) {
            try {
                const GuestImage raceOverlay = LoadOverlayImage(disc, kRaceOverlayIndex);
                if (!hudCheck) hudCheck = std::make_unique<Hud>(renderer, vol, LoadExeImage(disc), raceOverlay);
                const TwoPlayerHudRam in = TwoPlayerHudFromRam(ram, buildProfile, raceOverlay, hudCheck->Strings(), tyrePanelSeen);
                if (in.valid) {
                    const bool split = in.split;
                    const std::array<HudFrame, 2>& frames = in.frames;
                    std::vector<DrawItem> hudItems;
                    const uint32_t followed = in.followed;
                    if (split) {
                        hudCheck->Build2P(frames[0], frames[1], 4.0f / 3.0f, hudItems);
                    } else {
                        // the course map of the race's course (0x800AF230 = its .crsinfo entry; the file whose CourseFileId it holds)
                        const int courseIndex = in.courseIndex;
                        if (courseIndex != mapCourse2P) {
                            mapCourse2P = courseIndex;
                            mapCourse2PName.clear();
                            const CourseInfoTable info = ParseCourseInfo(vol.Read(".crsinfo"));
                            if (size_t(courseIndex) < info.entries.size())
                                for (const GtfsEntry& e : vol.Files()) {
                                    std::string path = e.path;
                                    if (path.size() > 3 && path.compare(path.size() - 3, 3, ".gz") == 0) path.resize(path.size() - 3);
                                    if (path.rfind("crsobj/", 0) != 0 || path.size() < 11 || path.compare(path.size() - 4, 4, ".tro") != 0) continue;
                                    const std::string base = path.substr(7, path.size() - 11);
                                    if (CourseFileId(base) == info.entries[size_t(courseIndex)].fileId) { mapCourse2PName = base; break; }
                                }
                        }
                        hudCheck->UseCourse(mapCourse2PName);
                        hudCheck->Build2PFull(frames[followed], frames[0].revLimitRpm, frames[1].revLimitRpm, 4.0f / 3.0f, hudItems);
                    }
                    std::fprintf(out, "# hud2p-inputs: %s (car %u) lap %d / %d position %d / %d rpm %d / %d speed %d / %d start %d replay view %d / %d\n",
                                 split ? "split" : "full view", followed, frames[0].lap, frames[1].lap, frames[0].position, frames[1].position, frames[0].rpm,
                                 frames[1].rpm, frames[0].speedReadout, frames[1].speedReadout, frames[0].startTimer, frames[0].replayView, frames[1].replayView);
                    for (const std::string& l : hudCheck->ListPrimitives()) std::fprintf(out, "# hud2p-ours: %s\n", l.c_str());
                }
            } catch (const std::exception& e) {
                std::fprintf(out, "# hud2p-ours: not built (%s)\n", e.what());
            }
        };
        if (!hud2pRamPath.empty()) { // dev aid: the 2 player HUD check on a capture's RAM, then exit
            std::FILE* f = std::fopen(hud2pRamPath.c_str(), "rb");
            if (!f) throw std::runtime_error("cannot read " + hud2pRamPath);
            std::vector<uint8_t> ram(0x200000);
            const size_t got = std::fread(ram.data(), 1, ram.size(), f);
            std::fclose(f);
            if (got != ram.size()) throw std::runtime_error(hud2pRamPath + ": not a 2 MB RAM image");
            std::FILE* out = std::fopen(hud2pOutPath.c_str(), "w");
            if (!out) throw std::runtime_error("cannot write " + hud2pOutPath);
            hud2pCheck(ram.data(), out);
            std::fclose(out);
            if (hud2pCapturePath.empty() || !hudCheck) return 0;
            // The pixel check: the capture's frames (between "# flip" lines), the longest run of our primitives in them (E1 by its
            // texture page word), and that run rasterised with the interpreter GPU's rules on black twice - with the capture's VRAM
            // dump and with our HUD's VRAM copy (Hud::Vram): equal primitives and equal pixels = our HUD draws the original's frame.
            std::vector<std::string> ours = hudCheck->ListPrimitives();
            auto norm = [](const std::string& l) { return l.rfind("E1 ", 0) == 0 ? l.substr(0, l.find(' ', 3)) : l; };
            std::vector<std::vector<std::string>> frames(1);
            {
                std::FILE* c = std::fopen(hud2pCapturePath.c_str(), "r");
                if (!c) throw std::runtime_error("cannot read " + hud2pCapturePath);
                char line[4096];
                while (std::fgets(line, sizeof line, c)) {
                    std::string l(line);
                    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                    if (l.rfind("# flip", 0) == 0) { if (!frames.back().empty()) frames.emplace_back(); continue; }
                    const size_t space = l.find(' ');
                    if (l.empty() || l[0] < '0' || l[0] > '9' || space == std::string::npos) continue;
                    frames.back().push_back(l.substr(space + 1));
                }
                std::fclose(c);
            }
            size_t bestFrame = 0, bestAt = 0, bestLen = 0;
            for (size_t fi = 0; fi < frames.size(); fi++)
                for (size_t at = 0; at < frames[fi].size(); at++) {
                    size_t n = 0;
                    while (n < ours.size() && at + n < frames[fi].size() && norm(frames[fi][at + n]) == ours[n]) n++;
                    if (n > bestLen) bestLen = n, bestAt = at, bestFrame = fi;
                }
            const std::vector<std::string> run(frames[bestFrame].begin() + std::ptrdiff_t(bestAt), frames[bestFrame].begin() + std::ptrdiff_t(bestAt + bestLen));
            std::vector<uint16_t> captureVram(size_t(1024) * 512, 0);
            {
                std::FILE* v = std::fopen((hud2pCapturePath + ".vram.bin").c_str(), "rb");
                if (!v) throw std::runtime_error("cannot read " + hud2pCapturePath + ".vram.bin");
                const size_t words = std::fread(captureVram.data(), 2, captureVram.size(), v);
                std::fclose(v);
                if (words != captureVram.size()) throw std::runtime_error(hud2pCapturePath + ".vram.bin: short");
            }
            const std::vector<raceui::Gp0Prim> prims = raceui::ParseGp0Lines(run);
            raceui::OverlayCanvas theirs(captureVram), mine(hudCheck->Vram());
            theirs.Clear(0, 0, 320, 240), mine.Clear(0, 0, 320, 240);
            theirs.Draw(prims, 0);
            mine.Draw(prims, 0);
            size_t differing = 0;
            for (int y = 0; y < 240; y++)
                for (int x = 0; x < 320; x++) differing += (theirs.At(x, y) & 0x7FFF) != (mine.At(x, y) & 0x7FFF) ? 1 : 0;
            std::printf("hud2p-check: %zu of our %zu primitives equal in sequence (capture frame %zu at %zu); %zu differing pixels of 320 x 240 (the run "
                        "rasterised with the capture's VRAM and with ours)\n", bestLen, ours.size(), bestFrame, bestAt, differing);
            return (bestLen == ours.size() && differing == 0) ? 0 : 1;
        }
        machine.cpu.onCall = smokeHook;
        std::FILE* autoLog = autoFrom != UINT64_MAX ? stdout : nullptr;
        double busySum = 0, busyMax = 0;
        bool f12WasDown = false;

        while (g_running) {
            MSG msg;
            while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            if (!g_running) break;
            if (g_toggleNative) { native = !native; g_toggleNative = false; }

            fieldNumber++;
            uint16_t buttons = ReadPad(hwnd), buttons2 = 0;
            for (const Press& p : presses)
                if (fieldNumber >= p.at && fieldNumber < p.at + p.length) (p.port == 1 ? buttons2 : buttons) |= p.mask;
            buttons |= autoPad;
            machine.padButtons = buttons;
            machine.pad2Connected = pad2;
            machine.pad2Buttons = buttons2;
            for (const Poke& p : pokes)
                if (p.at == fieldNumber) {
                    uint8_t* w = machine.bus.Ram() + (p.address & 0x1FFFFC);
                    for (int k = 0; k < 4; k++) w[k] = uint8_t(p.value >> (8 * k));
                    std::printf("poke: field %llu %08X = %08X\n", static_cast<unsigned long long>(fieldNumber), p.address, p.value);
                }
            if (cdLog) {
                machine.Disc()->logTag = fieldNumber;
                if (fieldNumber % 60 == 0) std::fflush(cdLog);
            }
            if (machine.spu.log) { // the oracle for game/audio: car 0's sound object (car + 0xAA4) as the original computed it last frame
                machine.spu.logTag = fieldNumber;
                const uint8_t* car = machine.bus.Ram() + (0x800A9688u & 0x1FFFFF);
                const uint8_t* s = car + 0xAA4;
                auto u16 = [](const uint8_t* p) { return unsigned(p[0] | (p[1] << 8)); };
                std::fprintf(machine.spu.log, "%llu car0 rpm %u doppler %u master %u pan %u engVol %u exhVol %u squeal %u", static_cast<unsigned long long>(fieldNumber),
                             u16(car + 0x6D8), u16(s + 2), u16(s + 0xC), u16(s + 0x10), u16(s + 0x16), u16(s + 0x18), u16(s + 0x1C));
                for (int bank = 0; bank < 2; bank++) {
                    const uint8_t* e = s + 0x24 + bank * 0x28;
                    std::fprintf(machine.spu.log, " %s(vol %u/%u", bank ? "exhaust" : "engine", u16(e + 0xC), u16(e + 0xE));
                    for (int slot = 0; slot < 2; slot++) {
                        const uint8_t* sl = e + 0x10 + slot * 0xC;
                        std::fprintf(machine.spu.log, " slot%d v%d layer %d pitch %04X vol %u/%u", slot, int(int8_t(sl[0])), int(int8_t(sl[1])), u16(sl + 8), u16(sl + 4), u16(sl + 6));
                    }
                    std::fprintf(machine.spu.log, ")");
                }
                std::fprintf(machine.spu.log, "\n");
            }

            const auto busyStart = Clock::now();
            if (!primsPath.empty() && fieldNumber == primsField) {
                primLogger.out = std::fopen(primsPath.c_str(), "w");
                if (!primLogger.out) throw std::runtime_error("cannot write " + primsPath);
                machine.onGpuWord = [&](bool gp1, uint32_t w) { primLogger.Word(gp1, w); };
                // The scenery LOD choice as the original computes it (0x8001F7F8 -> 0x8007AE38 measure -> 0x8007AEF4):
                // at the measure call the combined view-space translation is at scratchpad 0x1F800014 (16.16) and the
                // instance position at 0x1F8000B0; k (a2) is set before the call, the measure is in v0 at the next one.
                machine.cpu.onCall = [&](uint32_t from, uint32_t to) {
                    if (smokeHook) smokeHook(from, to);
                    if (to == 0x80043108u && from >= 0x8002DE8Cu && from < 0x8002E204u) tyrePanelSeen = true; // the tyre panel draws
                    if (to == 0x8002EB60u && from == 0x800154E0u) // the smoke draw (a1 = s3 in the delay slot): our projection of the guest's pool
                        for (const std::string& l : SmokeGuestMirror::DrawLines(machine.bus.Ram(), machine.cpu.gpr[19])) std::fprintf(primLogger.out, "# smoke-ours: %s\n", l.c_str());
                    // Glow records (gt2view/glow.h): in each chunk pass 0x80020110 our primitives for the guest's view, taken at
                    // its first call (0x80020E38 from 0x80020194: s0 = view, a1 = the course's chunk table - 12, a2 = mirror
                    // pass; the arguments of the pass itself are not final at its jal, a2 is set in the delay slot); each chunk
                    // the original then sets up (0x80026BB4, a0 = chunk) is listed.
                    if (to == glowPassAddress && from == glowPassCallAddress && extractor.CurrentTrack()) {
                        const uint8_t* ram = machine.bus.Ram();
                        auto u32 = [&](uint32_t a) { const uint8_t* p = ram + (a & 0x1FFFFF); return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; };
                        const uint32_t view = machine.cpu.gpr[16];
                        const bool mirror = machine.cpu.gpr[6] != 0;
                        glowChunkTable = machine.cpu.gpr[5] + 12;
                        const GlowView gv = GlowView::FromViewBytes(ram + ((view + 8) & 0x1FFFFF));
                        const int cameraChunk = int32_t(u32(view + 0xA0));
                        const GlowFrame gf = BuildChunkGlows(*extractor.CurrentTrack(), gv, cameraChunk, mirror);
                        std::fprintf(primLogger.out, "# glow-pass %s camera chunk %d, %zu chunks drawn, %zu primitives; chunks", mirror ? "mirror" : "main", cameraChunk,
                                     gf.chunks.size(), gf.primitives.size());
                        for (uint16_t e : gf.chunks) std::fprintf(primLogger.out, " %u%s", e & 0x3FFF, (e >> 14) > 2 ? "g" : ""); // g: glows only in a player's race
                        std::fprintf(primLogger.out, "\n");
                        if (!mirror) glowSubmitted = gf.submitted; // the main view's ordering table: the chunks' glows, then the models'
                    }
                    // The scenery models' glows (0x8001FBA8, inline before the model's polygons 0x80019B58 / 0x8001C17C, s3 = model):
                    // our drawer on the guest's records with the model's GTE state as 0x8007B778 left it in the scratchpad.
                    if ((to == glowModelDraw[0] && from == glowModelCall[0]) || (to == glowModelDraw[1] && from == glowModelCall[1])) {
                        const uint8_t* ram = machine.bus.Ram();
                        const uint8_t* sc = machine.bus.Scratch();
                        auto rd16 = [](const uint8_t* p) { return int16_t(uint16_t(p[0] | (p[1] << 8))); };
                        auto rd32 = [](const uint8_t* p) { return int32_t(uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24); };
                        const uint32_t model = machine.cpu.gpr[19];
                        const uint16_t count = uint16_t(rd16(ram + ((model + 0x42) & 0x1FFFFF)));
                        if (count != 0) {
                            std::vector<TrackGlow> glows;
                            const uint32_t records = uint32_t(rd32(ram + ((model + 0x28) & 0x1FFFFF)));
                            for (uint32_t k = 0; k < count; k++) {
                                const uint8_t* r = ram + ((records + k * 20) & 0x1FFFFF);
                                TrackGlow g;
                                g.position = {rd16(r), rd16(r + 2), rd16(r + 4)};
                                g.size = rd16(r + 6);
                                std::memcpy(g.unknown.data(), r + 8, 8);
                                g.colour = uint32_t(rd32(r + 16));
                                glows.push_back(g);
                            }
                            int parsed = -1; // the parsed model with these records (the parser's check)
                            if (extractor.CurrentTrack())
                                for (size_t m = 0; m < extractor.CurrentTrack()->sceneryModels.size() && parsed < 0; m++) {
                                    const auto& mg = extractor.CurrentTrack()->sceneryModels[m].glows;
                                    if (mg.size() == glows.size() && std::equal(mg.begin(), mg.end(), glows.begin(), [](const TrackGlow& a, const TrackGlow& b) {
                                            return a.position == b.position && a.size == b.size && a.colour == b.colour && a.unknown == b.unknown; }))
                                        parsed = int(m);
                                }
                            GlowGte gte;
                            for (int k = 0; k < 9; k++) gte.rotation[k / 3][k % 3] = rd16(sc + 0x84 + k * 2);
                            for (int k = 0; k < 3; k++) gte.translation[k] = rd32(sc + 0x78 + k * 4);
                            gte.ofx = rd32(sc + 0x54);
                            gte.ofy = rd32(sc + 0x58);
                            gte.h = uint16_t(rd16(sc + 0x5C));
                            const std::vector<GlowPrimitive> mp = DrawModelGlows(gte, uint16_t(rd16(sc + 0x98)), glows);
                            std::fprintf(primLogger.out, "# glow-model %08X: %u records (parsed model %d), %zu primitives; shift %d rotation row 0 (%d %d %d) scale exponent %d\n", model, count, parsed, mp.size(), rd16(sc + 0x98), gte.rotation[0][0], gte.rotation[0][1], gte.rotation[0][2], parsed >= 0 ? int(extractor.CurrentTrack()->sceneryModels[size_t(parsed)].scaleExponent) : -1);
                            glowSubmitted.insert(glowSubmitted.end(), mp.begin(), mp.end());
                        }
                    }
                    if (to == glowChunkSetupAddress && glowChunkTable != 0 && extractor.CurrentTrack()) {
                        const uint8_t* ram = machine.bus.Ram();
                        const size_t n = extractor.CurrentTrack()->chunks.size();
                        for (size_t i = 0; i < n; i++) {
                            const uint8_t* p = ram + ((glowChunkTable + uint32_t(i) * 4) & 0x1FFFFF);
                            if ((uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24) == machine.cpu.gpr[4]) {
                                std::fprintf(primLogger.out, "# glow-guest-chunk %zu\n", i);
                                break;
                            }
                        }
                    }
                    auto s32 = [&](uint32_t off) { const uint8_t* p = machine.bus.Scratch() + off; return int32_t(uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24); };
                    if (to == 0x8007AE38u) {
                        std::fprintf(primLogger.out, "# lod-measure from %08X list %08X k %d t (%d %d %d) inst (%d %d %d)", from, machine.cpu.gpr[4], int32_t(machine.cpu.gpr[6]),
                                     s32(0x14), s32(0x18), s32(0x1C), s32(0xB0), s32(0xB4), s32(0xB8));
                        pendingLod = {from, {s32(0xB0), s32(0xB4), s32(0xB8)}, {s32(0x14), s32(0x18), s32(0x1C)}, int32_t(machine.cpu.gpr[6]), 0, -1};
                    }
                    else if (to == 0x8007AEF4u) { // the entry 0x8007AEF4 will pick (list = u32 count + {u32 threshold, u32 model})
                        const uint32_t measure = machine.cpu.gpr[2], list = machine.cpu.gpr[4] & 0x1FFFFF;
                        auto u32 = [&](uint32_t off) { const uint8_t* p = machine.bus.Ram() + ((list + off) & 0x1FFFFF); return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; };
                        int choice = -1;
                        for (uint32_t e = 0; e < u32(0); e++)
                            if (u32(4 + e * 8) > measure) { choice = int(e); break; }
                        std::fprintf(primLogger.out, " measure %u choice %d of %u thresholds", measure, choice, u32(0));
                        for (uint32_t e = 0; e < u32(0) && e < 8; e++) std::fprintf(primLogger.out, " %u", u32(4 + e * 8));
                        std::fprintf(primLogger.out, "\n");
                        pendingLod.measure = measure;
                        pendingLod.choice = choice;
                        lodTrace.push_back(pendingLod);
                    }
                };
            }
            std::string reason = machine.Run(Machine::kInstructionsPerVBlank);
            if (autoLog) { // the autopilot's pad for the next field (as gt2run session auto=)
                const size_t before = autopilot.Script().size();
                autoPad = fieldNumber >= autoFrom ? autopilot.Next(machine, fieldNumber, autoLog) : 0;
                if (autopilot.Script().size() != before)
                    if (std::FILE* f = std::fopen(autoScriptPath.c_str(), "w")) {
                        std::fprintf(f, "%s%s%s\n", script.c_str(), script.empty() ? "" : ",", autopilot.Script().c_str());
                        std::fclose(f);
                    }
                if (fieldNumber % 3000 == 0) {
                    std::printf("field %llu\n", static_cast<unsigned long long>(fieldNumber));
                    std::fflush(stdout);
                }
            }
            if (!primsPath.empty() && fieldNumber == primsField + 6) // the state of the captured screen (the comparisons' ram=)
                if (std::FILE* ram = std::fopen((primsPath + ".ram.bin").c_str(), "wb")) {
                    std::fwrite(machine.bus.Ram(), 1, 0x200000, ram);
                    std::fclose(ram);
                }
            if (primLogger.out && fieldNumber >= primsField + 7) { // eight fields: at least two draw lists with the transform sets reported at the following flips
                machine.onGpuWord = nullptr;
                machine.cpu.onCall = smokeHook;
                // The transforms of the frame whose primitives were just listed, with their attribution.
                std::fprintf(primLogger.out, "# scene: course %s, %zu cars\n", scene.trackName.c_str(), scene.cars.size());
                for (const auto& car : scene.cars)
                    std::fprintf(primLogger.out, "# car %s at (%.2f, %.2f, %.2f)\n", car.id.c_str(), car.world[12], car.world[13], car.world[14]);
                std::fprintf(primLogger.out, "# camera (%.2f, %.2f, %.2f) h=%g axes", scene.cameraPosition[0], scene.cameraPosition[1],
                             scene.cameraPosition[2], scene.projectionDistance);
                for (const auto& row : scene.cameraAxes) std::fprintf(primLogger.out, " [%.4f %.4f %.4f]", row[0], row[1], row[2]);
                std::fprintf(primLogger.out, "\n");
                if (scene.valid && extractor.CurrentTrack()) {
                    CheckScenery(*extractor.CurrentTrack(), scene, lastFrame.transforms, lastFrame.vertices, primLogger.out);
                    // Which traced frame goes with the extracted camera is not certain around a camera cut (the flip
                    // that reports the transforms can come before or after the frame's LOD calls): both are checked.
                    std::fputs("# scenery-lod: calls of the latest frame\n", primLogger.out);
                    CheckLodTrace(*extractor.CurrentTrack(), scene, lastFrameLod, primLogger.out);
                    // Our HUD from the original's own inputs of this frame (guest RAM), listed like the capture
                    // ("# hud-ours" lines, draw order) for comparing with the draw list above.
                    try {
                        if (!hudCheck) hudCheck = std::make_unique<Hud>(renderer, vol, LoadExeImage(disc), LoadOverlayImage(disc, kRaceOverlayIndex));
                        hudCheck->UseCourse(scene.trackName);
                        const uint8_t* ram = flipRam.size() == 0x200000 ? flipRam.data() : machine.bus.Ram();
                        auto u8 = [&](uint32_t a) { return int(ram[a & 0x1FFFFF]); };
                        auto s16 = [&](uint32_t a) { return int(int16_t(uint16_t(ram[a & 0x1FFFFF] | ram[(a + 1) & 0x1FFFFF] << 8))); };
                        auto u16 = [&](uint32_t a) { return int(uint16_t(ram[a & 0x1FFFFF] | ram[(a + 1) & 0x1FFFFF] << 8)); };
                        auto s32 = [&](uint32_t a) { return int32_t(uint32_t(u16(a)) | uint32_t(u16(a + 2)) << 16); };
                        const uint32_t car = 0x800A9688u, results = 0x801D5E88u;
                        HudFrame hf;
                        hf.gameMode = u8(0x801D5866u);
                        hf.licenseByte = u8(0x801D5867u);
                        hf.licenseType = u8(0x801C98A2u);
                        const uint32_t licence = 0x801CACFCu + uint32_t(u8(0x801D5867u)) * 0x668 + uint32_t(u8(0x801D5868u)) * 0xA4;
                        hf.licenseRecordMs = s32(licence);
                        hf.licenseRecordSpeed = s16(licence + 0x10);
                        hf.replay = u8(0x800A951Cu) != 0;
                        hf.lapCount = u8(0x801D586Bu);
                        hf.lap = s16(car + 0x634);
                        hf.position = int(int8_t(u8(car + 0x77C)));
                        hf.finished = u8(car + 0x728) != 0;
                        const int32_t clock = s32(0x80046F64u) / 3;
                        hf.totalMs = hf.finished ? s32(results + 0xF8) : clock;
                        hf.lapMs = clock - s32(car + 0x7AC);
                        hf.subFrame = u8(0x8002F864u) & 0xF;
                        for (int i = 0; i < s16(results + 4) && i < 10; i++) hf.laps.push_back(s32(results + 8 + uint32_t(i) * 20));
                        hf.resultsLapNumber = s16(results + 2);
                        hf.bestLapMs = s32(results + 0xD0);
                        hf.bestLapSpeed = s16(results + 0xD0 + 0x10);
                        const uint32_t record = uint32_t(s32(0x800A9524u));
                        if ((record & 0xFFE00000u) == 0x80000000u) { hf.recordMs = s32(record); hf.recordSpeed = s16(record + 0x10); }
                        if (hf.gameMode >= 7 && hf.gameMode <= 9) // 0x8002D20C: the machine-test record's entry 0 (career + 0x3A88 + test * 0xA4 + 4)
                            hf.machineRecord = uint32_t(s32(0x801CD368u + uint32_t(hf.gameMode - 7) * 0xA4u + 8u));
                        hf.rpm = s16(car + 0x6D8);
                        hf.revLimitRpm = u16(car + 0x134);
                        hf.redlineRpm = u16(car + 0x3C2);
                        hf.speedReadout = u16(car + 0x6DA);
                        hf.gear = u8(car + 0x644);
                        hf.clutchEngaged = u8(car + 0x645) == 1;
                        hf.turbo = s16(car + 0x154);
                        hf.boost = s16(car + 0x76E);
                        hf.startTimer = s16(0x800AF224u);  // 0x8002A19C
                        hf.messageCode = u8(car + 0x790);   // 0x8002E204
                        for (uint32_t m = 0; m < 3; m++) {  // 0x8003D7B8 on the licence settings block 0x801C98A0
                            const int b0 = u8(0x801C98A0u + 0x26 + 2 * (m + 1)), b1 = u8(0x801C98A0u + 0x27 + 2 * (m + 1));
                            hf.medalMs[m] = (b0 / 100) * 60000 + (b0 % 100) * 1000 + b1 * 10;
                        }
                        hf.timeInvalid = u8(car + 0xA8C) != 0; // the message block 0x8002D664 (car + 0xA8C..0xAA3)
                        hf.crashKind = u8(car + 0xA8D);
                        hf.captionTimer = s16(car + 0xA8E);
                        hf.splitTimer = s16(car + 0xA90);
                        hf.crashTimer = s16(car + 0xA92);
                        hf.captionMs = s32(car + 0xA94);
                        hf.caption = hudCheck->Strings().At(uint32_t(s32(car + 0xA98)));
                        hf.splitA = uint32_t(s32(car + 0xA9C));
                        hf.splitB = uint32_t(s32(car + 0xAA0));
                        hf.tyrePanel = tyrePanelSeen;       // 0x8002DE8C reached its drawing (0x80043108 called from it)
                        for (uint32_t w = 0; w < 4; w++) {
                            hf.wheelDamage[w] = u8(car + 0x4AE + w * 0x68);
                            hf.wheelWearStage[w] = int(int8_t(u8(car + 0x4CB + w * 0x68)));
                        }
                        for (int i = 0; i < u8(0x800AF231u); i++) {
                            const uint32_t c = car + uint32_t(i) * 0xB40;
                            hf.mapCars.push_back({int16_t(s16(c + 0x832)), int16_t(s16(c + 0x83A)), u8(c + 0x0E) != 0});
                        }
                        std::vector<DrawItem> hudItems;
                        hudCheck->Build(hf, 4.0f / 3.0f, hudItems);
                        std::fprintf(primLogger.out, "# hud-inputs: mode %d replay %d lap %d/%d position %d rpm %d limit %d red line %d speed %d gear %d clutch %d turbo %d laps %zu best %d record %d cars %zu start %d message %d tyres %d\n",
                                     hf.gameMode, int(hf.replay), hf.lap, hf.lapCount, hf.position, hf.rpm, hf.revLimitRpm, hf.redlineRpm, hf.speedReadout, hf.gear,
                                     int(hf.clutchEngaged), hf.turbo, hf.laps.size(), hf.bestLapMs, hf.recordMs, hf.mapCars.size(), hf.startTimer, hf.messageCode, int(hf.tyrePanel));
                        for (const std::string& l : hudCheck->ListPrimitives()) std::fprintf(primLogger.out, "# hud-ours: %s\n", l.c_str());
                    } catch (const std::exception& e) {
                        std::fprintf(primLogger.out, "# hud-ours: not built (%s)\n", e.what());
                    }
                    std::fputs("# scenery-lod: calls of the frame before\n", primLogger.out);
                    CheckLodTrace(*extractor.CurrentTrack(), scene, previousFrameLod, primLogger.out);
                }
                hud2pCheck(flipRam.size() == 0x200000 ? flipRam.data() : machine.bus.Ram(), primLogger.out);
                std::fclose(primLogger.out);
                primLogger.out = nullptr;
                int vw = Gpu::kVramWidth, vh = Gpu::kVramHeight;
                std::vector<uint8_t> rgba(size_t(vw) * vh * 4);
                const auto& vram = machine.gpu.Vram();
                for (size_t i = 0; i < vram.size(); i++) {
                    const uint16_t c = vram[i];
                    rgba[i * 4 + 0] = uint8_t(((c & 31) << 3) | ((c & 31) >> 2));
                    rgba[i * 4 + 1] = uint8_t((((c >> 5) & 31) << 3) | (((c >> 5) & 31) >> 2));
                    rgba[i * 4 + 2] = uint8_t((((c >> 10) & 31) << 3) | (((c >> 10) & 31) >> 2));
                    rgba[i * 4 + 3] = 255;
                }
                WritePngRgba(primsPath + ".vram.png", vw, vh, rgba);
                if (std::FILE* raw = std::fopen((primsPath + ".vram.bin").c_str(), "wb")) {
                    std::fwrite(vram.data(), sizeof(uint16_t), vram.size(), raw);
                    std::fclose(raw);
                }
                std::printf("wrote %s (+ .vram.png)\n", primsPath.c_str());
                nextPrims();
                if (shotPath.empty() && primsPath.empty() && (callTrigger == 0 || callTriggered)) break;
            }
            const double busy = std::chrono::duration<double>(Clock::now() - busyStart).count();
            busySum += busy;
            busyMax = std::max(busyMax, busy);
            if (reason != "instruction budget exhausted") {
                MessageBoxA(hwnd, reason.c_str(), "gt2play - guest stopped", MB_OK | MB_ICONWARNING);
                break;
            }

            const bool turbo = !shotPath.empty() || !primsPath.empty() || autoLog != nullptr || (GetForegroundWindow() == hwnd && (GetAsyncKeyState(VK_TAB) & 0x8000));
            if (waveOut && !turbo && !machine.spu.output.empty()) {
                for (AudioBuffer& b : audioPool) {
                    if (b.queued && !(b.header.dwFlags & WHDR_DONE)) continue;
                    if (b.queued) waveOutUnprepareHeader(waveOut, &b.header, sizeof(WAVEHDR));
                    b.samples = machine.spu.output;
                    b.header = WAVEHDR{};
                    b.header.lpData = reinterpret_cast<LPSTR>(b.samples.data());
                    b.header.dwBufferLength = DWORD(b.samples.size() * sizeof(int16_t));
                    waveOutPrepareHeader(waveOut, &b.header, sizeof(WAVEHDR));
                    waveOutWrite(waveOut, &b.header, sizeof(WAVEHDR));
                    b.queued = true;
                    break;
                }
            }
            machine.spu.output.clear();

            // ---- draw
            const float aspect = renderer.AspectRatio();
            const bool drawNative = native && scene.valid && extractor.CurrentTrack();
            std::vector<DrawItem> items;
            if (drawNative) {
                if (scene.trackName != sponsorTrack) { // the race's sponsor boards, from the original's own inputs (0x800275E8)
                    sponsorTrack = scene.trackName;
                    const uint8_t* ram = machine.bus.Ram();
                    std::string category = "General01";
                    if (ram[0x1D5865] != 0) category.assign(reinterpret_cast<const char*>(ram + 0x1D58A0), strnlen(reinterpret_cast<const char*>(ram + 0x1D58A0), 16));
                    const uint32_t seed = uint32_t(ram[0x1D58B0]) | uint32_t(ram[0x1D58B1]) << 8 | uint32_t(ram[0x1D58B2]) << 16 | uint32_t(ram[0x1D58B3]) << 24;
                    uint16_t flags = 0;
                    try {
                        const CourseInfoTable info = ParseCourseInfo(vol.Read(".crsinfo"));
                        const int entry = info.FindByFileName(scene.trackName);
                        if (entry >= 0) flags = info.entries[size_t(entry)].flags;
                        sponsorTable = ParseSponsorTable(vol.Read(".crstims.tsd")); // the uploads point into it
                        sponsors = PlaceSponsorBoards(sponsorTable, LoadSponsorSlots(LoadOverlayImage(disc, kRaceOverlayIndex)), category, seed, flags);
                        std::printf("sponsor boards: category %s, seed 0x%X, %zu slots\n", category.c_str(), seed, sponsors.size());
                    } catch (const std::exception& e) {
                        std::printf("sponsor boards: none (%s)\n", e.what());
                        sponsors.clear();
                    }
                }
                nativeScene.UseTrack(scene.trackName, *extractor.CurrentTrack(), -1, &sponsors);
                const std::array<float, 16> vp = SceneExtractor::ViewProjection(scene, aspect);
                // Backdrop (sky dome + ground fill) around the camera, then the course, the cars, the blended layers.
                float backdropModel[16], backdropMvp[16];
                SceneAssets::BackdropModel(scene.cameraPosition.data(), backdropModel);
                Multiply(vp.data(), backdropModel, backdropMvp);
                std::vector<DrawItem> blended;
                nativeScene.AppendBackdropItems(items, blended, backdropMvp);
                { // the course as the original selects it; its render-list rule follows the game's own flag
                    SceneAssets::TrackView view;
                    view.eye = scene.cameraPosition;
                    view.right = scene.cameraAxes[0];
                    view.forward = scene.cameraAxes[2];
                    view.projectionDistance = scene.projectionDistance;
                    view.fullDetail = machine.bus.Ram()[0x800A951Cu & 0x1FFFFF] != 0; // 1 = attract race / replay
                    nativeScene.AppendTrackItems(items, blended, vp.data(), view);
                }
                for (const auto& car : scene.cars) {
                    const int slot = nativeScene.UseCar(car.id);
                    if (slot < 0) continue;
                    float mvp[16];
                    Multiply(vp.data(), car.world.data(), mvp);
                    nativeScene.UpdateCarReflection(slot, car.world.data(), scene.cameraAxes);
                    nativeScene.AppendCarItems(items, slot, mvp, nativeScene.FindPaint(slot, machine.gpu.Vram()), 0); // paint = the one the original loaded
                }
                { // tyre smoke: the original's pool (guest RAM 0x800ADA0C) drawn by our sprite path (gt2view/particles.h)
                    const SmokePool guestSmoke = SmokeGuestMirror::ReadGuestPool(machine.bus.Ram());
                    const float up[3] = {-scene.cameraAxes[1][0], -scene.cameraAxes[1][1], -scene.cameraAxes[1][2]}; // rows: right, down, forward
                    nativeScene.AppendSmokeItems(items, blended, guestSmoke, vp.data(), scene.cameraPosition.data(), scene.cameraAxes[0].data(), up,
                                                 scene.cameraAxes[2].data(), float(scene.projectionDistance));
                }
                items.insert(items.end(), blended.begin(), blended.end());
                nativeScene.SetOverlay(hudRgba, hudWidth, hudHeight, aspect);
                std::copy(nativeScene.SkyColor().begin(), nativeScene.SkyColor().end(), renderer.clearColor);
            } else {
                int w = 0, h = 0;
                std::vector<uint8_t> rgba = machine.gpu.DisplayRgba(w, h);
                nativeScene.SetOverlay(rgba, w, h, aspect);
            }
            DrawItem overlay;
            overlay.firstVertex = SceneAssets::kOverlayVertexBase;
            overlay.vertexCount = 6;
            const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::copy(identity, identity + 16, overlay.mvp);
            items.push_back(overlay);

            std::string capture;
            const bool f12 = GetForegroundWindow() == hwnd && (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
            if (f12 && !f12WasDown) {
                std::filesystem::create_directories("work/play");
                capture = "work/play/shot_" + std::to_string(fieldNumber) + ".png";
            }
            f12WasDown = f12;
            if (!shotPath.empty() && fieldNumber == shotField) capture = shotPath;
            const bool capturing = !shotPath.empty() || !primsPath.empty() || autoLog != nullptr; // --shot / --prims runs skip presenting (a hidden window throttles it)
            if (!capturing || (!shotPath.empty() && fieldNumber + 3 >= shotField)) renderer.Draw(items, capture);
            if (!shotPath.empty() && fieldNumber >= shotField) {
                std::printf("wrote %s (native %s, course %s, %zu cars, %zu/%zu transforms explained)\n", shotPath.c_str(),
                            drawNative ? "yes" : "no", scene.trackName.c_str(), scene.cars.size(), scene.transformsExplained,
                            scene.transformsTotal);
                break;
            }

            // Pace to real time unless unthrottled.
            nextField += fieldTime;
            auto now = Clock::now();
            if (turbo || nextField < now) nextField = now;
            else std::this_thread::sleep_until(nextField);

            if (++statFields == 60) {
                double seconds = std::chrono::duration<double>(Clock::now() - statStart).count();
                char title[200];
                std::snprintf(title, sizeof(title), "gt2play - %s - %.0f%% speed - emulation avg %.1f ms, worst %.1f ms of 16.7 - field %llu",
                              drawNative ? "NATIVE scene" : "original picture", 100.0 / seconds, busySum / 60 * 1000, busyMax * 1000,
                              static_cast<unsigned long long>(fieldNumber));
                SetWindowTextA(hwnd, title);
                statStart = Clock::now();
                statFields = 0;
                busySum = busyMax = 0;
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
