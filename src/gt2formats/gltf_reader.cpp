#include "gt2formats/gltf_reader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>

#include "gt2formats/json.h"

namespace gt2 {
namespace {

using json::Value;

std::vector<uint8_t> ReadBinaryFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("gltf: cannot open " + path);
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    return data;
}

std::vector<uint8_t> DecodeBase64(std::string_view text) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const int v = value(c);
        if (v < 0) throw std::runtime_error("gltf: bad base64 data URI");
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t(acc >> bits));
        }
    }
    return out;
}

// Resolves a glTF uri: data URIs are decoded, everything else is a path relative to the .gltf.
std::vector<uint8_t> LoadUri(const std::string& uri, const std::filesystem::path& baseDir) {
    if (uri.rfind("data:", 0) == 0) {
        const size_t comma = uri.find(',');
        if (comma == std::string::npos) throw std::runtime_error("gltf: bad data URI");
        if (uri.find(";base64", 0) == std::string::npos || uri.find(";base64") > comma) throw std::runtime_error("gltf: only base64 data URIs are supported");
        return DecodeBase64(std::string_view(uri).substr(comma + 1));
    }
    // Percent-decoding of the few characters exporters escape (spaces).
    std::string path;
    for (size_t i = 0; i < uri.size(); i++) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            path.push_back(char(std::strtol(uri.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else {
            path.push_back(uri[i]);
        }
    }
    return ReadBinaryFile((baseDir / std::filesystem::path(path)).string());
}

using Mat4 = std::array<float, 16>; // column-major

Mat4 Identity() {
    Mat4 m{};
    m[0] = m[5] = m[10] = m[15] = 1;
    return m;
}

Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[size_t(k * 4 + row)] * b[size_t(c * 4 + k)];
            r[size_t(c * 4 + row)] = s;
        }
    return r;
}

Mat4 NodeLocalMatrix(const Value& node) {
    if (const Value* m = node.Get("matrix")) {
        if (m->Size() != 16) throw std::runtime_error("gltf: node matrix must have 16 elements");
        Mat4 out;
        for (size_t i = 0; i < 16; i++) out[i] = float(m->At(i).AsDouble());
        return out;
    }
    float t[3] = {0, 0, 0}, s[3] = {1, 1, 1}, q[4] = {0, 0, 0, 1};
    if (const Value* v = node.Get("translation")) for (size_t i = 0; i < 3; i++) t[i] = float(v->At(i).AsDouble());
    if (const Value* v = node.Get("scale")) for (size_t i = 0; i < 3; i++) s[i] = float(v->At(i).AsDouble());
    if (const Value* v = node.Get("rotation")) for (size_t i = 0; i < 4; i++) q[i] = float(v->At(i).AsDouble());
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    Mat4 m = Identity();
    m[0] = (1 - 2 * (y * y + z * z)) * s[0];
    m[1] = (2 * (x * y + z * w)) * s[0];
    m[2] = (2 * (x * z - y * w)) * s[0];
    m[4] = (2 * (x * y - z * w)) * s[1];
    m[5] = (1 - 2 * (x * x + z * z)) * s[1];
    m[6] = (2 * (y * z + x * w)) * s[1];
    m[8] = (2 * (x * z + y * w)) * s[2];
    m[9] = (2 * (y * z - x * w)) * s[2];
    m[10] = (1 - 2 * (x * x + y * y)) * s[2];
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    return m;
}

struct Reader {
    Value doc;
    std::filesystem::path baseDir;
    std::vector<std::vector<uint8_t>> buffers;
    GltfMesh out;

    const Value& Item(const char* array, int64_t index) const {
        const Value* arr = doc.Get(array);
        if (!arr || !arr->IsArray() || index < 0 || size_t(index) >= arr->Size()) throw std::runtime_error(std::string("gltf: bad ") + array + " index " + std::to_string(index));
        return arr->At(size_t(index));
    }

    // Returns the bytes of an accessor as (pointer to the first element, byte stride, count, component type, component count).
    struct AccessorView {
        const uint8_t* data = nullptr;
        size_t stride = 0, count = 0;
        int componentType = 0, components = 0;
        bool normalized = false;
    };

    AccessorView View(int64_t accessorIndex) const {
        const Value& acc = Item("accessors", accessorIndex);
        if (acc.Get("sparse")) throw std::runtime_error("gltf: sparse accessors are not supported");
        AccessorView v;
        v.count = size_t(acc.IntAt("count"));
        v.componentType = int(acc.IntAt("componentType"));
        v.normalized = acc.BoolOr("normalized", false);
        const std::string& type = acc.Require("type").AsString();
        v.components = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : type == "MAT4" ? 16 : 0;
        if (v.components == 0) throw std::runtime_error("gltf: unsupported accessor type " + type);
        const size_t componentSize = v.componentType == 5126 || v.componentType == 5125 ? 4 : v.componentType == 5123 || v.componentType == 5122 ? 2 : 1;
        const size_t elementSize = componentSize * size_t(v.components);
        const Value* bv = acc.Get("bufferView");
        if (!bv) throw std::runtime_error("gltf: accessors without a bufferView are not supported");
        const Value& view = Item("bufferViews", bv->AsInt());
        const std::vector<uint8_t>& buffer = buffers.at(size_t(view.IntAt("buffer")));
        const size_t viewOffset = size_t(view.IntOr("byteOffset", 0)), viewLength = size_t(view.IntAt("byteLength"));
        v.stride = size_t(view.IntOr("byteStride", 0));
        if (v.stride == 0) v.stride = elementSize;
        const size_t accOffset = size_t(acc.IntOr("byteOffset", 0));
        if (viewOffset + viewLength > buffer.size()) throw std::runtime_error("gltf: bufferView out of the buffer");
        if (v.count && accOffset + (v.count - 1) * v.stride + elementSize > viewLength) throw std::runtime_error("gltf: accessor out of its bufferView");
        v.data = buffer.data() + viewOffset + accOffset;
        return v;
    }

    static float Component(const AccessorView& v, const uint8_t* p, int i) {
        switch (v.componentType) {
        case 5126: { float f; std::memcpy(&f, p + i * 4, 4); return f; }
        case 5125: { uint32_t u; std::memcpy(&u, p + i * 4, 4); return float(u); }
        case 5123: { uint16_t u; std::memcpy(&u, p + i * 2, 2); return v.normalized ? u / 65535.0f : float(u); }
        case 5122: { int16_t s; std::memcpy(&s, p + i * 2, 2); return v.normalized ? std::max(s / 32767.0f, -1.0f) : float(s); }
        case 5121: return v.normalized ? p[i] / 255.0f : float(p[i]);
        case 5120: { const int8_t s = int8_t(p[i]); return v.normalized ? std::max(s / 127.0f, -1.0f) : float(s); }
        default: throw std::runtime_error("gltf: bad componentType");
        }
    }

    std::vector<float> Floats(int64_t accessorIndex, int wantComponents) const {
        const AccessorView v = View(accessorIndex);
        std::vector<float> values(v.count * size_t(wantComponents), 0.0f);
        for (size_t e = 0; e < v.count; e++)
            for (int c = 0; c < wantComponents && c < v.components; c++) values[e * size_t(wantComponents) + size_t(c)] = Component(v, v.data + e * v.stride, c);
        return values;
    }

    std::vector<uint32_t> Indices(int64_t accessorIndex) const {
        const AccessorView v = View(accessorIndex);
        if (v.components != 1) throw std::runtime_error("gltf: index accessor must be SCALAR");
        std::vector<uint32_t> indices(v.count);
        for (size_t e = 0; e < v.count; e++) {
            const uint8_t* p = v.data + e * v.stride;
            if (v.componentType == 5125) std::memcpy(&indices[e], p, 4);
            else if (v.componentType == 5123) { uint16_t u; std::memcpy(&u, p, 2); indices[e] = u; }
            else if (v.componentType == 5121) indices[e] = p[0];
            else throw std::runtime_error("gltf: bad index componentType");
        }
        return indices;
    }

    void LoadBuffers(const std::vector<uint8_t>* glbBin) {
        if (const Value* arr = doc.Get("buffers"))
            for (size_t i = 0; i < arr->Size(); i++) {
                const Value& b = arr->At(i);
                if (const Value* uri = b.Get("uri")) buffers.push_back(LoadUri(uri->AsString(), baseDir));
                else if (glbBin && i == 0) buffers.push_back(*glbBin);
                else throw std::runtime_error("gltf: buffer without uri");
                if (buffers.back().size() < size_t(b.IntAt("byteLength"))) throw std::runtime_error("gltf: buffer shorter than byteLength");
            }
    }

    void LoadImages(const std::vector<uint8_t>* glbBin) {
        (void)glbBin;
        if (const Value* arr = doc.Get("images"))
            for (size_t i = 0; i < arr->Size(); i++) {
                const Value& img = arr->At(i);
                PngImage decoded;
                try {
                    std::vector<uint8_t> bytes;
                    if (const Value* uri = img.Get("uri")) bytes = LoadUri(uri->AsString(), baseDir);
                    else if (const Value* bv = img.Get("bufferView")) {
                        const Value& view = Item("bufferViews", bv->AsInt());
                        const std::vector<uint8_t>& buffer = buffers.at(size_t(view.IntAt("buffer")));
                        const size_t off = size_t(view.IntOr("byteOffset", 0)), len = size_t(view.IntAt("byteLength"));
                        if (off + len > buffer.size()) throw std::runtime_error("image bufferView out of the buffer");
                        bytes.assign(buffer.begin() + std::ptrdiff_t(off), buffer.begin() + std::ptrdiff_t(off + len));
                    } else throw std::runtime_error("image without uri or bufferView");
                    decoded = DecodePng(bytes);
                } catch (const std::exception& e) {
                    out.warnings.push_back("image " + std::to_string(i) + ": " + e.what());
                }
                out.images.push_back(std::move(decoded));
            }
    }

    void LoadMaterials() {
        if (const Value* arr = doc.Get("materials"))
            for (size_t i = 0; i < arr->Size(); i++) {
                const Value& m = arr->At(i);
                GltfMaterial mat;
                mat.name = m.StringOr("name", "");
                mat.doubleSided = m.BoolOr("doubleSided", false);
                if (const Value* extras = m.Get("extras"))
                    if (const Value* g = extras->IsObject() ? extras->Get("gt2pc") : nullptr)
                        if (g->IsObject()) mat.reflection = g->BoolOr("reflection", false);
                if (const Value* pbr = m.Get("pbrMetallicRoughness")) {
                    if (const Value* f = pbr->Get("baseColorFactor"))
                        for (size_t k = 0; k < 4 && k < f->Size(); k++) mat.baseColorFactor[k] = float(f->At(k).AsDouble());
                    if (const Value* t = pbr->Get("baseColorTexture")) {
                        const Value& tex = Item("textures", t->IntAt("index"));
                        if (const Value* src = tex.Get("source")) {
                            const int64_t image = src->AsInt();
                            if (image >= 0 && size_t(image) < out.images.size()) mat.baseColorImage = int(image);
                        }
                    }
                }
                out.materials.push_back(mat);
            }
    }

    void AddPrimitive(const Value& prim, const Mat4& world) {
        const int mode = int(prim.IntOr("mode", 4));
        if (mode != 4 && mode != 5 && mode != 6) { out.warnings.push_back("primitive with mode " + std::to_string(mode) + " skipped (not triangles)"); return; }
        const Value& attrs = prim.Require("attributes");
        const Value* pos = attrs.Get("POSITION");
        if (!pos) { out.warnings.push_back("primitive without POSITION skipped"); return; }
        GltfPrimitive p;
        p.material = int(prim.IntOr("material", -1));
        const std::vector<float> positions = Floats(pos->AsInt(), 3);
        const size_t count = positions.size() / 3;
        std::vector<float> normals, uvs, colors;
        if (const Value* a = attrs.Get("NORMAL")) { normals = Floats(a->AsInt(), 3); p.hasNormals = normals.size() == count * 3; }
        if (const Value* a = attrs.Get("TEXCOORD_0")) { uvs = Floats(a->AsInt(), 2); p.hasUvs = uvs.size() == count * 2; }
        if (const Value* a = attrs.Get("COLOR_0")) {
            const AccessorView v = View(a->AsInt());
            colors = Floats(a->AsInt(), 4);
            if (v.components == 3) for (size_t i = 0; i < count; i++) colors[i * 4 + 3] = 1.0f;
            p.hasColors = colors.size() == count * 4;
        }
        // Normal matrix: the upper 3x3 of the world matrix (normals are re-normalised; non-uniform scale is rare in car meshes).
        p.vertices.resize(count);
        for (size_t i = 0; i < count; i++) {
            GltfVertex& v = p.vertices[i];
            const float x = positions[i * 3], y = positions[i * 3 + 1], z = positions[i * 3 + 2];
            for (size_t r = 0; r < 3; r++) v.position[r] = world[r] * x + world[4 + r] * y + world[8 + r] * z + world[12 + r];
            if (p.hasNormals) {
                const float nx = normals[i * 3], ny = normals[i * 3 + 1], nz = normals[i * 3 + 2];
                std::array<float, 3> n;
                for (size_t r = 0; r < 3; r++) n[r] = world[r] * nx + world[4 + r] * ny + world[8 + r] * nz;
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                v.normal = len > 1e-12f ? std::array<float, 3>{n[0] / len, n[1] / len, n[2] / len} : std::array<float, 3>{0, 1, 0};
            }
            if (p.hasUvs) v.uv = {uvs[i * 2], uvs[i * 2 + 1]};
            if (p.hasColors) v.color = {colors[i * 4], colors[i * 4 + 1], colors[i * 4 + 2], colors[i * 4 + 3]};
        }
        std::vector<uint32_t> raw;
        if (const Value* idx = prim.Get("indices")) raw = Indices(idx->AsInt());
        else { raw.resize(count); for (size_t i = 0; i < count; i++) raw[i] = uint32_t(i); }
        for (const uint32_t i : raw) if (i >= count) throw std::runtime_error("gltf: index out of range");
        if (mode == 4) {
            raw.resize(raw.size() - raw.size() % 3);
            p.indices = std::move(raw);
        } else if (mode == 5) { // strip
            for (size_t i = 2; i < raw.size(); i++) {
                if (i % 2 == 0) p.indices.insert(p.indices.end(), {raw[i - 2], raw[i - 1], raw[i]});
                else p.indices.insert(p.indices.end(), {raw[i - 1], raw[i - 2], raw[i]});
            }
        } else { // fan
            for (size_t i = 2; i < raw.size(); i++) p.indices.insert(p.indices.end(), {raw[0], raw[i - 1], raw[i]});
        }
        if (!p.indices.empty()) out.primitives.push_back(std::move(p));
    }

    void Visit(int64_t nodeIndex, const Mat4& parent, int depth) {
        if (depth > 64) throw std::runtime_error("gltf: node hierarchy too deep (cycle?)");
        const Value& node = Item("nodes", nodeIndex);
        const Mat4 world = Multiply(parent, NodeLocalMatrix(node));
        if (const Value* mesh = node.Get("mesh")) {
            const Value& m = Item("meshes", mesh->AsInt());
            if (const Value* prims = m.Get("primitives"))
                for (size_t i = 0; i < prims->Size(); i++) AddPrimitive(prims->At(i), world);
        }
        if (const Value* children = node.Get("children"))
            for (size_t i = 0; i < children->Size(); i++) Visit(children->At(i).AsInt(), world, depth + 1);
    }

    void Run(const std::vector<uint8_t>* glbBin) {
        if (const Value* asset = doc.Get("asset")) {
            const std::string version = asset->StringOr("version", "");
            if (version.rfind("2.", 0) != 0) throw std::runtime_error("gltf: unsupported asset version \"" + version + "\" (need 2.x)");
        }
        LoadBuffers(glbBin);
        LoadImages(glbBin);
        LoadMaterials();
        const Value* scenes = doc.Get("scenes");
        if (scenes && scenes->Size() > 0) {
            const Value& scene = scenes->At(size_t(doc.IntOr("scene", 0)));
            if (const Value* nodes = scene.Get("nodes"))
                for (size_t i = 0; i < nodes->Size(); i++) Visit(nodes->At(i).AsInt(), Identity(), 0);
        } else if (const Value* nodes = doc.Get("nodes")) {
            for (size_t i = 0; i < nodes->Size(); i++) Visit(int64_t(i), Identity(), 0); // no scene: every node is a root
        }
        out.boundsMin = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        out.boundsMax = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
        for (const GltfPrimitive& p : out.primitives)
            for (const GltfVertex& v : p.vertices)
                for (size_t k = 0; k < 3; k++) {
                    out.boundsMin[k] = std::min(out.boundsMin[k], v.position[k]);
                    out.boundsMax[k] = std::max(out.boundsMax[k], v.position[k]);
                }
        if (out.primitives.empty()) { out.boundsMin = {}; out.boundsMax = {}; }
    }
};

} // namespace

size_t GltfMesh::TriangleCount() const {
    size_t n = 0;
    for (const GltfPrimitive& p : primitives) n += p.indices.size() / 3;
    return n;
}

GltfMesh ReadGltf(const std::string& path) {
    Reader r;
    r.baseDir = std::filesystem::path(path).parent_path();
    const std::vector<uint8_t> file = ReadBinaryFile(path);
    std::vector<uint8_t> glbBin;
    const std::vector<uint8_t>* bin = nullptr;
    if (file.size() >= 12 && std::memcmp(file.data(), "glTF", 4) == 0) {
        auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &file[o], 4); return v; };
        if (u32(4) != 2) throw std::runtime_error("gltf: unsupported glb version");
        std::string jsonText;
        for (size_t pos = 12; pos + 8 <= file.size();) {
            const uint32_t length = u32(pos), type = u32(pos + 4);
            if (pos + 8 + length > file.size()) throw std::runtime_error("gltf: truncated glb chunk");
            if (type == 0x4E4F534Au) jsonText.assign(reinterpret_cast<const char*>(&file[pos + 8]), length);
            else if (type == 0x004E4942u) { glbBin.assign(file.begin() + std::ptrdiff_t(pos + 8), file.begin() + std::ptrdiff_t(pos + 8 + length)); bin = &glbBin; }
            pos += 8 + length;
        }
        if (jsonText.empty()) throw std::runtime_error("gltf: glb without a JSON chunk");
        r.doc = json::Parse(jsonText);
    } else {
        r.doc = json::Parse(std::string_view(reinterpret_cast<const char*>(file.data()), file.size()));
    }
    try {
        r.Run(bin);
    } catch (const std::exception& e) {
        throw std::runtime_error(path + ": " + e.what());
    }
    return std::move(r.out);
}

} // namespace gt2
