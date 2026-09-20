#pragma once
// gt2tool commands of the per-build address profile (gt2formats/exe_profile.h, docs/research/arcade_disc.md section 8).
#include <string>

#include "gt2formats/exe_map.h"

namespace gt2 {

// exe-map ... --ranges-yaml <file>: the GENERATED block of range facts for db/<build B>_symbols.yaml: the runs of lui-built
// data references of aligned code (one delta per run, filtered: see the file's header) and the word-alignment runs of
// every module image (code + static data).
void WriteProfileRangesYaml(const CodeModules& a, const CodeModules& b, const ProgramMap& map, const std::string& path);

// gen-profile --build <name>=<db.yaml>... [--out <exe_profiles.inc>] [--check <Sim disc> <build disc>]:
// the C++ table of every build's range facts (kind data / data-range / aligned-range entries of the yaml files), emitted
// as `constexpr std::array<ProfileRange, N> k<Name>Ranges`. --check reports, for every fact inside a module image,
// whether the bytes of both builds are identical over the fact's size.
int CmdGenProfile(int argc, char** argv);

// profile-check <disc> <dir>...: every 0x80xxxxxx literal of the sources resolved through the disc's profile (race map):
// how many resolve, and the ones that do not (they need a fact).
int CmdProfileCheck(int argc, char** argv);

} // namespace gt2
