#include "../tools/gt2game/menu_shortcuts.h"
#include <iostream>
#include <stdexcept>

static void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    try {
        using namespace gt2::input::ps1;
        gt2game::MenuShortcutState keys;
        int presses = 0;
        for (int frame = 0; frame < 600; ++frame) {
            keys.Update(kStart, false, false, true);
            presses += keys.pause;
        }
        Check(presses == 1, "holding Menu toggles pause repeatedly");
        keys.Update(0, false, false, true);
        keys.Update(kStart, false, false, true);
        Check(keys.pause, "second Menu press did not toggle pause");
        keys.Update(0, false, false, false);
        keys.Update(kStart, false, false, true);
        Check(!keys.pause, "focus regain retriggered held Menu");
        keys.Update(0, false, false, true);
        presses = 0;
        for (int frame = 0; frame < 600; ++frame) {
            keys.Update(0, true, true, true);
            presses += keys.pause;
            Check(!keys.menu, "Escape must navigate back, not act as Menu in settings");
        }
        Check(presses == 1, "repeated Escape events toggled pause while held");
        keys.Update(0, false, false, true);
        keys.Update(0, false, true, true);
        Check(keys.pause, "short keyboard tap was lost");
        for (const auto first : {kStart, kSelect}) {
            keys = {};
            keys.Update(first, false, false, true);
            keys.Update(kStart | kSelect, false, false, true);
            Check(keys.settings && !keys.pause, "settings chord depends on button order");
            for (int frame = 0; frame < 600; ++frame) {
                keys.Update(kStart | kSelect, false, false, true);
                Check(!keys.settings && !keys.pause, "held settings chord retriggered");
            }
            keys.Update(0, false, false, true);
            keys.Update(kStart | kSelect, false, false, true);
            Check(keys.settings && !keys.pause, "second settings chord did not trigger");
            keys.Update(kStart, false, false, true);
            Check(!keys.pause, "releasing View leaked a pause press");
        }
        std::cout << "Menu shortcut checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
