// The title's DATA TRANSFER rules (src/game/shell/title_transfer.h) against the original in a RAM image with GT2.OVL member 1
// loaded (work/re/title/ram.bin); they skip themselves elsewhere. The loaded save is a file image in free RAM that the manager
// object 0x800B1588 points to (+0x418), as member 1's own card manager leaves it; the career is RAM 0x801C98E0.
//   DtCourse 0x8001E014, DtLicRank 0x8001E11C, DtLicence 0x8001E284 (+ EXE 0x8005DEFC), DtMachine EXE 0x8005E0D0,
//   DtMachMix 0x8001E38C / 0x8001E408 / 0x8001E484, DtGt1Sum 0x8001FBFC, DtGt1Ok 0x8001FC7C, DtConvert 0x8001E61C,
//   DtAvail 0x8001EAD8, DtBuy 0x8001EB60.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/career/career_state.h"
#include "game/shell/title_transfer.h"
#include "guest.h"

namespace gt2::verify {
namespace {

constexpr uint32_t kCourse = 0x8001E014u, kLicRank = 0x8001E11Cu, kLicence = 0x8001E284u, kMachine = 0x8005E0D0u, kGt1Sum = 0x8001FBFCu,
                   kGt1Ok = 0x8001FC7Cu, kConvert = 0x8001E61Cu, kAvail = 0x8001EAD8u, kBuy = 0x8001EB60u;
constexpr uint32_t kMachineMix[3] = {0x8001E38Cu, 0x8001E408u, 0x8001E484u};
constexpr uint32_t kManagerPointer = 0x800B1588u, kManager = 0x80170000u, kFile = 0x80178000u, kScratch = 0x80160000u;
constexpr uint32_t kCareer = career::kStateAddress;

uint8_t* At(uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }
career::CareerState& Career(uint8_t* ram) { return *reinterpret_cast<career::CareerState*>(At(ram, kCareer)); }
career::CareerState& Loaded(uint8_t* ram) { return *reinterpret_cast<career::CareerState*>(At(ram, kFile + 0x200)); }
void SetWord(uint8_t* ram, uint32_t address, uint32_t v) { std::memcpy(At(ram, address), &v, 4); }

// Random but plausible records: some empty (-1), times in a narrow range (ties happen), names from a small set.
void RandomTime(career::TimeRecord& t, std::mt19937& rng, bool allowEmpty) {
    auto r = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    if (allowEmpty && r(0, 3) == 0) {
        career::InitTimeRecord(t);
        return;
    }
    t.time[0] = r(0, 9) == 0 ? int32_t(r(0, 0x7FFFFFFF)) : 60000 + r(0, 40) * 50;
    for (int i = 1; i < 4; i++) t.time[i] = r(0, 3) == 0 ? -1 : t.time[0] * i / 4;
    t.word10 = uint16_t(r(0, 3));
    t.word12 = uint16_t(r(0, 1) ? 0xFFFF : r(0, 0xFFFF));
}
void RandomLicence(career::LicenceTestRecord& rec, std::mt19937& rng) {
    auto r = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    static const char* const kNames[] = {"GT", "ABC", "PLAYER", "", "X"};
    career::InitLicenceTestRecord(rec);
    rec.passed = uint8_t(r(0, 4));
    const int n = r(0, 5);
    std::vector<career::TimeRecord> sorted(static_cast<size_t>(n));
    for (auto& t : sorted) RandomTime(t, rng, false);
    std::sort(sorted.begin(), sorted.end(), [](const career::TimeRecord& a, const career::TimeRecord& b) { return uint32_t(a.time[0]) < uint32_t(b.time[0]); });
    for (int k = 0; k < n; k++) {
        rec.times[k] = sorted[size_t(k)];
        std::memset(rec.entries[k], 0, 12);
        std::strcpy(reinterpret_cast<char*>(rec.entries[k]), kNames[r(0, 4)]);
    }
}
void RandomMachine(career::MachineTestRecord& rec, std::mt19937& rng) {
    auto r = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    career::InitMachineTestRecord(rec);
    const int n = r(0, 3) == 0 ? r(0, 12) : r(0, 8);
    rec.bytes[0] = uint8_t(n);
    for (int k = 0; k < std::min(n, 8); k++) {
        uint8_t* e = rec.bytes + 4 + k * 0x14;
        const uint32_t car = uint32_t(r(0, 5)), value = uint32_t(r(0, 1) ? r(1000, 1040) : r(0, 0x7FFFFFFF));
        std::memcpy(e, &car, 4);
        std::memcpy(e + 4, &value, 4);
        for (int b = 8; b < 0x14; b++) e[b] = uint8_t(rng());
    }
}
void RandomCareer(career::CareerState& s, std::mt19937& rng) {
    for (auto& c : s.courses) RandomTime(c.best, rng, true);
    for (auto& l : s.licences)
        for (auto& t : l) RandomLicence(t, rng);
    for (auto& m : s.machineTests) RandomMachine(m, rng);
}

} // namespace

int VerifyTitleTransfer(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng) {
    uint32_t tail;
    std::memcpy(&tail, pristine.data() + (0x8001E550u & 0x1FFFFF) + 0x80, 4);
    { // member 1 in the dump: 0x8001E550's call of 0x8001E014 (jal 0x8001E014 = 0x0C007805)
        bool found = false;
        for (uint32_t a = 0x8001E550u; a < 0x8001E600u && !found; a += 4) {
            uint32_t w;
            std::memcpy(&w, pristine.data() + (a & 0x1FFFFF), 4);
            found = w == 0x0C007805u;
        }
        if (!found) {
            std::printf("%-10s skipped (the title overlay, GT2.OVL member 1, is not loaded in this dump)\n", "Transfer");
            return 0;
        }
    }
    int failures = 0;
    auto r = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    std::vector<uint8_t> ours(Bus::kRamSize);
    auto reset = [&] {
        std::memcpy(guest.Ram(), pristine.data(), pristine.size());
        SetWord(guest.Ram(), kManagerPointer, kManager);
        SetWord(guest.Ram(), kManager + 0x418, kFile);
        RandomCareer(Career(guest.Ram()), rng);
        RandomCareer(Loaded(guest.Ram()), rng);
        std::memcpy(ours.data(), guest.Ram(), ours.size());
    };
    auto sameCareer = [&] { return std::memcmp(At(guest.Ram(), kCareer), At(ours.data(), kCareer), sizeof(career::CareerState)) == 0; };

    { // 0x8001E014
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 300; n++) {
            reset();
            guest.Call(kCourse);
            shell::MixCourseRecords(Career(ours.data()), Loaded(ours.data()));
            cases++;
            if (!sameCareer() && bad++ < 3) std::printf("    MISMATCH DtCourse case %d\n", n);
        }
        Report("DtCourse", kCourse, cases, bad, failures);
    }
    { // 0x8001E11C(time, name, record)
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 4000; n++) {
            reset();
            const career::LicenceTestRecord& rec = Career(guest.Ram()).licences[r(0, 5)][r(0, 9)];
            const career::LicenceTestRecord& other = Loaded(guest.Ram()).licences[r(0, 5)][r(0, 9)];
            const int k = r(0, 4);
            career::TimeRecord time = other.times[k];
            const uint8_t* name = other.entries[k];
            if (r(0, 2) == 0 && rec.times[0].time[0] != -1) { // a duplicate / an equal time
                const int j = r(0, 4);
                time = rec.times[j];
                name = r(0, 1) ? rec.entries[j] : other.entries[k];
            }
            std::memcpy(At(guest.Ram(), kScratch), &time, sizeof time);
            std::memcpy(At(guest.Ram(), kScratch + 0x20), name, 12);
            const uint32_t recAddress = kCareer + uint32_t(reinterpret_cast<const uint8_t*>(&rec) - At(guest.Ram(), kCareer));
            const int32_t original = int32_t(guest.Call(kLicRank, kScratch, kScratch + 0x20, recAddress));
            char nameCopy[13] = {};
            std::memcpy(nameCopy, name, 12);
            const int32_t native = shell::MixLicenceRank(time, nameCopy, rec);
            cases++;
            if (original != native && bad++ < 3) std::printf("    MISMATCH DtLicRank case %d: original %d ours %d\n", n, original, native);
        }
        Report("DtLicRank", kLicRank, cases, bad, failures);
    }
    { // 0x8001E284
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 200; n++) {
            reset();
            guest.Call(kLicence);
            try {
                shell::MixLicenceRecords(Career(ours.data()), Loaded(ours.data()));
            } catch (const std::exception& e) {
                std::printf("    DtLicence case %d: %s\n", n, e.what());
            }
            cases++;
            if (!sameCareer() && bad++ < 3) std::printf("    MISMATCH DtLicence case %d\n", n);
        }
        Report("DtLicence", kLicence, cases, bad, failures);
    }
    { // EXE 0x8005E0D0(record, entry, higher)
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 4000; n++) {
            reset();
            const int test = r(0, 2);
            uint8_t entry[0x14];
            const uint32_t car = uint32_t(r(0, 5)), value = uint32_t(r(0, 1) ? r(1000, 1040) : r(0, 0x7FFFFFFF));
            std::memcpy(entry, &car, 4);
            std::memcpy(entry + 4, &value, 4);
            for (int b = 8; b < 0x14; b++) entry[b] = uint8_t(rng());
            std::memcpy(At(guest.Ram(), kScratch), entry, sizeof entry);
            const uint32_t recAddress = kCareer + uint32_t(offsetof(career::CareerState, machineTests)) + uint32_t(test) * 0xA4;
            const bool higher = r(0, 1) != 0;
            const int32_t original = int32_t(guest.Call(kMachine, recAddress, kScratch, higher ? 1u : 0u));
            const int32_t native = shell::InsertMachineTestEntry(Career(ours.data()).machineTests[test], entry, higher);
            cases++;
            const bool same = original == native && std::memcmp(At(guest.Ram(), recAddress), At(ours.data(), recAddress), 0xA4) == 0;
            if (!same && bad++ < 3) std::printf("    MISMATCH DtMachine case %d: original %d ours %d\n", n, original, native);
        }
        Report("DtMachine", kMachine, cases, bad, failures);
    }
    { // 0x8001E38C / 0x8001E408 / 0x8001E484
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 300; n++) {
            reset();
            for (int i = 0; i < 3; i++) guest.Call(kMachineMix[i]);
            shell::MixMachineTests(Career(ours.data()), Loaded(ours.data()));
            cases++;
            if (!sameCareer() && bad++ < 3) std::printf("    MISMATCH DtMachMix case %d\n", n);
        }
        Report("DtMachMix", kMachineMix[0], cases, bad, failures);
    }
    { // 0x8001FBFC(data, size) and 0x8001FC7C(data)
        size_t cases = 0, bad = 0, okCases = 0, okBad = 0;
        for (int n = 0; n < 60; n++) {
            reset();
            std::vector<uint8_t> data(shell::kGt1CheckedSize + 4);
            for (uint8_t& b : data) b = uint8_t(r(0, 3) ? 0 : rng());
            const size_t size = r(0, 1) ? shell::kGt1CheckedSize : size_t(r(0, 0x200));
            const uint32_t sum = shell::Gt1Checksum(std::span<const uint8_t>(data.data(), shell::kGt1CheckedSize));
            if (r(0, 1)) std::memcpy(data.data() + shell::kGt1CheckedSize, &sum, 4); // a valid file half of the time
            std::memcpy(At(guest.Ram(), kFile), data.data(), data.size());
            const uint32_t original = guest.Call(kGt1Sum, kFile, uint32_t(size));
            cases++;
            if (original != shell::Gt1Checksum(std::span<const uint8_t>(data.data(), size)) && bad++ < 3) std::printf("    MISMATCH DtGt1Sum case %d\n", n);
            const bool originalOk = guest.Call(kGt1Ok, kFile) != 0;
            okCases++;
            if (originalOk != shell::Gt1DataOk(data) && okBad++ < 3) std::printf("    MISMATCH DtGt1Ok case %d\n", n);
        }
        Report("DtGt1Sum", kGt1Sum, cases, bad, failures);
        Report("DtGt1Ok", kGt1Ok, okCases, okBad, failures);
    }
    { // 0x8001E61C(data)
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 400; n++) {
            reset();
            std::vector<uint8_t> data(0x2B70, 0);
            for (int i = 0; i < 16; i++) data[0x2B5C + size_t(i)] = uint8_t(r(0, 5) == 0 ? 0 : r(1, 255));
            if (r(0, 1)) for (int i = 0; i < 16; i++) data[0x2B5C + size_t(i)] = uint8_t(r(1, 255)); // all passed
            std::memcpy(At(guest.Ram(), kFile), data.data(), data.size());
            guest.Call(kConvert, kFile);
            shell::ConvertGt1Licences(Career(ours.data()), data);
            std::memcpy(At(ours.data(), kFile), data.data(), data.size());
            cases++;
            if (!sameCareer() && bad++ < 3) std::printf("    MISMATCH DtConvert case %d\n", n);
        }
        Report("DtConvert", kConvert, cases, bad, failures);
    }
    { // 0x8001EAD8(row) / 0x8001EB60(row)
        size_t cases = 0, bad = 0, buyCases = 0, buyBad = 0;
        for (int n = 0; n < 600; n++) {
            reset();
            career::GarageBlock& mine = Career(guest.Ram()).garage;
            career::GarageBlock& theirs = Loaded(guest.Ram()).garage;
            mine.count = int16_t(r(0, 3) == 0 ? 100 : r(0, 99));
            mine.money = r(0, 3) == 0 ? r(-5, 5) : r(0, 2000000);
            theirs.count = int16_t(r(1, 100));
            const int row = r(0, theirs.count - 1);
            for (int b = 0; b < int(sizeof(career::GarageCar)); b++) reinterpret_cast<uint8_t*>(&theirs.cars[row])[b] = uint8_t(rng());
            theirs.cars[row].value = r(0, 3) == 0 ? mine.money : r(0, 2000000);
            std::memcpy(ours.data(), guest.Ram(), ours.size());
            const int32_t original = int32_t(guest.Call(kAvail, uint32_t(row)));
            const int32_t native = shell::TradeAvailability(Career(ours.data()), Loaded(ours.data()).garage, row);
            cases++;
            if (original != native && bad++ < 3) std::printf("    MISMATCH DtAvail case %d: original %d ours %d\n", n, original, native);
            if (original != 0) continue;
            guest.Call(kBuy, uint32_t(row));
            shell::TradeBuy(Career(ours.data()), Loaded(ours.data()).garage, row);
            buyCases++;
            if (!sameCareer() && buyBad++ < 3) std::printf("    MISMATCH DtBuy case %d\n", n);
        }
        Report("DtAvail", kAvail, cases, bad, failures);
        Report("DtBuy", kBuy, buyCases, buyBad, failures);
    }
    (void)tail;
    return failures;
}

} // namespace gt2::verify
