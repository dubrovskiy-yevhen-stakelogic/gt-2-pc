#pragma once
namespace gt2 { class DiscImage; }
namespace gt2game {
class GameWindow;
// Original disc artwork; the console BIOS animation is a separate startup stage.
bool PlayBootScreens(GameWindow& window, const gt2::DiscImage& disc, bool sound = true);
}
