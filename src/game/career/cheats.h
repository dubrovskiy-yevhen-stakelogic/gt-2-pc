#pragma once
#include "game/career/garage.h"
namespace gt2::career {
enum class Cheat { GoldLicences, MaxCredits, AddCar };
std::vector<size_t> CheatCatalogue(const CareerData& data);
// Mutate a candidate; never remove existing cars or exceed the original garage capacity.
void ApplyCheat(CareerSave& candidate, const CareerData* data, Cheat action, size_t carIndex = 0);
// Verify a complete replacement before committing; preserve a one-time pre-cheat backup.
void SaveCheat(const std::string& path, const CareerSave& before, const CareerSave& candidate);
}
