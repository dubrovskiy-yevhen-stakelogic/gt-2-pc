#pragma once
// gt2tool commands for the replay file "BASCUS-94455REPLAY" of memory cards and the demo files (replay_cmds.cpp,
// docs/formats/replay.md section 9).
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"

namespace gt2 {
// replay-list <card.mcd | replay file | disc:<vol path> <disc.bin>>: the directory (entries, kinds, sectors, CRCs, the directory's validity).
int CmdReplayList(int argc, char** argv);
// replay-add <disc> <card.mcd> <source>[#N] [title]: entry N (default 0) of a card image, a replay file or a demo file of
// the disc ("disc:arcade/demofile_us.gmr#3") stored as a new entry of the card's replay file (0x80069418; the file is
// created with 3 blocks when the card has none). Test data for the Replay Theater screens (work\ only).
int CmdReplayAdd(const DiscImage& disc, const GtfsVolume& vol, int argc, char** argv);
} // namespace gt2
