#pragma once
#include "game_window.h"
#include "panel.h"
#include <cstdlib>
namespace gt2game {
inline void AppendNetworkPanel(Panels& panels, const GameWindow& window, float aspect, std::vector<gt2view::DrawItem>& items) {
    const size_t first=items.size();
    panels.Append(aspect,items);
    if (!window.XrPaced()) return;
    // Keep the complete panel inside the binocular HUD area. Move only this
    // menu layer, never the race HUD, road or overhead player labels.
    for(size_t i=first;i<items.size();++i) {
        auto& m=items[i].mvp;
        for(int col=0;col<4;++col) {
            m[col*4]=.82f*m[col*4]+.12f*m[col*4+3];
            m[col*4+1]*=.82f;
        }
    }
}

inline bool NetworkControllerHints(const GameWindow& w){return w.XrPaced()||w.Pad().type!=gt2::input::kTypeNone||std::getenv("GT2_STEAMDECK");}
inline std::string NetworkMenuHint(const GameWindow& w){return w.XrPaced()?"Left stick: move. A: select. B: back.":NetworkControllerHints(w)?"D-pad: move. A: select. B: back.":"Arrow keys: move. Enter: select. Esc: back.";}
inline std::string NetworkRoomHint(const GameWindow& w){return w.XrPaced()?"Menu: room. Hold: settings. L3: emotes.":NetworkControllerHints(w)?"Menu: room. Hold: settings. L3: emotes.":"Esc: room. F10: settings. G: emotes.";}
inline std::string NetworkReadyHint(const GameWindow& w){return NetworkControllerHints(w)?"A: ready / not ready. All ready: start.":"Enter: ready / not ready. All ready: start.";}
}
