#include "boot_screen.h"
#include "movie_player.h"
#include "gt2formats/hd_media.h"
#include "gt2formats/png_reader.h"
#include <filesystem>
#include "game_window.h"
#include "gt2formats/boot_images.h"
#include "gt2formats/overlay_data.h"
#include "gt2view/movie_view.h"
#include <algorithm>
#include <cstdio>

namespace gt2game {
bool PlayBootScreens(GameWindow& window, const gt2::DiscImage& disc, bool sound) {
    if (!VrMode()) {
        const auto startup = gt2::hd::StartupMovie();
        if (!startup.empty()) {
            MovieSpec spec; spec.skippable = false; spec.displayWidth=640; spec.displayHeight=480; spec.x=spec.y=0;
            try { if (PlayPreparedMovie(window,startup,spec,sound)==MovieResult::kClosed) return false; }
            catch (const std::exception& e) { std::printf("BIOS intro fallback: %s\n",e.what()); }
        }
    }
    const auto exe = gt2::LoadExeImage(disc);
    // Decode both before presenting, so corrupt data cannot strand startup on a splash.
    const gt2::BootImage images[] = {gt2::LoadBootImage(exe, "logo-scea.tim"), gt2::LoadBootImage(exe, "notice.tim")};
    auto& renderer = window.Renderer();
    const float oldClear[] = {renderer.clearColor[0], renderer.clearColor[1], renderer.clearColor[2]};
    gt2view::MovieView view(renderer);
    bool closed = false;
    for (int screen = 0; screen < 2 && !closed; ++screen) {
        const auto& picture = images[screen];
        bool hdLoaded = false;
        const auto hdPath = gt2::hd::Asset(screen ? "images/notice.tim.png" : "images/logo-scea.tim.png");
        if (!hdPath.empty()) try {
            const auto p = gt2::ReadPngFile(hdPath);
            if (int64_t(p.width)*picture.height != int64_t(p.height)*picture.width) throw std::runtime_error("HD splash aspect mismatch");
            std::vector<uint8_t> rgb(size_t(p.width)*p.height*3);
            for (size_t i=0;i<rgb.size()/3;++i) std::copy_n(p.rgba.data()+i*4,3,rgb.data()+i*3);
            view.Upload(rgb.data(),p.width,p.height,picture.width,picture.height); hdLoaded=true;
        } catch (const std::exception& e) { std::printf("HD splash fallback: %s\n",e.what()); }
        if (!hdLoaded) view.Upload(picture.rgb.data(), picture.width, picture.height);
        std::printf("startup: %s (%dx%d)\n", screen ? "GT2 notice" : "Sony Computer Entertainment", picture.width, picture.height);
        window.ResetPacing();
        const int duration = screen ? 300 : 180;
        for (int field = 0; field < duration; ++field) {
            if (!window.BeginFrame()) { closed = true; break; }
            if (window.Pressed(gt2::keys::kReturn) || window.Pressed('S') || window.Pressed(gt2::keys::kEscape)) break;
            const float brightness = std::min({1.0f, float(field + 1) / 24.0f, float(duration - field) / 24.0f});
            renderer.clearColor[0] = renderer.clearColor[1] = renderer.clearColor[2] = 0;
            std::vector<gt2view::DrawItem> items;
            view.Append(items, picture.width, picture.height, 0, 0, renderer.AspectRatio(), brightness);
            window.EndFrame(items, {}, std::chrono::nanoseconds(16'683'333));
        }
    }
    std::copy(oldClear, oldClear + 3, renderer.clearColor);
    window.ResetPacing();
    return !closed;
}
}
