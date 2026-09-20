#pragma once
// Dev check of CHANGE PARTS (gt2view/change_parts.h) against gt2play captures of the original, either disc:
//   gt2game <disc> --change-parts-check <ram.bin> <open field> <script> <out dir> <cap.vram.bin>@<field> ...
// <ram.bin>: a capture's RAM while the page is shown (gt2play --prims writes <cap>.ram.bin): the race car's sheet (0x8016E894,
// the arcade's through the build profile), the race block's garage car (+ 0x582 / + 0x584 into the career's garages) and
// restrictions (+ 0x586 / + 0x588). <open field>: the field of the original's "Settings ..." choice (the view is pushed in the
// next field: 0x800572C4). <script>: the gt2play script of the capture (only presses after the open field matter: the page's pad
// bits in the field of the press). Then the page runs field by field with our port and every capture (VRAM of field F + 6, as
// gt2play dumps it) is compared with our frames of F + 4 .. F + 8 (the software canvas with the interpreter's rules; the view's
// header as settled): the best field is printed, ours | original | diff written to <out dir>. Returns 1 when a capture differs.
// The pages run in the view manager's stack as the arcade session runs them (arcade_post_race.cpp): L1 goes on to PARTS SETTING
// (gt2view/change_parts.h PartsSettingView) with the manager's sideways slide, its R1 back to CHANGE PARTS; each exit commits
// (0x80056FF0); leaving either page ends the run. after=<ram.bin>: the commits against a dump after the page was left.
namespace gt2 {
class DiscImage;
class GtfsVolume;
} // namespace gt2

int RunChangePartsCheck(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, int argc, char** argv);
