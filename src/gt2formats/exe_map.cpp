#include "gt2formats/exe_map.h"

#include <algorithm>
#include <set>
#include <unordered_map>

namespace gt2 {
namespace {

constexpr uint32_t kJrRa = 0x03E00008u;
constexpr size_t kAnchorWords = 12;

uint32_t Op(uint32_t w) { return w >> 26; }
uint32_t Rs(uint32_t w) { return (w >> 21) & 31; }
uint32_t Rt(uint32_t w) { return (w >> 16) & 31; }
bool IsPrologue(uint32_t w) { return (w & 0xFFFF8000u) == 0x27BD8000u; } // addiu sp,sp,-X
uint32_t JumpTarget(uint32_t w, uint32_t pc) { return ((pc + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2); }
// "lui rX, hi" whose value is the upper half of a guest RAM (0x8000..0x8020) or scratchpad (0x1F80) address;
// other lui values are constants and a difference there is a real change.
bool IsAddressHalf(uint32_t luiWord) {
    const uint32_t hi = luiWord & 0xFFFFu;
    return (hi >= 0x8000u && hi <= 0x8020u) || hi == 0x1F80u;
}

// I-type operations whose 16-bit immediate can be the low half of a lui-built address: addiu, ori, loads, stores
// (also lwc2 / swc2, the GTE loads / stores).
bool IsImmediateUse(uint32_t op) {
    return op == 0x09 || op == 0x0D || (op >= 0x20 && op <= 0x26) || (op >= 0x28 && op <= 0x2B) || op == 0x32 || op == 0x3A;
}

// Relocation mask of docs/research/scout_exe.md (tools/Gt2Exe Mips.MaskRelocatable): jump targets, lui immediates
// and the immediates of non-sp/zero based I-type operations are zeroed.
uint32_t Masked(uint32_t w) {
    const uint32_t op = Op(w);
    if (op == 2 || op == 3) return w & 0xFC000000u;
    if (op == 0x0F) return w & 0xFFFF0000u;
    if (IsImmediateUse(op) && op != 0x32 && op != 0x3A && Rs(w) != 29 && Rs(w) != 0) return w & 0xFFFF0000u;
    return w;
}

uint32_t WordAt(const GuestImage& m, uint32_t address) { return m.Get<uint32_t>(address); }

// Tracks "lui rX, hi" per register over straight-line code (reset after "jr ra" + delay slot) and resolves the
// absolute address an I-type operation forms with it.
struct LuiTracker {
    std::optional<uint32_t> hi[32];
    std::optional<uint32_t> Resolve(uint32_t w) const {
        const uint32_t op = Op(w);
        if (!IsImmediateUse(op) || !hi[Rs(w)]) return std::nullopt;
        if (op == 0x0D) return *hi[Rs(w)] | (w & 0xFFFFu);
        return *hi[Rs(w)] + uint32_t(int32_t(int16_t(w & 0xFFFFu)));
    }
    void Step(uint32_t w) {
        const uint32_t op = Op(w);
        if (op == 0x0F) { hi[Rt(w)] = (w & 0xFFFFu) << 16; return; }
        // Any other write of a register forgets its lui value.
        if ((op >= 0x08 && op <= 0x0E) || (op >= 0x20 && op <= 0x26)) hi[Rt(w)] = std::nullopt;
        else if (op == 0) {
            // addu / add rd, rs, rt with one lui-built operand: an indexed element of the same table keeps the
            // table's upper half (lui v0, hi; addu v0, v0, index; lw x, lo(v0)).
            const uint32_t funct = w & 0x3F, rd = (w >> 11) & 31;
            std::optional<uint32_t> keep;
            if ((funct == 0x20 || funct == 0x21) && (hi[Rs(w)].has_value() != hi[Rt(w)].has_value()))
                keep = hi[Rs(w)] ? hi[Rs(w)] : hi[Rt(w)];
            hi[rd] = keep;
        }
        else if (op == 3) hi[31] = std::nullopt;
    }
    void Reset() { for (auto& h : hi) h = std::nullopt; }
};

} // namespace

CodeModules CodeModules::Load(const DiscImage& disc) {
    CodeModules m;
    m.exe = LoadExeImage(disc);
    for (const IsoFile& f : disc.RootFiles())
        if (f.name.rfind("SCUS_", 0) == 0 || f.name.rfind("SCES_", 0) == 0 || f.name.rfind("SCPS_", 0) == 0) m.exeName = f.name;
    for (uint32_t i = 0;; i++) {
        try {
            m.overlays.push_back(LoadOverlayImage(disc, i));
        } catch (const std::exception&) {
            if (i == 0) throw;
            break;
        }
    }
    return m;
}

// ================================================================ word alignment

ModuleAlignment::ModuleAlignment(const GuestImage& a, const GuestImage& b) {
    const size_t na = a.bytes.size() / 4, nb = b.bytes.size() / 4;
    std::vector<uint32_t> ma(na), mb(nb);
    for (size_t i = 0; i < na; i++) ma[i] = Masked(WordAt(a, a.base + uint32_t(i * 4)));
    for (size_t i = 0; i < nb; i++) mb[i] = Masked(WordAt(b, b.base + uint32_t(i * 4)));

    // k-gram keys; k-grams made mostly of zero words are skipped (padding, BSS inside the images).
    auto grams = [](const std::vector<uint32_t>& w) {
        std::unordered_map<uint64_t, int64_t> first; // key -> index, -1 when repeated
        if (w.size() < kAnchorWords) return first;
        for (size_t i = 0; i + kAnchorWords <= w.size(); i++) {
            size_t zeros = 0;
            uint64_t h = 1469598103934665603ull;
            for (size_t k = 0; k < kAnchorWords; k++) {
                if (w[i + k] == 0) zeros++;
                h = (h ^ w[i + k]) * 1099511628211ull;
            }
            if (zeros * 2 > kAnchorWords) continue;
            auto [it, inserted] = first.try_emplace(h, int64_t(i));
            if (!inserted) it->second = -1;
        }
        return first;
    };
    const auto ga = grams(ma), gb = grams(mb);
    std::vector<std::pair<uint32_t, uint32_t>> anchors; // (word index in A, in B)
    for (const auto& [h, ia] : ga) {
        if (ia < 0) continue;
        auto it = gb.find(h);
        if (it == gb.end() || it->second < 0) continue;
        if (!std::equal(ma.begin() + ia, ma.begin() + ia + int64_t(kAnchorWords), mb.begin() + it->second)) continue; // hash collision
        anchors.emplace_back(uint32_t(ia), uint32_t(it->second));
    }
    std::sort(anchors.begin(), anchors.end());

    // Longest chain increasing in both A and B (patience LIS on the B index).
    std::vector<size_t> tails, prev(anchors.size(), SIZE_MAX);
    for (size_t i = 0; i < anchors.size(); i++) {
        auto it = std::lower_bound(tails.begin(), tails.end(), i,
                                   [&](size_t t, size_t x) { return anchors[t].second < anchors[x].second; });
        if (it != tails.begin()) prev[i] = *(it - 1);
        if (it == tails.end()) tails.push_back(i);
        else *it = i;
    }
    std::vector<std::pair<uint32_t, uint32_t>> chain;
    for (size_t i = tails.empty() ? SIZE_MAX : tails.back(); i != SIZE_MAX; i = prev[i]) chain.push_back(anchors[i]);
    std::reverse(chain.begin(), chain.end());

    // Runs of constant delta, then extended word by word while the masked words agree (bounded by the neighbours).
    struct WordRun { int64_t startA, endA, delta; };
    std::vector<WordRun> runs;
    for (const auto& [ia, ib] : chain) {
        const int64_t delta = int64_t(ib) - int64_t(ia);
        if (!runs.empty() && runs.back().delta == delta && int64_t(ia) <= runs.back().endA + int64_t(kAnchorWords)) {
            runs.back().endA = std::max(runs.back().endA, int64_t(ia + kAnchorWords));
            continue;
        }
        runs.push_back({int64_t(ia), int64_t(ia + kAnchorWords), delta});
    }
    for (size_t r = 0; r < runs.size(); r++) {
        WordRun& run = runs[r];
        const int64_t lowA = r == 0 ? 0 : runs[r - 1].endA;
        const int64_t highA = r + 1 < runs.size() ? runs[r + 1].startA : int64_t(na);
        // Inside a run the masked words must agree; a run may only contain gaps (changed words) when both of its
        // ends are anchored, so the gaps are kept - they are the "changed" instructions of the comparison.
        while (run.startA > lowA && run.startA + run.delta > 0 && ma[size_t(run.startA - 1)] == mb[size_t(run.startA - 1 + run.delta)]) run.startA--;
        while (run.endA < highA && run.endA + run.delta < int64_t(nb) && ma[size_t(run.endA)] == mb[size_t(run.endA + run.delta)]) run.endA++;
    }
    // Close the gap between two runs of the same delta (a changed instruction between them).
    std::vector<WordRun> merged;
    for (const WordRun& run : runs) {
        if (!merged.empty() && merged.back().delta == run.delta) { merged.back().endA = run.endA; continue; }
        merged.push_back(run);
    }
    for (const WordRun& run : merged)
        runs_.push_back({a.base + uint32_t(run.startA * 4), a.base + uint32_t(run.endA * 4), run.delta * 4});
}

std::optional<uint32_t> ModuleAlignment::Map(uint32_t addressA) const {
    auto it = std::upper_bound(runs_.begin(), runs_.end(), addressA, [](uint32_t x, const Run& r) { return x < r.startA; });
    if (it == runs_.begin()) return std::nullopt;
    --it;
    if (addressA >= it->endA) return std::nullopt;
    return uint32_t(int64_t(addressA) + it->delta);
}

uint32_t ModuleAlignment::CoveredWords() const {
    uint32_t n = 0;
    for (const Run& r : runs_) n += (r.endA - r.startA) / 4;
    return n;
}

const char* FunctionMatchName(FunctionMatch m) {
    switch (m) {
    case FunctionMatch::Same: return "same";
    case FunctionMatch::Shifted: return "shifted";
    case FunctionMatch::Changed: return "changed";
    case FunctionMatch::Unmapped: return "unmapped";
    }
    return "?";
}

// ================================================================ program map

ProgramMap::ProgramMap(const CodeModules& a, const CodeModules& b) : a_(a), b_(b), exe_(a.exe, b.exe) {
    const size_t n = std::min(a.overlays.size(), b.overlays.size());
    for (size_t i = 0; i < n; i++) overlays_.emplace_back(a.overlays[i], b.overlays[i]);
    CollectDataRefs(-1);
    for (size_t i = 0; i < n; i++) CollectDataRefs(int(i));
}

uint32_t ProgramMap::MapCallTarget(int module, uint32_t targetA, bool* ok) const {
    // Overlay code calls the resident executable above the overlay image; everything else stays in the module.
    const bool inOverlay = module >= 0 && targetA >= a_.Module(module).base && targetA < a_.Module(module).End();
    const ModuleAlignment& al = inOverlay ? Alignment(module) : exe_;
    const auto mapped = al.Map(targetA);
    *ok = mapped.has_value();
    return mapped.value_or(0);
}

std::vector<uint32_t> ProgramMap::FunctionStarts(int module, const std::vector<uint32_t>& extraStarts) const {
    const GuestImage& m = a_.Module(module);
    std::set<uint32_t> starts(extraStarts.begin(), extraStarts.end());
    auto addJalTargets = [&](const GuestImage& caller, uint32_t lowest) {
        for (uint32_t at = caller.base; at + 4 <= caller.End(); at += 4) {
            const uint32_t w = WordAt(caller, at);
            if (Op(w) != 3) continue;
            const uint32_t t = JumpTarget(w, at);
            if (t >= std::max(m.base, lowest) && t < m.End()) starts.insert(t);
        }
    };
    addJalTargets(m, 0);
    if (module < 0) { // overlays call the resident executable above their own image (their own calls stay inside)
        uint32_t overlayEnd = 0;
        for (const GuestImage& o : a_.overlays) overlayEnd = std::max(overlayEnd, o.End());
        for (const GuestImage& o : a_.overlays) addJalTargets(o, overlayEnd);
    }
    for (uint32_t at = m.base; at + 8 <= m.End(); at += 4) {
        if (WordAt(m, at) != kJrRa) continue;
        uint32_t next = at + 8;
        while (next + 4 <= m.End() && WordAt(m, next) == 0) next += 4;
        for (uint32_t k = 0; k < 8 && next + k * 4 + 4 <= m.End(); k++)
            if (IsPrologue(WordAt(m, next + k * 4))) { starts.insert(next); break; }
    }
    std::vector<uint32_t> out;
    for (uint32_t s : starts)
        if (s >= m.base && s < m.End() && (s & 3) == 0) out.push_back(s);
    return out;
}

FunctionComparison ProgramMap::CompareFunction(int module, uint32_t addressA, uint32_t endA) const {
    FunctionComparison c;
    c.module = module;
    c.addressA = addressA;
    c.endA = endA;
    const GuestImage& ia = a_.Module(module);
    const GuestImage& ib = b_.Module(module);
    const ModuleAlignment& al = Alignment(module);
    c.addressB = al.Map(addressA);
    if (!c.addressB) { c.note = "start not aligned"; return c; }
    const uint32_t lastB = uint32_t(int64_t(*c.addressB) + int64_t(endA - 4 - addressA));
    const auto mappedLast = al.Map(endA - 4);
    if (mappedLast) c.endB = *mappedLast + 4;
    const bool sameLength = mappedLast && *mappedLast == lastB;

    LuiTracker ta, tb;         // straight-line lui values
    LuiTracker la, lb;         // the last lui of every register in the function, kept across other writes
    bool identical = true;
    auto difference = [&](uint32_t at, uint32_t wa, uint32_t wb, const char* why) {
        if (c.differences++ == 0) { c.firstDifference = at; c.wordA = wa; c.wordB = wb; if (c.note.empty()) c.note = why; }
    };
    for (uint32_t at = addressA; at < endA; at += 4) {
        const uint32_t atB = uint32_t(int64_t(at) - int64_t(addressA) + int64_t(*c.addressB));
        if (!ib.Contains(atB, 4)) { difference(at, WordAt(ia, at), 0, "past the end of the module in B"); identical = false; break; }
        const uint32_t wa = WordAt(ia, at), wb = WordAt(ib, atB);
        if (wa != wb) {
            identical = false;
            const uint32_t op = Op(wa);
            if (op == Op(wb) && (op == 2 || op == 3)) {
                bool ok = false;
                const uint32_t mapped = MapCallTarget(module, JumpTarget(wa, at), &ok);
                if (ok && mapped == JumpTarget(wb, atB)) c.relocCalls++;
                else difference(at, wa, wb, "call / jump to a function that does not map");
            } else if (op == 0x0F && Op(wb) == 0x0F && Rt(wa) == Rt(wb) && IsAddressHalf(wa) && IsAddressHalf(wb)) {
                c.relocData++; // lui half of an address in KSEG0 RAM / the scratchpad
            } else if (op == Op(wb) && (wa & 0xFFFF0000u) == (wb & 0xFFFF0000u) && ta.Resolve(wa) && tb.Resolve(wb)) {
                c.relocData++; // low half of a lui-built address
            } else if (op == Op(wb) && (wa & 0xFFFF0000u) == (wb & 0xFFFF0000u) && la.Resolve(wa) && lb.Resolve(wb) &&
                       MapData(module, *la.Resolve(wa)) == *lb.Resolve(wb)) {
                // Low half whose lui sits on another path (branch delay slot, earlier block): accepted only when
                // the address pair agrees with the data map of the straight-line references.
                c.relocData++;
            } else {
                difference(at, wa, wb, "instruction differs");
            }
        }
        ta.Step(wa);
        tb.Step(wb);
        if (Op(wa) == 0x0F) la.hi[Rt(wa)] = (wa & 0xFFFFu) << 16;
        if (Op(wb) == 0x0F) lb.hi[Rt(wb)] = (wb & 0xFFFFu) << 16;
        if (at > addressA && WordAt(ia, at - 4) == kJrRa) { ta.Reset(); tb.Reset(); }
    }
    if (!sameLength) {
        if (c.differences == 0) { c.firstDifference = endA - 4; c.note = "length differs (the aligned end moved)"; }
        c.differences++;
    }
    c.bytesIdentical = identical && sameLength;
    c.match = c.differences > 0 ? FunctionMatch::Changed : (*c.addressB == addressA ? FunctionMatch::Same : FunctionMatch::Shifted);
    return c;
}

void ProgramMap::CollectDataRefs(int module) {
    const GuestImage& ia = a_.Module(module);
    const GuestImage& ib = b_.Module(module);
    for (const ModuleAlignment::Run& run : Alignment(module).Runs()) {
        LuiTracker ta, tb;
        for (uint32_t at = run.startA; at < run.endA; at += 4) {
            const uint32_t atB = uint32_t(int64_t(at) + run.delta);
            const uint32_t wa = WordAt(ia, at), wb = WordAt(ib, atB);
            if (Op(wa) == Op(wb) && (wa & 0xFFFF0000u) == (wb & 0xFFFF0000u)) {
                const auto ra = ta.Resolve(wa), rb = tb.Resolve(wb);
                auto isRam = [](uint32_t x) { return (x >= 0x80000000u && x < 0x80200000u) || (x >= 0x1F800000u && x < 0x1F800400u); };
                if (ra && rb && isRam(*ra) && isRam(*rb)) dataRefs_[ScopeOf(module, *ra)][*ra][*rb]++;
            }
            ta.Step(wa);
            tb.Step(wb);
            if (at > run.startA && WordAt(ia, at - 4) == kJrRa) { ta.Reset(); tb.Reset(); }
        }
    }
}

int ProgramMap::ScopeOf(int module, uint32_t addressA) const {
    return module >= 0 && addressA >= a_.Module(module).base && addressA < a_.Module(module).End() ? module : -1;
}

const ProgramMap::RefMap& ProgramMap::DataRefs(int scope) const {
    static const RefMap kEmpty;
    auto it = dataRefs_.find(scope);
    return it == dataRefs_.end() ? kEmpty : it->second;
}

std::optional<uint32_t> ProgramMap::MapData(int module, uint32_t addressA, bool* exact, uint32_t maxGap) const {
    if (exact) *exact = false;
    const RefMap& refs = DataRefs(ScopeOf(module, addressA));
    auto best = [](const std::map<uint32_t, uint32_t>& targets) {
        uint32_t address = 0, uses = 0;
        for (const auto& [b, n] : targets) if (n > uses) { address = b; uses = n; }
        return address;
    };
    auto it = refs.lower_bound(addressA);
    if (it != refs.end() && it->first == addressA) {
        if (exact) *exact = true;
        return best(it->second);
    }
    if (it == refs.end() || it == refs.begin()) return std::nullopt;
    auto below = std::prev(it);
    const int64_t dBelow = int64_t(best(below->second)) - int64_t(below->first);
    const int64_t dAbove = int64_t(best(it->second)) - int64_t(it->first);
    if (dBelow != dAbove || addressA - below->first > maxGap || it->first - addressA > maxGap) return std::nullopt;
    return uint32_t(int64_t(addressA) + dBelow);
}

} // namespace gt2
