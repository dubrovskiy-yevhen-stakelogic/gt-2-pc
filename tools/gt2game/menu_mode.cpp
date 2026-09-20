// gt2game --menu: the GT-mode menus (src/game/menu, gt2view/menu_view). See menu_mode.h.
#include "menu_mode.h"
#include "pc_overlay.h"

#include "platform/os/keys.h"
#include "platform/os/paths.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>

#include "game/audio/menu_audio.h"
#include "game/career/results.h"
#include "game/menu/menu_runtime.h"
#include "game_window.h"
#include "gt2export/png_writer.h"
#include "gt2formats/car_info.h"
#include "gt2formats/gt_menu.h"
#include "gt2formats/gt_menu_images.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/png_reader.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "gt2view/menu_view.h"
#include "gt2view/vk_scene_renderer.h"
#include "panel.h"

using namespace gt2;
using gt2game::GameWindow;

namespace {

uint32_t KeyBit(int key) {
    switch (key) {
    case gt2::keys::kUp: return menu::pad::kUp;
    case gt2::keys::kDown: return menu::pad::kDown;
    case gt2::keys::kLeft: return menu::pad::kLeft;
    case gt2::keys::kRight: return menu::pad::kRight;
    // The original's logical bits (0x800A6F3C): choose = cross 0x200 / circle 0x800, back = triangle 0x100 /
    // square 0x400. Enter = cross, Space = circle (the other choose: 0x95 / 0x96 step -1), Backspace / Esc = back
    // (triangle), Delete = square (scripts).
    case gt2::keys::kReturn: return menu::pad::kCross;
    case gt2::keys::kSpace: return menu::pad::kCircle;
    case gt2::keys::kBack:
    case gt2::keys::kEscape: return menu::pad::kTriangle;
    case gt2::keys::kDelete: return menu::pad::kSquare;
    case 'S': return menu::pad::kStart;
    default: return 0;
    }
}

uint32_t HeldKeys(const GameWindow& w) {
    uint32_t bits = 0;
    for (int k : {gt2::keys::kUp, gt2::keys::kDown, gt2::keys::kLeft, gt2::keys::kRight, gt2::keys::kReturn, gt2::keys::kSpace, gt2::keys::kBack, gt2::keys::kEscape, gt2::keys::kDelete, int('S')})
        if (w.Held(k)) bits |= KeyBit(k);
    return bits;
}

// Our Vulkan frame (a PNG of a 512 x 480 square-pixel window) against a gt2play --prims VRAM dump: 5-bit channels,
// ours | original | differences.
void CompareShot(const std::string& shot, const std::string& capture) {
    const PngImage ours = ReadPngFile(shot);
    std::FILE* f = std::fopen(capture.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot read " + capture);
    std::vector<uint16_t> vram(1024 * 512);
    const size_t n = std::fread(vram.data(), 2, vram.size(), f);
    std::fclose(f);
    if (n != vram.size()) throw std::runtime_error(capture + ": not a 1024 x 512 VRAM dump");
    if (ours.width != MenuCanvas::kWidth || ours.height != MenuCanvas::kHeight) {
        std::printf("compare: the shot is %d x %d (needs --window 512x480 --menu-square)\n", ours.width, ours.height);
        return;
    }
    const int W = MenuCanvas::kWidth, H = MenuCanvas::kHeight;
    std::vector<uint8_t> side(size_t(W) * 3 * H * 4, 255);
    size_t diff = 0;
    int minX = W, minY = H, maxX = -1, maxY = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t* o = &ours.rgba[(size_t(y) * W + x) * 4];
            const uint16_t c = vram[size_t(y) * 1024 + size_t(x)];
            const int cr = c & 31, cg = (c >> 5) & 31, cb = (c >> 10) & 31;
            const bool d = (o[0] >> 3) != cr || (o[1] >> 3) != cg || (o[2] >> 3) != cb;
            if (d) {
                diff++;
                minX = std::min(minX, x), minY = std::min(minY, y), maxX = std::max(maxX, x), maxY = std::max(maxY, y);
            }
            uint8_t* a = &side[(size_t(y) * W * 3 + size_t(x)) * 4];
            std::memcpy(a, o, 3);
            uint8_t* b = &side[(size_t(y) * W * 3 + size_t(W + x)) * 4];
            b[0] = uint8_t(cr << 3), b[1] = uint8_t(cg << 3), b[2] = uint8_t(cb << 3);
            uint8_t* e = &side[(size_t(y) * W * 3 + size_t(2 * W + x)) * 4];
            const uint8_t grey = uint8_t((b[0] + b[1] + b[2]) / 12);
            e[0] = d ? 255 : grey, e[1] = d ? 0 : grey, e[2] = d ? 0 : grey;
        }
    std::printf("compare %s vs %s: %zu differing pixels", shot.c_str(), capture.c_str(), diff);
    if (diff) std::printf(" (box %d,%d - %d,%d)", minX, minY, maxX, maxY);
    std::printf("\n");
    std::filesystem::path sidePath = shot;
    sidePath.replace_filename(sidePath.stem().string() + "_side.png");
    WritePngRgba(sidePath.string(), W * 3, H, side);
    std::printf("wrote %s (ours | original | differences)\n", sidePath.string().c_str());
}

} // namespace

int RunMenuSession(GameWindow& window, const DiscImage& disc, const GtfsVolume& vol, career::CareerSave& save, const MenuModeOptions& options, const MenuRaceHook& race) {
    // The card the career goes back into on F5 (--save-out .mcd over the card it came from keeps the card's other files).
    std::vector<uint8_t> inputCard;
    if (!options.careerPath.empty()) {
        std::vector<uint8_t> bytes = career::ReadFileBytes(options.careerPath);
        if (bytes.size() == 128 * 1024 && bytes[0] == 'M' && bytes[1] == 'C') inputCard = std::move(bytes);
    }
    const menu::MenuData data = menu::MenuData::Load(disc, vol);
    const MenuPages pages = MenuPages::Load(vol);
    const MenuAssets assets = MenuAssets::Load(disc, vol);
    menu::CareerMenuActions actions(save.state, data);
    std::string pendingRace;
    int pendingPath = 0;
    bool exitRequested = false;
    actions.onRace = [&](const std::string& name, int path) {
        pendingRace = name;
        pendingPath = path;
        std::printf("menu: race %s requested (0x801EF5F5 = %d)\n", name.c_str(), path);
    };
    actions.onExit = [&] { exitRequested = true; };
    actions.onNotAvailable = [](const std::string& what) { std::printf("menu: not available: %s\n", what.c_str()); };
    // Sound: the menu effects (0x80060840) and the SEQG menu music of the pages' flags (game/audio/menu_audio.h). The
    // device is closed while a race has its own (one output at a time) and opened again afterwards.
    std::unique_ptr<audio::MenuAudio> menuAudio;
    std::FILE* audioEvents = nullptr;
    auto openMenuAudio = [&](bool record) {
        if (!options.sound) return;
        try {
            menuAudio = std::make_unique<audio::MenuAudio>();
            menuAudio->Load(vol, LoadExeImage(disc), LoadOverlayImage(disc, 4));
            menuAudio->SetReverbEnabled(options.reverb);
            std::string error;
            if (record && !options.recordAudio.empty()) {
                if (!menuAudio->RecordTo(options.recordAudio)) std::printf("menu sound: cannot record to %s\n", options.recordAudio.c_str());
                const std::string eventsPath = options.recordAudio + ".events.txt"; // effects, track starts, sequence notes per frame
                audioEvents = std::fopen(eventsPath.c_str(), "w");
                if (audioEvents) menuAudio->SetEventLog(audioEvents);
            }
            if (!menuAudio->OpenDevice(error)) std::printf("menu sound: no device (%s), running silently\n", error.c_str());
        } catch (const std::exception& e) {
            std::printf("menu sound: %s - running silently\n", e.what());
            menuAudio.reset();
        }
    };
    openMenuAudio(true);
    actions.onSound = [&](int id) {
        if (menuAudio) menuAudio->Sound(id);
    };
    actions.onMusic = [&](int track) {
        std::printf("menu: music track %d\n", track);
        if (menuAudio) menuAudio->RequestMusic(track);
    };
    actions.onShowCar = [](const menu::ShowCarRequest& r) {
        std::printf("menu: show car %s paint %d (garage %d)\n", UnpackCarId(r.modelId).c_str(), r.paintIndex, r.garageIndex);
    };
    menu::MenuRuntime runtime(pages, data, actions);
    runtime.AttachLists(menu::MakeCareerPopupList(MenuListKind::kGarage, assets, data, actions),
                        menu::MakeCareerPopupList(MenuListKind::kUsedCars, assets, data, actions));
    runtime.Start(options.startPage);

    gt2view::VkSceneRenderer& renderer = window.Renderer();
    renderer.clearColor[0] = renderer.clearColor[1] = renderer.clearColor[2] = 0.0f;
    gt2view::MenuView view(renderer);
    auto carView = std::make_unique<gt2view::MenuCarView>(renderer, vol);
    std::unique_ptr<gt2game::Panels> panels;
    uint32_t logoModel = 0; // the car whose name logo is in the VRAM (0 = none)
    // Fitted wheels of the shown car (game/menu/menu_car.h): the carwheel/ table of the boot (0x8001194C) and what the view
    // shows now (model, wheel word, preview dish), set again when any of them changes.
    const GuestImage wheelExe = LoadExeImage(disc);
    const menu::MenuWheelFiles wheelFiles = menu::LoadMenuWheelFiles(vol, wheelExe);
    struct ShownWheels { uint32_t model = 0, word = 0; int dish = -1; bool valid = false; } shownWheels;
    // Comparisons with gt2play captures use the rules of the interpreter's GPU that made them (polygons / lines).
    const MenuCanvas::Rules rules = options.compare.empty() ? MenuCanvas::Rules::kPs1 : MenuCanvas::Rules::kInterpreter;
    view.SetRasterRules(rules);

    uint32_t uploadedPage = 0xFFFFFFFFu;
    uint32_t previousHeld = 0;
    int repeatTimer = 0;
    int lastShot = 0;
    for (const auto& s : options.shots) lastShot = std::max(lastShot, s.first);
    lastShot = std::max(lastShot, window.LastShotField());
    const auto fieldTime = std::chrono::nanoseconds(16'683'333); // the menus run one view update per field (NTSC 59.94 Hz)
    window.ResetPacing();
    for (;;) {
        gt2game::SetSimulationCheatContext(&save, &data.career, options.saveOut);
        const bool keepRunning = window.BeginFrame();
        gt2game::SetSimulationCheatContext(nullptr, nullptr);
        if (!keepRunning) break;
        const int field = window.Field();
        // Pad: keyboard / script (held + presses since the last field; directions auto-repeat - our choice: after 20
        // fields, every 5).
        menu::PadState pad;
        const uint32_t held = HeldKeys(window);
        uint32_t pressedKeys = 0;
        for (int k : window.PressedKeys()) pressedKeys |= KeyBit(k);
        pad.held = held;
        pad.pressed = (held & ~previousHeld) | pressedKeys;
        const uint32_t dirs = held & (menu::pad::kUp | menu::pad::kDown | menu::pad::kLeft | menu::pad::kRight);
        if (dirs && dirs == (previousHeld & dirs)) {
            if (++repeatTimer >= 20 && (repeatTimer - 20) % 5 == 0) pad.repeat = dirs;
        } else {
            repeatTimer = 0;
        }
        previousHeld = held;
        if (window.Pressed(gt2::keys::kF5)) { // ours: write the career now
            if (options.saveOut.empty()) {
                std::printf("menu: F5 - no --save-out file given, the career is not written\n");
            } else {
                career::SaveCareer(options.saveOut, save, inputCard);
                const career::CareerSave reread = career::LoadCareer(options.saveOut);
                const bool same = std::memcmp(&reread.state, &save.state, sizeof(career::CareerState)) == 0;
                std::printf("menu: F5 - career written to %s: CRC %s, reloaded state %s\n", options.saveOut.c_str(), reread.CrcOk() ? "ok" : "MISMATCH", same ? "identical" : "DIFFERS");
            }
        }

        runtime.Update(pad);
        if (menuAudio) menuAudio->Frame(); // the view update's sound part: music start countdown, sequencer tick
        for (const std::string& line : runtime.log) std::printf("menu f%d: %s\n", field, line.c_str());
        runtime.log.clear();
        if (!pendingRace.empty()) {
            const std::string name = pendingRace;
            pendingRace.clear();
            const uint32_t page = runtime.PageId();
            if (menuAudio) menuAudio->StopMusic(); // 0x80018FF0: the menus are left
            menuAudio.reset();                     // the race opens its own output
            bool ran = false;
            if (race) {
                if (!panels) panels = std::make_unique<gt2game::Panels>(renderer, disc, vol);
                try {
                    ran = race(save, name, pendingPath, &window, panels.get());
                } catch (const std::exception& e) {
                    std::printf("menu: race %s failed: %s\n", name.c_str(), e.what());
                }
            }
            if (!race) std::printf("menu: event start not wired yet (%s)\n", name.c_str());
            else if (!ran) std::printf("menu: the race path did not run %s (see above)\n", name.c_str());
            if (window.Closed()) break;
            // Back in the menus: the GT-mode entry returns to the page the race was started from (0x801EF5F8). The
            // race used the renderer's VRAM rows and vertex ranges: the page, the logo and the 3D car are loaded again.
            runtime.Start(page);
            uploadedPage = 0xFFFFFFFFu;
            logoModel = 0;
            shownWheels = ShownWheels{};
            carView = std::make_unique<gt2view::MenuCarView>(renderer, vol);
            renderer.clearColor[0] = renderer.clearColor[1] = renderer.clearColor[2] = 0.0f;
            window.SetTitle("gt2game - GT mode");
            openMenuAudio(false);
            window.ResetPacing();
            continue;
        }
        if (exitRequested) {
            std::printf("menu: exit GT mode (to the title)\n");
            break;
        }
        // The shown car (0x8001AC20): its model for the 3D view, its name logo into the VRAM (576, 164..).
        const menu::CarView& car = runtime.Car();
        bool vramDirty = runtime.PageId() != uploadedPage;
        if (car.shown && car.modelId != logoModel) {
            logoModel = car.modelId;
            std::optional<menu::MenuCarLogo> logo;
            try {
                logo = menu::LoadMenuCarLogo(vol, car.modelId);
            } catch (const std::exception& e) {
                std::printf("menu: logo of %s: %s\n", UnpackCarId(car.modelId).c_str(), e.what());
            }
            runtime.SetCarLogo(std::move(logo));
            carView->Use(car.modelId);
            shownWheels = ShownWheels{};
            vramDirty = true;
        }
        if (car.shown) { // 0x8001AC20 (garage car: CarConfig +0x00, default dish) / 0x8001AEF8 (shop preview: dish of the colour)
            const int dish = car.wheelPreview ? int(menu::MenuWheelDish(wheelExe, car.wheelColour)) : -1;
            if (!shownWheels.valid || shownWheels.model != car.modelId || shownWheels.word != car.wheelId || shownWheels.dish != dish) {
                shownWheels = ShownWheels{car.modelId, car.wheelId, dish, true};
                const int file = menu::MenuWheelFile(wheelFiles, car.wheelId);
                std::optional<menu::MenuWheelTexture> wheel;
                if (file >= 0) {
                    try {
                        wheel = menu::LoadMenuWheelTexture(vol, wheelFiles.paths[size_t(file)]);
                    } catch (const std::exception& e) {
                        std::printf("menu: wheels %s: %s\n", wheelFiles.paths[size_t(file)].c_str(), e.what());
                    }
                    std::printf("menu: wheels 0x%08X -> %s, dish %d\n", car.wheelId, wheelFiles.paths[size_t(file)].c_str(), dish);
                }
                carView->SetWheels(wheel ? &*wheel : nullptr, dish);
            }
        }
        if (vramDirty) {
            MenuVram vram;
            ComposeMenuVram(assets, runtime.Page(), vram);
            if (runtime.CarLogo()) menu::UploadMenuCarLogo(vram, *runtime.CarLogo());
            view.UploadVram(vram);
            uploadedPage = runtime.PageId();
        }
        MenuFrame frame = BuildMenuFrame(assets, runtime.Page(), runtime.RenderState(assets));
        // Type 0x0A (0x8001A8A4): the car view in the item's rectangle - the floor disc (2D, exact GTE path) and the
        // 3D car, drawn between the text OT and the late OT (MenuFrame::layer3dAt).
        const MenuItem* viewport = nullptr;
        for (const MenuItem& it : runtime.Page().items)
            if (it.Type() == 0x0A) viewport = &it;
        std::function<void(std::vector<gt2view::DrawItem>&)> layer3d;
        if (viewport && car.shown) {
            const menu::MenuCarProjection floorView = menu::MenuCarProject(*viewport, runtime.Camera(), false);
            const std::vector<MenuPrim> floor = menu::MenuCarFloor(floorView, runtime.Camera());
            frame.prims.insert(frame.prims.begin() + std::ptrdiff_t(frame.layer3dAt), floor.begin(), floor.end());
            frame.layer3dAt += floor.size();
            const menu::MenuCarProjection carProjection = menu::MenuCarProject(*viewport, runtime.Camera(), true);
            const int paint = car.paintIndex;
            gt2view::MenuCarView* cv = carView.get();
            layer3d = [&renderer, &options, cv, carProjection, paint](std::vector<gt2view::DrawItem>& out) {
                cv->Append(out, carProjection, paint, renderer.AspectRatio(), options.squarePixels);
            };
        }
        std::vector<gt2view::DrawItem> items;
        view.Build(frame, renderer.AspectRatio(), items, options.squarePixels, layer3d);
        std::string shotPath, comparePath;
        for (size_t k = 0; k < options.shots.size(); k++)
            if (options.shots[k].first == field) {
                shotPath = options.shots[k].second;
                if (k < options.compare.size()) comparePath = options.compare[k];
            }
        window.EndFrame(items, shotPath, fieldTime);
        if (!shotPath.empty()) {
            std::printf("menu f%d: page 0x%X, cursor (%d,%d) item %d, popup %d, car %s paint %d yaw %d pitch %d -> %s\n", field, runtime.PageId(), runtime.CursorState().x,
                        runtime.CursorState().y, runtime.CursorState().item, runtime.PopupMode(), car.shown ? UnpackCarId(car.modelId).c_str() : "-", car.paintIndex,
                        runtime.Camera().yaw, runtime.Camera().pitch, shotPath.c_str());
            std::printf("menu f%d: %zu primitives, 3D layer at %zu%s\n", field, frame.prims.size(), frame.layer3dAt, layer3d ? " (car view)" : "");
            if (!comparePath.empty()) {
                CompareShot(shotPath, comparePath);
                // The same primitives through the software canvas (gt2formats): separates state from GPU differences.
                MenuVram vram;
                ComposeMenuVram(assets, runtime.Page(), vram);
                if (runtime.CarLogo()) menu::UploadMenuCarLogo(vram, *runtime.CarLogo());
                MenuCanvas canvas;
                canvas.rules = rules;
                for (const MenuPrim& p : frame.prims) canvas.Draw(vram, p);
                std::filesystem::path canvasPath = shotPath;
                canvasPath.replace_filename(canvasPath.stem().string() + "_canvas.png");
                WritePngRgba(canvasPath.string(), MenuCanvas::kWidth, MenuCanvas::kHeight, canvas.Rgba());
                CompareShot(canvasPath.string(), comparePath);
            }
        }
        if (options.endAfterShots && lastShot > 0 && field >= lastShot && options.quitAfter == 0 && field >= window.ScriptEnd()) break;
        if (options.quitAfter > 0 && field + 1 >= options.quitAfter) break;
    }
    menuAudio.reset(); // closes the device (and the recording)
    if (audioEvents) std::fclose(audioEvents);
    std::printf("menu: day %u, money %d, %d car(s), current %d\n", save.state.record.days, save.state.garage.money, save.state.garage.count, save.state.garage.currentCar);
    return 0;
}

int RunMenuMode(const DiscImage& disc, const GtfsVolume& vol, const MenuModeOptions& options, const MenuRaceHook& race) {
    // The career: a save / card image, or the new game of 0x800104A0.
    career::CareerSave save;
    if (!options.careerPath.empty()) {
        save = career::LoadCareer(options.careerPath);
        std::printf("career %s: CRC %s, day %u, money %d, %d car(s)\n", options.careerPath.c_str(), save.CrcOk() ? "ok" : "MISMATCH", save.state.record.days,
                    save.state.garage.money, save.state.garage.count);
    } else {
        save.state = career::NewCareer(career::ReadNewGameDefaults(disc));
        save.header = career::BuildSaveHeader(LoadExeImage(disc));
        std::printf("career: new game (day %u, money %d)\n", save.state.record.days, save.state.garage.money);
    }
    int rc = 0;
    {
        GameWindow window("gt2game - GT mode", options.windowWidth, options.windowHeight);
        window.SetPacing(options.pacing);
        if (!options.script.empty()) window.AddScript(options.script);
        for (const auto& s : options.anyShots) window.AddShot(s.first, s.second);
        rc = RunMenuSession(window, disc, vol, save, options, race);
    }
    if (!options.saveOut.empty()) {
        std::vector<uint8_t> inputCard;
        if (!options.careerPath.empty()) {
            std::vector<uint8_t> bytes = career::ReadFileBytes(options.careerPath);
            if (bytes.size() == 128 * 1024 && bytes[0] == 'M' && bytes[1] == 'C') inputCard = std::move(bytes);
        }
        career::SaveCareer(options.saveOut, save, inputCard);
        const career::CareerSave reread = career::LoadCareer(options.saveOut);
        const bool same = std::memcmp(&reread.state, &save.state, sizeof(career::CareerState)) == 0;
        std::printf("career written to %s: CRC %s, reloaded state %s\n", options.saveOut.c_str(), reread.CrcOk() ? "ok" : "MISMATCH", same ? "identical" : "DIFFERS");
    }
    return rc;
}
