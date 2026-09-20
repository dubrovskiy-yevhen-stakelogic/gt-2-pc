// gt2tool - inspection CLI over gt2vfs/gt2formats. Reads the user's own disc image directly.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <optional>
#include <stdexcept>
#include <string>

#include "gt2export/car_gltf.h"
#include "gt2export/car_mesh.h"
#include "gt2export/car_preview.h"
#include "gt2formats/car_info.h"
#include "gt2formats/car_json.h"
#include "gt2formats/car_model.h"
#include "gt2formats/arcade_data.h"
#include "gt2formats/car_params.h"
#include "gt2formats/car_texture.h"
#include "gt2formats/gltf_reader.h"
#include "gt2formats/gtmode_tables.h"
#include "gt2formats/save_data.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/exe_map.h"
#include <cctype>
#include "game/career/career_state.h"
#include "game/career/garage.h"
#include "game/career/tuning.h"
#include <memory>
#include <span>
#include "gt2formats/track.h"
#include "gt2export/track_preview.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "menu_cmds.h"
#include "arcade_cmds.h"
#include "movie_cmds.h"
#include "replay_cmds.h"
#include "profile_cmds.h"
#include "track_cmds.h"

using namespace gt2;

namespace {

int Usage() {
    std::puts("usage:\n"
              "  gt2tool ls <disc.bin> [path-prefix]\n"
              "  gt2tool vol-index <disc.bin> [hex file number]...                VOL file numbers (the game loader's argument) -> paths\n"
              "  gt2tool param-scan <disc.bin> [table.dat] [--compare other.dat] [--dump ram.bin hexaddr] [--verbose]   stock records of every car of a GTDT car table\n"
              "  gt2tool str-info <disc.bin> <STREAM.DAT[:n]> [--decode]         Arcade disc movies: table, frames, size, audio (--decode: every frame)\n"
              "  gt2tool str-export <disc.bin> <STREAM.DAT:n> <out-dir> [--frames a-b] [--step n] [--no-audio]   PNG frames + audio.wav\n"
              "  gt2tool car-info <disc.bin> <car-id>\n"
              "  gt2tool car-scan <disc.bin>\n"
              "  gt2tool car-gltf <disc.bin> <car-id> <out-dir> [--lod N] [--paint N] [--night] [--strip]\n"
              "  gt2tool car-preview <disc.bin> <car-id> <out.png> [--lod N] [--paint N] [--yaw DEG] [--pitch DEG]\n"
              "  gt2tool export-cars <disc.bin> <out-dir> [--no-mesh] [--car ID]   one <id>.json (+ .gltf/.bin/.png) per car, stock configuration\n"
              "  gt2tool import-car <disc.bin> <car.json>                          validates a car file, prints the record diff against stock\n"
              "  gt2tool export-tracks <disc.bin> <out-dir> [--course NAME] [--no-gltf]   one <name>.json (+ .gltf/.bin/.png) per course, round trip checked\n"
              "  gt2tool import-track <disc.bin> <track.json>                      validates a course file, prints the diff against its base course\n"
              "  gt2tool extract <disc.bin> <vol-path> <out-file>                  one VOL file (inflated) to a local file (work\\ only)\n"
              "  gt2tool used-cars <disc.bin> [period 0..59] [--all]              used-car lots of a period (--all: also the cars hidden in the US)\n"
              "  gt2tool events <disc.bin> [event-name]                           GT-mode event table (carparam/usa_gtmode_race.dat)\n"
              "  gt2tool car-prices <disc.bin>                                    car catalogue (usa_gtmode_data table 30): maker, year, price\n"
              "  gt2tool menu-data <disc.bin>                                     checks the gtmenu containers, unistrdb, carcolor, solodata\n"
              "  gt2tool save-info <disc.bin> <card.mcd | save file | ram.bin>    GT-mode career (BASCUS-94455GAME): CRC, career, garage + stock check"
              "\n  gt2tool career-new <disc.bin> <out.mcd | save file> [card.mcd]      new-game career (0x800104A0) + header check / round trip of a card"
              "\n  gt2tool replay-list <card.mcd | replay file | disc:<path> disc.bin> the replay file's directory (BASCUS-94455REPLAY)"
              "\n  gt2tool replay-add <disc.bin> <card.mcd> <source>[#N] [title]     adds replay N of a card / replay file / disc:<vol path> to the card"
              "\n  gt2tool career-cars <disc.bin> <save>                              garage: figures, parts owned, sheet stages, available settings"
              "\n  gt2tool career-buy <disc.bin> <save> <car-id> <out> [--paint c] [--money N]   buy a car at its catalogue price (0x8001796C)"
              "\n  gt2tool career-part <disc.bin> <save> <car> <kind-hex> <out> [--buy] [--remove] [--body N]   buy / fit (0x80017D6C) / remove a part"
              "\n  gt2tool career-set <disc.bin> <save> <car> <setting> <entry> <delta> <out> [--reset]   settings screen (0x8005FC9C / 0x80054D10 / 0x8005F9DC)"
              "\n  gt2tool menu-page <disc.bin> <page> [--png out.png] [--compare cap.vram.bin [--side out.png]] [--money N] [--day N] [--no-cursor]   GM page items + render"
              "\n  gt2tool menu-dump <disc.bin> <out-dir> [--no-png]                   all GM pages: pages.txt + page_NNNN.png (work\\ only)"
              "\n  gt2tool exe-map <discA.bin> <discB.bin> [--db symbols.yaml] [--src dir[=ovlN]]... [--out map.tsv] [--data-out refs.tsv] [--yaml-out facts.yaml] [--ranges-yaml ranges.yaml] [--all]"
              "\n                  cross-build map: functions / data of program A in program B (same / shifted / changed / unmapped)"
              "\n  gt2tool gen-profile --build <Name>=<db.yaml>... [--out exe_profiles.inc] [--check <Sim disc> <build disc>]"
              "\n  gt2tool profile-check <disc> <source dir>...");
    return 2;
}

// export-cars: every car of the chassis table -> <outDir>/<id>.json (docs/formats/car_json.md) with the stock
// configuration and the record the game builds from it, plus the body mesh as glTF (car_gltf.h) and the paint
// list; every file is read back and compared byte for byte with the records it was written from.
int CmdExportCars(const GtfsVolume& vol, int argc, char** argv) {
    CheckCarJsonDescriptors();
    const std::string outDir = argv[3];
    bool mesh = true;
    std::string only;
    for (int i = 4; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--no-mesh") mesh = false;
        else if (a == "--car" && i + 1 < argc) only = argv[++i];
        else return Usage();
    }
    std::filesystem::create_directories(outDir);
    const CarParamTables tables = CarParamTables::Load(vol);
    const CarInfo info(vol.Read(".carinfoe"));
    size_t written = 0, meshes = 0, failures = 0, roundTripFailures = 0;
    for (size_t row = 0; row < tables.RowCount(kTableChassis); row++) {
        const std::string id = UnpackCarId(tables.RowAs<ChassisRow>(kTableChassis, row).carId);
        if (!only.empty() && id != only) continue;
        try {
            CarConfig config = StockCarConfig(tables, PackCarId(id));
            const std::string modelId = UnpackCarId(tables.RowAs<RacingModifyRow>(kTableRacingModify, config.racingModify).modelId);
            const CarModel model = ParseCarModel(vol.Read("carobj/" + modelId + ".cdo"));
            const sim::CarParams params = BuildCarParams(tables, config, BodyDimensionsOf(model));
            CarJson car = MakeCarJson(id, config, params);
            car.modelId = modelId;
            const CarTexture texture = ParseCarTexture(vol.Read("carobj/" + modelId + ".cdp"));
            std::vector<uint8_t> paintIds;
            for (const auto& p : texture.paints) paintIds.push_back(p.id);
            std::optional<CarInfoEntry> entry;
            try {
                entry = info.Lookup(id, paintIds);
            } catch (const std::exception& e) {
                std::printf("%s: .carinfoe: %s\n", id.c_str(), e.what());
            }
            if (entry) car.name = entry->name;
            for (size_t p = 0; p < texture.paints.size(); p++) {
                CarJsonPaint paint;
                paint.id = texture.paints[p].id;
                if (entry && p < entry->chipColors.size()) paint.chipColor = entry->chipColors[p];
                car.paints.push_back(paint);
            }
            const float ws = static_cast<float>(kCarWheelMetresPerUnit);
            car.hasBody = true;
            car.wheelFront = {model.wheels[0].z * ws, model.wheels[0].y * ws, model.wheels[0].x * ws};
            car.wheelRear = {model.wheels[2].z * ws, model.wheels[2].y * ws, model.wheels[2].x * ws};
            car.wheelRadius = {model.wheelRadiusFront * ws, model.wheelRadiusRear * ws};
            car.wheelWidth = {model.wheelWidthFront * ws, model.wheelWidthRear * ws};
            if (mesh) {
                CarGltfOptions opt;
                opt.lod = 0;
                opt.paint = 0;
                ExportCarGltf(model, texture, outDir, id, opt);
                car.hasMesh = true;
                car.meshFile = id + ".gltf";
                car.meshPaint = 0;
                meshes++;
            }
            const std::string path = outDir + "/" + id + ".json";
            WriteCarJson(path, car);
            written++;
            // Round trip.
            const CarJson back = ReadCarJson(path);
            const bool same = back.hasConfig && back.hasParams && std::memcmp(&back.config, &config, sizeof(config)) == 0 && std::memcmp(&back.params, &params, sizeof(params)) == 0 &&
                              back.configMask.All() && back.paramsMask.All() && back.warnings.empty();
            if (!same) {
                roundTripFailures++;
                std::printf("%s: ROUND TRIP MISMATCH\n", id.c_str());
                for (const std::string& d : DiffCarParams(params, back.params)) std::printf("  params %s\n", d.c_str());
                for (const std::string& d : DiffCarConfig(config, back.config)) std::printf("  config %s\n", d.c_str());
                for (const std::string& w : back.warnings) std::printf("  %s\n", w.c_str());
            }
        } catch (const std::exception& e) {
            std::printf("%s: FAIL: %s\n", id.c_str(), e.what());
            failures++;
        }
    }
    std::printf("export-cars: %zu car(s) in the chassis table, %zu json written, %zu mesh(es), %zu failure(s), %zu round-trip mismatch(es) -> %s\n",
                tables.RowCount(kTableChassis), written, meshes, failures, roundTripFailures, outDir.c_str());
    return failures || roundTripFailures ? 1 : 0;
}

// import-car: reads a car file, resolves it over the disc (car_json.h ResolveCarJson) and prints what differs
// from the base car's stock record - the same resolution gt2game --mods performs.
int CmdImportCar(const GtfsVolume& vol, const std::string& jsonPath) {
    CheckCarJsonDescriptors();
    const CarJson car = ReadCarJson(jsonPath);
    for (const std::string& w : car.warnings) std::printf("warning: %s\n", w.c_str());
    size_t configFields = 0, paramFields = 0;
    for (const bool b : car.configMask.present) configFields += b ? 1 : 0;
    for (const bool b : car.paramsMask.present) paramFields += b ? 1 : 0;
    std::printf("%s: car \"%s\"%s%s; config fields %zu/%zu, params fields %zu/%zu%s%s%s\n", jsonPath.c_str(), car.carId.c_str(), car.baseCar.empty() ? "" : ", base ",
                car.baseCar.c_str(), configFields, CarConfigFields().size(), paramFields, CarParamsFields().size(), car.hasMesh ? ", mesh " : "", car.hasMesh ? car.meshFile.c_str() : "",
                car.hasBody ? ", body block" : "");
    const CarParamTables tables = CarParamTables::Load(vol);
    const ResolvedCar r = ResolveCarJson(vol, tables, car, jsonPath);
    for (const std::string& n : r.notes) std::printf("note: %s\n", n.c_str());
    if (r.externalMesh)
        std::printf("mesh: %zu triangles, %zu image(s), bounds x %.3f..%.3f y %.3f..%.3f z %.3f..%.3f m (x scale %.3f)\n", r.mesh->TriangleCount(), r.mesh->images.size(),
                    r.mesh->boundsMin[0], r.mesh->boundsMax[0], r.mesh->boundsMin[1], r.mesh->boundsMax[1], r.mesh->boundsMin[2], r.mesh->boundsMax[2], car.meshScale);
    std::printf("wheels: front (%.3f, %.3f, %.3f) r %.3f w %.3f, rear (%.3f, %.3f, %.3f) r %.3f w %.3f m\n", r.wheelFront[0], r.wheelFront[1], r.wheelFront[2], r.wheelRadius[0],
                r.wheelWidth[0], r.wheelRear[0], r.wheelRear[1], r.wheelRear[2], r.wheelRadius[1], r.wheelWidth[1]);
    std::printf("sound: engine set %05u, exhaust %u, turbo %s\n", r.engineSoundId, r.exhaustByte, r.turbo ? "yes" : "no");
    if (r.baseCar.empty()) {
        std::puts("stand-alone car (no base in the tables): no stock record to diff against; effective record:");
        const sim::CarParams zero{};
        for (const std::string& d : DiffCarParams(zero, r.params)) std::printf("  %s\n", d.c_str());
        return 0;
    }
    const std::vector<std::string> configDiff = DiffCarConfig(r.stockConfig, r.config), paramsDiff = DiffCarParams(r.stockParams, r.params);
    std::printf("config vs stock %s: %zu field(s) differ\n", r.baseCar.c_str(), configDiff.size());
    for (const std::string& d : configDiff) std::printf("  %s\n", d.c_str());
    std::printf("params vs stock %s: %zu field(s) differ\n", r.baseCar.c_str(), paramsDiff.size());
    for (const std::string& d : paramsDiff) std::printf("  %s\n", d.c_str());
    return 0;
}

bool EndsWith(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// exe-map: cross-build map of the program on disc A (e.g. US Simulation v1.2) to the program on disc B (e.g. US
// Arcade v1.1): every function / data address named in db\*.yaml or referenced in the given source trees is
// classified same / shifted / changed / unmapped (gt2formats/exe_map.h); --all adds every heuristic function.
struct MapQuery {
    int module = 0;
    uint32_t address = 0;
    std::string name, source;
};

std::string ModuleName(int module) { return module < 0 ? "exe" : "ovl" + std::to_string(module); }

int ModuleFromName(const std::string& s) {
    if (s == "exe") return -1;
    if (s.rfind("ovl", 0) == 0 && s.size() == 4 && s[3] >= '0' && s[3] <= '9') return s[3] - '0';
    throw std::runtime_error("bad module name: " + s + " (exe | ovl0..ovl9)");
}

// Module of an address of build A without an explicit tag: the resident code starts at the executable's pc0
// area above the largest overlay; below it the default module applies (boot block entries: names "boot_*").
int DefaultModule(const CodeModules& a, uint32_t address, int fallback, const std::string& name) {
    uint32_t overlayEnd = 0;
    for (const GuestImage& o : a.overlays) overlayEnd = std::max(overlayEnd, o.End());
    if (address >= overlayEnd || name.rfind("boot_", 0) == 0) return -1;
    return fallback;
}

void ReadDbQueries(const CodeModules& a, const std::string& path, std::vector<MapQuery>& out) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) throw std::runtime_error("cannot read " + path);
    char line[1024];
    std::vector<MapQuery> entries;
    std::vector<bool> tagged;
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (s.rfind("- address: ", 0) == 0) {
            MapQuery q;
            q.address = uint32_t(std::strtoul(s.c_str() + 11, nullptr, 16));
            q.source = "db";
            entries.push_back(q);
            tagged.push_back(false);
        } else if (!entries.empty() && s.rfind("  name: ", 0) == 0) {
            entries.back().name = s.substr(8);
        } else if (!entries.empty() && s.rfind("  overlay: ", 0) == 0) {
            entries.back().module = ModuleFromName(s.substr(11));
            tagged.back() = true;
        }
    }
    std::fclose(f);
    for (size_t i = 0; i < entries.size(); i++) {
        if (!tagged[i]) entries[i].module = DefaultModule(a, entries[i].address, 0, entries[i].name);
        out.push_back(entries[i]);
    }
}

// Every "0x80xxxxxx" literal in the .h / .cpp files of `dir` (comments included: they name the original functions).
void ReadSourceQueries(const CodeModules& a, const std::string& dir, int fallback, std::vector<MapQuery>& out) {
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        const std::string ext = e.path().extension().string();
        if (ext != ".h" && ext != ".cpp") continue;
        std::FILE* f = std::fopen(e.path().string().c_str(), "rb");
        if (!f) continue;
        std::string text;
        char buf[65536];
        for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
        std::fclose(f);
        for (size_t at = text.find("0x80"); at != std::string::npos; at = text.find("0x80", at + 4)) {
            if (at > 0 && (std::isalnum(uint8_t(text[at - 1])) || text[at - 1] == '_')) continue;
            size_t end = at + 2;
            while (end < text.size() && std::isxdigit(uint8_t(text[end]))) end++;
            if (end - at != 10) continue;
            MapQuery q;
            q.address = uint32_t(std::strtoul(text.c_str() + at + 2, nullptr, 16));
            if (q.address < 0x80010000u || q.address >= 0x80200000u) continue;
            q.module = DefaultModule(a, q.address, fallback, "");
            q.source = e.path().filename().string();
            out.push_back(q);
        }
    }
}

int CmdExeMap(int argc, char** argv) {
    const DiscImage discA(argv[2]), discB(argv[3]);
    const CodeModules a = CodeModules::Load(discA), b = CodeModules::Load(discB);
    std::vector<MapQuery> queries;
    std::string outPath, dataOutPath, yamlOutPath, rangesYamlPath;
    bool all = false;
    for (int i = 4; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--db" && i + 1 < argc) ReadDbQueries(a, argv[++i], queries);
        else if (arg == "--src" && i + 1 < argc) {
            std::string dir = argv[++i];
            int module = 0;
            if (const size_t eq = dir.find('='); eq != std::string::npos) { module = ModuleFromName(dir.substr(eq + 1)); dir = dir.substr(0, eq); }
            ReadSourceQueries(a, dir, module, queries);
        } else if (arg == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (arg == "--data-out" && i + 1 < argc) dataOutPath = argv[++i];
        else if (arg == "--all") all = true;
        else if (arg == "--yaml-out" && i + 1 < argc) yamlOutPath = argv[++i];
        else if (arg == "--ranges-yaml" && i + 1 < argc) rangesYamlPath = argv[++i];
        else return Usage();
    }
    std::printf("A: %s  B: %s\n", a.exeName.c_str(), b.exeName.c_str());
    const ProgramMap map(a, b);
    for (int m = -1; m < int(std::min(a.overlays.size(), b.overlays.size())); m++) {
        const ModuleAlignment& al = map.Alignment(m);
        std::printf("  %-4s A %u bytes, B %u bytes: %zu aligned runs cover %u of %u words of A\n", ModuleName(m).c_str(),
                    uint32_t(a.Module(m).bytes.size()), uint32_t(b.Module(m).bytes.size()), al.Runs().size(), al.CoveredWords(),
                    uint32_t(a.Module(m).bytes.size() / 4));
    }

    // Merge duplicate queries (same module + address): db names win, sources are listed.
    std::map<std::pair<int, uint32_t>, MapQuery> merged;
    for (const MapQuery& q : queries) {
        auto [it, inserted] = merged.try_emplace({q.module, q.address}, q);
        if (inserted) continue;
        if (it->second.name.empty()) it->second.name = q.name;
        if (it->second.source.find(q.source) == std::string::npos) it->second.source += "," + q.source;
    }
    // Function starts per module: the heuristic ones plus every queried address that looks like code.
    std::map<int, std::vector<uint32_t>> heuristic, starts;
    auto isCode = [&](int module, uint32_t address) {
        const GuestImage& img = a.Module(module);
        if (!img.Contains(address, 4)) return false;
        auto& h = heuristic[module];
        if (h.empty()) h = map.FunctionStarts(module);
        if (std::binary_search(h.begin(), h.end(), address)) return true;
        // After "jr ra" + delay slot (+ zero padding): a function without a prologue that nothing calls directly.
        uint32_t at = address;
        while (at >= img.base + 8 && img.Get<uint32_t>(at - 4) == 0 && img.Get<uint32_t>(at - 8) != 0x03E00008u) at -= 4;
        return at >= img.base + 8 && img.Get<uint32_t>(at - 8) == 0x03E00008u;
    };
    for (const auto& [key, q] : merged)
        if (isCode(key.first, key.second)) starts[key.first].push_back(key.second);
    for (auto& [module, list] : starts) {
        auto extra = list;
        list = map.FunctionStarts(module, extra);
    }
    auto endOf = [&](int module, uint32_t address) {
        auto& list = starts[module];
        if (list.empty()) list = map.FunctionStarts(module);
        auto it = std::upper_bound(list.begin(), list.end(), address);
        uint32_t end = it == list.end() ? a.Module(module).End() : *it;
        return std::min(end, address + 0x8000u);
    };

    std::FILE* out = outPath.empty() ? nullptr : std::fopen(outPath.c_str(), "w");
    if (!outPath.empty() && !out) throw std::runtime_error("cannot write " + outPath);
    // --yaml-out: the named entries (db) as facts keyed by B's executable (db/<disc>_symbols.yaml format).
    std::FILE* yaml = yamlOutPath.empty() ? nullptr : std::fopen(yamlOutPath.c_str(), "w");
    if (!yamlOutPath.empty() && !yaml) throw std::runtime_error("cannot write " + yamlOutPath);
    if (yaml)
        std::fprintf(yaml, "# GENERATED by gt2tool exe-map (%s -> %s) from the named entries of the A-side symbol file; do not edit,%s"
                           "# regenerate. address = the address in %s; sim_address = the entry of the A-side file it maps from.%s",
                     a.exeName.c_str(), b.exeName.c_str(), "\n", b.exeName.c_str(), "\n");
    if (out) std::fprintf(out, "module\taddress_a\taddress_b\tkind\tmatch\twords_a\treloc_calls\treloc_data\tdifferences\tfirst_difference\tname\tsource\tnote\n");
    std::map<std::string, std::map<std::string, int>> counts; // module -> match -> n
    for (const auto& [key, q] : merged) {
        const auto [module, address] = key;
        std::string kind, match, note;
        std::optional<uint32_t> addressB;
        uint32_t words = 0, relocCalls = 0, relocData = 0, differences = 0, firstDifference = 0;
        if (isCode(module, address)) {
            const FunctionComparison c = map.CompareFunction(module, address, endOf(module, address));
            kind = "code";
            match = FunctionMatchName(c.match);
            addressB = c.addressB;
            words = (c.endA - c.addressA) / 4;
            relocCalls = c.relocCalls;
            relocData = c.relocData;
            differences = c.differences;
            firstDifference = c.firstDifference;
            note = c.note;
            if (c.differences) {
                char text[64];
                std::snprintf(text, sizeof(text), " (A %08X / B %08X)", c.wordA, c.wordB);
                note += text;
            }
        } else {
            kind = "data";
            bool exact = false;
            addressB = map.MapData(module, address, &exact);
            if (addressB) match = exact ? "ref" : "near-ref";
            else if (a.Module(module).Contains(address, 4) && (addressB = map.Alignment(module).Map(address))) match = "aligned";
            else match = "unmapped";
            if (addressB && match != "unmapped") match = (*addressB == address ? "same-" : "moved-") + match;
        }
        counts[ModuleName(module) + " " + kind][match]++;
        // Data placed only by the word alignment (no code reference) is too weak for a fact file: TSV only.
        const bool weak = kind == "data" && match.find("aligned") != std::string::npos;
        if (yaml && !q.name.empty() && addressB && match != "unmapped" && !weak) {
            std::fprintf(yaml, "\n- address: 0x%08X\n  name: %s\n", *addressB, q.name.c_str());
            if (module >= 0) std::fprintf(yaml, "  overlay: %s\n", ModuleName(module).c_str());
            std::fprintf(yaml, "  sim_address: 0x%08X\n  match: %s\n  status: mapped\n", address, match.c_str());
            if (kind == "code")
                std::fprintf(yaml, "  evidence: \"gt2tool exe-map: %u words compared with the %s function; %u relocated call(s), %u relocated data half(s), %u other difference(s)%s%s\"\n",
                             words, a.exeName.c_str(), relocCalls, relocData, differences, differences ? " - " : "", differences ? note.c_str() : "");
            else
                std::fprintf(yaml, "  evidence: \"gt2tool exe-map: data address from %s (ref = the same lui-built reference in aligned code of both builds, near-ref = delta of the nearest references, aligned = word alignment of the module image)\"\n",
                             match.c_str());
        }
        if (out) {
            std::fprintf(out, "%s\t0x%08X\t%s\t%s\t%s\t%u\t%u\t%u\t%u\t%s\t%s\t%s\t%s\n", ModuleName(module).c_str(), address,
                         addressB ? ("0x" + [&] { char t[16]; std::snprintf(t, sizeof(t), "%08X", *addressB); return std::string(t); }()).c_str() : "-",
                         kind.c_str(), match.c_str(), words, relocCalls, relocData, differences,
                         differences ? [&] { char t[16]; std::snprintf(t, sizeof(t), "0x%08X", firstDifference); return std::string(t); }().c_str() : "-",
                         q.name.c_str(), q.source.c_str(), note.c_str());
        }
    }
    if (out) std::fclose(out);
    if (yaml) std::fclose(yaml);
    std::printf("queried addresses (%zu):\n", merged.size());
    for (const auto& [group, byMatch] : counts) {
        std::printf("  %-10s", group.c_str());
        for (const auto& [m, n] : byMatch) std::printf(" %s %d", m.c_str(), n);
        std::printf("\n");
    }

    if (all) {
        std::printf("all heuristic functions (>= 8 instructions):\n");
        for (int m = -1; m < int(std::min(a.overlays.size(), b.overlays.size())); m++) {
            const auto list = map.FunctionStarts(m);
            std::map<FunctionMatch, std::pair<int, uint32_t>> n; // count, words
            int identicalBytes = 0;
            for (size_t i = 0; i < list.size(); i++) {
                const uint32_t end = i + 1 < list.size() ? list[i + 1] : a.Module(m).End();
                if (end - list[i] < 32 || end - list[i] > 0x8000) continue;
                bool returns = false; // code has a "jr ra"; stretches of data between functions do not
                for (uint32_t at = list[i]; at < end && !returns; at += 4) returns = a.Module(m).Get<uint32_t>(at) == 0x03E00008u;
                if (!returns) continue;
                const FunctionComparison c = map.CompareFunction(m, list[i], end);
                n[c.match].first++;
                n[c.match].second += (end - list[i]) / 4;
                if (c.bytesIdentical) identicalBytes++;
            }
            std::printf("  %-4s", ModuleName(m).c_str());
            for (const auto& [match, cw] : n) std::printf("  %s %d (%u words)", FunctionMatchName(match), cw.first, cw.second);
            std::printf("  [byte-identical %d]\n", identicalBytes);
        }
    }

    if (!dataOutPath.empty()) {
        std::FILE* d = std::fopen(dataOutPath.c_str(), "w");
        if (!d) throw std::runtime_error("cannot write " + dataOutPath);
        // Ranges of data references with one delta per scope; conflicts (one A address, several B addresses) apart.
        std::fprintf(d, "# data references of aligned code: scope\tstart_a\tend_a\tdelta\trefs\n");
        size_t total = 0;
        uint32_t conflicts = 0;
        for (int scope = -1; scope < int(a.overlays.size()); scope++) {
            int64_t runDelta = 0;
            uint32_t runStart = 0, runEnd = 0, runRefs = 0;
            auto flush = [&] {
                if (runRefs)
                    std::fprintf(d, "%s\t0x%08X\t0x%08X\t%s0x%llX\t%u\n", ModuleName(scope).c_str(), runStart, runEnd, runDelta < 0 ? "-" : "+",
                                 static_cast<unsigned long long>(runDelta < 0 ? -runDelta : runDelta), runRefs);
            };
            for (const auto& [addrA, targets] : map.DataRefs(scope)) {
                total++;
                if (targets.size() > 1) {
                    conflicts++;
                    std::fprintf(d, "# conflict %s 0x%08X ->", ModuleName(scope).c_str(), addrA);
                    for (const auto& [addrB, uses] : targets) std::fprintf(d, " 0x%08X x%u", addrB, uses);
                    std::fprintf(d, "\n");
                }
                uint32_t best = 0, uses = 0;
                for (const auto& [addrB, u] : targets) if (u > uses) { best = addrB; uses = u; }
                const int64_t delta = int64_t(best) - int64_t(addrA);
                if (runRefs && delta == runDelta) { runEnd = addrA; runRefs++; continue; }
                flush();
                runStart = runEnd = addrA;
                runDelta = delta;
                runRefs = 1;
            }
            flush();
        }
        std::fclose(d);
        std::printf("data references: %zu addresses, %u with conflicting targets -> %s\n", total, conflicts, dataOutPath.c_str());
    }
    if (!rangesYamlPath.empty()) WriteProfileRangesYaml(a, b, map, rangesYamlPath);
    return 0;
}

// param-scan: every chassis row of a GTDT car table (default carparam/usa_arcade_data.dat, the table the arcade
// race loads) -> stock configuration -> the 0x1C0-byte parameter record (BuildCarParams); with --compare the
// record of the same car built from a second table file (e.g. carparam/usa_gtmode_data.dat) is diffed.
int CmdParamScan(const GtfsVolume& vol, int argc, char** argv) {
    std::string path = "carparam/usa_arcade_data.dat", comparePath, dumpPath;
    uint32_t dumpRecords = 0;
    bool verbose = false;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--compare" && i + 1 < argc) comparePath = argv[++i];
        else if (a == "--dump" && i + 2 < argc) { dumpPath = argv[++i]; dumpRecords = uint32_t(std::strtoul(argv[++i], nullptr, 16)); }
        else if (a == "--verbose") verbose = true;
        else path = a;
    }
    const CarParamTables tables = CarParamTables::Load(vol, path);
    if (path == "carparam/usa_arcade_data.dat") {
        // The arcade shell's rules (gt2formats/arcade_data.h): the player's cars = rows of tables 32 / 33 (each names its
        // part rows, the racing-modification row too: no stock-row search), the AI = the opponent rows of table 31.
        const ArcadeData ad = ArcadeData::Load(vol, path);
        size_t ok = 0, bad = 0;
        auto build = [&](const std::string& what, CarConfig config) {
            try {
                (void)BuildCarParams(vol, ad.Tables(), config);
                ok++;
            } catch (const std::exception& e) {
                bad++;
                std::printf("  %s FAILED: %s\n", what.c_str(), e.what());
            }
        };
        for (size_t row = 0; row < ad.PlayerCarCount(); row++)
            for (size_t table : {kArcadePlayerCarTable, kArcadePlayerCarTableAlt})
                build(UnpackCarId(ad.PlayerCarId(row)) + " (table " + std::to_string(table) + ")", ad.PlayerCarConfig(row, table));
        for (uint32_t n = 1; n <= ad.OpponentCount(); n++) build("opponent " + std::to_string(n), ad.OpponentConfig(n));
        std::printf("arcade rules: %zu player car rows x 2 tables + %zu opponent rows, %zu events: %zu records built, %zu failed\n", ad.PlayerCarCount(),
                    ad.OpponentCount(), ad.EventCount(), ok, bad);
        // Chassis rows (table 3) that no player car row and no opponent row selects: the arcade shell never builds them.
        std::set<uint16_t> chassisUsed;
        for (size_t row = 0; row < ad.PlayerCarCount(); row++) {
            const CarConfig c = ad.PlayerCarConfig(row);
            chassisUsed.insert(c.chassis);
            // The chassis rows without a stock racing-modification row (StockCarConfig fails): the row names it itself.
            const uint32_t chassisCar = tables.RowAs<ChassisRow>(kTableChassis, c.chassis).carId;
            try {
                (void)StockCarConfig(tables, chassisCar);
            } catch (const std::exception&) {
                std::printf("arcade rules: %s (chassis row %u) is player car row %zu (%s): racing modify row %u named by the row\n", UnpackCarId(chassisCar).c_str(),
                            c.chassis, row, UnpackCarId(ad.PlayerCarId(row)).c_str(), c.racingModify);
            }
        }
        for (uint32_t n = 1; n <= ad.OpponentCount(); n++) chassisUsed.insert(ad.OpponentConfig(n).chassis);
        for (size_t row = 0; row < tables.RowCount(kTableChassis); row++)
            if (!chassisUsed.count(uint16_t(row)))
                std::printf("arcade rules: chassis row %zu (%s) is selected by no player car row (tables 32 / 33) and no opponent row (table 31)\n", row,
                            UnpackCarId(tables.RowAs<ChassisRow>(kTableChassis, row).carId).c_str());
    }
    std::optional<CarParamTables> other;
    if (!comparePath.empty()) other = CarParamTables::Load(vol, comparePath);
    std::printf("%s:", path.c_str());
    for (size_t t = 0; t < kCarParamTableCount; t++) std::printf(" %zu", tables.RowCount(t));
    std::printf(" rows per table\n");
    size_t built = 0, failed = 0, compared = 0, identical = 0, missing = 0;
    std::map<std::string, sim::CarParams> records;
    for (size_t row = 0; row < tables.RowCount(kTableChassis); row++) {
        const uint32_t carId = tables.RowAs<ChassisRow>(kTableChassis, row).carId;
        const std::string id = UnpackCarId(carId);
        try {
            CarConfig config = StockCarConfig(tables, carId);
            const sim::CarParams params = BuildCarParams(vol, tables, config);
            built++;
            records[id] = params;
            if (!other) {
                if (verbose) std::printf("  %-6s ok\n", id.c_str());
                continue;
            }
            CarConfig otherConfig;
            try {
                otherConfig = StockCarConfig(*other, carId);
            } catch (const std::exception&) {
                missing++;
                if (verbose) std::printf("  %-6s not in %s\n", id.c_str(), comparePath.c_str());
                continue;
            }
            const sim::CarParams otherParams = BuildCarParams(vol, *other, otherConfig);
            const std::vector<std::string> diff = DiffCarParams(otherParams, params);
            compared++;
            if (diff.empty()) identical++;
            if (verbose || (!diff.empty() && compared - identical <= 5)) {
                std::printf("  %-6s %zu field(s) differ from %s\n", id.c_str(), diff.size(), comparePath.c_str());
                for (size_t k = 0; k < diff.size() && k < 12; k++) std::printf("      %s\n", diff[k].c_str());
            }
        } catch (const std::exception& e) {
            failed++;
            std::printf("  %-6s FAILED: %s\n", id.c_str(), e.what());
        }
    }
    if (!dumpPath.empty()) {
        // The six race records of a RAM dump (slot * 0x1C0 from `dumpRecords`) against the closest native record.
        std::FILE* f = std::fopen(dumpPath.c_str(), "rb");
        if (!f) throw std::runtime_error("cannot read " + dumpPath);
        std::vector<uint8_t> ram(0x200000);
        const size_t got = std::fread(ram.data(), 1, ram.size(), f);
        std::fclose(f);
        if (got != ram.size()) throw std::runtime_error(dumpPath + ": not a 2 MB RAM dump");
        for (uint32_t slot = 0; slot < 6; slot++) {
            const uint32_t at = (dumpRecords & 0x1FFFFFu) + slot * uint32_t(sizeof(sim::CarParams));
            if (at + sizeof(sim::CarParams) > ram.size()) break;
            sim::CarParams dumped;
            std::memcpy(&dumped, ram.data() + at, sizeof(dumped));
            std::string best;
            size_t bestBytes = SIZE_MAX;
            for (const auto& [id, p] : records) {
                const uint8_t* q = reinterpret_cast<const uint8_t*>(&p);
                size_t n = 0;
                for (size_t k = 0; k < sizeof(p); k++) n += q[k] != ram[at + k];
                if (n < bestBytes) { bestBytes = n; best = id; }
            }
            std::printf("dump slot %u (0x%08X): closest native record %s, %zu byte(s) differ\n", slot, 0x80000000u | at, best.c_str(), bestBytes);
            if (!best.empty())
                for (const std::string& line : DiffCarParams(records[best], dumped)) std::printf("      %s\n", line.c_str());
        }
    }
    std::printf("%zu chassis rows: %zu records built, %zu failed", tables.RowCount(kTableChassis), built, failed);
    if (other) std::printf("; vs %s: %zu compared, %zu identical, %zu differ, %zu not there", comparePath.c_str(), compared, identical, compared - identical, missing);
    std::printf("\n");
    return failed ? 1 : 0;
}

// vol-index: VOL file numbers (the argument of the game's VOL loader, hex) -> paths; without numbers every file.
int CmdVolIndex(const GtfsVolume& vol, int argc, char** argv) {
    std::map<uint32_t, const GtfsEntry*> byIndex;
    for (const auto& f : vol.Files()) byIndex[f.index] = &f;
    if (argc <= 3) {
        for (const auto& [index, f] : byIndex) std::printf("0x%04X  %10u  %s\n", index, f->size, f->path.c_str());
        return 0;
    }
    for (int i = 3; i < argc; i++) {
        const uint32_t index = uint32_t(std::strtoul(argv[i], nullptr, 16));
        auto it = byIndex.find(index);
        std::printf("0x%04X  %s\n", index, it == byIndex.end() ? "(no file)" : it->second->path.c_str());
    }
    return 0;
}

int CmdLs(const GtfsVolume& vol, const std::string& prefix) {
    size_t count = 0;
    for (const auto& f : vol.Files()) {
        if (f.path.rfind(prefix, 0) != 0) continue;
        std::printf("%10u  %s\n", f.size, f.path.c_str());
        count++;
    }
    std::printf("%zu files\n", count);
    return 0;
}

void PrintModel(const CarModel& m) {
    std::printf("wheel radius/width front %d/%d rear %d/%d, dish rgb %u,%u,%u\n", m.wheelRadiusFront, m.wheelWidthFront,
                m.wheelRadiusRear, m.wheelWidthRear, m.wheelDishColor[0], m.wheelDishColor[1], m.wheelDishColor[2]);
    for (size_t i = 0; i < 4; i++)
        std::printf("wheel %zu: w=%d y=%d x=%d z=%d\n", i, m.wheels[i].w, m.wheels[i].y, m.wheels[i].x, m.wheels[i].z);
    for (size_t i = 0; i < m.lods.size(); i++) {
        const CarLod& l = m.lods[i];
        std::printf("LOD%zu: dist %u verts %u normals %u tris %u quads %u uvtris %u uvquads %u scale %d (unk %d)\n", i,
                    l.maxDistance, l.counts[0], l.counts[1], l.counts[2], l.counts[3], l.counts[6], l.counts[7], l.scale,
                    l.scaleUnk);
        std::printf("      bbox min %d,%d,%d,%d max %d,%d,%d,%d\n", l.bbox[0], l.bbox[1], l.bbox[2], l.bbox[3], l.bbox[4],
                    l.bbox[5], l.bbox[6], l.bbox[7]);
    }
}

int CmdCarInfo(const GtfsVolume& vol, const std::string& id) {
    CarModel m = ParseCarModel(vol.Read("carobj/" + id + ".cdo"));
    PrintModel(m);
    CarTexture t = ParseCarTexture(vol.Read("carobj/" + id + ".cdp"));
    std::vector<uint8_t> ids;
    for (const auto& p : t.paints) ids.push_back(p.id);
    CarInfo info(vol.Read(".carinfoe"));
    if (auto e = info.Lookup(id, ids)) std::printf("name: %s (code %02X, flags %04X)\n", e->name.c_str(), e->code, e->flags);
    else std::printf("name: <not listed in .carinfoe>\n");
    std::printf("paints: %zu, ids:", t.paints.size());
    for (const auto& p : t.paints) std::printf(" %02X", p.id);
    std::printf("\nbody length %.3f m, wheelbase %.3f m (scale hypothesis)\n",
                (m.lods[0].bbox[6] - m.lods[0].bbox[2]) * CarBodyMetresPerUnit(m.lods[0]),
                (m.wheels[2].x - m.wheels[0].x) * kCarWheelMetresPerUnit);
    return 0;
}

int CmdCarScan(const GtfsVolume& vol) {
    std::map<std::string, size_t> errors;
    std::map<int, size_t> scales, primCodes, rawPalettes, lodCounts, paintCounts;
    size_t models = 0, textures = 0, ok = 0;
    size_t quadStrip = 0, quadRing = 0, quadAmbiguous = 0;
    size_t wheelsInsideBody = 0, wheelsChecked = 0;
    std::string firstError;
    CarInfo info(vol.Read(".carinfoe"));
    size_t infoFound = 0, infoMissing = 0, infoBadName = 0;

    for (const auto& f : vol.Files()) {
        if (f.path.rfind("carobj/", 0) != 0) continue;
        bool isModel = EndsWith(f.path, ".cdo.gz") || EndsWith(f.path, ".cno.gz");
        bool isTexture = EndsWith(f.path, ".cdp.gz") || EndsWith(f.path, ".cnp.gz");
        if (!isModel && !isTexture) continue;
        try {
            auto data = vol.Read(f);
            if (isTexture) {
                textures++;
                CarTexture t = ParseCarTexture(data);
                paintCounts[static_cast<int>(t.paints.size())]++;
                if (EndsWith(f.path, ".cdp.gz")) {
                    std::vector<uint8_t> ids;
                    for (const auto& p : t.paints) ids.push_back(p.id);
                    std::string id = f.path.substr(7, 5);
                    if (auto e = info.Lookup(id, ids)) {
                        infoFound++;
                        bool printable = !e->name.empty();
                        for (char c : e->name) printable = printable && static_cast<unsigned char>(c) >= 0x20;
                        if (!printable) infoBadName++;
                    } else {
                        infoMissing++;
                    }
                }
                ok++;
                continue;
            }
            models++;
            CarModel m = ParseCarModel(data);
            ok++;
            lodCounts[static_cast<int>(m.lods.size())]++;
            for (const CarLod& l : m.lods) {
                scales[l.scale]++;
                for (const CarPolygon& p : l.polygons) {
                    primCodes[p.primCode]++;
                    if (p.IsTextured()) rawPalettes[p.rawPalette]++;
                    if (!p.IsQuad()) continue;
                    auto vec = [&](int i) {
                        const CarVertex& v = l.vertices[p.vertex[i]];
                        return std::array<double, 3>{double(v.x), double(v.y), double(v.z)};
                    };
                    auto sub = [](auto a, auto b) { return std::array<double, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
                    auto cross = [](auto a, auto b) {
                        return std::array<double, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
                    };
                    auto dot = [](auto a, auto b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
                    auto a = vec(0), b = vec(1), c = vec(2), d = vec(3);
                    auto n1 = cross(sub(b, a), sub(c, a));
                    double ring = dot(n1, cross(sub(c, a), sub(d, a)));
                    double strip = dot(n1, cross(sub(d, b), sub(c, b)));
                    if (strip > 0 && ring <= 0) quadStrip++;
                    else if (ring > 0 && strip <= 0) quadRing++;
                    else quadAmbiguous++;
                }
            }
            // Scale hypothesis check: front and rear axle X must lie inside the body's Z extent.
            const CarLod& l0 = m.lods[0];
            double bs = CarBodyMetresPerUnit(l0), ws = kCarWheelMetresPerUnit;
            wheelsChecked++;
            if (m.wheels[0].x * ws > l0.bbox[2] * bs && m.wheels[2].x * ws < l0.bbox[6] * bs) wheelsInsideBody++;
            else std::printf("  scale outlier: %s scale %d bodyZ %d..%d axleX %d..%d\n", f.path.c_str(), l0.scale, l0.bbox[2], l0.bbox[6], m.wheels[0].x, m.wheels[2].x);
        } catch (const std::exception& e) {
            if (errors.empty()) firstError = f.path;
            errors[e.what()]++;
        }
    }

    std::printf("models %zu, textures %zu, parsed ok %zu\n", models, textures, ok);
    for (const auto& [msg, n] : errors) std::printf("  ERROR x%zu: %s\n", n, msg.c_str());
    if (!firstError.empty()) std::printf("  first failing file: %s\n", firstError.c_str());
    auto dump = [](const char* title, const std::map<int, size_t>& h, bool hex) {
        std::printf("%s:", title);
        for (const auto& [k, n] : h) std::printf(hex ? " %02X=%zu" : " %d=%zu", k, n);
        std::printf("\n");
    };
    dump("LOD count", lodCounts, false);
    dump("paint count", paintCounts, false);
    dump("LOD scale field", scales, false);
    dump("prim codes", primCodes, true);
    dump("raw palette values", rawPalettes, true);
    std::printf(".carinfoe: %zu entries; cars found %zu, not listed %zu, bad names %zu\n", info.Count(), infoFound, infoMissing, infoBadName);
    std::printf("quad vertex order votes: strip %zu, ring %zu, ambiguous %zu\n", quadStrip, quadRing, quadAmbiguous);
    std::printf("scale hypothesis: axles inside body extent for %zu of %zu models\n", wheelsInsideBody, wheelsChecked);
    return errors.empty() ? 0 : 1;
}

int CmdCarGltf(const GtfsVolume& vol, int argc, char** argv) {
    std::string id = argv[3], outDir = argv[4];
    CarGltfOptions opt;
    bool night = false;
    for (int i = 5; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--lod" && i + 1 < argc) opt.lod = static_cast<size_t>(std::atoi(argv[++i]));
        else if (a == "--paint" && i + 1 < argc) opt.paint = static_cast<size_t>(std::atoi(argv[++i]));
        else if (a == "--night") night = true;
        else if (a == "--strip") opt.quadStripOrder = true;
        else return Usage();
    }
    CarModel m = ParseCarModel(vol.Read("carobj/" + id + (night ? ".cno" : ".cdo")));
    CarTexture t = ParseCarTexture(vol.Read("carobj/" + id + (night ? ".cnp" : ".cdp")));
    std::filesystem::create_directories(outDir);
    std::string base = id + (night ? "_night" : "") + "_lod" + std::to_string(opt.lod) + "_paint" + std::to_string(opt.paint);
    ExportCarGltf(m, t, outDir, base, opt);
    std::printf("wrote %s/%s.gltf (+ .bin, .png)\n", outDir.c_str(), base.c_str());
    return 0;
}

int CmdCarPreview(const GtfsVolume& vol, int argc, char** argv) {
    std::string id = argv[3], out = argv[4];
    CarPreviewOptions opt;
    for (int i = 5; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--lod") opt.lod = static_cast<size_t>(std::atoi(argv[i + 1]));
        else if (a == "--paint") opt.paint = static_cast<size_t>(std::atoi(argv[i + 1]));
        else if (a == "--yaw") opt.yawDegrees = std::atof(argv[i + 1]);
        else if (a == "--pitch") opt.pitchDegrees = std::atof(argv[i + 1]);
        else return Usage();
    }
    CarModel m = ParseCarModel(vol.Read("carobj/" + id + ".cdo"));
    CarTexture t = ParseCarTexture(vol.Read("carobj/" + id + ".cdp"));
    std::filesystem::create_directories(std::filesystem::path(out).parent_path());
    RenderCarPreview(m, t, out, opt);
    std::printf("wrote %s\n", out.c_str());
    return 0;
}

int CmdTrackScan(const GtfsVolume& vol) {
    std::map<std::string, size_t> errors;
    size_t files = 0, ok = 0, chunks = 0, polygons = 0, vertices = 0, tims = 0, uvEntries = 0;
    for (const auto& f : vol.Files()) {
        if (f.path.rfind("crsobj/", 0) != 0 || !EndsWith(f.path, ".tro.gz")) continue;
        files++;
        try {
            Track t = ParseTrack(vol.Read(f));
            PsxVram vram;
            tims += vram.LoadTimPack(vol.Read(f.path.substr(0, f.path.size() - 7) + ".trp"));
            uvEntries += t.uvTable.size();
            ok++;
            chunks += t.chunks.size();
            size_t chunkLights = 0, modelLights = 0; // the glow ("light") records of chunk shapes and scenery models (+0x42)
            for (const auto& c : t.chunks) {
                polygons += c.road.polygons.size() + c.surround.polygons.size();
                vertices += c.road.vertices.size() + c.surround.vertices.size();
                chunkLights += c.lightCount;
            }
            for (const auto& m : t.sceneryModels) modelLights += m.lightCount;
            if (chunkLights + modelLights != 0) std::printf("  %s: light records: chunks %zu, models %zu\n", f.path.c_str(), chunkLights, modelLights);
        } catch (const std::exception& e) {
            std::printf("  %s: %s\n", f.path.c_str(), e.what());
            errors[e.what()]++;
        }
    }
    std::printf("tracks %zu, parsed ok %zu, chunks %zu, vertices %zu, polygons %zu, UV entries %zu, TIMs %zu\n", files, ok,
                chunks, vertices, polygons, uvEntries, tims);
    return errors.empty() ? 0 : 1;
}

int CmdTrackPreview(const GtfsVolume& vol, int argc, char** argv) {
    std::string name = argv[3], out = argv[4];
    TrackPreviewOptions opt;
    bool textured = true;
    for (int i = 5; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--chunk" && i + 1 < argc) opt.chunk = std::atoi(argv[++i]);
        else if (a == "--height" && i + 1 < argc) opt.eyeHeight = std::atof(argv[++i]);
        else if (a == "--road-only") opt.surround = false;
        else if (a == "--untextured") textured = false;
        else return Usage();
    }
    Track t = ParseTrack(vol.Read("crsobj/" + name + ".tro"));
    std::printf("%s: length %d m, %zu chunks\n", name.c_str(), t.lengthMetres, t.chunks.size());
    std::filesystem::create_directories(std::filesystem::path(out).parent_path());
    PsxVram vram;
    if (textured) vram.LoadTimPack(vol.Read("crsobj/" + name + ".trp"));
    RenderTrackPreview(t, textured ? &vram : nullptr, out, opt);
    std::printf("wrote %s\n", out.c_str());
    return 0;
}

// ---------------------------------------------------------------- GT-mode menu data (docs/formats/gtmode_tables.md)

std::string CarLabel(const CarInfoDirectory& cars, uint32_t carId) {
    const CarInfoRecord* r = cars.Find(carId);
    return UnpackCarId(carId) + " " + (r ? r->name : std::string("?"));
}

// used-cars: the 39 lots of one period (default 0) of .usedcar_usa, as 0x800224E0 filters them for the US.
int CmdUsedCars(const GtfsVolume& vol, int argc, char** argv) {
    size_t period = 0;
    bool all = false;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--all") all = true;
        else period = size_t(std::strtoul(a.c_str(), nullptr, 10));
    }
    const UsedCarLists lists = UsedCarLists::Load(vol);
    const CarInfoDirectory cars = CarInfoDirectory::Load(vol);
    const CarColorNames colours = CarColorNames::Load(vol);
    std::printf("period %zu of %zu (shown for u32[0x801C99D8] / 10 %% 60 == %zu)\n", period, kUsedCarPeriodCount, period);
    for (size_t m = 0; m < kUsedCarMakerCount; m++) {
        const std::vector<UsedCarEntry> stored = lists.List(period, m);
        if (stored.empty()) continue;
        const std::vector<UsedCarEntry> lot = lists.Lot(period, m, cars);
        std::printf("maker %zu: %zu cars stored, %zu shown\n", m, stored.size(), lot.size());
        for (const UsedCarEntry& e : all ? stored : lot) {
            const int32_t ci = cars.IndexOf(e.carId);
            std::string colour = "?";
            if (ci >= 0) {
                const CarInfoRecord& r = cars.At(size_t(ci));
                const int p = r.PaintIndex(e.paintId);
                if (p >= 0) colour = colours.Name(colours.NameIndices(size_t(ci), r.PaintCount())[size_t(p)]);
            }
            const bool hidden = ci < 0 || !cars.At(size_t(ci)).AvailableIn(kLanguageUsa);
            std::printf("  %8u  %-40s paint '%c' %s%s\n", e.price, CarLabel(cars, e.carId).c_str(), char(e.paintId), colour.c_str(), hidden ? "  [hidden in US]" : "");
        }
    }
    return 0;
}

// events: every row of carparam/usa_gtmode_race.dat table 0 (or one event with its opponents).
int CmdEvents(const GtfsVolume& vol, int argc, char** argv) {
    const GtModeRaceData race = GtModeRaceData::Load(vol);
    const CarInfoDirectory cars = CarInfoDirectory::Load(vol);
    static const char* kDrive[] = {"-", "FF", "FR", "MR", "RR", "4WD"};
    auto print = [&](const RaceEvent& e) {
        std::printf("%3zu %-14s course %-16s laps %3u start %3u licence %u opp %2zu prize", e.row, e.name.c_str(), e.course.c_str(), e.Laps(), e.StartSpeed(),
                    e.LicenceRequired(), e.OpponentCount());
        for (uint32_t p : e.prize) std::printf(" %u", p);
        std::printf(" | tag %s", e.tag.c_str());
        if (e.powerLimit) std::printf(" power<=%u", e.powerLimit);
        if (e.DriveRestriction()) std::printf(" drive %s", e.DriveRestriction() < 6 ? kDrive[e.DriveRestriction()] : "?");
        if (e.DirtTyresRequired()) std::printf(" dirt-tyres");
        if (e.aspiration) std::printf(" aspiration %u", e.aspiration);
        if (e.byte9B) std::printf(" b9B %u", e.byte9B);
        if (e.bonus) std::printf(" bonus %u", e.bonus);
        if (e.CarListIndex()) std::printf(" cars-list %u (%zu)", e.CarListIndex(), race.CarList(e.CarListIndex()).size());
        for (uint32_t id : e.prizeCars)
            if (id) std::printf(" prize-car %s", UnpackCarId(id).c_str());
        std::printf("\n");
    };
    if (argc > 3) {
        const int32_t row = race.FindEvent(argv[3]);
        if (row < 0) throw std::runtime_error(std::string("no event ") + argv[3]);
        const RaceEvent e = race.EventAt(size_t(row));
        print(e);
        for (size_t s = 0; s < e.OpponentCount(); s++) {
            const uint32_t n = RaceEvent::SlotOpponent(e.slots[s]);
            const OpponentCarRow o = race.Opponent(n);
            std::printf("  slot %2zu: opponent %4u paint '%c' %-40s torque %u%%\n", s, n, RaceEvent::SlotPaint(e.slots[s]), CarLabel(cars, o.spec.carId).c_str(),
                        o.torqueMultiplier100);
        }
        for (uint32_t id : race.CarList(e.CarListIndex())) std::printf("  allowed: %s\n", CarLabel(cars, id).c_str());
        return 0;
    }
    std::printf("%zu events, %zu opponents, %zu car lists, %zu pool names\n", race.EventCount(), race.OpponentCount(), race.CarListCount(), race.Names().size());
    for (size_t i = 0; i < race.EventCount(); i++) print(race.EventAt(i));
    return 0;
}

// car-prices: the catalogue (table 30 of usa_gtmode_data.dat) + the check that 0x80076FC0's part rows equal StockCarConfig.
int CmdCarPrices(const GtfsVolume& vol) {
    const CarParamTables tables = CarParamTables::Load(vol);
    const CarInfoDirectory cars = CarInfoDirectory::Load(vol);
    const std::vector<std::u16string> strings = ParseUniStrDb(vol.Read("carparam/usa_unistrdb.dat"));
    size_t same = 0, stockFailed = 0;
    for (size_t i = 0; i < CarCatalogueCount(tables); i++) {
        const CarCatalogueRow c = CarCatalogueAt(tables, i);
        const std::string model = c.modelName < strings.size() ? Utf16ToUtf8(strings[c.modelName]) : "?";
        const std::string grade = c.gradeName < strings.size() ? Utf16ToUtf8(strings[c.gradeName]) : "?";
        std::printf("%3zu %s maker %2u year %2u race %u price %8u  %s | %s | %s\n", i, UnpackCarId(c.spec.carId).c_str(), c.maker, c.year, c.raceCar, c.price,
                    model.c_str(), grade.c_str(), cars.Find(c.spec.carId) ? cars.Find(c.spec.carId)->name.c_str() : "?");
        const CarConfig spec = ConfigFromCarSpec(tables, tables.Row(kCarCatalogueTable, i));
        try {
            const CarConfig stock = StockCarConfig(tables, c.spec.carId);
            if (std::memcmp(reinterpret_cast<const uint8_t*>(&spec) + 4, reinterpret_cast<const uint8_t*>(&stock) + 4, 0x34) == 0) same++;
        } catch (const std::exception&) {
            stockFailed++;
        }
    }
    std::printf("part rows of 0x80076FC0(catalogue row) == StockCarConfig: %zu of %zu (%zu without a stock config)\n", same, CarCatalogueCount(tables), stockFailed);
    // The attract replay's six configurations against 0x80076954 (catalogue config of the same car): differing bytes.
    const std::vector<uint8_t> replay = vol.Read("arcade/demofile_us.gmr");
    for (size_t s = 0; s < kReplayCarSlotCount; s++) {
        const ReplayCar rc = ReplayCarAt(replay, s);
        const std::optional<CarConfig> cat = CatalogueCarConfig(tables, rc.carId);
        std::printf("replay slot %zu %s:", s, UnpackCarId(rc.carId).c_str());
        if (!cat) {
            std::printf(" not in the catalogue\n");
            continue;
        }
        const auto* a = reinterpret_cast<const uint8_t*>(&rc.config);
        const auto* b = reinterpret_cast<const uint8_t*>(&*cat);
        size_t diffs = 0;
        for (size_t i = 0; i < sizeof(CarConfig); i++)
            if (a[i] != b[i]) {
                if (diffs++ < 24) std::printf(" +%02zX:%02X/%02X", i, a[i], b[i]);
            }
        std::printf("  (%zu bytes differ, replay/catalogue)\n", diffs);
    }
    return 0;
}

// menu-data: parses every container of the GT-mode menus and reports the counts.
int CmdMenuData(const GtfsVolume& vol) {
    const std::vector<std::u16string> strings = ParseUniStrDb(vol.Read("carparam/usa_unistrdb.dat"));
    std::printf("unistrdb: %zu strings\n", strings.size());
    const CarInfoDirectory cars = CarInfoDirectory::Load(vol);
    const CarColorNames colours = CarColorNames::Load(vol);
    size_t paints = 0;
    for (size_t i = 0; i < cars.Count(); i++) {
        for (uint16_t n : colours.NameIndices(i, cars.At(i).PaintCount()))
            if (n >= colours.NameCount()) throw std::runtime_error("carcolor: name index out of range");
        paints += cars.At(i).PaintCount();
    }
    std::printf("carcolor: %zu cars (carinfo %zu), %zu paints, %zu colour names\n", colours.CarCount(), cars.Count(), paints, colours.NameCount());
    const MenuPackIndex common = ParseMenuPackIndex(vol.Read("gtmenu/commonpic.idx"));
    const GtfsEntry* commonDat = vol.Find("gtmenu/commonpic.dat");
    if (!commonDat) throw std::runtime_error("no gtmenu/commonpic.dat");
    const std::vector<uint8_t> commonBytes = vol.ReadStored(*commonDat);
    size_t gtmp = 0;
    for (size_t i = 0; i < common.Count(); i++) {
        const std::vector<uint8_t> e = MenuPackEntry(commonBytes, common, i, false);
        if (e.size() >= 4 && std::memcmp(e.data(), "GTMP", 4) == 0) gtmp++;
    }
    std::printf("commonpic: %zu entries, %zu with GTMP magic, last offset %u = file size %u\n", common.Count(), gtmp, common.offsets.back(), commonDat->size);
    const MenuPackIndex menu = ParseMenuPackIndex(vol.Read("gtmenu/usa/gtmenudat.idx"));
    const GtfsEntry* menuDat = vol.Find("gtmenu/usa/gtmenudat.dat");
    if (!menuDat) throw std::runtime_error("no gtmenu/usa/gtmenudat.dat");
    const std::vector<uint8_t> menuBytes = vol.ReadStored(*menuDat);
    size_t gm = 0, inflated = 0;
    for (size_t i = 0; i < menu.Count(); i++) {
        const std::vector<uint8_t> e = MenuPackEntry(menuBytes, menu, i, true);
        inflated += e.size();
        if (e.size() >= 4 && std::memcmp(e.data(), "GM\x03\0", 4) == 0) gm++;
    }
    std::printf("gtmenudat: %zu gzip entries, %zu inflate to GM\\3 pages (%zu bytes), last offset %u = file size %u\n", menu.Count(), gm, inflated, menu.offsets.back(),
                menuDat->size);
    const SoloData solo = ParseSoloData(vol.Read("gtmenu/usa/solodata.dat"));
    size_t soloOutside = 0, soloUnknownCars = 0;
    for (uint16_t p : solo.pages) soloOutside += p >= menu.Count();
    for (const auto& [id, v] : solo.cars) {
        soloOutside += v >= menu.Count();
        soloUnknownCars += cars.IndexOf(id) < 0;
    }
    std::printf("solodata: %zu pages, %zu cars (%zu not in carinfo), %zu values >= the gtmenudat entry count\n", solo.pages.size(), solo.cars.size(),
                soloUnknownCars, soloOutside);
    std::printf("iconimg: %zu bytes (VRAM %d,%d %dx%d)\n", vol.Read("gtmenu/usa/iconimg.dat").size(), kIconImageVramX, kIconImageVramY, kIconImageWidth, kIconImageHeight);
    return 0;
}

// save-info: the GT-mode career of a memory card image (.mcd, file BASCUS-94455GAME), a bare save file or a 2 MB
// RAM dump (state at 0x801C98E0) - docs/research/menus_gtmode.md. Oracle check per garage car: the stock
// configuration built natively (StockCarConfig + the record builder's write-backs) against the stored config, and
// the weight / drive type bits against the built record.
int CmdSaveInfo(const GtfsVolume& vol, const std::string& path) {
    std::vector<uint8_t> bytes;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        bytes.resize(size_t(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
        std::fclose(f);
    }
    if (bytes.empty()) throw std::runtime_error("cannot read " + path);
    GameSave save;
    if (bytes.size() == 128 * 1024 && bytes[0] == 'M' && bytes[1] == 'C') {
        std::optional<MemoryCardFile> file;
        for (MemoryCardFile& f : ReadMemoryCardFiles(bytes)) {
            std::printf("card file %-20s %6u bytes, first block %d\n", f.name.c_str(), f.size, f.firstBlock);
            if (f.name == kSaveGameFileName) file = std::move(f);
        }
        if (!file) throw std::runtime_error(std::string("no ") + kSaveGameFileName + " on the card");
        save = ParseGameSave(file->bytes);
        std::printf("save: header '%c%c' icon flags 0x%02X blocks %u, CRC stored %08X computed %08X -> %s\n", save.header[0], save.header[1],
                    save.header[2], save.header[3], save.storedCrc, save.computedCrc, save.CrcOk() ? "ok" : "MISMATCH");
    } else if (bytes.size() == 2 * 1024 * 1024) {
        save = GameSaveFromState(std::span<const uint8_t>(bytes).subspan(kCareerStateAddress & 0x1FFFFF, kSaveStateSize));
        std::printf("RAM dump: career state at 0x%08X\n", kCareerStateAddress);
    } else {
        save = ParseGameSave(bytes);
        std::printf("save file: CRC stored %08X computed %08X -> %s\n", save.storedCrc, save.computedCrc, save.CrcOk() ? "ok" : "MISMATCH");
    }
    const CareerRecord career = save.Career();
    size_t results = 0;
    for (size_t n = 0; n < career.resultNibbles.size() * 2; n++) results += career.Result(n) != 0;
    std::printf("language %u; career: day %u, races %u, wins %u, position sum %u, prize total %u + %u x 100,000,000, %zu result entries set\n",
                save.Language(), career.days, career.races, career.wins, career.positionSum, career.prize, career.prizeHundredMillions, results);
    const Garage garage = save.GarageBlock();
    std::printf("garage: %u car(s), %u credits, current car %d\n", garage.count, garage.money, garage.currentCar);
    const CarParamTables tables = CarParamTables::Load(vol);
    int failures = 0;
    for (size_t i = 0; i < garage.cars.size(); i++) {
        const GarageCar& c = garage.cars[i];
        std::string parts;
        for (uint8_t b : c.partsOwned) { char h[4]; std::snprintf(h, sizeof(h), "%02X", b); parts += h; }
        std::printf("  [%zu] %-8s model %-8s colour %3u price %8u  %u kg drive %u power %u word96 %u flags %d%d parts %s\n", i,
                    UnpackCarId(c.carId).c_str(), UnpackCarId(c.modelId).c_str(), c.colour, c.price, c.weightKg, c.driveType, c.power, c.word96,
                    int(c.flag15), int(c.flag14), parts.c_str());
        try {
            // 0x8001796C (buy): 0x80076954 = the catalogue configuration, then 0x8001EC0C runs the record builder
            // 0x800771AC on it (write-backs +76 / +78 / +7A) before storing it.
            const std::optional<CarConfig> catalogue = CatalogueCarConfig(tables, c.carId);
            if (!catalogue) throw std::runtime_error("car not in the catalogue (table 30)");
            CarConfig stock = *catalogue;
            // 0x80017750 -> 0x80016FEC: the setting-screen object sets flags |= 0xC0 and copies the config back
            // (after 0x8005EAC0(obj, 6, gearbox stage), not ported: no effect on a stock gearbox here).
            stock.flags |= 0xC0;
            const sim::CarParams params = BuildCarParams(vol, tables, stock);
            const uint8_t* a = reinterpret_cast<const uint8_t*>(&stock);
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&c.config);
            std::string diffs;
            for (size_t k = 0; k < sizeof(CarConfig); k++)
                if (a[k] != b[k]) { char h[32]; std::snprintf(h, sizeof(h), " +%02zX (native %02X stored %02X)", k, a[k], b[k]); diffs += h; }
            const bool bodyOk = (uint16_t(params.weightKg) & 0x1FFF) == c.weightKg && (params.driveType & 7) == c.driveType;
            std::printf("      catalogue config (0x80076954 + 0x80016FEC + builder) vs stored: %s; weight/drive vs built record: %s\n",
                        diffs.empty() ? "equal" : ("differs at" + diffs).c_str(), bodyOk ? "equal" : "DIFFER");
            const bool untouched = std::all_of(c.partsOwned.begin(), c.partsOwned.end(), [](uint8_t b) { return b == 0; });
            failures += !bodyOk || (untouched && !diffs.empty());
        } catch (const std::exception& e) {
            std::printf("      (no stock configuration: %s)\n", e.what());
        }
    }
    return failures ? 1 : 0;
}

// career-new <out> [template card]: a new-game career (src/game/career: 0x800104A0 with the executable's defaults and
// the save header of 0x8006A038) written as a save file or a card image; with a card holding a game save, the built
// header is compared with the card's header and the card is round-tripped (load + store = the same bytes).
int CmdCareerNew(const DiscImage& disc, const std::string& out, const std::string& templatePath) {
    using namespace gt2::career;
    CareerSave save;
    save.state = NewCareer(ReadNewGameDefaults(disc));
    save.header = BuildSaveHeader(LoadExeImage(disc));
    int failures = 0;
    std::vector<uint8_t> templateCard;
    if (!templatePath.empty()) {
        templateCard = ReadFileBytes(templatePath);
        const CareerSave existing = LoadCareerFromCard(templateCard);
        const bool headerSame = existing.header == save.header;
        std::printf("save header built from the executable vs %s: %s\n", templatePath.c_str(), headerSame ? "identical" : "DIFFERS");
        std::vector<uint8_t> again = templateCard;
        StoreCareerOnCard(again, existing);
        const bool roundTrip = again == templateCard;
        std::printf("card round trip (load + store): %s\n", roundTrip ? "byte-identical" : "DIFFERS");
        failures += (headerSame ? 0 : 1) + (roundTrip ? 0 : 1);
        templateCard = FormatMemoryCard(); // the new career goes to a fresh card
    }
    SaveCareer(out, save, templateCard);
    const CareerSave reread = LoadCareer(out);
    std::printf("new career -> %s: day %u, money %d, current car %d, CRC %08X (%s)\n", out.c_str(), reread.state.record.days, reread.state.garage.money,
                reread.state.garage.currentCar, reread.storedCrc, reread.CrcOk() ? "ok" : "MISMATCH");
    return failures || !reread.CrcOk() ? 1 : 0;
}

// ---------------------------------------------------------------- career garage commands (src/game/career/tuning.*)

struct CareerFile {
    std::string path;
    std::vector<uint8_t> bytes;
    career::CareerSave save;
    bool card = false;
    static CareerFile Load(const std::string& path) {
        CareerFile f;
        f.path = path;
        f.bytes = career::ReadFileBytes(path);
        f.save = career::LoadCareer(path);
        f.card = f.bytes.size() == 128 * 1024 && f.bytes[0] == 'M' && f.bytes[1] == 'C';
        return f;
    }
    void Write(const std::string& out) const {
        career::SaveCareer(out, save, card ? std::span<const uint8_t>(bytes) : std::span<const uint8_t>());
        const career::CareerSave reread = career::LoadCareer(out);
        std::printf("saved %s: CRC %08X (%s), money %d, %d car(s)\n", out.c_str(), reread.storedCrc, reread.CrcOk() ? "ok" : "MISMATCH", reread.state.garage.money,
                    reread.state.garage.count);
    }
};

// The record the race builds for a garage car (model dimensions of its racing-modification body) and its figures.
void PrintCarSummary(const GtfsVolume& vol, const career::CareerData& d, const career::GarageCar& car, const char* label) {
    CarConfig config = car.config;
    const sim::CarParams record = BuildCarParams(vol, d.tables, config);
    std::printf("%s: %s (model %s) value %d cr: power %u PS, torque %.1f kgm, weight %u kg, drive %u; record: weight %d kg, power %% %u / %u, gears %u, final %d\n", label,
                UnpackCarId(car.carId).c_str(), UnpackCarId(car.modelId).c_str(), car.value, car.powerFlags & 0x3FFF, car.torqueFigure / 10.0, car.weightDrive & 0x1FFF,
                car.weightDrive >> 13, record.weightKg, record.powerPercent, record.powerPercentTop, record.gearCount, record.finalDrive);
}

// career-cars <save>: the garage with the figures, the parts owned and the settings the parts allow.
int CmdCareerCars(const GtfsVolume& vol, const career::CareerData& d, const std::string& path) {
    using namespace gt2::career;
    const CareerFile f = CareerFile::Load(path);
    const GarageBlock& g = f.save.state.garage;
    std::printf("%s: money %d cr, %d car(s), current %d\n", path.c_str(), g.money, g.count, g.currentCar);
    auto sheet = std::make_unique<TuneSheet>();
    static const char* const kSettings[] = {"springs", "ride height", "dampers bump", "dampers rebound", "camber", "toe", "anti-roll", "brake balance", "gears",
                                            "gear auto", "LSD initial", "LSD accel", "LSD decel", "LSD rear initial", "ASM", "TCS", "downforce"};
    for (int32_t i = 0; i < g.count; i++) {
        const career::GarageCar& car = g.cars[i];
        PrintCarSummary(vol, d, car, ("car " + std::to_string(i)).c_str());
        std::printf("  parts owned:");
        for (int32_t k = 0; k < 0x32; k++)
            if (PartOwned(car, k)) std::printf(" %02X", k);
        LoadCarSheet(*sheet, car, d.tables);
        std::printf("\n  stages:");
        for (int32_t k = 0; k <= kTuneLsd; k++) std::printf(" %d", sheet->stage[k]);
        std::printf("\n  settings:");
        for (int32_t k = 0; k < kSettingCount; k++) {
            SettingValue v[9]{};
            const int32_t n = GetSetting(*sheet, k, v, d);
            if (n < 1) continue;
            std::printf(" %s[", kSettings[k]);
            for (int32_t e = 0; e < n; e++) std::printf("%s%d (%d..%d)", e ? ", " : "", v[e].value, v[e].min, v[e].max);
            std::printf("]");
        }
        std::printf("\n");
    }
    return 0;
}

// career-buy <save> <car id> <out> [--paint c] [--money N]: 0x8001796C (a new car at its catalogue price; --money sets
// the credits first, a test aid).
int CmdCareerBuy(const GtfsVolume& vol, const career::CareerData& d, int argc, char** argv) {
    using namespace gt2::career;
    CareerFile f = CareerFile::Load(argv[3]);
    GarageBlock& g = f.save.state.garage;
    const uint32_t id = PackCarId(argv[4]);
    uint32_t paint = 0;
    for (int i = 6; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--paint" && i + 1 < argc) paint = uint8_t(argv[++i][0]);
        else if (a == "--money" && i + 1 < argc) g.money = std::atoi(argv[++i]);
    }
    const CarInfoRecord& info = d.cars.At(size_t(std::max(0, d.cars.IndexOf(id))));
    if (paint == 0) paint = info.paintIds.at(0);
    auto sheet = std::make_unique<TuneSheet>();
    std::vector<uint8_t> scratch(0x400);
    const int32_t price = CataloguePrice(d.tables, id);
    const int32_t r = BuyCar(g, id, paint, price, d, *sheet, BuildScratch{scratch.data()});
    std::printf("buy %s at %d cr: %d\n", argv[4], price, r);
    if (r != 1) return 1;
    if (g.currentCar < 0) SelectCar(g, int16_t(g.count - 1)); // 0x8001DDAC case 0: the first car becomes current
    PrintCarSummary(vol, d, g.cars[g.count - 1], "bought");
    f.Write(argv[5]);
    return 0;
}

// career-part <save> <car index> <part kind (hex)> <out> [--buy] [--remove] [--body N] [--paint c]: buys (0x80017C98)
// and / or fits (0x80017D6C through the car's sheet 0x800173E8) a part, or puts its kind back to stage 0; prints the
// configuration and record differences.
int CmdCareerPart(const GtfsVolume& vol, const career::CareerData& d, int argc, char** argv) {
    using namespace gt2::career;
    CareerFile f = CareerFile::Load(argv[3]);
    GarageBlock& g = f.save.state.garage;
    const int32_t index = std::atoi(argv[4]);
    const int32_t kind = int32_t(std::strtol(argv[5], nullptr, 16));
    if (index < 0 || index >= g.count) throw std::runtime_error("no garage car " + std::to_string(index));
    bool buy = false, remove = false;
    int16_t body = 1;
    uint32_t paint = g.cars[index].paint;
    for (int i = 7; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--buy") buy = true;
        else if (a == "--remove") remove = true;
        else if (a == "--body" && i + 1 < argc) body = int16_t(std::atoi(argv[++i]));
        else if (a == "--paint" && i + 1 < argc) paint = uint8_t(argv[++i][0]);
    }
    const career::GarageCar before = g.cars[index];
    std::vector<uint8_t> scratch(0x400);
    PrintCarSummary(vol, d, before, "before");
    if (buy) {
        const int32_t price = PartPrice(d.tables, before.carId, kind);
        const int32_t r = BuyPart(g, index, kind, d.tables);
        std::printf("buy part %02X (%d cr): %d (0x80017B40: 1 ok, -8 no such part, -1 money, -3 owned, -5..-7 prerequisite)\n", kind, price, r);
        if (r != 1) return 1;
    }
    int32_t r = 0;
    if (remove) {
        r = RemovePart(g, index, kind, d, BuildScratch{scratch.data()});
        std::printf("remove part %02X (its kind back to stage 0): %d\n", kind, r);
    } else {
        r = FitPartToCar(g, index, kind, paint, body, d, BuildScratch{scratch.data()});
        std::printf("fit part %02X (0x80017D6C): %d (1 fitted, -4 not owned, -8 no such part)\n", kind, r);
    }
    if (r != 1) return 1;
    const career::GarageCar& after = g.cars[index];
    PrintCarSummary(vol, d, after, "after");
    for (const std::string& line : DiffCarConfig(before.config, after.config)) std::printf("  config %s\n", line.c_str());
    CarConfig cb = before.config, ca = after.config;
    const sim::CarParams rb = BuildCarParams(vol, d.tables, cb), ra = BuildCarParams(vol, d.tables, ca);
    for (const std::string& line : DiffCarParams(rb, ra)) std::printf("  record %s\n", line.c_str());
    f.Write(argv[6]);
    return 0;
}

// career-set <save> <car index> <setting 0..16> <entry> <delta> <out> [--reset]: the settings screen (0x8005FC9C, the
// slider 0x80054D10, 0x8005F9DC; --reset: 0x80060410).
int CmdCareerSet(const GtfsVolume& vol, const career::CareerData& d, int argc, char** argv) {
    using namespace gt2::career;
    CareerFile f = CareerFile::Load(argv[3]);
    GarageBlock& g = f.save.state.garage;
    const int32_t index = std::atoi(argv[4]), setting = std::atoi(argv[5]), entry = std::atoi(argv[6]), delta = std::atoi(argv[7]);
    if (index < 0 || index >= g.count) throw std::runtime_error("no garage car " + std::to_string(index));
    const bool reset = argc > 9 && std::string(argv[9]) == "--reset";
    const career::GarageCar before = g.cars[index];
    std::vector<uint8_t> scratch(0x400);
    if (reset) {
        ResetSetting(g, index, setting, d, BuildScratch{scratch.data()});
        std::printf("setting %d reset (0x80060410)\n", setting);
    } else {
        const int32_t v = AdjustSetting(g, index, setting, entry, delta, d, BuildScratch{scratch.data()});
        std::printf("setting %d entry %d %+d: %d%s\n", setting, entry, delta, v, v < 0 ? " (not available with the car's parts)" : "");
        if (v < 0) return 1;
    }
    for (const std::string& line : DiffCarConfig(before.config, g.cars[index].config)) std::printf("  config %s\n", line.c_str());
    CarConfig cb = before.config, ca = g.cars[index].config;
    const sim::CarParams rb = BuildCarParams(vol, d.tables, cb), ra = BuildCarParams(vol, d.tables, ca);
    for (const std::string& line : DiffCarParams(rb, ra)) std::printf("  record %s\n", line.c_str());
    f.Write(argv[8]);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return Usage();
    try {
        std::string cmd = argv[1];
        if (cmd == "exe-map" && argc >= 4) return CmdExeMap(argc, argv);
        if (cmd == "gen-profile") return CmdGenProfile(argc, argv);
        if (cmd == "profile-check" && argc >= 4) return CmdProfileCheck(argc, argv);
        if (cmd == "replay-list") return CmdReplayList(argc, argv);
        DiscImage disc(argv[2]);
        GtfsVolume vol(disc);
        if (cmd == "ls") return CmdLs(vol, argc > 3 ? argv[3] : "");
        if (cmd == "vol-index") return CmdVolIndex(vol, argc, argv);
        if (cmd == "param-scan") return CmdParamScan(vol, argc, argv);
        if (cmd == "arcade-entries") return CmdArcadeEntries(disc, vol, argc, argv);
        if (cmd == "str-info") return CmdStrInfo(disc, argc, argv);
        if (cmd == "replay-add") return CmdReplayAdd(disc, vol, argc, argv);
        if (cmd == "str-export") return CmdStrExport(disc, argc, argv);
        if (cmd == "car-info" && argc == 4) return CmdCarInfo(vol, argv[3]);
        if (cmd == "car-scan") return CmdCarScan(vol);
        if (cmd == "car-gltf" && argc >= 5) return CmdCarGltf(vol, argc, argv);
        if (cmd == "track-scan") return CmdTrackScan(vol);
        if (cmd == "track-preview" && argc >= 5) return CmdTrackPreview(vol, argc, argv);
        if (cmd == "car-preview" && argc >= 5) return CmdCarPreview(vol, argc, argv);
        if (cmd == "export-cars" && argc >= 4) return CmdExportCars(vol, argc, argv);
        if (cmd == "import-car" && argc == 4) return CmdImportCar(vol, argv[3]);
        if (cmd == "export-tracks" && argc >= 4) return gt2tool::CmdExportTracks(vol, argc, argv);
        if (cmd == "import-track" && argc == 4) return gt2tool::CmdImportTrack(vol, argc, argv);
        if (cmd == "used-cars") return CmdUsedCars(vol, argc, argv);
        if (cmd == "events") return CmdEvents(vol, argc, argv);
        if (cmd == "car-prices") return CmdCarPrices(vol);
        if (cmd == "menu-data") return CmdMenuData(vol);
        if (cmd == "menu-page" && argc >= 4) return CmdMenuPage(disc, vol, argc, argv);
        if (cmd == "menu-dump" && argc >= 4) return CmdMenuDump(disc, vol, argc, argv);
        if (cmd == "save-info" && argc == 4) return CmdSaveInfo(vol, argv[3]);
        if (cmd == "career-new" && argc >= 4) return CmdCareerNew(disc, argv[3], argc > 4 ? argv[4] : "");
        if (cmd.rfind("career-", 0) == 0) {
            const career::CareerData d = career::CareerData::Load(disc, vol);
            if (cmd == "career-cars" && argc == 4) return CmdCareerCars(vol, d, argv[3]);
            if (cmd == "career-buy" && argc >= 6) return CmdCareerBuy(vol, d, argc, argv);
            if (cmd == "career-part" && argc >= 7) return CmdCareerPart(vol, d, argc, argv);
            if (cmd == "career-set" && argc >= 9) return CmdCareerSet(vol, d, argc, argv);
        }
        if (cmd == "extract" && argc == 5) {
            // One VOL file (gzip members inflated) -> a local file; for inspection under work\ only.
            const std::vector<uint8_t> data = vol.Read(std::string(argv[3]));
            std::FILE* f = std::fopen(argv[4], "wb");
            if (!f) throw std::runtime_error(std::string("cannot write ") + argv[4]);
            std::fwrite(data.data(), 1, data.size(), f);
            std::fclose(f);
            std::printf("%s: %zu bytes -> %s\n", argv[3], data.size(), argv[4]);
            return 0;
        }
        return Usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
