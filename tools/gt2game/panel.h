#pragma once
// The 2D layers of the race flow (gt2view/panel_view.h):
//   - the race overlay's own screens over the race, ported from GT2.OVL member 0 (gt2view/race_overlay_screens.h:
//     the pause menu 0x80029E80, the race-end display 0x8002B170 with the licence prize) - Overlay();
//   - our own simple panels in the menus' 512 x 480 frame drawn with the GT-mode menus' font from the disc, for the
//     screens whose original is not ported yet (DrawMenu / Text / Box).
// One console VRAM image serves both: the menus' font page (tpage 10) and the race overlay's areas.
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "game/audio/menu_audio.h"
#include "gt2formats/gt_menu_images.h"
#include "gt2view/menu_view.h"
#include "gt2view/panel_view.h"
#include "gt2view/race_menus.h"
#include "gt2view/race_overlay_screens.h"
#include "gt2view/race_result_screens.h"

namespace gt2 {
class DiscImage;
class GtfsVolume;
} // namespace gt2

namespace gt2game {

class Panels {
public:
    Panels(gt2view::VkSceneRenderer& renderer, const gt2::DiscImage& disc, const gt2::GtfsVolume& vol);
    // (Re)uploads the layers' VRAM into the panel rows.
    void Upload();

    void Clear() { prims_.clear(), overlay_.clear(), screen_.clear(), model_.reset(); }
    // A full-screen 352 x 480 frame of the race overlay's menus (gt2view/race_menus.h, GPU order) drawn instead of the
    // scene, with the VRAM of its pictures (licence: arcade/license_tim.tim, event / settings: arcade/setting.tim).
    enum class Screen { kLicence, kSettings };
    void FullScreen(std::vector<gt2::MenuPrim> prims, Screen which) { screen_ = std::move(prims), screenKind_ = which; }
    // The same with the 3D model of a post-race view (gt2view/race_result_screens.h PostRaceModel: RESULTS / the post-race
    // menu's car, the championship end's trophy) drawn by the renderer between prims[0 .. modelAt) (the clear and the
    // model's floor disc) and the rest, clipped to the model environment. `carId` / `paint` = car 0's model and paint
    // (ignored for the trophy).
    void FullScreenModel(std::vector<gt2::MenuPrim> prims, Screen which, size_t modelAt, const std::optional<gt2::screens::PostRaceModel>& model, uint32_t carId,
                         int paint);
    const gt2::RaceMenuAssets& MenuAssets(Screen which) const { return which == Screen::kLicence ? licenceAssets_ : settingsAssets_; }
    // The race overlay's primitives (GPU order) of this frame, drawn over the race / HUD before the panels. Dev aid:
    // GT2_RACE_OVERLAY_LOG=<file> appends every such frame in the capture text format (race_overlay_screens.h ListGp0),
    // one "# frame N" line before each, for comparisons with gt2play --prims captures of the original.
    void Overlay(const std::vector<gt2::raceui::Gp0Prim>& prims);
    const gt2::raceui::RaceOverlayAssets& OverlayAssets() const { return overlayAssets_; }
    // A flat rectangle (`semi`: blended 50 / 50 over what is below).
    void Box(int x, int y, int w, int h, uint32_t rgb, bool semi = false);
    // Text in the menus' font A (8-bit codes); colour 0x808080 = the glyphs' own colours. Returns the width.
    int Text(int x, int y, const std::string& text, uint32_t colour = 0x808080);
    int TextRight(int right, int y, const std::string& text, uint32_t colour = 0x808080);
    int TextCentre(int centre, int y, const std::string& text, uint32_t colour = 0x808080);
    int Width(const std::string& text) const;
    static constexpr int kLine = 20; // line pitch of the panels

    // A titled panel with lines of text and optional selectable items (the selected one highlighted); the box is
    // centred horizontally at `top`.
    struct Menu {
        std::string title;
        std::vector<std::string> lines;  // information lines above the items
        std::vector<std::string> items;  // selectable items
        int selected = 0;
        std::vector<std::string> footer; // hint lines below
        int width = 400;
    };
    void DrawMenu(const Menu& m, int top = 60);

    // Appends the panel's draw items (in front of everything drawn before).
    void Append(float windowAspect, std::vector<gt2view::DrawItem>& items);

    // The race overlay screens' effects: 0x80060840(id) with the resident bank sound/sys.ins (game/audio/menu_audio.h;
    // 0 buzzer, 1 accept, 2 back, 3 done, 5 cursor, 6 list move, 8 count tick). The output opens on the first effect
    // when enabled (SetSound) and plays next to the race's own; CloseSound() before the GT-mode menus open theirs.
    void SetSound(bool on);
    void Sound(int id);
    void Sounds(const std::vector<int>& ids) {
        for (int id : ids) Sound(id);
    }
    void SoundFrame(); // once per presented frame: the mixer's voice poll
    void CloseSound();

private:
    gt2view::PanelView view_;
    bool arcade_ = false; // the US Arcade disc: race overlay screens only, no GT-mode menu font (panel.cpp)
    gt2::MenuFonts fonts_;
    gt2::raceui::RaceOverlayAssets overlayAssets_;
    gt2::RaceMenuAssets licenceAssets_, settingsAssets_;
    std::vector<uint16_t> vram_; // 1024 x 512 console words: the overlay's areas + the menus' font page
    std::vector<gt2::MenuPrim> screen_;
    Screen screenKind_ = Screen::kLicence;
    int uploaded_ = -1;          // which image the panel rows hold: 0 = vram_, 1 / 2 = the licence / settings menus' VRAM
    std::vector<gt2::MenuPrim> prims_;
    std::vector<gt2::raceui::Gp0Prim> overlay_;
    // FullScreenModel: the frame through a MenuView on the panel rows (the far / 3D / front split), the model by MenuCarView.
    gt2view::VkSceneRenderer& renderer_;
    const gt2::GtfsVolume& vol_;
    const gt2::DiscImage& disc_;
    std::unique_ptr<gt2::audio::MenuAudio> sfx_;
    bool soundOn_ = false, soundFailed_ = false;
    std::unique_ptr<gt2view::MenuView> modelFrame_;
    std::unique_ptr<gt2view::MenuCarView> modelView_;
    std::optional<gt2::screens::PostRaceModel> model_;
    size_t modelAt_ = 0;
    uint32_t modelCar_ = 0;
    int modelPaint_ = 0;
};

// gt2game <disc> --race-screen-check <pause | licence-end> <capture.txt> <out.png> [state]: our frame of a race overlay
// screen against a gt2play --prims capture of the original - the primitive lists (the capture's text format) and both
// rasterised on black with the interpreter GPU's rules (ours with our VRAM, the capture's with its VRAM dump); writes
// the capture's frame | the original's primitives | ours | differences. State: pause <selection> <counter>;
// licence-end <timer> <time ms> <gold> <silver> <bronze> <fourth ms> [fourth-counts 0/1] [result code, 1 = pass]. Returns 0 when equal.
int RunRaceScreenCheck(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const std::vector<std::string>& args);

} // namespace gt2game
