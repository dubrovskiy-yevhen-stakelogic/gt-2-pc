// Checks the native car parameter record builder (src/gt2formats/car_params.*) against the six records of the
// attract race in the dump (0x801DE8BA + slot * 0x1C0, built by the original's 0x800771AC from the replay's car
// configurations and the tables of carparam/usa_gtmode_data.dat, lengths / tracks by 0x80017E74).
// Two comparisons per car:
//   raw:   the native record against the dump's, skipping the bytes the setup (0x800319A8) patches;
//   exact: the native record is handed to the original setup on the guest (which patches it exactly as the game
//          did at race start) and then compared byte for byte.
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "game/sim/race_sim.h"
#include "gt2formats/arcade_data.h"
#include "gt2formats/car_info.h"
#include "gt2formats/car_params.h"
#include "gt2vfs/gtfs.h"
#include "guest.h"

namespace gt2::verify {

namespace {

constexpr uint32_t kDumpRecords = 0x801DE8BAu;
constexpr uint32_t kRecordCopy = 0x801E8000u; // free guest RAM for the native record
constexpr uint32_t kReplayInRam = 0x801D42DCu; // where the attract race keeps arcade/demofile_us.gmr

// Bytes of the record that the setup rewrites: the first sample of every tyre curve (0x800314DC, 0x800316F4,
// 0x80031794) and slipRatioNegYs2[0], plus the last torque point and the gear ratios in the cases below.
bool PatchedBySetup(const sim::CarParams& p, size_t offset) {
    static constexpr size_t kFirstSamples[] = {
        offsetof(sim::CarParams, slipAngleXs), offsetof(sim::CarParams, slipAngleYs), offsetof(sim::CarParams, slipAngleXsRear),
        offsetof(sim::CarParams, slipAngleYsRear), offsetof(sim::CarParams, slipRatioNegXs), offsetof(sim::CarParams, slipRatioNegYs),
        offsetof(sim::CarParams, slipRatioNegYs2), offsetof(sim::CarParams, slipRatioPosXs), offsetof(sim::CarParams, slipRatioPosYs),
        offsetof(sim::CarParams, slipRatioNegXsRear), offsetof(sim::CarParams, slipRatioNegYsRear), offsetof(sim::CarParams, slipRatioNegYs2Rear),
        offsetof(sim::CarParams, slipRatioPosXsRear), offsetof(sim::CarParams, slipRatioPosYsRear), offsetof(sim::CarParams, loadGripXs),
        offsetof(sim::CarParams, loadGripXsRear), offsetof(sim::CarParams, camberGripXs), offsetof(sim::CarParams, camberGripXsRear)};
    for (size_t s : kFirstSamples)
        if (offset == s) return true;
    const size_t last = size_t(p.torquePointCount) - 1;
    if (p.torquePointCount >= 2 && p.torque[last] == 0 && (offset == offsetof(sim::CarParams, torque) + last * 2 || offset == offsetof(sim::CarParams, torque) + last * 2 + 1))
        return true;
    if (p.gearAutoSet != 0 && p.gearAutoFinal != 0 && offset >= offsetof(sim::CarParams, gearRatio) && offset < offsetof(sim::CarParams, gearRatio) + 16) return true;
    return false;
}

// The arcade race of an arcade dump (docs/research/arcade_disc.md section 11): the race launcher 0x800121DC (GT2.OVL member 3)
// fills the race block from carparam/usa_arcade_data.dat. Rows:
//   ArcadeEvent: the settings block 0x801C98A0 = the race block's event (+ 0x10 name) row + 0x44 of table 30;
//   ArcadeCars:  every entry's configuration = the rule's (player: ConfigFromCarSpec of its row of table 32 / 33; AI: OpponentCarConfig
//                of one of the event's opponent rows, both without the GT-mode flag), and the native builder's record of it =
//                the dump's record (the bytes the setup 0x800319A8 rewrites excepted, as BuildParams).
int VerifyArcadeRace(const std::vector<uint8_t>& pristine, const GtfsVolume& vol) {
    int failures = 0;
    const ArcadeData ad = ArcadeData::Load(vol);
    const uint8_t* block = &pristine[D(0x801D585Cu) & 0x1FFFFF];
    const std::string eventName(reinterpret_cast<const char*>(block + 0x10), strnlen(reinterpret_cast<const char*>(block + 0x10), 16));
    // 2 player Battle (game mode 0, event "A2P"): the menus' build 0x80010C84 copies the settings of "ATT" (member 2 0x800267B0;
    // docs/research/arcade_disc.md section 19), as Rally / Time Trial do.
    const int32_t row = ad.FindEvent(block[0x0A] == 0 ? std::string("ATT") : eventName);
    if (row < 0) {
        std::printf("ArcadeEvent skipped (the race block's event \"%s\" is not in usa_arcade_data.dat)\n", eventName.c_str());
        return 0;
    }
    const RaceEvent event = ad.EventAt(size_t(row));
    // The race load 0x8003C12C rewrites the settings of the race block's options (race_sim.h RaceLoadSettings: the 2 player
    // Battle's Slow Car Boost + 7 and Tire Damage + 3, the other modes' wear bytes of option 0) before the dump was taken.
    std::vector<uint8_t> expected(event.settings.begin(), event.settings.end());
    sim::RaceLoadSettings(block[0x0A], block[0x03], block[0x07], expected);
    size_t bad = 0;
    for (size_t i = 0; i < expected.size(); i++) bad += expected[i] != pristine[(D(0x801C98A0u) & 0x1FFFFF) + i];
    std::printf("ArcadeEvent %-4s      settings block 0x801C98A0 vs table 30 row %d + 0x44: 64 bytes, %zu differ  %s\n", eventName.c_str(), row, bad, bad ? "FAIL" : "ok");
    failures += bad ? 1 : 0;

    size_t cases = 0, mismatches = 0;
    const size_t count = block[0x5A];
    for (size_t slot = 0; slot < count && slot < kMaxCars; slot++) {
        const uint8_t* e = block + 0x5C + slot * 0xD0;
        uint32_t carId;
        std::memcpy(&carId, e, 4);
        CarConfig config;
        std::memcpy(&config, e + 8, sizeof(config));
        auto same = [&](CarConfig c) { // engineWord / exhaustByte / flags bit 1 are written back by the builder
            CarConfig a = config;
            a.engineWord = c.engineWord = 0;
            a.exhaustByte = c.exhaustByte = 0;
            a.flags = uint8_t(a.flags & 0xFDu);
            c.flags = uint8_t(c.flags & 0xFDu);
            return std::memcmp(&a, &c, sizeof(a)) == 0;
        };
        std::string rule;
        if (e[0x8E] == sim::kEntryPlayer1 || e[0x8E] == sim::kEntryPlayer2) {
            if (const auto r = ad.FindPlayerCar(carId)) {
                if (same(ad.PlayerCarConfig(*r, kArcadePlayerCarTable))) rule = "player car table 32 row " + std::to_string(*r);
                else if (same(ad.PlayerCarConfig(*r, kArcadePlayerCarTableAlt))) rule = "player car table 33 row " + std::to_string(*r);
            }
        } else {
            for (size_t k = 0; k < event.OpponentCount() && rule.empty(); k++) {
                const uint32_t n = RaceEvent::SlotOpponent(event.slots[k]) & 0xFFFF;
                if (ad.Opponent(n).spec.carId == carId && same(ad.OpponentConfig(n))) rule = "opponent " + std::to_string(n);
            }
        }
        CarConfig built = config;
        const sim::CarParams native = BuildCarParams(vol, ad.Tables(), built);
        const uint8_t* nativeBytes = reinterpret_cast<const uint8_t*>(&native);
        const uint8_t* dump = &pristine[(D(kDumpRecords) + uint32_t(slot) * uint32_t(sizeof(sim::CarParams))) & 0x1FFFFF];
        size_t recordBad = 0;
        for (size_t i = 0; i < sizeof(sim::CarParams); i++) recordBad += nativeBytes[i] != dump[i] && !PatchedBySetup(native, i);
        cases++;
        const bool ok = !rule.empty() && recordBad == 0;
        mismatches += ok ? 0 : 1;
        std::printf("           slot %zu %s kind %u grid %u: %s, record %zu byte(s) differ  %s\n", slot, UnpackCarId(carId).c_str(), e[0x8E], e[0x8D],
                    rule.empty() ? "NO arcade rule gives the configuration" : rule.c_str(), recordBad, ok ? "ok" : "FAIL");
    }
    std::printf("%-10s ovl3 0x800121DC  %zu cases, %zu mismatches  %s\n", "ArcadeCars", cases, mismatches, mismatches ? "FAIL" : "ok");
    failures += mismatches ? 1 : 0;
    return failures;
}

} // namespace

int VerifyParams(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937&, const GtfsVolume* vol) {
    if (!vol) return 0;
    if (ActiveProfile().arcade) return VerifyArcadeRace(pristine, *vol); // the arcade disc's race (its car tables and launcher)
    int failures = 0;
    const CarParamTables tables = CarParamTables::Load(*vol);
    const std::vector<uint8_t> replay = vol->Read("arcade/demofile_us.gmr");
    // The dump holds the replay image; the slots must agree with the file (otherwise the wrong replay was loaded).
    if (kReplayCarSlotOffset + kReplayCarSlotCount * kReplayCarSlotStride > replay.size()) throw std::runtime_error("demofile_us.gmr is too short");
    // Only the attract race takes its cars from this replay; a dump of any other race (a player race, a license
    // test) has other car configurations, which this check cannot reconstruct - skip it there.
    if (std::memcmp(&pristine[(D(kReplayInRam) & 0x1FFFFF) + kReplayCarSlotOffset], &replay[kReplayCarSlotOffset], kReplayCarSlotCount * kReplayCarSlotStride) != 0) {
        std::puts("BuildParams skipped (the dump is not the attract race: its car slots differ from arcade/demofile_us.gmr)");
        return 0;
    }

    size_t rawCases = 0, rawMismatches = 0, exactCases = 0, exactMismatches = 0;
    for (size_t slot = 0; slot < kReplayCarSlotCount; slot++) {
        ReplayCar car = ReplayCarAt(replay, slot);
        const CarConfig configBefore = car.config;
        const sim::CarParams native = BuildCarParams(*vol, tables, car.config);
        const uint8_t* nativeBytes = reinterpret_cast<const uint8_t*>(&native);
        const uint32_t recordAddress = D(kDumpRecords) + uint32_t(slot) * uint32_t(sizeof(sim::CarParams));
        const uint8_t* dump = &pristine[recordAddress & 0x1FFFFF];
        const CarConfig* dumpConfig = reinterpret_cast<const CarConfig*>(&pristine[(D(kReplayInRam) & 0x1FFFFF) + kReplayCarSlotOffset + slot * kReplayCarSlotStride + kReplayCarConfigOffset]);

        // raw comparison
        rawCases++;
        size_t bad = 0;
        for (size_t i = 0; i < sizeof(sim::CarParams); i++) {
            if (nativeBytes[i] == dump[i] || PatchedBySetup(native, i)) continue;
            if (bad++ < 4) std::printf("    MISMATCH slot %zu (%s) record + 0x%03zX: original %02X native %02X\n", slot, UnpackCarId(car.carId).c_str(), i, dump[i], nativeBytes[i]);
        }
        // the bytes the builder writes back into the configuration
        if (car.config.engineWord != dumpConfig->engineWord || car.config.exhaustByte != dumpConfig->exhaustByte || car.config.flags != dumpConfig->flags) {
            bad++;
            std::printf("    MISMATCH slot %zu config write-back: engineWord %04X/%04X exhaust %02X/%02X flags %02X/%02X (original/native)\n", slot, dumpConfig->engineWord,
                        car.config.engineWord, dumpConfig->exhaustByte, car.config.exhaustByte, dumpConfig->flags, car.config.flags);
        }
        rawMismatches += bad ? 1 : 0;

        // exact comparison: let the original setup patch the native record on the guest
        std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
        std::memset(guest.Scratch(), 0, kScratchSize);
        std::memcpy(guest.Ram() + (kRecordCopy & 0x1FFFFF), &native, sizeof(native));
        const uint32_t body = kCarBase + uint32_t(slot) * kCarStride + kBodyOffset;
        const uint32_t fifth = 0;
        std::memcpy(guest.Ram() + ((kStack + 0x10) & 0x1FFFFF), &fifth, 4);
        guest.Call(0x800319A8, body, kRecordCopy, 2, 0);
        exactCases++;
        size_t exactBad = 0;
        for (size_t i = 0; i < sizeof(sim::CarParams); i++) {
            const uint8_t patched = guest.Ram()[(kRecordCopy & 0x1FFFFF) + i];
            if (patched == dump[i]) continue;
            if (exactBad++ < 4) std::printf("    MISMATCH slot %zu (%s) after the setup, record + 0x%03zX: original %02X native %02X\n", slot, UnpackCarId(car.carId).c_str(), i, dump[i], patched);
        }
        exactMismatches += exactBad ? 1 : 0;
        std::printf("           slot %zu %s \"%s\": rows brakes %u chassis %u engine %u gearbox %u suspension %u lsd %u tyres %u/%u rm %u, power x%u.%03u, flags %02X  %s\n", slot,
                    UnpackCarId(car.carId).c_str(), car.name.c_str(), configBefore.brakes, configBefore.chassis, configBefore.engine, configBefore.gearbox, configBefore.suspension,
                    configBefore.lsd, configBefore.tyresFront, configBefore.tyresRear, configBefore.racingModify, native.powerPercentTop / 1000, native.powerPercentTop % 1000,
                    configBefore.flags, bad || exactBad ? "FAIL" : "ok");
    }
    Report("BuildParams", 0x800771ACu, rawCases, rawMismatches, failures);
    Report("BuildParams+Setup", 0x800319A8u, exactCases, exactMismatches, failures);

    // The stock configuration of every attract car must select the rows and settings of the replay's configuration.
    // Exceptions: the replay-only words (word00, word38, torqueMultiplier100) and, for the AI cars (flags bit 0),
    // the tyre rows - the shell gives them the stage-1 tyres instead of the stock ones.
    {
        size_t cases = 0, bad = 0;
        for (size_t slot = 0; slot < kReplayCarSlotCount; slot++) {
            const ReplayCar car = ReplayCarAt(replay, slot);
            const CarConfig stock = StockCarConfig(tables, car.carId);
            cases++;
            const uint8_t* a = reinterpret_cast<const uint8_t*>(&stock);
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&car.config);
            auto skipped = [&](size_t i) {
                if (i < offsetof(CarConfig, brakes) || i >= offsetof(CarConfig, engineWord)) return true;
                if (i >= offsetof(CarConfig, word38) && i < offsetof(CarConfig, gearRatio)) return true;
                if ((car.config.flags & 1) && i >= offsetof(CarConfig, tyresFront) && i < offsetof(CarConfig, lightweight)) return true;
                return false;
            };
            size_t differ = 0;
            for (size_t i = 0; i < sizeof(CarConfig); i++)
                if (!skipped(i) && a[i] != b[i] && differ++ < 2) std::printf("    MISMATCH slot %zu stock config + 0x%02zX: replay %02X stock %02X\n", slot, i, b[i], a[i]);
            bad += differ ? 1 : 0;
        }
        Report("StockConfig", 0x800763E8u, cases, bad, failures);
    }
    return failures;
}

} // namespace gt2::verify
