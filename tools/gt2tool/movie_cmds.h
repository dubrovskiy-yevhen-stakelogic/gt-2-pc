#pragma once
// gt2tool commands for the movies of the Arcade disc (movie_cmds.cpp, docs/formats/str_video.md).
#include "gt2vfs/disc_image.h"

namespace gt2 {
// str-info <disc> <STREAM.DAT[:n]> [--decode]: the movie table, per movie its sectors, frames, size, chunks and audio;
// --decode runs every frame through the bitstream decoder and the MDEC model.
int CmdStrInfo(const DiscImage& disc, int argc, char** argv);
// str-export <disc> <STREAM.DAT:n> <outDir> [--frames a-b] [--step n] [--no-audio]: PNG frames + audio.wav (under work\ only).
int CmdStrExport(const DiscImage& disc, int argc, char** argv);
} // namespace gt2
