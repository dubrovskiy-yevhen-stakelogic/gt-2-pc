#include "profile_cmds.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

#include "gt2formats/exe_profile.h"
#include "gt2vfs/disc_image.h"

namespace gt2 {
namespace {

std::string ModuleTag(int module) { return module < 0 ? "exe" : "ovl" + std::to_string(module); }

std::string SignedHex(int64_t v) {
    char text[32];
    std::snprintf(text, sizeof(text), "%s0x%llX", v < 0 ? "-" : "", static_cast<unsigned long long>(v < 0 ? -v : v));
    return text;
}

struct RefRun {
    int scope = -1;
    uint32_t start = 0, end = 0; // first / last referenced address
    int64_t delta = 0;
    std::vector<uint32_t> refs;
    std::string rejected;        // reason, empty = accepted
};

} // namespace

void WriteProfileRangesYaml(const CodeModules& a, const CodeModules& b, const ProgramMap& map, const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) throw std::runtime_error("cannot write " + path);
    std::fprintf(f,
                 "# ---- address profile ranges (the block below is GENERATED; replace it wholesale, never edit by hand):\n"
                 "#   build\\gt2tool.exe exe-map \"<Sim v1.2 disc>\" \"<this build's disc>\" --ranges-yaml <file>\n"
                 "# kind data-range = a run of lui-built data references of aligned code of both builds (%s -> %s) with one delta,\n"
                 "# [first reference, last reference]: an address inside maps by the run's delta. Runs are dropped when a reference\n"
                 "# lies inside the module image and the image's word alignment places that word at another delta (a constant that\n"
                 "# looks like an address, e.g. lui 0x8009 / addiu -1), or when a run of fewer than 3 references sits between two runs\n"
                 "# that share another delta (an isolated pair resolved across paths). kind aligned-range = a run of the word\n"
                 "# alignment of the module image (relocation-masked words equal; code and static data), [start, start + size).\n"
                 "# gen-profile (gt2tool) turns these entries and the facts of kind data into src/gt2formats/exe_profiles.inc.\n",
                 a.exeName.c_str(), b.exeName.c_str());
    std::vector<RefRun> runs;
    for (int scope = -1; scope < int(std::min(a.overlays.size(), b.overlays.size())); scope++) {
        std::vector<RefRun> list;
        for (const auto& [addrA, targets] : map.DataRefs(scope)) {
            uint32_t best = 0, uses = 0;
            for (const auto& [addrB, u] : targets)
                if (u > uses) { best = addrB; uses = u; }
            const int64_t delta = int64_t(best) - int64_t(addrA);
            if (!list.empty() && list.back().delta == delta) {
                list.back().end = addrA;
                list.back().refs.push_back(addrA);
                continue;
            }
            RefRun r;
            r.scope = scope;
            r.start = r.end = addrA;
            r.delta = delta;
            r.refs.push_back(addrA);
            list.push_back(r);
        }
        const GuestImage& image = a.Module(scope);
        const ModuleAlignment& alignment = map.Alignment(scope);
        for (size_t i = 0; i < list.size(); i++) {
            RefRun& r = list[i];
            uint32_t disagree = 0;
            for (uint32_t ref : r.refs) {
                if (!image.Contains(ref & ~3u, 4)) continue;
                const auto mapped = alignment.Map(ref & ~3u);
                if (mapped && int64_t(*mapped) - int64_t(ref & ~3u) != r.delta) disagree++;
            }
            if (disagree) r.rejected = std::to_string(disagree) + " reference(s) where the image alignment has another delta";
            else if (r.refs.size() < 3 && i > 0 && i + 1 < list.size() && list[i - 1].delta == list[i + 1].delta && list[i - 1].delta != r.delta)
                r.rejected = "isolated between two runs of delta " + SignedHex(list[i - 1].delta);
        }
        runs.insert(runs.end(), list.begin(), list.end());
    }
    size_t accepted = 0;
    for (const RefRun& r : runs) {
        if (!r.rejected.empty()) {
            std::fprintf(f, "# dropped %s 0x%08X..0x%08X %s (%zu refs): %s\n", ModuleTag(r.scope).c_str(), r.start, r.end, SignedHex(r.delta).c_str(),
                         r.refs.size(), r.rejected.c_str());
            continue;
        }
        accepted++;
        std::fprintf(f, "\n- address: 0x%08X\n  name: ref_run_%s_%08X\n", uint32_t(int64_t(r.start) + r.delta), ModuleTag(r.scope).c_str(), r.start);
        if (r.scope >= 0) std::fprintf(f, "  overlay: %s\n", ModuleTag(r.scope).c_str());
        std::fprintf(f, "  sim_address: 0x%08X\n  size: 0x%X\n  kind: data-range\n  delta: %s\n  status: mapped\n", r.start, r.end - r.start + 1,
                     SignedHex(r.delta).c_str());
        std::fprintf(f, "  evidence: \"gt2tool exe-map: %zu lui-built data reference(s) of aligned code, 0x%08X..0x%08X, all at delta %s\"\n", r.refs.size(),
                     r.start, r.end, SignedHex(r.delta).c_str());
    }
    size_t aligned = 0;
    for (int module = -1; module < int(std::min(a.overlays.size(), b.overlays.size())); module++)
        for (const ModuleAlignment::Run& run : map.Alignment(module).Runs()) {
            aligned++;
            std::fprintf(f, "\n- address: 0x%08X\n  name: aligned_%s_%08X\n", uint32_t(int64_t(run.startA) + run.delta), ModuleTag(module).c_str(), run.startA);
            if (module >= 0) std::fprintf(f, "  overlay: %s\n", ModuleTag(module).c_str());
            std::fprintf(f, "  sim_address: 0x%08X\n  size: 0x%X\n  kind: aligned-range\n  delta: %s\n  status: mapped\n", run.startA, run.endA - run.startA,
                         SignedHex(run.delta).c_str());
            std::fprintf(f, "  evidence: \"gt2tool exe-map: word alignment of the %s image, %u words with equal relocation-masked contents\"\n",
                         ModuleTag(module).c_str(), (run.endA - run.startA) / 4);
        }
    std::fclose(f);
    std::printf("profile ranges: %zu reference run(s) accepted, %zu dropped, %zu aligned run(s) -> %s\n", accepted, runs.size() - accepted, aligned, path.c_str());
}

namespace {

struct YamlFact {
    uint32_t address = 0, simAddress = 0, size = 0;
    bool haveSim = false, haveSize = false;
    int scope = -1;
    std::string name, kind;
};

std::vector<YamlFact> ReadFacts(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) throw std::runtime_error("cannot read " + path);
    std::vector<YamlFact> out;
    char line[2048];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        auto value = [&](const char* key) -> const char* {
            const size_t n = std::strlen(key);
            return s.compare(0, n, key) == 0 ? s.c_str() + n : nullptr;
        };
        if (const char* v = value("- address: ")) {
            out.emplace_back();
            out.back().address = uint32_t(std::strtoul(v, nullptr, 16));
        } else if (out.empty()) {
            continue;
        } else if (const char* v2 = value("  name: ")) {
            out.back().name = v2;
        } else if (const char* v3 = value("  overlay: ovl")) {
            out.back().scope = std::atoi(v3);
        } else if (const char* v4 = value("  sim_address: ")) {
            out.back().simAddress = uint32_t(std::strtoul(v4, nullptr, 16));
            out.back().haveSim = true;
        } else if (const char* v5 = value("  size: ")) {
            out.back().size = uint32_t(std::strtoul(v5, nullptr, 0));
            out.back().haveSize = true;
        } else if (const char* v6 = value("  kind: ")) {
            out.back().kind = v6;
        }
    }
    std::fclose(f);
    return out;
}

} // namespace

int CmdGenProfile(int argc, char** argv) {
    std::vector<std::pair<std::string, std::string>> builds; // CamelCase name, yaml path
    std::string outPath, checkSim, checkBuild;
    for (int i = 2; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--build" && i + 1 < argc) {
            const std::string v = argv[++i];
            const size_t eq = v.find('=');
            if (eq == std::string::npos) throw std::runtime_error("--build <Name>=<db.yaml>");
            builds.emplace_back(v.substr(0, eq), v.substr(eq + 1));
        } else if (arg == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (arg == "--check" && i + 2 < argc) { checkSim = argv[++i]; checkBuild = argv[++i]; }
        else throw std::runtime_error("gen-profile --build <Name>=<db.yaml>... [--out file.inc] [--check <Sim disc> <build disc>]");
    }
    std::string text;
    std::unique_ptr<CodeModules> simModules, buildModules;
    if (!checkSim.empty()) {
        simModules = std::make_unique<CodeModules>(CodeModules::Load(DiscImage(checkSim)));
        buildModules = std::make_unique<CodeModules>(CodeModules::Load(DiscImage(checkBuild)));
    }
    for (const auto& [name, path] : builds) {
        const std::vector<YamlFact> facts = ReadFacts(path);
        std::string rows;
        size_t counts[3] = {};
        for (const YamlFact& e : facts) {
            int kind = -1;
            if (e.kind == "data") kind = ProfileRange::kFact;
            else if (e.kind == "data-range") kind = ProfileRange::kRefRun;
            else if (e.kind == "aligned-range") kind = ProfileRange::kAligned;
            if (kind < 0) continue;
            if (!e.haveSim || !e.haveSize || e.size == 0) throw std::runtime_error(path + ": entry " + e.name + " of kind " + e.kind + " needs sim_address and size");
            counts[kind]++;
            static const char* const kKinds[] = {"ProfileRange::kFact", "ProfileRange::kRefRun", "ProfileRange::kAligned"};
            char row[256];
            std::snprintf(row, sizeof(row), "    {%d, %s, 0x%08Xu, 0x%08Xu, %s}, // %s\n", e.scope, kKinds[kind], e.simAddress, e.simAddress + e.size,
                          SignedHex(int64_t(e.address) - int64_t(e.simAddress)).c_str(), e.name.c_str());
            rows += row;
            if (simModules && kind == ProfileRange::kFact) {
                const GuestImage& ia = simModules->Module(e.scope);
                const GuestImage& ib = buildModules->Module(e.scope);
                if (ia.Contains(e.simAddress, e.size) && ib.Contains(e.address, e.size)) {
                    uint32_t differ = 0;
                    for (uint32_t k = 0; k < e.size; k++) differ += ia.Get<uint8_t>(e.simAddress + k) != ib.Get<uint8_t>(e.address + k);
                    std::printf("  %-32s %s 0x%08X -> 0x%08X, 0x%X bytes: %s\n", e.name.c_str(), ModuleTag(e.scope).c_str(), e.simAddress, e.address, e.size,
                                differ ? (std::to_string(differ) + " byte(s) differ").c_str() : "identical");
                } else {
                    std::printf("  %-32s %s 0x%08X -> 0x%08X, 0x%X bytes: runtime data (outside the images)\n", e.name.c_str(), ModuleTag(e.scope).c_str(),
                                e.simAddress, e.address, e.size);
                }
            }
        }
        std::printf("%s (%s): %zu fact(s), %zu reference run(s), %zu aligned run(s)\n", name.c_str(), path.c_str(), counts[0], counts[1], counts[2]);
        text += "// " + name + ": " + std::to_string(counts[0]) + " fact(s), " + std::to_string(counts[1]) + " reference run(s), " + std::to_string(counts[2]) +
                " aligned run(s) from " + path + "\n";
        text += "constexpr std::array<ProfileRange, " + std::to_string(counts[0] + counts[1] + counts[2]) + "> k" + name + "Ranges{{\n" + rows + "}};\n";
    }
    const std::string header = "// GENERATED by gt2tool gen-profile from db/*.yaml (facts: addresses of the builds); do not edit, regenerate:\n"
                               "//   gt2tool gen-profile --build <Name>=<db.yaml>... --out <output.inc>\n"
                               "// Row: {scope (-1 = executable / RAM, N = GT2.OVL member N), kind, Simulation start, Simulation end (exclusive), delta}.\n";
    if (!outPath.empty()) {
        std::FILE* f = std::fopen(outPath.c_str(), "wb");
        if (!f) throw std::runtime_error("cannot write " + outPath);
        const std::string all = header + text;
        std::fwrite(all.data(), 1, all.size(), f);
        std::fclose(f);
        std::printf("-> %s\n", outPath.c_str());
    }
    return 0;
}

int CmdProfileCheck(int argc, char** argv) {
    const DiscImage disc(argv[2]);
    const ExeProfile& profile = ProfileOf(disc);
    std::map<uint32_t, std::set<std::string>> literals;
    for (int i = 3; i < argc; i++)
        for (const auto& e : std::filesystem::recursive_directory_iterator(argv[i])) {
            if (!e.is_regular_file()) continue;
            const std::string ext = e.path().extension().string();
            if (ext != ".h" && ext != ".cpp") continue;
            std::FILE* f = std::fopen(e.path().string().c_str(), "rb");
            if (!f) continue;
            std::string t;
            char buf[65536];
            for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) t.append(buf, n);
            std::fclose(f);
            for (size_t at = t.find("0x80"); at != std::string::npos; at = t.find("0x80", at + 4)) {
                if (at > 0 && (std::isalnum(uint8_t(t[at - 1])) || t[at - 1] == '_')) continue;
                size_t end = at + 2;
                while (end < t.size() && std::isxdigit(uint8_t(t[end]))) end++;
                if (end - at != 10) continue;
                const uint32_t address = uint32_t(std::strtoul(t.c_str() + at + 2, nullptr, 16));
                if (address < 0x80010000u || address >= 0x80200000u) continue;
                literals[address].insert(e.path().filename().string());
            }
        }
    size_t data = 0, code = 0, none = 0;
    for (const auto& [address, files] : literals) {
        const auto d = profile.TryData(address, ExeProfile::RaceScope(address));
        const auto c = profile.TryCode(address);
        if (d) data++;
        else if (c) code++;
        else {
            none++;
            std::string list;
            for (const std::string& s : files) list += (list.empty() ? "" : ",") + s;
            std::printf("  unresolved 0x%08X  %s\n", address, list.c_str());
        }
    }
    std::printf("%s: %zu literal address(es): %zu resolve as data, %zu only as code (aligned runs), %zu unresolved\n", profile.name, literals.size(), data, code, none);
    return 0;
}

} // namespace gt2
