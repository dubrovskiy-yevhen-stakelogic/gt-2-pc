// gt4tool: dump / convert GT4 car assets (CAR4, Pat0, MDLS, Tex1) with the gt4formats library, and produce the
// canonical digests that are compared against scripts/gt4_car4.py (two independent implementations).
//
//   gt4tool info <file>
//   gt4tool tex <file> <out dir> [--offset N]           Tex1 -> PNG (all textures, all CLUT variants)
//   gt4tool obj <file> <out dir> [--color C] [--pat P] [--lod L]   CAR4 parts / MDLS model 0 -> OBJ + MTL + PNG
//   gt4tool digest <vol root> <out.tsv> [--jobs N]      canonical SHA-256 per file (same TSV as the Python scout)
//   gt4tool compare <a.tsv> <b.tsv>
//   gt4tool check <vol root> <python> <scripts/gt4_car4.py> <out dir>   both digests + compare (ctest)
//
// Outputs belong under work\ (game data never goes into the repository).
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "gt4formats/car4.h"

namespace fs = std::filesystem;
using namespace gt4fmt;

namespace {

std::vector<std::uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------- PNG (stored deflate blocks, no zlib needed)
std::uint32_t crc32(const std::uint8_t* p, std::size_t n, std::uint32_t crc = 0) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void writePng(const fs::path& path, const Image& img) {
    std::vector<std::uint8_t> raw;
    raw.reserve((img.w * 4 + 1) * img.h);
    for (std::uint32_t y = 0; y < img.h; ++y) {
        raw.push_back(0);
        for (std::uint32_t x = 0; x < img.w; ++x) {
            std::uint32_t v = img.rgba[y * img.w + x];
            std::uint32_t a = std::min<std::uint32_t>(255, (v >> 24) * 2);
            raw.push_back(static_cast<std::uint8_t>(v));
            raw.push_back(static_cast<std::uint8_t>(v >> 8));
            raw.push_back(static_cast<std::uint8_t>(v >> 16));
            raw.push_back(static_cast<std::uint8_t>(a));
        }
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    std::uint32_t s1 = 1, s2 = 0;
    for (std::uint8_t c : raw) {
        s1 = (s1 + c) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    for (std::size_t pos = 0; pos < raw.size() || pos == 0;) {
        std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n));
        z.push_back(static_cast<std::uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        if (last) break;
    }
    std::uint32_t ad = (s2 << 16) | s1;
    for (int i = 3; i >= 0; --i) z.push_back(static_cast<std::uint8_t>(ad >> (8 * i)));
    std::ofstream f(path, std::ios::binary);
    auto be32 = [&](std::uint32_t v) {
        for (int i = 3; i >= 0; --i) f.put(static_cast<char>(v >> (8 * i)));
    };
    auto chunk = [&](const char* type, const std::vector<std::uint8_t>& d) {
        be32(static_cast<std::uint32_t>(d.size()));
        std::vector<std::uint8_t> td(type, type + 4);
        td.insert(td.end(), d.begin(), d.end());
        f.write(reinterpret_cast<const char*>(td.data()), static_cast<std::streamsize>(td.size()));
        be32(crc32(td.data(), td.size()));
    };
    f.write("\x89PNG\r\n\x1a\n", 8);
    std::vector<std::uint8_t> ihdr;
    for (std::uint32_t v : {img.w, img.h})
        for (int i = 3; i >= 0; --i) ihdr.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
}

// ---------------------------------------------------------------- info
void describeTex1(const Tex1& t, const std::string& indent) {
    std::printf("%sTex1 size=0x%x blocks=%u textures=%zu transfers=%zu clut_variants=%zu\n", indent.c_str(), t.size,
                t.totalBlocks, t.textures.size(), t.transfers.size(), t.clutPatches.size());
    for (std::size_t i = 0; i < t.textures.size(); ++i) {
        Tex0Fields f = tex0Fields(t.textures[i].tex0);
        std::uint64_t c = t.textures[i].clamp;
        std::printf("%s  tex%zu: psm=0x%02x %ux%u tbp=0x%x tbw=%u cbp=0x%x cpsm=%u csa=%u cld=%u wrap=(%u,%u) max=(%u,%u)\n",
                    indent.c_str(), i, f.psm, 1u << f.tw, 1u << f.th, f.tbp0, f.tbw, f.cbp, f.cpsm, f.csa, f.cld,
                    static_cast<unsigned>(c & 3), static_cast<unsigned>((c >> 2) & 3),
                    static_cast<unsigned>((c >> 14) & 0x3FF), static_cast<unsigned>((c >> 34) & 0x3FF));
    }
}

void describeModelSet(Bytes b, const ModelSet& ms, const std::string& indent) {
    std::uint64_t tv = 0, tt = 0, np = 0;
    for (const Shape& s : ms.shapes) {
        tv += s.totalVerts;
        tt += s.totalTris;
        np += s.packets.size();
    }
    std::printf("%sMDLS size=0x%x models=%zu shapes=%zu materials=%zu texlists=%zux%u variations=%u "
                "vertices=%llu triangles=%llu packets=%llu\n",
                indent.c_str(), ms.size, ms.models.size(), ms.shapeOffsets.size(), ms.materials.size(),
                ms.texsets.size(), ms.nTexsets, ms.nVariations, static_cast<unsigned long long>(tv),
                static_cast<unsigned long long>(tt), static_cast<unsigned long long>(np));
    for (std::size_t i = 0; i < ms.models.size(); ++i) {
        const Model& m = ms.models[i];
        auto draws = drawList(b, ms, static_cast<int>(i));
        std::printf("%s  model%zu: radius=%.3f origin=(%.3f,%.3f,%.3f) commands=%zu lod0 draws=%zu\n", indent.c_str(),
                    i, m.radius, m.origin[0], m.origin[1], m.origin[2], m.commands.size(), draws.size());
    }
}

int cmdInfo(const fs::path& path) {
    auto data = readFile(path);
    Bytes b(data);
    std::string magic(reinterpret_cast<const char*>(data.data()), std::min<std::size_t>(4, data.size()));
    if (magic == "CAR4") {
        Car4 car = parseCar4(b);
        std::printf("CAR4 size=0x%x\n", car.header[2]);
        for (std::uint32_t s = kSlotInfo; s <= kSlotTex38; s += 4)
            if (car.slot(s)) std::printf("  %-10s 0x%x\n", car4SlotName(s), car.slot(s));
        for (auto& [s, p] : car.pats) std::printf("  %s: Pat0 colours=%u entries=%u\n", car4SlotName(s), p.colors, p.entries);
        for (std::uint32_t s : car.mdlsSlots) {
            std::printf("  %s:\n", car4SlotName(s));
            describeModelSet(b, parseModelSet(b, car.slot(s)), "    ");
        }
    } else if (magic == "MDLS") {
        describeModelSet(b, parseModelSet(b, 0), "");
    } else if (magic == "Tex1") {
        describeTex1(parseTex1(b, 0), "");
    } else if (magic == "Pat0") {
        Pat0 p = parsePat0(b, 0);
        std::printf("Pat0 colours=%u entries=%u\n", p.colors, p.entries);
    } else {
        std::fprintf(stderr, "unknown magic\n");
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- tex
int cmdTex(const fs::path& path, const fs::path& outDir, std::size_t offset) {
    auto data = readFile(path);
    Tex1 t = parseTex1(Bytes(data), offset);
    describeTex1(t, "");
    fs::create_directories(outDir);
    GsMemory gs = buildGs(t);
    int n = 0;
    for (std::size_t v = 0; v < t.numVariants(); ++v) {
        if (v < t.clutPatches.size() && !t.clutPatches[v]) continue;
        for (std::size_t i = 0; i < t.textures.size(); ++i) {
            if (v > 0) {
                bool patched = false;
                for (const auto& p : *t.clutPatches[v]) patched |= p.tex == i;
                if (!patched) continue;
            }
            Image img = decodeTexture(gs, t.variantTex0(i, v));
            std::string name = path.filename().string() + "_" + std::to_string(i) + (v ? "_v" + std::to_string(v) : "") + ".png";
            writePng(outDir / name, img);
            ++n;
        }
    }
    std::printf("%d PNG -> %s\n", n, outDir.string().c_str());
    return 0;
}

// ---------------------------------------------------------------- obj
struct ObjStats {
    std::size_t triangles = 0, materials = 0;
};

ObjStats exportObj(Bytes b, const ModelSet& ms, const fs::path& outDir, const std::string& name, int lod) {
    fs::create_directories(outDir / "textures");
    auto draws = drawList(b, ms, 0, lod);
    std::ofstream obj(outDir / (name + ".obj")), mtl(outDir / (name + ".mtl"));
    obj << "# GT4 MDLS export (gt4tool), model 0, lod " << lod << "\nmtllib " << name << ".mtl\n";
    std::set<std::string> mats;
    std::map<int, GsMemory> gsCache;
    std::size_t vbase = 1;
    ObjStats st;
    int boundSet = -1, boundTex = -1;  // FUN_0049cea0: index 0 keeps the previous binding
    for (const Draw& d : draws) {
        const Shape& shape = ms.shapes.at(d.shape);
        for (std::size_t pi = 0; pi < shape.packets.size(); ++pi) {
            const VifPacket& pk = shape.packets[pi];
            if (pk.tex == 0x1FF) {
                boundSet = boundTex = -1;
            } else if (pk.tex != 0 && d.texset >= 0) {
                boundSet = d.texset;
                boundTex = pk.tex - 1;
            }
            bool textured = (pk.gif[1] >> 15) & 0x10;  // PRIM.TME
            std::string mname = "untextured";
            if (textured && boundSet >= 0) mname = "set" + std::to_string(boundSet) + "_tex" + std::to_string(boundTex);
            if (!mats.count(mname)) {
                mats.insert(mname);
                mtl << "newmtl " << mname << "\nKd 1 1 1\n";
                if (mname != "untextured" && !ms.texsets.empty()) {
                    const auto& ts = ms.texsets[0][static_cast<std::size_t>(boundSet)];
                    if (ts && static_cast<std::size_t>(boundTex) < ts->textures.size()) {
                        if (!gsCache.count(boundSet)) gsCache.emplace(boundSet, buildGs(*ts));
                        Image img = decodeTexture(gsCache.at(boundSet), ts->variantTex0(static_cast<std::size_t>(boundTex), 0));
                        std::string fn = name + "_" + mname + ".png";
                        writePng(outDir / "textures" / fn, img);
                        mtl << "map_Kd textures/" << fn << "\nmap_d textures/" << fn << "\n";
                    }
                }
                mtl << "\n";
            }
            std::vector<Vertex> vs;
            bool ok = true, uv = true, nrm = true;
            for (const VertexRaw& r : pk.verts) {
                vs.push_back(interpretVertex(r, shape.scale()));
                ok &= vs.back().hasPos;
                uv &= vs.back().hasUv;
                nrm &= vs.back().hasNrm;
            }
            if (!ok) continue;
            obj << "o shape" << d.shape << "_p" << pi << "\nusemtl " << mname << "\n";
            char line[160];
            for (const Vertex& v : vs) {
                std::snprintf(line, sizeof line, "v %.6f %.6f %.6f\n", v.pos[0], v.pos[1], v.pos[2]);
                obj << line;
            }
            for (const Vertex& v : vs) {
                std::snprintf(line, sizeof line, "vt %.6f %.6f\n", uv ? v.uv[0] : 0.f, uv ? 1.0f - v.uv[1] : 0.f);
                obj << line;
            }
            for (const Vertex& v : vs) {
                float l = nrm ? std::sqrt(v.nrm[0] * v.nrm[0] + v.nrm[1] * v.nrm[1] + v.nrm[2] * v.nrm[2]) : 0.f;
                if (l == 0.f) l = 1.f;
                std::snprintf(line, sizeof line, "vn %.5f %.5f %.5f\n", nrm ? v.nrm[0] / l : 0.f, nrm ? v.nrm[1] / l : 1.f,
                              nrm ? v.nrm[2] / l : 0.f);
                obj << line;
            }
            for (const auto& t : stripTriangles(pk.strips)) {
                std::size_t a = vbase + t[0], bb = vbase + t[1], c = vbase + t[2];
                obj << "f " << a << '/' << a << '/' << a << ' ' << bb << '/' << bb << '/' << bb << ' ' << c << '/' << c
                    << '/' << c << '\n';
                ++st.triangles;
            }
            vbase += vs.size();
        }
    }
    st.materials = mats.size();
    return st;
}

int cmdObj(const fs::path& path, const fs::path& outDir, int color, const fs::path& patPath, int lod) {
    auto data = readFile(path);
    Bytes b(data);
    std::string magic(reinterpret_cast<const char*>(data.data()), std::min<std::size_t>(4, data.size()));
    fs::path dir = outDir / path.filename();
    if (magic == "MDLS") {
        ObjStats st = exportObj(b, parseModelSet(b, 0), dir, "model0", lod);
        std::printf("model0: %zu triangles, %zu materials\n", st.triangles, st.materials);
        return 0;
    }
    if (magic != "CAR4") throw FormatError("obj expects a CAR4 or MDLS file");
    Car4 car = parseCar4(b);
    std::vector<std::uint8_t> patData;
    std::optional<Pat0> extPat;
    fs::path pp = patPath;
    if (color >= 0 && pp.empty() && !car.pats.count(kSlotBodyPat) && fs::exists(path.string() + ".pat"))
        pp = path.string() + ".pat";  // race models keep the body Pat0 in '<name>.pat' (FUN_001553c0)
    if (!pp.empty()) {
        patData = readFile(pp);
        extPat = parsePat0(Bytes(patData), 0);
    }
    for (std::uint32_t s : car.mdlsSlots) {
        const Pat0* pat = nullptr;
        if (s == kSlotBody) pat = extPat ? &*extPat : (car.pats.count(kSlotBodyPat) ? &car.pats.at(kSlotBodyPat) : nullptr);
        if (s == kSlotWheel && car.pats.count(kSlotWheelPat)) pat = &car.pats.at(kSlotWheelPat);
        std::string tag = car4SlotName(s);
        ObjStats st;
        if (color >= 0 && pat) {
            std::vector<std::uint8_t> image = data;  // Pat0 patches the MDLS image (FUN_00458290)
            std::uint32_t off = car.slot(s);
            pat->apply(image, off, rd32(b, off + 0x10), color);
            tag += "_c" + std::to_string(color);
            Bytes pb(image);
            st = exportObj(pb, parseModelSet(pb, off), dir, tag, lod);
        } else {
            st = exportObj(b, parseModelSet(b, car.slot(s)), dir, tag, lod);
        }
        std::printf("%s: %zu triangles, %zu materials\n", tag.c_str(), st.triangles, st.materials);
    }
    return 0;
}

// ---------------------------------------------------------------- digest / compare
std::string tsvLine(const std::string& rel, const FileDigest& d) {
    const DigestStats& s = d.stats;
    std::ostringstream o;
    o << rel << '\t' << d.kind << '\t' << d.digest << '\t' << s.models << '\t' << s.shapes << '\t' << s.packets << '\t'
      << s.vertices << '\t' << s.triangles << '\t' << s.texsets << '\t' << s.textures << '\t' << s.texels << '\t'
      << s.texFail << '\t' << s.pats << '\t' << s.badClutSets << '\t' << d.error;
    return o.str();
}

int cmdDigest(const fs::path& root, const fs::path& out, unsigned jobs) {
    std::vector<fs::path> files;
    for (auto& e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file()) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    std::vector<std::string> lines(files.size());
    std::vector<char> used(files.size(), 0);
    std::atomic<std::size_t> next{0};
    auto worker = [&]() {
        for (std::size_t i; (i = next++) < files.size();) {
            std::vector<std::uint8_t> head(4);
            {
                std::ifstream f(files[i], std::ios::binary);
                f.read(reinterpret_cast<char*>(head.data()), 4);
                if (f.gcount() < 4) continue;
            }
            std::string m(head.begin(), head.end());
            if (m != "CAR4" && m != "MDLS" && m != "Tex1" && m != "Pat0") continue;
            auto data = readFile(files[i]);
            auto d = digestFile(Bytes(data));
            if (!d) continue;
            lines[i] = tsvLine(fs::relative(files[i], root).generic_string(), *d);
            used[i] = 1;
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < jobs; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    fs::create_directories(out.parent_path().empty() ? fs::path(".") : out.parent_path());
    std::ofstream f(out, std::ios::binary);
    std::map<std::string, std::array<std::uint64_t, 4>> kinds;  // files, failed, textures, triangles
    std::size_t n = 0, failed = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (!used[i]) continue;
        f << lines[i] << '\n';
        ++n;
        std::istringstream ls(lines[i]);
        std::string rel, kind, dg;
        std::getline(ls, rel, '\t');
        std::getline(ls, kind, '\t');
        std::getline(ls, dg, '\t');
        kinds[kind][0]++;
        if (dg == "ERROR") {
            kinds[kind][1]++;
            ++failed;
            std::printf("FAIL %s\n", lines[i].c_str());
        }
    }
    for (auto& [k, v] : kinds)
        std::printf("%s: %llu files, %llu failed\n", k.c_str(), static_cast<unsigned long long>(v[0]),
                    static_cast<unsigned long long>(v[1]));
    std::printf("%zu files, %zu failed -> %s\n", n, failed, out.string().c_str());
    return failed ? 1 : 0;
}

std::map<std::string, std::string> readTsv(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::map<std::string, std::string> m;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        m[line.substr(0, line.find('\t'))] = line;
    }
    return m;
}

int cmdCompare(const fs::path& a, const fs::path& b) {
    auto ma = readTsv(a), mb = readTsv(b);
    std::size_t same = 0, diff = 0, onlyA = 0, onlyB = 0;
    std::vector<std::string> shown;
    for (auto& [k, v] : ma) {
        auto it = mb.find(k);
        if (it == mb.end()) {
            ++onlyA;
            shown.push_back("only in first: " + v);
        } else if (it->second == v) {
            ++same;
        } else {
            ++diff;
            shown.push_back("differs: " + v + "\n         " + it->second);
        }
    }
    for (auto& [k, v] : mb)
        if (!ma.count(k)) {
            ++onlyB;
            shown.push_back("only in second: " + v);
        }
    std::printf("%zu vs %zu lines: %zu identical, %zu differ, %zu only in first, %zu only in second\n", ma.size(),
                mb.size(), same, diff, onlyA, onlyB);
    for (std::size_t i = 0; i < shown.size() && i < 20; ++i) std::printf("  %s\n", shown[i].c_str());
    return (diff || onlyA || onlyB) ? 1 : 0;
}

// Runs the Python scout digest, the C++ digest, and compares them (ctest car_agreement).
int cmdCheck(const fs::path& root, const std::string& python, const fs::path& script, const fs::path& outDir) {
    fs::create_directories(outDir);
    fs::path py = outDir / "car_digest_py.tsv", cpp = outDir / "car_digest_cpp.tsv";
    std::string cmd = "\"\"" + python + "\" \"" + script.string() + "\" digest \"" + root.string() + "\" \"" +
                      py.string() + "\"\"";
    if (std::system(cmd.c_str()) != 0) {
        std::fprintf(stderr, "python digest failed\n");
        return 1;
    }
    int rc = cmdDigest(root, cpp, std::max(1u, std::thread::hardware_concurrency()));
    return cmdCompare(py, cpp) | rc;
}

int usage() {
    std::fprintf(stderr,
                 "usage: gt4tool info <file>\n"
                 "       gt4tool tex <file> <out dir> [--offset N]\n"
                 "       gt4tool obj <file> <out dir> [--color C] [--pat P] [--lod L]\n"
                 "       gt4tool digest <vol root> <out.tsv> [--jobs N]\n"
                 "       gt4tool compare <a.tsv> <b.tsv>\n"
                 "       gt4tool check <vol root> <python> <scripts/gt4_car4.py> <out dir>\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1];
    std::vector<std::string> pos;
    std::map<std::string, std::string> opt;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) == 0 && i + 1 < argc) opt[a] = argv[++i];
        else pos.push_back(a);
    }
    try {
        if (cmd == "info" && pos.size() == 1) return cmdInfo(pos[0]);
        if (cmd == "tex" && pos.size() == 2)
            return cmdTex(pos[0], pos[1], opt.count("--offset") ? std::stoull(opt["--offset"], nullptr, 0) : 0);
        if (cmd == "obj" && pos.size() == 2)
            return cmdObj(pos[0], pos[1], opt.count("--color") ? std::stoi(opt["--color"]) : -1,
                          opt.count("--pat") ? fs::path(opt["--pat"]) : fs::path(),
                          opt.count("--lod") ? std::stoi(opt["--lod"]) : 0);
        if (cmd == "digest" && pos.size() == 2) {
            unsigned hw = std::max(1u, std::thread::hardware_concurrency());
            return cmdDigest(pos[0], pos[1], opt.count("--jobs") ? static_cast<unsigned>(std::stoul(opt["--jobs"])) : hw);
        }
        if (cmd == "compare" && pos.size() == 2) return cmdCompare(pos[0], pos[1]);
        if (cmd == "check" && pos.size() == 4) return cmdCheck(pos[0], pos[1], pos[2], pos[3]);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return usage();
}
