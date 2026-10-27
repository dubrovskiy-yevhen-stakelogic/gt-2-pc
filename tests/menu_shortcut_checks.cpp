#include "../tools/gt2game/menu_shortcuts.h"
#include <iostream>
#include <stdexcept>

static void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    try {
        using namespace gt2::input::ps1;
        gt2game::MenuInputRelease release;
        gt2::input::Ps1PadFrame pad;pad.buttons=kCircle;pad.pressureR2=180;
        release.Block(pad,0);release.Block(pad,1);
        for(int i=0;i<120;++i) {
            const auto filtered=release.Filter(pad,0);
            Check(filtered.buttons==0 && filtered.pressureR2==0,"menu exit leaked held pad input");
        }
        pad.buttons|=kCross;
        Check(release.Filter(pad,0).buttons==kCross,"fresh unrelated press was swallowed");
        Check(release.Filter(pad,1).buttons==kCross,"second pad leaked menu cancel");
        pad.buttons=0;pad.pressureR2=0;release.Filter(pad,0);
        pad.buttons=kCircle;pad.pressureR2=180;
        Check(release.Filter(pad,0).buttons==kCircle && release.Filter(pad,0).pressureR2==180,"new press after release stayed blocked");
        release.keys[27]=true;release.UpdateKey(27,true);
        Check(release.KeyBlocked(27),"held keyboard menu exit was not blocked");
        release.UpdateKey(27,false);release.UpdateKey(27,true);
        Check(!release.KeyBlocked(27),"keyboard release did not rearm input");
        gt2game::MenuShortcutState keys;
        {
            using Clock=std::chrono::steady_clock;
            const auto start=Clock::time_point{};
            gt2game::MenuShortcutState hold;
            hold.Update(kStart,false,false,true,start);
            Check(hold.menu && !hold.settings,"short Menu behavior changed");
            hold.Update(kStart,false,false,true,start+std::chrono::milliseconds(999));
            Check(!hold.settings,"long Menu fired before one second");
            hold.Update(kStart,false,false,true,start+std::chrono::seconds(1));
            Check(hold.settings && !hold.pause && !hold.menu,"one-second Menu did not open settings");
            hold.Update(kStart,false,false,true,start+std::chrono::seconds(5));
            Check(!hold.settings,"held Menu reopened settings repeatedly");
            hold.Update(0,false,false,true,start+std::chrono::seconds(6));
            hold.Update(kStart,false,false,true,start+std::chrono::seconds(7));
            hold.Update(0,false,false,true,start+std::chrono::milliseconds(7500));
            hold.Update(kStart,false,false,true,start+std::chrono::seconds(8));
            Check(!hold.settings,"separate short holds accumulated into a long press");
            hold.Update(kStart,false,false,true,start+std::chrono::seconds(9));
            Check(hold.settings,"second long press did not rearm");
        }
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
