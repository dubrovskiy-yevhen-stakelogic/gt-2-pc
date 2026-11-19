#pragma once

#include <string>
#include <string_view>

namespace gt2 {

class GtfsVolume;

// The car tables a career's car ids resolve through (.carcolor / .carinfoa / .carinfoe / .carinfoj). Two discs with
// different tables read the same save as different cars and power figures, so their hash names the dataset a save
// belongs to. Silent's combined disc copies the Simulation volume's tables unchanged, so a combined and a standalone
// disc of the same mod share a dataset (docs/MOD-SUPPORT.md).
struct DiscDataset {
    std::string fingerprint; // SHA-1 of the four tables; empty when the volume lacks them
    std::string suffix;      // save/install suffix: "" for the retail tables, else a registered name or 8 hex digits
    std::string label;       // display name of a registered dataset, else empty
};

// Reads the four car tables of an installed volume.
DiscDataset IdentifyDataset(const GtfsVolume& volume);

// The suffix for a car-table fingerprint. Known datasets get a readable name ("aspec", "plus"); any other dataset
// gets its first eight hex digits, so a new mod keeps its saves apart without a code change.
std::string DatasetSuffix(std::string_view fingerprint);

// "simulation" / "combined", plus "-<suffix>" for a modified dataset.
std::string InstallFolder(std::string_view mode, std::string_view suffix);

} // namespace gt2
