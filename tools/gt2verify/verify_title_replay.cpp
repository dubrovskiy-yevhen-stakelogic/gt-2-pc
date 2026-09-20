// The replay file class of the executable (gt2formats/replay_card.h) against the original in any RAM image (the EXE
// routines are resident): directories taken from the disc's demo files (arcade/demofile*.gmr, the same format as the
// card file "BASCUS-94455REPLAY"), new ones, and random mutations of both.
//   RepValid 0x800691DC   directory check (count, sector sums, table size, block count, CRC)
//   RepCrc   0x800692DC   an entry's payload CRC
//   RepFits  0x80069358   room for a payload (new entry / replacing one)
//   RepStore 0x80069418   storing a payload's entry (sector allocation, chain, entry, count, CRC; + the sector list)
//   RepDel   0x800695DC   removing an entry
//   RepNew   0x8006911C   a new file (save header from the EXE, empty directory)
//   RepUnpack 0x80069AC4  a payload into RAM (race block, parameter / results records, streams; mode 0 / 6 layouts)
//   RepPack  0x80069948   the RAM state back into a payload
//   RepGather 0x80020E14  (title overlay only) an entry's sectors gathered and unpacked
//   RepGhostUnpack 0x80069D58 / RepGhostPack 0x80069CC0  the ghost file's payload (entry 1, parameters, the reference lap)
//   RepTitle 0x80069028   an entry's title (strcpy)
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

#include "gt2formats/overlay_data.h"
#include "gt2formats/replay_card.h"
#include "gt2formats/save_data.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "guest.h"

namespace gt2::verify {
namespace {

constexpr uint32_t kValid = 0x800691DCu, kCrc = 0x800692DCu, kFits = 0x80069358u, kStore = 0x80069418u, kDel = 0x800695DCu, kNew = 0x8006911Cu;
constexpr uint32_t kUnpack = 0x80069AC4u, kPack = 0x80069948u, kGather = 0x80020E14u;
constexpr uint32_t kDir = 0x80100000u, kPayload = 0x80130000u, kDesc = 0x80158000u, kList = 0x80158100u, kGatherBuf = 0x80160000u;
// The payload's RAM (Simulation addresses; the dump's build maps them through its profile, exe_profile.h).
struct PayloadRam {
    uint32_t race = 0x801D585Cu, params = 0x801DE8BAu, results = 0x801D5E88u, stream = 0x801D5F84u, ghostHead = 0x801D5F88u, ghostStream = 0x801D6068u;
    // nullopt when the dump's build has no mapping for one of them
    static std::optional<PayloadRam> OfActiveBuild() {
        PayloadRam r;
        for (uint32_t* a : {&r.race, &r.params, &r.results, &r.stream, &r.ghostHead, &r.ghostStream}) {
            const std::optional<uint32_t> mapped = ActiveProfile().TryData(*a, -1);
            if (!mapped) return std::nullopt;
            *a = *mapped;
        }
        return r;
    }
};

uint8_t* At(uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }

// The native side of 0x80069AC4: the payload's pieces at their RAM addresses.
void ApplyPayload(uint8_t* ram, const ReplayPayload& p, const PayloadRam& a) {
    std::memcpy(At(ram, a.race), p.race.data(), p.race.size());
    if (p.GameMode() == 6) {
        std::memcpy(At(ram, a.results), p.results6.data(), p.results6.size());
        const int16_t count = int16_t(p.ghosts.size());
        std::memcpy(At(ram, a.stream), &count, 2);
        for (size_t k = 0; k < p.ghosts.size(); k++) {
            std::memcpy(At(ram, a.ghostHead + uint32_t(k) * 0x10FC), p.ghosts[k].first.data(), p.ghosts[k].first.size());
            std::memcpy(At(ram, a.ghostStream + uint32_t(k) * 0x10FC), p.ghosts[k].second.data(), p.ghosts[k].second.size());
        }
        return;
    }
    for (size_t i = 0; i < p.players.size(); i++) {
        const ReplayPayload::Player& pl = p.players[i];
        if (!pl.params.empty()) std::memcpy(At(ram, a.params + uint32_t(i) * 0x1C0), pl.params.data(), pl.params.size());
        std::memcpy(At(ram, a.results + uint32_t(i) * 0x4518), pl.results.data(), pl.results.size());
        std::memcpy(At(ram, a.stream + uint32_t(i) * 0x4518), pl.stream.data(), pl.stream.size());
    }
}

std::vector<uint8_t> RandomBytes(std::mt19937& rng, size_t n) {
    std::vector<uint8_t> v(n);
    for (uint8_t& b : v) b = uint8_t(rng());
    return v;
}

// A random payload of game mode `mode` (streams with a sane "used" field).
ReplayPayload RandomPayload(std::mt19937& rng, uint8_t mode) {
    auto r = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    ReplayPayload p;
    const std::vector<uint8_t> race = RandomBytes(rng, p.race.size());
    std::copy(race.begin(), race.end(), p.race.begin());
    p.race[0xA] = mode;
    auto stream = [&] {
        const int used = r(0, 3) == 0 ? r(0, 0x40) : r(0, 0x900);
        std::vector<uint8_t> s = RandomBytes(rng, size_t(used) + ReplayStream::kHeaderSize);
        s[0x10] = uint8_t(used), s[0x11] = uint8_t(used >> 8);
        return s;
    };
    if (mode == 6) {
        p.results6 = RandomBytes(rng, kReplayPayloadResults);
        const int ghosts = r(0, 3);
        for (int k = 0; k < ghosts; k++) p.ghosts.emplace_back(RandomBytes(rng, kReplayPayloadGhostHead), stream());
        return p;
    }
    for (int i = 0; i < (mode == 0 ? 2 : 1); i++) {
        ReplayPayload::Player pl;
        if (mode != 0) pl.params = RandomBytes(rng, kReplayPayloadParams);
        pl.results = RandomBytes(rng, kReplayPayloadResults);
        pl.stream = stream();
        p.players.push_back(std::move(pl));
    }
    return p;
}

} // namespace

int VerifyTitleReplay(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const DiscImage* disc, const GtfsVolume* vol) {
    if (!vol || !disc) {
        std::printf("%-10s skipped (needs the disc: the demo files are the directories)\n", "Replay");
        return 0;
    }
    int failures = 0;
    auto r = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    GuestImage exe = LoadExeImage(*disc);
    std::vector<ReplayCardFile> bases;
    for (const char* name : {"arcade/demofile_us.gmr", "arcade/demofile.gmr", "arcade/demofile_eu.gmr", "arcade/demofile_jp.gmr"})
        bases.push_back(ReplayCardFile::FromBytes(vol->Read(name)));
    for (int blocks : {3, 7, 15}) bases.push_back(ReplayCardFile::Create(blocks, BuildReplayCardHeader(exe, blocks)));
    // Every base directory must be valid to start with (the disc's files are what the original reads).
    for (size_t b = 0; b < 4; b++)
        if (!bases[b].Valid()) {
            std::printf("    demo file %zu: directory not valid natively\n", b);
            failures++;
        }
    auto pick = [&]() -> ReplayCardFile { return bases[size_t(r(0, int(bases.size()) - 1))]; };
    auto load = [&](const std::vector<uint8_t>& dir) {
        std::memcpy(guest.Ram(), pristine.data(), pristine.size());
        std::memcpy(At(guest.Ram(), kDir), dir.data(), dir.size());
    };
    // A mutated copy: random header / table / entry fields (the CRC recomputed half of the time).
    auto mutate = [&](ReplayCardFile f) -> ReplayCardFile {
        std::vector<uint8_t> b = f.Bytes();
        const int edits = r(0, 3);
        for (int k = 0; k < edits; k++) {
            switch (r(0, 5)) {
            case 0: b[3] = uint8_t(r(0, 3) ? r(1, 16) : r(0, 255)); break;
            case 1: b[kReplayDirCount] = uint8_t(r(0, 3) ? r(0, 33) : r(0, 255)); break;
            case 2: { const size_t o = kReplayDirTable + 2 + size_t(r(0, 959)) * 2; const int16_t v = int16_t(r(0, 2) ? kReplaySectorFree : r(-1, 960)); b[o] = uint8_t(v), b[o + 1] = uint8_t(uint16_t(v) >> 8); break; }
            case 3: { const size_t o = kReplayDirEntries + size_t(r(0, 31)) * kReplayEntrySize + 0x50 + size_t(r(0, 1)) * 2; const int16_t v = int16_t(r(-2, 1000)); b[o] = uint8_t(v), b[o + 1] = uint8_t(uint16_t(v) >> 8); break; }
            case 4: { const int16_t v = int16_t(r(0, 3) ? r(0, 917) : r(-5, 2000)); b[kReplayDirTotal] = uint8_t(v), b[kReplayDirTotal + 1] = uint8_t(uint16_t(v) >> 8); break; }
            default: b[kReplayDirCrc + size_t(r(0, 3))] ^= uint8_t(r(1, 255)); break;
            }
        }
        ReplayCardFile m = ReplayCardFile::FromBytes(b);
        if (r(0, 1)) m.UpdateCrc();
        return m;
    };

    { // 0x800691DC
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 3000; n++) {
            const ReplayCardFile f = n < int(bases.size()) ? bases[size_t(n)] : mutate(pick());
            load(f.Bytes());
            const bool original = guest.Call(kValid, kDir) != 0;
            cases++;
            if (original != f.Valid() && bad++ < 3) std::printf("    MISMATCH RepValid case %d: original %d ours %d\n", n, original, f.Valid());
        }
        Report("RepValid", kValid, cases, bad, failures);
    }
    { // 0x800692DC (entries of the valid bases; the gathered payload in a buffer, sometimes corrupted)
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 400; n++) {
            ReplayCardFile f = bases[size_t(r(0, 3))];
            const int i = r(0, f.Count() - 1);
            std::vector<uint8_t> data = f.EntryData(i);
            if (r(0, 3) == 0 && !data.empty()) data[size_t(r(0, int(data.size()) - 1))] ^= uint8_t(r(1, 255));
            load(f.Bytes());
            std::memcpy(At(guest.Ram(), kPayload), data.data(), data.size());
            const bool original = guest.Call(kCrc, kDir, uint32_t(i), kPayload) != 0;
            const ReplayCardEntry e = f.Entry(i);
            const bool ours = Crc32(std::span<const uint8_t>(data.data(), size_t(e.size))) == e.crc;
            const bool oursFile = f.EntryCrcOk(i);
            cases++;
            if ((original != ours || !oursFile) && bad++ < 3) std::printf("    MISMATCH RepCrc entry %d: original %d ours %d (file %d)\n", i, original, ours, oursFile);
        }
        Report("RepCrc", kCrc, cases, bad, failures);
    }
    { // 0x80069358
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 2000; n++) {
            const ReplayCardFile f = r(0, 2) ? pick() : mutate(pick());
            const int index = f.Count() > 0 && r(0, 1) ? r(0, std::min(31, f.Count() - 1)) : -1;
            const int32_t size = r(0, 3) ? r(0, 0x4000) : r(0, 0x40000);
            load(f.Bytes());
            const int32_t original = int32_t(guest.Call(kFits, kDir, uint32_t(index), uint32_t(size)));
            const int ours = f.Fits(index, size);
            cases++;
            if (original != ours && bad++ < 3) std::printf("    MISMATCH RepFits index %d size %d: original %d ours %d\n", index, size, original, ours);
        }
        Report("RepFits", kFits, cases, bad, failures);
    }
    { // 0x80069418: the directory part (0..0x1580) and the sector list (the data is written by the card routine, not here)
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 1500; n++) {
            ReplayCardFile f = pick();
            if (r(0, 2) == 0) { // some entries removed first, for gaps in the table
                for (int k = r(0, 2); k > 0 && f.Count() > 0; k--) f.Remove(r(0, f.Count() - 1));
                f.UpdateCrc();
            }
            const int index = f.Count() > 0 && r(0, 1) ? r(0, f.Count() - 1) : -1;
            const std::vector<uint8_t> payload = RandomBytes(rng, size_t(r(0, 3) ? r(1, 0x2000) : r(1, 0x20000)));
            std::array<uint8_t, 0x50> desc{};
            const std::vector<uint8_t> d = RandomBytes(rng, desc.size());
            std::copy(d.begin(), d.end(), desc.begin());
            load(f.Bytes());
            std::memcpy(At(guest.Ram(), kPayload), payload.data(), payload.size());
            std::memcpy(At(guest.Ram(), kDesc), desc.data(), desc.size());
            std::memset(At(guest.Ram(), kList), 0, 0x800);
            const uint32_t stackArgs[2] = {uint32_t(payload.size()), kList};
            std::memcpy(At(guest.Ram(), kStack + 0x10), stackArgs, sizeof(stackArgs));
            const bool original = guest.Call(kStore, kDir, uint32_t(index), kDesc, kPayload) != 0;
            ReplayCardFile ours = f;
            const bool stored = ours.Store(index, desc, payload);
            cases++;
            bool same = original == stored && std::memcmp(At(guest.Ram(), kDir), ours.Bytes().data(), kReplayDataStart) == 0;
            if (same && stored) { // the sector list {count, sectors...} = the entry's chain
                const ReplayCardEntry e = ours.Entry(index < 0 ? ours.Count() - 1 : index);
                const std::vector<int16_t> chain = ours.Chain(e.first);
                int16_t count;
                std::memcpy(&count, At(guest.Ram(), kList), 2);
                same = count == int16_t(chain.size());
                for (size_t k = 0; same && k < chain.size(); k++) {
                    int16_t s;
                    std::memcpy(&s, At(guest.Ram(), kList + 2 + uint32_t(k) * 2), 2);
                    same = s == chain[k];
                }
                same = same && ours.Valid() && ours.EntryCrcOk(index < 0 ? ours.Count() - 1 : index);
            }
            if (!same && bad++ < 3) std::printf("    MISMATCH RepStore index %d size %zu: original %d ours %d\n", index, payload.size(), original, stored);
        }
        Report("RepStore", kStore, cases, bad, failures);
    }
    { // 0x800695DC
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 600; n++) {
            ReplayCardFile f = pick();
            if (f.Count() == 0) continue;
            const int index = r(0, f.Count() - 1);
            load(f.Bytes());
            guest.Call(kDel, kDir, uint32_t(index));
            f.Remove(index);
            cases++;
            if (std::memcmp(At(guest.Ram(), kDir), f.Bytes().data(), kReplayDataStart) != 0 && bad++ < 3) std::printf("    MISMATCH RepDel index %d\n", index);
        }
        Report("RepDel", kDel, cases, bad, failures);
    }
    { // 0x8006911C on a cleared buffer (it does not compute the directory CRC)
        size_t cases = 0, bad = 0;
        for (int blocks = 2; blocks <= 15; blocks++) {
            load(std::vector<uint8_t>(kReplayDataStart, 0));
            guest.Call(kNew, kDir, uint32_t(blocks));
            std::vector<uint8_t> ours = ReplayCardFile::Create(blocks, BuildReplayCardHeader(exe, blocks)).Bytes();
            std::memset(ours.data() + kReplayDirCrc, 0, 4);
            cases++;
            if (std::memcmp(At(guest.Ram(), kDir), ours.data(), kReplayDataStart) != 0 && bad++ < 3) {
                size_t first = 0;
                while (first < kReplayDataStart && At(guest.Ram(), kDir)[first] == ours[first]) first++;
                std::printf("    MISMATCH RepNew %d blocks: first differing byte +0x%zX\n", blocks, first);
            }
        }
        Report("RepNew", kNew, cases, bad, failures);
    }
    // Payloads: the demo files' entries and random ones of every layout.
    std::vector<std::vector<uint8_t>> payloads;
    // (the entries of the three files the title plays: 0x8004C8A8[language] -> demofile_jp / _us / _eu; the uncompressed
    // arcade/demofile.gmr of the VOL is not loaded by any code, and its entry 1 does not unpack within its size)
    for (size_t b : {size_t(0), size_t(2), size_t(3)})
        for (int i = 0; i < bases[b].Count(); i++) {
            std::vector<uint8_t> d = bases[b].EntryData(i);
            d.resize(size_t(bases[b].Entry(i).size));
            payloads.push_back(std::move(d));
        }
    const size_t realPayloads = payloads.size();
    for (int n = 0; n < 300; n++) {
        static const uint8_t kModes[] = {0, 1, 2, 3, 4, 6, 6, 7, 10, 11};
        payloads.push_back(PackReplayPayload(RandomPayload(rng, kModes[size_t(r(0, 9))])));
    }
    const std::optional<PayloadRam> payloadRam = PayloadRam::OfActiveBuild();
    if (!payloadRam) {
        std::printf("%-10s skipped (the payload's RAM blocks have no mapping in this build's profile)\n", "RepUnpack");
        std::printf("%-10s skipped (the payload's RAM blocks have no mapping in this build's profile)\n", "RepPack");
    } else { // 0x80069AC4(buffer, 0)
        size_t cases = 0, bad = 0;
        for (size_t n = 0; n < payloads.size(); n++) {
            std::memcpy(guest.Ram(), pristine.data(), pristine.size());
            std::memcpy(At(guest.Ram(), kPayload), payloads[n].data(), payloads[n].size());
            guest.Call(kUnpack, kPayload, 0);
            std::vector<uint8_t> ours(pristine);
            std::memcpy(At(ours.data(), kPayload), payloads[n].data(), payloads[n].size());
            try {
                ApplyPayload(ours.data(), UnpackReplayPayload(payloads[n]), *payloadRam);
            } catch (const std::exception& e) {
                std::printf("    payload %zu (mode %u, %zu bytes): %s\n", n, unsigned(payloads[n][0xA]), payloads[n].size(), e.what());
                throw;
            }
            cases++;
            // the guest stack is the harness's scratch
            const size_t stackLo = (kStack - 0x400) & 0x1FFFFF, stackHi = kStack & 0x1FFFFF;
            bool same = std::memcmp(guest.Ram(), ours.data(), stackLo) == 0 && std::memcmp(guest.Ram() + stackHi, ours.data() + stackHi, ours.size() - stackHi) == 0;
            if (!same && bad++ < 3) {
                size_t first = 0;
                while (first < ours.size() && guest.Ram()[first] == ours[first]) first++;
                std::printf("    MISMATCH RepUnpack payload %zu (%s, mode %u): first differing RAM 0x%08zX\n", n, n < realPayloads ? "demo" : "random",
                            unsigned(payloads[n][0xA]), 0x80000000u + first);
            }
        }
        Report("RepUnpack", kUnpack, cases, bad, failures);
    }
    if (payloadRam) { // 0x80069948(buffer) after the unpack: the same bytes again (padding cleared on both sides)
        size_t cases = 0, bad = 0;
        for (size_t n = 0; n < payloads.size(); n++) {
            std::memcpy(guest.Ram(), pristine.data(), pristine.size());
            ApplyPayload(guest.Ram(), UnpackReplayPayload(payloads[n]), *payloadRam);
            std::memset(At(guest.Ram(), kPayload), 0, 0x10000);
            const uint32_t size = guest.Call(kPack, kPayload);
            const std::vector<uint8_t> ours = PackReplayPayload(UnpackReplayPayload(payloads[n]));
            cases++;
            const bool same = size == ours.size() && std::memcmp(At(guest.Ram(), kPayload), ours.data(), ours.size()) == 0;
            if (!same && bad++ < 3) std::printf("    MISMATCH RepPack payload %zu: original %u bytes ours %zu\n", n, size, ours.size());
        }
        Report("RepPack", kPack, cases, bad, failures);
    }
    // The ghost file's payload (0x80069CC0 packs / 0x80069D58 unpacks: race block entry 1, its parameter record, the reference
    // lap's head and stream object) - random records.
    std::optional<std::array<uint32_t, 4>> ghostRam;
    {
        std::array<uint32_t, 4> a = {0x801D5988u, 0x801DEA7Au, 0x801DA4A0u, 0x801DA580u};
        bool ok = true;
        for (uint32_t& x : a) {
            const std::optional<uint32_t> m = ActiveProfile().TryData(x, -1);
            if (!m) ok = false;
            else x = *m;
        }
        if (ok) ghostRam = a;
    }
    auto randomGhost = [&]() {
        ReplayGhostRecord g;
        const std::vector<uint8_t> e = RandomBytes(rng, g.entry.size()), p = RandomBytes(rng, g.params.size()), h = RandomBytes(rng, g.head.size());
        std::copy(e.begin(), e.end(), g.entry.begin());
        std::copy(p.begin(), p.end(), g.params.begin());
        std::copy(h.begin(), h.end(), g.head.begin());
        const int used = r(0, 3) == 0 ? r(0, 0x20) : r(0, 0xFE0);
        g.stream = RandomBytes(rng, size_t(used) + ReplayStream::kHeaderSize);
        g.stream[0x10] = uint8_t(used), g.stream[0x11] = uint8_t(used >> 8);
        return g;
    };
    auto applyGhost = [&](uint8_t* ram, const ReplayGhostRecord& g) {
        const std::array<uint32_t, 4>& a = *ghostRam;
        std::memcpy(At(ram, a[0]), g.entry.data(), g.entry.size());
        std::memcpy(At(ram, a[1]), g.params.data(), g.params.size());
        std::memcpy(At(ram, a[2]), g.head.data(), g.head.size());
        std::memcpy(At(ram, a[3]), g.stream.data(), g.stream.size());
    };
    if (!ghostRam) {
        std::printf("%-10s skipped (the ghost's RAM blocks have no mapping in this build's profile)\n", "RepGhost");
    } else {
        const uint32_t kGhostUnpack = 0x80069D58u, kGhostPack = 0x80069CC0u;
        size_t cases = 0, bad = 0, packCases = 0, packBad = 0;
        const size_t stackLo = (kStack - 0x400) & 0x1FFFFF, stackHi = kStack & 0x1FFFFF;
        for (int n = 0; n < 300; n++) {
            const ReplayGhostRecord g = randomGhost();
            const std::vector<uint8_t> packed = PackGhostRecord(g);
            // 0x80069D58(buffer)
            std::memcpy(guest.Ram(), pristine.data(), pristine.size());
            std::memcpy(At(guest.Ram(), kPayload), packed.data(), packed.size());
            guest.Call(kGhostUnpack, kPayload);
            std::vector<uint8_t> ours(pristine);
            std::memcpy(At(ours.data(), kPayload), packed.data(), packed.size());
            applyGhost(ours.data(), UnpackGhostRecord(packed));
            cases++;
            const bool same = std::memcmp(guest.Ram(), ours.data(), stackLo) == 0 && std::memcmp(guest.Ram() + stackHi, ours.data() + stackHi, ours.size() - stackHi) == 0;
            if (!same && bad++ < 3) std::printf("    MISMATCH RepGhostUnpack record %d\n", n);
            // 0x80069CC0(buffer) of the same RAM state
            std::memcpy(guest.Ram(), pristine.data(), pristine.size());
            applyGhost(guest.Ram(), g);
            std::memset(At(guest.Ram(), kPayload), 0, 0x2000);
            const uint32_t size = guest.Call(kGhostPack, kPayload);
            packCases++;
            const bool samePack = size == packed.size() && std::memcmp(At(guest.Ram(), kPayload), packed.data(), packed.size()) == 0;
            if (!samePack && packBad++ < 3) std::printf("    MISMATCH RepGhostPack record %d: original %u bytes ours %zu\n", n, size, packed.size());
        }
        Report("RepGhostUnpack", kGhostUnpack, cases, bad, failures);
        Report("RepGhostPack", kGhostPack, packCases, packBad, failures);
    }
    { // 0x80069028(entry, title): strcpy of a title into an entry's description (the name entry of Rename & Delete / the saves)
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 300; n++) {
            ReplayCardFile f = bases[size_t(r(0, 3))];
            if (f.Count() == 0) continue;
            const int index = r(0, f.Count() - 1);
            const int length = r(0, 31);
            std::string title;
            for (int k = 0; k < length; k++) title.push_back(char(r(0x20, 0x7E)));
            load(f.Bytes());
            std::memcpy(At(guest.Ram(), kDesc), title.c_str(), title.size() + 1);
            guest.Call(0x80069028u, kDir + uint32_t(kReplayDirEntries + size_t(index) * kReplayEntrySize), kDesc);
            f.SetEntryTitle(index, title);
            cases++;
            if (std::memcmp(At(guest.Ram(), kDir), f.Bytes().data(), kReplayDataStart) != 0 && bad++ < 3) std::printf("    MISMATCH RepTitle entry %d '%s'\n", index, title.c_str());
        }
        Report("RepTitle", 0x80069028u, cases, bad, failures);
    }
    // 0x80020E14 (title overlay): only when its code is in the dump (the call to 0x80069AC4 at its end).
    const uint32_t gatherTail = 0x0C01A6B1u; // jal 0x80069AC4
    bool titleLoaded = false;
    for (uint32_t a = kGather; a < kGather + 0x200 && !titleLoaded; a += 4) {
        uint32_t w;
        std::memcpy(&w, pristine.data() + (a & 0x1FFFFF), 4);
        titleLoaded = w == gatherTail;
    }
    if (!titleLoaded) {
        std::printf("%-10s skipped (the title overlay's 0x80020E14 is not in this dump)\n", "RepGather");
    } else {
        size_t cases = 0, bad = 0;
        for (size_t b : {size_t(0), size_t(2), size_t(3)})
            for (int i = 0; i < bases[b].Count(); i++) {
                load(bases[b].Bytes());
                guest.Call(kGather, kDir, kGatherBuf, uint32_t(i));
                std::vector<uint8_t> ours(pristine);
                std::memcpy(At(ours.data(), kDir), bases[b].Bytes().data(), bases[b].Bytes().size());
                const std::vector<uint8_t> data = bases[b].EntryData(i);
                std::memcpy(At(ours.data(), kGatherBuf), data.data(), data.size());
                ApplyPayload(ours.data(), UnpackReplayPayload(data), PayloadRam{});
                const size_t stackLo = (kStack - 0x400) & 0x1FFFFF, stackHi = kStack & 0x1FFFFF;
                cases++;
                const bool same = std::memcmp(guest.Ram(), ours.data(), stackLo) == 0 && std::memcmp(guest.Ram() + stackHi, ours.data() + stackHi, ours.size() - stackHi) == 0;
                if (!same && bad++ < 3) std::printf("    MISMATCH RepGather file %zu entry %d\n", b, i);
            }
        Report("RepGather", kGather, cases, bad, failures);
    }
    return failures;
}

} // namespace gt2::verify
