#pragma once
// gt2tool commands over the GT-mode menu pages (menu_cmds.cpp).
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"

namespace gt2 {
// menu-page <disc> <page> [--png out.png] [--money N] [--day N] [--no-cursor] [--compare capture.vram.bin [--side out.png]]
int CmdMenuPage(const DiscImage& disc, const GtfsVolume& vol, int argc, char** argv);
// menu-dump <disc> <outDir> [--no-png]
int CmdMenuDump(const DiscImage& disc, const GtfsVolume& vol, int argc, char** argv);
} // namespace gt2
