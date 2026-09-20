// Licence tests from the disc (src/gt2formats/license_data.*) against the original: the name / record lookups of the
// licence database (0x80076C74, 0x800781E0, 0x8007830C), the medal times (0x8003D7B8), and - on a dump of a licence
// test (work/re/license_race: B-1) - the test the native parser reads for the dump's test name must equal what the
// original had in memory: the race settings block 0x801C98A0, the course, the licence car (slot 0x801D58B8), its
// configuration (0x801D58C0) and its parameter record (0x801DE8BA). Also the result record clear 0x8005E2FC that
// the race load runs for the players (race_shell.h InitPlayerResults), which runs on any dump.
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>

#include "game/sim/race_shell.h"
#include "gt2formats/car_info.h"
#include "gt2formats/car_params.h"
#include "gt2formats/gtmode_tables.h"
#include "gt2formats/license_data.h"
#include "gt2vfs/gtfs.h"
#include "guest.h"

namespace gt2::verify {

namespace {

template <typename T> T Get(const uint8_t* ram, uint32_t address) { T v; std::memcpy(&v, ram + (address & 0x1FFFFF), sizeof(T)); return v; }

constexpr uint32_t kLicenseDatabase = 0x80092E6Cu;  // *: the relocated carparam/usa_license_data.dat
constexpr uint32_t kTestName = 0x801D586Cu;         // the shell's current test name ("LJB00")
constexpr uint32_t kSettings = 0x801C98A0u;         // the race settings block
constexpr uint32_t kCarSlot0 = 0x801D58B8u;         // the shell's car slot 0: { u32 carId; u32; CarConfig; ... }
constexpr uint32_t kCarRecord0 = 0x801DE8BAu;       // slot 0's car parameter record (0x1C0)
constexpr uint32_t kCarCountTick = 0x800AF231u, kGameMode = 0x801D5866u;
constexpr uint32_t kResults1 = 0x801D5E88u;
constexpr uint32_t kScratchString = 0x801FD000u;    // guest RAM below the harness stack for the name argument
constexpr uint32_t kRecordCopy = 0x801FC000u;       // ... and for the native record handed to the original setup

void PutString(uint8_t* ram, uint32_t address, const std::string& s) {
    std::memcpy(ram + (address & 0x1FFFFF), s.c_str(), s.size() + 1);
}

} // namespace

int VerifyLicense(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const GtfsVolume* vol, const std::string& trackName) {
    int failures = 0;

    // ---- 0x8005E2FC: the result record clear (any dump; random record contents)
    {
        const StatefulResult r = VerifyStateful(
            guest, pristine, 0x8005E2FCu, {D(kResults1)}, 200,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                if (variant % 4 == 0) return;
                for (uint32_t i = 0; i < sizeof(sim::PlayerResults); i++) ram[(object & 0x1FFFFF) + i] = uint8_t(rng());
            },
            [&](uint8_t* ram, uint8_t*, uint32_t object) { sim::InitPlayerResults(*reinterpret_cast<sim::PlayerResults*>(ram + (object & 0x1FFFFF))); });
        Report("ResultInit", 0x8005E2FCu, r.cases, r.mismatches, failures);
    }

    if (!vol) return failures;
    const uint32_t database = Get<uint32_t>(pristine.data(), D(kLicenseDatabase));
    if ((database & 0xFF000000u) != 0x80000000u || std::memcmp(&pristine[database & 0x1FFFFF], "GTDT", 4) != 0) {
        std::puts("License    skipped (no licence database at *0x80092E6C in this dump)");
        return failures;
    }
    const LicenseData lic = LicenseData::Load(*vol);
    auto call = [&](uint32_t function, uint32_t a0, const std::string& name, uint32_t a1Extra = 0) {
        std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
        PutString(guest.Ram(), kScratchString, name);
        return guest.Call(function, a0, a1Extra ? a1Extra : kScratchString);
    };

    // ---- 0x80076C74: name pool lookup (every pool name and a few absent ones)
    {
        const uint16_t entries = Get<uint16_t>(pristine.data(), database + 6);
        const uint32_t pool = Get<uint32_t>(pristine.data(), database + (entries >> 1) * 8 + 0xF8);
        size_t cases = 0, bad = 0;
        std::vector<std::string> names = lic.Names();
        names.insert(names.end(), {"LJB10", "", "ljb00", "TC_lisenc", "LJB000"});
        for (const std::string& n : names) {
            const int32_t original = int32_t(call(0x80076C74u, pool, n));
            cases++;
            if (original != lic.NameIndex(n) && bad++ < 3) std::printf("    MISMATCH name \"%s\": original %d native %d\n", n.c_str(), original, lic.NameIndex(n));
        }
        Report("LicName", 0x80076C74u, cases, bad, failures);
    }
    // ---- 0x800781E0 / 0x8007830C: the test row and its record (every test; absent names)
    {
        size_t cases = 0, bad = 0;
        std::vector<std::string> names;
        for (size_t row = 0; row < lic.TestCount(); row++) names.push_back(lic.TestAt(row).name);
        names.insert(names.end(), {"LJB10", "nothing"});
        for (const std::string& n : names) {
            const int32_t row = int32_t(call(0x800781E0u, database, n));
            const uint32_t record = call(0x8007830Cu, database, n);
            const int32_t nativeRow = lic.FindTestRow(n);
            cases++;
            bool ok = row == nativeRow;
            if (ok && nativeRow >= 0) ok = record != 0 && std::memcmp(&guest.Ram()[record & 0x1FFFFF], lic.TestAt(size_t(nativeRow)).bytes.data(), kLicenseTestRowSize) == 0;
            if (ok && nativeRow < 0) ok = record == 0;
            if (!ok && bad++ < 3) std::printf("    MISMATCH test \"%s\": original row %d record %08X, native row %d\n", n.c_str(), row, record, nativeRow);
        }
        Report("LicFind", 0x8007830Cu, cases, bad, failures);
    }
    // ---- 0x8003D7B8: the medal times of every test (medals 0..4)
    {
        size_t cases = 0, bad = 0;
        for (size_t row = 0; row < lic.TestCount(); row++) {
            const LicenseTest t = lic.TestAt(row);
            const uint32_t record = call(0x8007830Cu, database, t.name);
            for (uint32_t medal = 0; medal <= 4; medal++) {
                std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
                const uint32_t original = guest.Call(0x8003D7B8u, record + uint32_t(kLicenseSettingsOffset), medal);
                const uint32_t native = t.MedalTime(medal);
                const uint32_t shell = sim::LicenseTargetTime(t.settings.data(), medal);
                cases++;
                if ((original != native || original != shell) && bad++ < 3)
                    std::printf("    MISMATCH %s medal %u: original %u native %u shell %u\n", t.name.c_str(), medal, original, native, shell);
            }
        }
        Report("LicMedal", 0x8003D7B8u, cases, bad, failures);
    }

    // ---- the licence car rows and the race builder (mode 3 dumps: the licence database is the selected one)
    if (Get<uint32_t>(pristine.data(), D(0x80092878u)) != 3) {
        std::puts("LicSpec    skipped (0x80092878 != 3: the licence database is not selected in this dump)");
    } else {
        // 0x800768C0(number) -> table 31 row number - 1; 0x80076FC0(row, config) = ConfigFromCarSpec(gtMode false).
        size_t cases = 0, bad = 0;
        for (uint32_t number = 1; number <= lic.CarCount(); number++) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            const uint32_t row = guest.Call(0x800768C0u, number);
            const std::span<const uint8_t> native = lic.CarRowAt(number);
            bool ok = (row & 0xFF000000u) == 0x80000000u && std::memcmp(&guest.Ram()[row & 0x1FFFFF], native.data(), kLicenseCarRowSize) == 0;
            std::memset(&guest.Ram()[kRecordCopy & 0x1FFFFF], 0xA5, sizeof(CarConfig));
            guest.CallWithBios(0x80076FC0u, row, kRecordCopy);
            const CarConfig config = ConfigFromCarSpec(lic.Tables(), native, false);
            size_t first = sizeof(CarConfig);
            for (size_t i = 0; i < sizeof(CarConfig) && first == sizeof(CarConfig); i++)
                if (guest.Ram()[(kRecordCopy & 0x1FFFFF) + i] != reinterpret_cast<const uint8_t*>(&config)[i]) first = i;
            ok = ok && first == sizeof(CarConfig);
            cases++;
            if (!ok && bad++ < 4) std::printf("    MISMATCH licence car %u: row %08X, config first difference + 0x%02zX\n", number, row, first);
        }
        Report("LicSpec", 0x80076FC0u, cases, bad, failures);
    }
    if (Get<uint32_t>(pristine.data(), D(0x80092878u)) == 3 && Get<uint8_t>(pristine.data(), D(kGameMode)) == 3) {
        // ovl0 0x8004C7A0(name): the race block of every test; the native car slot 0 / settings / record must equal it.
        size_t cases = 0, bad = 0;
        for (size_t t = 0; t < lic.TestCount(); t++) {
            const LicenseTest test = lic.TestAt(t);
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            PutString(guest.Ram(), kScratchString, test.name);
            guest.CallWithBios(0x8004C7A0u, kScratchString);
            const uint8_t* ram = guest.Ram();
            const LicenseRaceCar car = lic.RaceCar(test);
            CarConfig config = car.config;
            const sim::CarParams record = BuildCarParams(*vol, lic.Tables(), config); // 0x800771AC writes back into the config
            std::string why;
            if (Get<uint32_t>(ram, D(kCarSlot0)) != car.carId) why += " car";
            if (Get<uint32_t>(ram, D(kCarSlot0) + 4) != uint32_t(uint8_t(car.paint))) why += " paint";
            if (std::memcmp(ram + ((D(kCarSlot0) + 8) & 0x1FFFFF), &config, sizeof(CarConfig)) != 0) why += " config";
            // 0x80017E74 (race start) replaces the lengths / tracks the builder leaves; the native record holds its values.
            auto startPatched = [](size_t i) {
                for (size_t o : {offsetof(sim::CarParams, frontLength), offsetof(sim::CarParams, rearLength), offsetof(sim::CarParams, frontTrack), offsetof(sim::CarParams, rearTrack)})
                    if (i == o || i == o + 1) return true;
                return false;
            };
            for (size_t i = 0; i < sizeof(record); i++)
                if (!startPatched(i) && ram[(D(kCarRecord0) & 0x1FFFFF) + i] != reinterpret_cast<const uint8_t*>(&record)[i]) { why += " record+" + std::to_string(i) + " (decimal)"; break; }
            if (std::memcmp(ram + (D(kSettings) & 0x1FFFFF), test.settings.data(), kLicenseSettingsSize) != 0) why += " settings";
            if (Get<uint8_t>(ram, D(0x801D5944u)) != 1 || Get<uint8_t>(ram, D(0x801D5945u)) != 0 || Get<uint8_t>(ram, D(0x801D5946u)) != 3) why += " entry";
            for (uint32_t i = 0; i < 0x90; i++) // car slot 1: never filled (the test row's slot 1 is not read)
                if (ram[(D(0x801D5948u) & 0x1FFFFF) + i] != 0) { why += " slot1"; break; }
            if (Get<uint8_t>(ram, D(0x801D5869u)) != (test.settings[0] == 0 ? 1 : 0)) why += " hold";
            if (Get<uint8_t>(ram, D(0x801D5866u)) != 3 || Get<uint8_t>(ram, D(0x801D586Bu)) != test.settings[1]) why += " mode/laps";
            cases++;
            if (!why.empty() && bad++ < 6) std::printf("    MISMATCH licence build %s:%s\n", test.name.c_str(), why.c_str());
        }
        Report("LicBuild", 0x8004C7A0u, cases, bad, failures);
    }

    // ---- the dump's own test (mode 3 dumps only): parameters from the disc == the original's memory
    if (Get<uint8_t>(pristine.data(), D(kGameMode)) != 3) {
        std::puts("LicTest    skipped (the dump is not a licence test: 0x801D5866 != 3)");
        return failures;
    }
    const std::string testName(reinterpret_cast<const char*>(&pristine[D(kTestName) & 0x1FFFFF]));
    const LicenseTest test = lic.Test(testName);
    {
        size_t bad = 0;
        const uint8_t* settings = &pristine[D(kSettings) & 0x1FFFFF];
        for (size_t i = 0; i < kLicenseSettingsSize; i++)
            if (settings[i] != test.settings[i] && bad++ < 4) std::printf("    MISMATCH %s settings + 0x%02zX: original %02X disc %02X\n", testName.c_str(), i, settings[i], test.settings[i]);
        if (test.course != trackName) { bad++; std::printf("    MISMATCH %s course: disc %s, dump's course %s\n", testName.c_str(), test.course.c_str(), trackName.c_str()); }
        const uint32_t carId = lic.RaceCar(test).carId;
        const uint32_t dumpCar = Get<uint32_t>(pristine.data(), D(kCarSlot0));
        if (carId != dumpCar) { bad++; std::printf("    MISMATCH %s car: disc %s, dump %s\n", testName.c_str(), UnpackCarId(carId).c_str(), UnpackCarId(dumpCar).c_str()); }
        const uint32_t cars = 1; // the test row's slot 1 is never read (work/re/lic2_b7: B-7 = LJB06 races one car)
        if (cars != Get<uint8_t>(pristine.data(), D(kCarCountTick))) { bad++; std::printf("    MISMATCH %s car count: disc %u, dump %u\n", testName.c_str(), cars, Get<uint8_t>(pristine.data(), D(kCarCountTick))); }
        std::printf("           %s: course %s, car %s (licence car %u), type %u, target lap %u, box %u0 m + %u m, medals %u / %u / %u ms\n", testName.c_str(),
                    test.course.c_str(), UnpackCarId(carId).c_str(), test.cars[0].carNumber, test.Type(), test.TargetLap(), test.BoxStart(), test.BoxLength(),
                    test.MedalTime(1), test.MedalTime(2), test.MedalTime(3));
        Report("LicTest", D(kSettings), 1, bad ? 1 : 0, failures);
    }
    // ---- the licence car: the stock configuration in the licence tables and the record built from it
    {
        const LicenseRaceCar car = lic.RaceCar(test);
        const uint32_t carId = car.carId;
        CarConfig config = car.config; // 0x80076FC0 of the table 31 row
        const CarConfig* dumpConfig = reinterpret_cast<const CarConfig*>(&pristine[(D(kCarSlot0) + 8) & 0x1FFFFF]);
        size_t bad = 0;
        const sim::CarParams native = BuildCarParams(*vol, lic.Tables(), config); // writes back into the config like 0x800771AC
        {
            const uint8_t* a = reinterpret_cast<const uint8_t*>(&config);
            const uint8_t* b = reinterpret_cast<const uint8_t*>(dumpConfig);
            for (size_t i = 0; i < sizeof(CarConfig); i++) // the whole configuration (the race start does not change it)
                if (a[i] != b[i] && bad++ < 6) std::printf("    MISMATCH licence car config + 0x%02zX: original %02X native %02X\n", i, b[i], a[i]);
        }
        // Let the original setup patch the native record like it patched the dump's at the race start.
        std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
        std::memset(guest.Scratch(), 0, kScratchSize);
        std::memcpy(guest.Ram() + (kRecordCopy & 0x1FFFFF), &native, sizeof(native));
        const uint32_t body = kCarBase + kBodyOffset;
        const uint32_t fifth = 0;
        std::memcpy(guest.Ram() + ((kStack + 0x10) & 0x1FFFFF), &fifth, 4);
        guest.Call(0x800319A8u, body, kRecordCopy, Get<uint8_t>(pristine.data(), body + 0x45D), 0);
        size_t recordBad = 0;
        for (size_t i = 0; i < sizeof(sim::CarParams); i++) {
            const uint8_t patched = guest.Ram()[(kRecordCopy & 0x1FFFFF) + i];
            const uint8_t dump = pristine[(D(kCarRecord0) & 0x1FFFFF) + i];
            if (patched != dump && recordBad++ < 6) std::printf("    MISMATCH licence car record + 0x%03zX: original %02X native %02X\n", i, dump, patched);
        }
        std::printf("           licence car %s: config rows brakes %u chassis %u engine %u gearbox %u tyres %u/%u rm %u; record %zu byte(s) differ\n",
                    UnpackCarId(carId).c_str(), config.brakes, config.chassis, config.engine, config.gearbox, config.tyresFront, config.tyresRear, config.racingModify, recordBad);
        Report("LicCar", 0x800771ACu, 2, (bad ? 1 : 0) + (recordBad ? 1 : 0), failures);
    }
    return failures;
}

} // namespace gt2::verify
