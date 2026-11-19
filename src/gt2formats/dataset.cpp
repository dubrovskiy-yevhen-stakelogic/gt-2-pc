#include "gt2formats/dataset.h"

#include "gt2formats/sha1.h"
#include "gt2vfs/gtfs.h"

#include <cstdint>
#include <vector>

namespace gt2 {
namespace {

struct KnownDataset {
    const char* fingerprint;
    const char* suffix;
    const char* label;
};

// Fingerprints of the car tables. New entries are one line; an unknown dataset still gets its own folder by hash.
//   be0dc795…  US Simulation / Combined v1.2 (retail, both discs)
//   03092e33…  Project A-Spec 1.2.1, Simulation and Combined
//   3e100467…  GT2+ Complete beta 7, Simulation, Arcade and Combined
//   a2526857…  US Arcade v1.1 (retail)
//   4c916e3b…  US Arcade v1.0 (retail)
constexpr KnownDataset kKnownDatasets[] = {
    {"be0dc795dbc8aca62d812ab5443edd8c82a2d979", "", "Original GT2"},
    {"03092e33fd4a2990bb6fd9b0c33f775227f8692b", "aspec", "Project A-Spec 1.2.1"},
    {"3e100467293cb53e574ed83a134b2d15ee8ce830", "plus", "GT2+ Complete"},
    {"a2526857eb6c9940c443a74683541d661e45abcc", "", "Original GT2 Arcade v1.1"},
    {"4c916e3b65bf66ecf3d6e1f1e42d2e682007f20a", "", "Original GT2 Arcade v1.0"},
};

constexpr const char* kTableNames[] = {".carcolor", ".carinfoa", ".carinfoe", ".carinfoj"};

} // namespace

DiscDataset IdentifyDataset(const GtfsVolume& volume) {
    DiscDataset dataset;
    std::vector<uint8_t> tables;
    for (const char* name : kTableNames) {
        const GtfsEntry* entry = volume.Find(name);
        if (!entry) return {};
        const std::vector<uint8_t> bytes = volume.Read(*entry);
        tables.insert(tables.end(), bytes.begin(), bytes.end());
    }
    dataset.fingerprint = Sha1Hex(tables);
    dataset.suffix = DatasetSuffix(dataset.fingerprint);
    for (const KnownDataset& known : kKnownDatasets)
        if (dataset.fingerprint == known.fingerprint) dataset.label = known.label;
    return dataset;
}

std::string DatasetSuffix(std::string_view fingerprint) {
    for (const KnownDataset& known : kKnownDatasets)
        if (fingerprint == known.fingerprint) return known.suffix;
    if (fingerprint.size() < 8) return {};
    return std::string(fingerprint.substr(0, 8));
}

std::string InstallFolder(std::string_view mode, std::string_view suffix) {
    return suffix.empty() ? std::string(mode) : std::string(mode) + "-" + std::string(suffix);
}

} // namespace gt2
