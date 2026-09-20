#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gt2vfs/disc_image.h"

namespace gt2 {

struct GtfsEntry {
    std::string path; // e.g. "carobj/a-emn.cdo.gz"
    uint32_t timestamp = 0;
    uint64_t offset = 0; // within GT2.VOL
    uint32_t size = 0;
    uint32_t index = 0;  // entry of the VOL offset table = the file number the game's VOL loader takes (docs/research/arcade_disc.md)
};

// GT2.VOL ("GTFS") read directly from the user's disc image. Layout: docs in tools/Gt2Vol/GtfsVolume.cs.
class GtfsVolume {
public:
    explicit GtfsVolume(const DiscImage& disc);

    const std::vector<GtfsEntry>& Files() const { return files_; }
    const GtfsEntry* Find(const std::string& path) const;

    std::vector<uint8_t> ReadStored(const GtfsEntry& e) const;
    // Transparently gunzips when the stored payload is a gzip stream.
    std::vector<uint8_t> Read(const GtfsEntry& e) const;
    // Looks up `path`, then `path + ".gz"`.
    std::vector<uint8_t> Read(const std::string& path) const;

private:
    const DiscImage& disc_;
    IsoFile vol_;
    std::vector<GtfsEntry> files_;
};

} // namespace gt2
