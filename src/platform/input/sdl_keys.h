#pragma once
#include "platform/os/keys.h"
#include <SDL.h>

namespace gt2::input {
inline int SdlKey(SDL_Scancode code) {
    if (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z) return 'A' + code - SDL_SCANCODE_A;
    if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_9) return '1' + code - SDL_SCANCODE_1;
    using namespace gt2::keys;
    if (code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F12) return kF1 + code - SDL_SCANCODE_F1;
    switch (code) {
    case SDL_SCANCODE_TAB: return kTab;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return kControl;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return kAlt;
    case SDL_SCANCODE_END: return kEnd;
    case SDL_SCANCODE_INSERT: return kInsert;
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return kReturn;
    case SDL_SCANCODE_BACKSPACE: return kBack;
    case SDL_SCANCODE_DELETE: return kDelete;
    case SDL_SCANCODE_ESCAPE: return kEscape;
    case SDL_SCANCODE_SPACE: return kSpace;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return kShift;
    case SDL_SCANCODE_PAGEUP: return kPageUp;
    case SDL_SCANCODE_PAGEDOWN: return kPageDown;
    case SDL_SCANCODE_HOME: return kHome;
    case SDL_SCANCODE_LEFT: return kLeft;
    case SDL_SCANCODE_RIGHT: return kRight;
    case SDL_SCANCODE_UP: return kUp;
    case SDL_SCANCODE_DOWN: return kDown;
    case SDL_SCANCODE_F5: return kF5;
    case SDL_SCANCODE_F8: return kF8;
    case SDL_SCANCODE_F10: return kF10;
    default: return 0;
    }
}
}
