#include "boot_screen.h"
#include "game_window.h"
#include "gt2formats/boot_images.h"
#include "gt2formats/overlay_data.h"
#include "gt2view/movie_view.h"
#include <algorithm>
#include <cstdio>

namespace gt2game {
bool PlayBootScreens(GameWindow& window, const gt2::DiscImage& disc) {
    const auto exe = gt2::LoadExeImage(disc);
    // Decode both before presenting, so corrupt data cannot strand startup on a splash.
    const gt2::BootImage images[] = {gt2::LoadBootImage(exe, "logo-scea.tim"), gt2::LoadBootImage(exe, "notice.tim")};
    auto& renderer = window.Renderer();
    const float oldClear[] = {renderer.clearColor[0], renderer.clearColor[1], renderer.clearColor[2]};
    gt2view::MovieView view(renderer);
    bool closed = false;
    for (int screen = 0; screen < 2 && !closed; ++screen) {
        const auto& picture = images[screen];
        view.Upload(picture.rgb.data(), picture.width, picture.height);
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
