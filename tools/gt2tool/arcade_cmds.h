#pragma once
// gt2tool commands for the US Arcade v1.1 disc (arcade_cmds.cpp).
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"

namespace gt2 {
// arcade-entries <disc> <ram.bin> [table file]: a race dump's race entries (configurations, kinds, grid) against the stock
// configurations of the table file and the dump's records against the native builder.
int CmdArcadeEntries(const DiscImage& disc, const GtfsVolume& vol, int argc, char** argv);
} // namespace gt2
