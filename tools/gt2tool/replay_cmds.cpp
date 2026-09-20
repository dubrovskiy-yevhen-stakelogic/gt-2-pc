// gt2tool replay-list / replay-add (replay_cmds.h).
#include "replay_cmds.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include "game/career/career_state.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/replay_card.h"

namespace gt2 {

namespace {

// The replay file of `path`: a card image's "BASCUS-94455REPLAY", or the file itself ("disc:<vol path>" = a VOL file).
std::vector<uint8_t> ReplayFileBytes(const GtfsVolume* vol, const std::string& path) {
    std::vector<uint8_t> bytes;
    if (path.rfind("disc:", 0) == 0) {
        if (!vol) throw std::runtime_error("disc: paths need the disc");
        bytes = vol->Read(path.substr(5));
    } else {
        bytes = career::ReadFileBytes(path);
    }
    if (bytes.size() == 128 * 1024 && bytes[0] == 'M' && bytes[1] == 'C') {
        std::vector<uint8_t> file = ReadReplayCardFile(bytes);
        if (file.empty()) throw std::runtime_error(path + ": no " + kReplayCardFileName + " on the card");
        return file;
    }
    return bytes;
}

void SplitIndex(const std::string& arg, std::string& path, int& index) {
    path = arg;
    index = 0;
    const size_t hash = arg.rfind('#');
    if (hash != std::string::npos && hash > 0) {
        path = arg.substr(0, hash);
        index = std::atoi(arg.c_str() + hash + 1);
    }
}

} // namespace

int CmdReplayList(int argc, char** argv) {
    if (argc < 3) throw std::runtime_error("usage: gt2tool replay-list <card.mcd | replay file | disc:<vol path> disc.bin>");
    std::unique_ptr<DiscImage> disc;
    std::unique_ptr<GtfsVolume> vol;
    if (argc > 3) {
        disc = std::make_unique<DiscImage>(argv[3]);
        vol = std::make_unique<GtfsVolume>(*disc);
    }
    const ReplayCardFile file = ReplayCardFile::FromBytes(ReplayFileBytes(vol.get(), argv[2]));
    std::printf("%s: %d block(s), %d replay(s), %d of %d sectors free, directory %s\n", argv[2], file.Blocks(), file.Count(), file.FreeSectors(), file.Total(),
                file.Valid() ? "valid" : "INVALID");
    for (int i = 0; i < file.Count(); i++) {
        const ReplayCardEntry e = file.Entry(i);
        std::printf("  %2d '%s' car '%s' mode %u flag %u ghost %u course %08X car id %08X first %d sectors %d size %d CRC %s\n", i, e.Title().c_str(), e.CarName().c_str(),
                    unsigned(e.GameMode()), unsigned(e.RaceFlag()), unsigned(e.desc[0x42]), e.CourseId(), e.CarId(), e.first, e.sectors, e.size,
                    file.EntryCrcOk(i) ? "ok" : "MISMATCH");
    }
    return file.Valid() ? 0 : 1;
}

int CmdReplayAdd(const DiscImage& disc, const GtfsVolume& vol, int argc, char** argv) {
    if (argc < 5) throw std::runtime_error("usage: gt2tool replay-add <disc> <card.mcd> <source>[#N] [title]");
    const std::string cardPath = argv[3];
    std::string sourcePath;
    int index = 0;
    SplitIndex(argv[4], sourcePath, index);
    const ReplayCardFile source = ReplayCardFile::FromBytes(ReplayFileBytes(&vol, sourcePath));
    if (index < 0 || index >= source.Count()) throw std::runtime_error("the source has no replay " + std::to_string(index));
    if (!source.EntryCrcOk(index)) throw std::runtime_error("replay " + std::to_string(index) + " of the source fails its CRC");
    std::vector<uint8_t> payload = source.EntryData(index);
    payload.resize(size_t(source.Entry(index).size));
    ReplayCardEntry desc = source.Entry(index);
    if (argc > 5) desc.SetTitle(argv[5]);

    std::vector<uint8_t> card = std::filesystem::exists(cardPath) ? career::ReadFileBytes(cardPath) : career::FormatMemoryCard();
    const std::vector<uint8_t> existing = ReadReplayCardFile(card);
    ReplayCardFile file = existing.size() >= kReplayDataStart ? ReplayCardFile::FromBytes(existing) : ReplayCardFile::Create(3, BuildReplayCardHeader(LoadExeImage(disc), 3));
    if (!file.Valid()) throw std::runtime_error("the card's replay file is not valid (0x800691DC)");
    if (!file.Store(-1, desc.desc, payload)) throw std::runtime_error("the replay does not fit (0x80069358: " + std::to_string(file.Fits(-1, int32_t(payload.size()))) + ")");
    StoreReplayCardFile(card, file.Bytes());
    career::WriteFileBytes(cardPath, card);
    std::printf("%s: replay %d of %s added as replay %d ('%s', %d sectors; %d of %d free)\n", cardPath.c_str(), index, sourcePath.c_str(), file.Count() - 1,
                desc.Title().c_str(), file.Entry(file.Count() - 1).sectors, file.FreeSectors(), file.Total());
    return 0;
}

} // namespace gt2
