#pragma once
// Dev check of the machine-test views (src/gt2view/machine_test_views.h) against gt2play captures, through the post-race
// views' check (race_result_check.h): gt2game <disc> --race-menu-check <mtmenu|mtnewrecord|mtresults|mtrecords> <cap.txt.vram.bin>
// <side.png> [ram=<ram.bin>] [advance=N [press=K:button,...]] [ps1] [vulkan]. The view's objects are read from the capture's RAM
// dump (W = *0x801C90A0, the view object = the manager's current / previous view); the rest (primitive sequence, pixels outside
// the 3D car's area, the car's floor / silhouette) is the post-race check's.
#include <cstdint>
#include <memory>
#include <string>

namespace gt2 {
struct GuestImage;
struct RaceMenuAssets;
namespace screens {
class PostRaceView;
}
} // namespace gt2

bool IsMachineTestScreen(const std::string& screen);
// The view of `screen` from the dump when it is the manager's current view (M + 0x1C8) or, during a switch (M + 0x211 > 0), the
// previous one (M + 0x1CC); `viewAddress` = that view (0 when neither: the view is not in the dump). `sim` >= 0: the view set up
// from the dump's race / career instead of read (RESULTS: the record entry's rank and the results record).
std::unique_ptr<gt2::screens::PostRaceView> MachineTestViewFromRam(const gt2::RaceMenuAssets& assets, const gt2::GuestImage& ram, const std::string& screen,
                                                                    uint32_t& viewAddress);
