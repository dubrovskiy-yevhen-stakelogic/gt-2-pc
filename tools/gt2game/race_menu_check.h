#pragma once
// Dev check of the race overlay's menu frames (src/gt2view/race_menus.h) against gt2play captures:
//   gt2game <disc> --race-menu-check <screen> <cap.vram.bin> <side.png> [ram=<ram.bin>] [phase=N] [sel=N] [ps1]
// screen: licence | event | parts (CHANGE PARTS) | msettings (PARTS SETTING), both in their group selection state. The state of the frame (licence / test, car, medals, rows, selection, flash phase; event
// title, course, rows) is read from a gt2run RAM dump of the same field (`ram=`, taken at the capture's last frame:
// gt2play --prims F draws fields F..F+7, its VRAM dump holds the frame of RAM snap F+6), texts and times from the disc.
// Renders our frame on the software canvas with the capture's rasteriser rules (`ps1` = the PS1 rules instead),
// compares the 352 x 480 drawing area with the capture's VRAM (5-bit channels), writes ours | original | diff to
// side.png and prints the differing pixels; also compares the menu pictures' VRAM (pages 6 / 7 and 0x16) word for word.
// msettings with `ram=`: the whole PARTS SETTING page (states 0 / 1 / 2: group / row selection, the popup of sliders)
// from the dump (page [0x801C90F0] + 0x2A7C, widget 0x8005D154, popup band 0x8005D188, globals 0x801C90DC..0x801C90EC,
// the sheet 0x8016E894) drawn by gt2view's MachineSettingsPage; prints whether our 0x80055B14 gives the dump's group
// list 0x8005D140. Phase overrides: phase= (arrows) sel= (group) flash= slphase= fade= wstate= band= scroll=.
// licence with `view`: the view object (gt2view/race_menu_views.h) with every object from the dump (lists, row texts, bands,
// labels, the TRANSMISSION bar). licence / event with from=<ram.bin> fromfield=N tofield=M script=<gt2play script>: our views and
// view manager run field by field from that dump (the pad of update f = the script's presses of field f - 1; licence: SAVE REPLAY
// pushed on card=<mcd>, the leave views; event: the leave view), the views' state compared with ram= and the frame with the capture
// (docs/formats/race_screens.md 5.6).
//   gt2game <disc> --race-menu-check msettings-route <script> <start.ram.bin> <start field> <ram.bin>@<field> ...
// drives the start dump's page with our 0x80056194 on the pad of a gt2play script and compares page, widget, sliders
// and the whole settings sheet with every later dump.
#include <cstdint>
#include <vector>

#include "gt2view/race_menus.h"

namespace gt2 {
class DiscImage;
class GtfsVolume;
} // namespace gt2

// Returns 0 when the frame equals the capture, 1 when pixels differ, 2 on usage errors.
int RunRaceMenuCheck(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, int argc, char** argv);
// The event menu's state (0x800585C0: title, course, rows 0x8005D244, selection, flash) from a RAM dump.
gt2::screens::EventMenuState EventMenuStateOfRam(const std::vector<uint8_t>& bytes);
