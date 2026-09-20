#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "gt2formats/overlay_data.h"

namespace gt2 {

// Cross-build code map: two builds of the same program (e.g. US Simulation v1.2 and US Arcade v1.1, which share
// one code base, docs/research/scout_exe.md section 6) are compared module by module - the resident executable
// and every GT2.OVL member. Nothing is executed; everything is derived from the bytes of both discs:
//   1. word alignment: k-grams of relocation-masked words that are unique in both builds anchor the two images;
//      the longest chain of anchors in address order gives runs with a constant address delta, extended word by
//      word while the masked words stay equal;
//   2. function comparison: a function of build A is compared instruction by instruction with its aligned
//      counterpart in build B. Differences that are pure relocations (jal/j targets that map to each other, lui
//      halves and the low 16 bits of the instructions that complete a lui-built address) are counted apart from
//      real code changes (other immediates, registers, opcodes, branch offsets, length);
//   3. data map: every lui-built address pair (A, B) seen in aligned code gives a data reference correspondence.
struct CodeModules {
    GuestImage exe;
    std::vector<GuestImage> overlays; // GT2.OVL members, all at 0x80010000
    std::string exeName;

    // Module index: -1 = the resident executable, 0.. = overlay member.
    const GuestImage& Module(int index) const { return index < 0 ? exe : overlays.at(size_t(index)); }
    static CodeModules Load(const DiscImage& disc);
};

class ModuleAlignment {
public:
    ModuleAlignment() = default;
    ModuleAlignment(const GuestImage& a, const GuestImage& b);

    // Address in build B of the word at `addressA`; nullopt outside the aligned runs.
    std::optional<uint32_t> Map(uint32_t addressA) const;
    // Number of words of A covered by runs.
    uint32_t CoveredWords() const;

    struct Run { uint32_t startA = 0, endA = 0; int64_t delta = 0; }; // [startA, endA) -> + delta
    const std::vector<Run>& Runs() const { return runs_; }

private:
    std::vector<Run> runs_;
};

enum class FunctionMatch { Same, Shifted, Changed, Unmapped };
const char* FunctionMatchName(FunctionMatch m);

struct FunctionComparison {
    int module = -1;
    uint32_t addressA = 0, endA = 0;
    std::optional<uint32_t> addressB, endB; // endB: from the alignment of the last word
    FunctionMatch match = FunctionMatch::Unmapped;
    bool bytesIdentical = false;            // every word equal (no relocation differs either)
    uint32_t relocCalls = 0, relocData = 0; // words that differ only by a relocation
    uint32_t differences = 0;               // words that differ otherwise (or a call to a function that does not map)
    uint32_t firstDifference = 0;           // address in A of the first real difference
    uint32_t wordA = 0, wordB = 0;          // the words at the first real difference
    std::string note;
};

class ProgramMap {
public:
    ProgramMap(const CodeModules& a, const CodeModules& b);

    const ModuleAlignment& Alignment(int module) const { return module < 0 ? exe_ : overlays_.at(size_t(module)); }
    // Heuristic function starts of module `module` of build A: jal targets, "jr ra + delay slot" followed by a
    // prologue within 8 words, plus `extraStarts`. Sorted.
    std::vector<uint32_t> FunctionStarts(int module, const std::vector<uint32_t>& extraStarts = {}) const;
    FunctionComparison CompareFunction(int module, uint32_t addressA, uint32_t endA) const;

    // Data references (lui-built addresses) of all aligned code, keyed by the address in A, per scope: an address
    // inside the image of overlay N belongs to scope N (the overlays share one address range), everything else
    // (executable, BSS, heap) to scope -1. A key with several different B addresses is a conflict.
    using RefMap = std::map<uint32_t, std::map<uint32_t, uint32_t>>; // A -> (B -> uses)
    const RefMap& DataRefs(int scope) const;
    int ScopeOf(int module, uint32_t addressA) const;
    // The B address of data address `addressA` referenced from `module`: the exact reference when seen, otherwise
    // the delta of the nearest seen references below and above when they agree and are at most `maxGap` bytes
    // away. `exact` tells which.
    std::optional<uint32_t> MapData(int module, uint32_t addressA, bool* exact = nullptr, uint32_t maxGap = 0x400) const;

private:
    uint32_t MapCallTarget(int module, uint32_t targetA, bool* ok) const;
    void CollectDataRefs(int module);

    const CodeModules& a_;
    const CodeModules& b_;
    ModuleAlignment exe_;
    std::vector<ModuleAlignment> overlays_;
    std::map<int, RefMap> dataRefs_; // scope -> references
};

} // namespace gt2
