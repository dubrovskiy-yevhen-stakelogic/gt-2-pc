#include "gt2formats/track_json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

#include "gt2formats/backdrop.h"
#include "gt2formats/gltf_reader.h"
#include "gt2formats/json.h"
#include "gt2vfs/gtfs.h"

namespace gt2 {
namespace {

using json::Value;

constexpr const char* kFormat = "gt2pc-track";
constexpr int kVersion = 1;
constexpr int64_t kCellUnits = 4096; // one 64 m cell in 1/64 m

// ---------------------------------------------------------------- units

// 16.16 fixed-point metres <-> decimal metres (exact: a 16.16 value is a dyadic rational, the writer prints doubles
// with round-trip precision).
Value Metres16(int32_t v) { return Value::Double(v / 65536.0); }
// 1/4096 m <-> decimal metres.
Value Metres12(int32_t v) { return Value::Double(v / 4096.0); }
// Angles: 4096 = one turn <-> degrees (360 / 4096 = 45 / 512, exact).
Value Degrees(int32_t a) { return Value::Double(a * (360.0 / 4096.0)); }

[[noreturn]] void Fail(const std::string& where, const std::string& what) { throw std::runtime_error(where + ": " + what); }

int64_t RoundChecked(double d, int64_t lo, int64_t hi, const std::string& where) {
    if (!std::isfinite(d)) Fail(where, "not a finite number");
    const double r = std::nearbyint(d);
    if (r < double(lo) || r > double(hi)) Fail(where, "value " + std::to_string(d) + " out of range");
    return int64_t(r);
}
int32_t FromMetres16(const Value& v, const std::string& where) {
    return int32_t(RoundChecked(v.AsDouble() * 65536.0, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max(), where));
}
int32_t FromMetres12(const Value& v, const std::string& where) {
    return int32_t(RoundChecked(v.AsDouble() * 4096.0, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max(), where));
}
int16_t FromDegrees16(const Value& v, const std::string& where) {
    return int16_t(RoundChecked(v.AsDouble() * (4096.0 / 360.0), -32768, 32767, where));
}
int64_t IntIn(const Value& v, int64_t lo, int64_t hi, const std::string& where) {
    if (!v.IsNumber()) Fail(where, "expected a number");
    const int64_t i = v.IsInt() ? v.AsInt() : RoundChecked(v.AsDouble(), lo, hi, where);
    if (v.IsInt() == false && double(i) != v.AsDouble()) Fail(where, "expected an integer");
    if (i < lo || i > hi) Fail(where, "value " + std::to_string(i) + " out of range " + std::to_string(lo) + ".." + std::to_string(hi));
    return i;
}
const Value& Req(const Value& obj, const char* key, const std::string& where) {
    const Value* v = obj.Get(key);
    if (!v) Fail(where, std::string("missing key \"") + key + "\"");
    return *v;
}
const Value& ArrayOf(const Value& v, const std::string& where, size_t minSize = 0) {
    if (!v.IsArray()) Fail(where, "expected an array");
    if (v.Size() < minSize) Fail(where, "array too short");
    return v;
}

std::string Hex(std::span<const uint8_t> b) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) { s.push_back(k[c >> 4]); s.push_back(k[c & 15]); }
    return s;
}
std::vector<uint8_t> FromHex(const Value& v, const std::string& where) {
    if (!v.IsString()) Fail(where, "expected a hex string");
    const std::string& s = v.AsString();
    if (s.size() % 2) Fail(where, "odd hex length");
    auto nib = [&](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::runtime_error(where + ": bad hex digit");
    };
    std::vector<uint8_t> out;
    for (size_t i = 0; i < s.size(); i += 2) out.push_back(uint8_t(nib(s[i]) * 16 + nib(s[i + 1])));
    return out;
}
Value Rgb(const std::array<uint8_t, 3>& c) { return Value::String(Hex(c)); }
std::array<uint8_t, 3> FromRgb(const Value& v, const std::string& where) {
    const std::vector<uint8_t> b = FromHex(v, where);
    if (b.size() != 3) Fail(where, "expected 6 hex digits (rrggbb)");
    return {b[0], b[1], b[2]};
}

void WarnUnknownKeys(const Value& obj, const std::set<std::string>& known, const std::string& section, std::vector<std::string>& warnings) {
    for (const auto& [k, v] : obj.Members())
        if (!known.count(k)) warnings.push_back("unknown key " + (section.empty() ? k : section + "." + k) + " ignored");
}

// ---------------------------------------------------------------- chunk frame

// Chunk-local vertices are 1/64 m relative to the 64 m cell of the chunk centre with axes (x -> world X, y -> world Z,
// z -> height); the file stores world 1/64 m (X, Y up, Z) so that a vertex does not move when a chunk's centre changes.
int32_t CellX(const TrackChunk& c) { return c.cellOrigin[0] / 1024; }
int32_t CellZ(const TrackChunk& c) { return c.cellOrigin[1] / 1024; }

int32_t FloorCell(int32_t v) { // track.cpp: floor(centre / 64 m) * 64 m in 16.16
    constexpr int64_t kCell = 64 * 65536;
    int64_t q = v / kCell;
    if (v % kCell < 0) q--;
    return int32_t(q * kCell);
}

int16_t LocalS16(int64_t world, int32_t cell, const std::string& where) {
    const int64_t l = world - cell;
    if (l < -32768 || l > 32767) Fail(where, "position is " + std::to_string(l) + " / 64 m from its chunk's 64 m cell, outside the s16 range");
    return int16_t(l);
}

// ---------------------------------------------------------------- writer

Value IntArray(std::initializer_list<int64_t> v) {
    Value a = Value::Array();
    for (int64_t x : v) a.Push(Value::Int(x));
    return a;
}
template <typename T, size_t N> Value IntArray(const std::array<T, N>& v) {
    Value a = Value::Array();
    for (const T& x : v) a.Push(Value::Int(int64_t(x)));
    return a;
}

Value ShapeToJson(const TrackShape& shape, const TrackChunk& chunk) {
    Value s = Value::Object();
    Value verts = Value::Array();
    for (const TrackVertex& v : shape.vertices) {
        verts.Push(Value::Int(CellX(chunk) + v.x));
        verts.Push(Value::Int(v.z));
        verts.Push(Value::Int(CellZ(chunk) + v.y));
    }
    s.Set("vertices", std::move(verts));
    Value polys = Value::Array();
    for (const TrackPolygon& p : shape.polygons) {
        Value a = Value::Array();
        a.Push(Value::Int(p.primCode));
        a.Push(Value::Int(p.renderOrder));
        for (uint16_t v : p.vertex) a.Push(Value::Int(v));
        a.Push(Value::Int(p.uvIndex));
        a.Push(Value::Int(p.flags >> 4)); // word1 bits 23-31
        const size_t corners = p.IsQuad() ? 4 : 3;
        for (size_t c = 0; c < (p.IsGouraud() ? corners : 1); c++) a.Push(Rgb(p.color[c]));
        polys.Push(std::move(a));
    }
    s.Set("polygons", std::move(polys));
    return s;
}

Value ChunkToJson(const TrackChunk& c) {
    Value o = Value::Object();
    o.Set("prev", Value::Int(c.prev));
    o.Set("next", Value::Int(c.next));
    o.Set("distance", Metres16(c.distance));
    o.Set("weightNext", Value::Int(c.weightNext));
    o.Set("weightThis", Value::Int(c.weightThis));
    o.Set("vcoord", Value::Int(c.vcoord));
    Value origin = Value::Array(), centre = Value::Array();
    for (int32_t v : c.origin) origin.Push(Metres16(v));
    for (int32_t v : c.centre) centre.Push(Metres16(v));
    o.Set("origin", std::move(origin));
    o.Set("direction", IntArray(c.direction));
    o.Set("centre", std::move(centre));
    o.Set("sceneryMask", Value::Int(c.sceneryMask));
    Value render = Value::Array();
    for (uint16_t e : c.renderList) render.Push(Value::Int(e));
    o.Set("renderList", std::move(render));
    o.Set("road", ShapeToJson(c.road, c));
    Value walls = Value::Array();
    for (const TrackBoundary& b : c.boundaries) walls.Push(IntArray({b.vertexA, b.vertexB, b.normal0, b.normal1}));
    o.Set("walls", std::move(walls));
    Value grid = Value::Object();
    grid.Set("origin", IntArray({CellX(c) + c.surfaceGrid.originX, CellZ(c) + c.surfaceGrid.originZ}));
    grid.Set("shift", IntArray({c.surfaceGrid.shiftX, c.surfaceGrid.shiftZ}));
    Value cells = Value::Array();
    for (const auto& cell : c.surfaceGrid.cells) {
        Value a = Value::Array();
        for (uint16_t p : cell) a.Push(Value::Int(p));
        cells.Push(std::move(a));
    }
    grid.Set("cells", std::move(cells));
    o.Set("surfaceGrid", std::move(grid));
    Value boards = Value::Array();
    for (const TrackBillboard& b : c.billboards) {
        Value a = IntArray({CellX(c) + b.position[0], b.position[2], CellZ(c) + b.position[1], b.uvIndex, b.width, b.height});
        a.Push(Rgb(b.color));
        a.Push(Value::Int(b.primCode));
        boards.Push(std::move(a));
    }
    o.Set("billboards", std::move(boards));
    o.Set("lightCount", Value::Int(c.lightCount));
    o.Set("mirror", ShapeToJson(c.surround, c));
    return o;
}

Value ModelToJson(const TrackSceneryModel& m) {
    Value o = Value::Object();
    o.Set("scaleExponent", Value::Int(m.scaleExponent));
    o.Set("boundsMin", IntArray(m.boundsMin));
    o.Set("boundsMax", IntArray(m.boundsMax));
    Value verts = Value::Array();
    for (const TrackVertex& v : m.vertices) { verts.Push(Value::Int(v.x)); verts.Push(Value::Int(v.y)); verts.Push(Value::Int(v.z)); }
    o.Set("vertices", std::move(verts));
    Value polys = Value::Array();
    for (const TrackSceneryPolygon& p : m.polygons) {
        Value a = Value::Array();
        a.Push(Value::Int(p.primCode));
        a.Push(Value::Int((p.sortFarthest ? 1 : 0) | (p.cullBackface ? 2 : 0)));
        for (uint16_t v : p.vertex) a.Push(Value::Int(v));
        if (p.IsTextured()) {
            for (size_t k = 0; k < 4; k++) { a.Push(Value::Int(p.u[k])); a.Push(Value::Int(p.v[k])); }
            a.Push(Value::Int(p.clut));
            a.Push(Value::Int(p.tpage));
        }
        const size_t corners = p.IsQuad() ? 4 : 3;
        for (size_t c = 0; c < (p.IsGouraud() ? corners : 1); c++) a.Push(Rgb(p.color[c]));
        polys.Push(std::move(a));
    }
    o.Set("polygons", std::move(polys));
    Value boards = Value::Array();
    for (const TrackBillboard& b : m.billboards) {
        Value a = IntArray({b.position[0], b.position[1], b.position[2], b.width, b.height});
        a.Push(Rgb(b.color));
        a.Push(Value::Int(b.primCode));
        for (size_t k = 0; k < 4; k++) { a.Push(Value::Int(b.u[k])); a.Push(Value::Int(b.v[k])); }
        a.Push(Value::Int(b.clut));
        a.Push(Value::Int(b.tpage));
        boards.Push(std::move(a));
    }
    o.Set("billboards", std::move(boards));
    o.Set("lightCount", Value::Int(m.lightCount));
    return o;
}

// ---------------------------------------------------------------- reader

TrackShape ShapeFromJson(const Value& s, const TrackChunk& chunk, const std::string& where) {
    TrackShape shape;
    const Value& verts = ArrayOf(Req(s, "vertices", where), where + ".vertices");
    if (verts.Size() % 3) Fail(where + ".vertices", "length is not a multiple of 3");
    for (size_t i = 0; i < verts.Size(); i += 3) {
        const std::string w = where + ".vertices[" + std::to_string(i / 3) + "]";
        TrackVertex v;
        v.x = LocalS16(IntIn(verts.At(i), INT32_MIN, INT32_MAX, w), CellX(chunk), w);
        v.z = int16_t(IntIn(verts.At(i + 1), -32768, 32767, w));
        v.y = LocalS16(IntIn(verts.At(i + 2), INT32_MIN, INT32_MAX, w), CellZ(chunk), w);
        shape.vertices.push_back(v);
    }
    const Value& polys = ArrayOf(Req(s, "polygons", where), where + ".polygons");
    for (size_t i = 0; i < polys.Size(); i++) {
        const std::string w = where + ".polygons[" + std::to_string(i) + "]";
        const Value& a = ArrayOf(polys.At(i), w, 9);
        TrackPolygon p;
        p.primCode = uint8_t(IntIn(a.At(0), 0x20, 0x3F, w + " code"));
        p.renderOrder = uint8_t(IntIn(a.At(1), 0, 31, w + " order"));
        for (size_t k = 0; k < 4; k++) p.vertex[k] = uint16_t(IntIn(a.At(2 + k), 0, 511, w + " vertex"));
        p.uvIndex = uint16_t(IntIn(a.At(6), 0, 0x3FFF, w + " uv"));
        const uint32_t surface = uint32_t(IntIn(a.At(7), 0, 511, w + " surface"));
        p.flags = uint32_t(p.uvIndex >> 10) | (surface << 4);
        const size_t corners = p.IsQuad() ? 4 : 3, colours = p.IsGouraud() ? corners : 1;
        if (a.Size() != 8 + colours) Fail(w, "expected " + std::to_string(8 + colours) + " elements for code " + std::to_string(p.primCode));
        for (size_t c = 0; c < corners; c++) p.color[c] = FromRgb(a.At(8 + (p.IsGouraud() ? c : 0)), w + " colour");
        shape.polygons.push_back(p);
    }
    return shape;
}

TrackChunk ChunkFromJson(const Value& o, const std::string& where, std::vector<std::string>& warnings) {
    WarnUnknownKeys(o, {"prev", "next", "distance", "weightNext", "weightThis", "vcoord", "origin", "direction", "centre", "sceneryMask", "renderList", "road",
                        "walls", "surfaceGrid", "billboards", "lightCount", "mirror"},
                    where, warnings);
    TrackChunk c;
    c.prev = uint16_t(IntIn(Req(o, "prev", where), 0, 0xFFFF, where + ".prev"));
    c.next = uint16_t(IntIn(Req(o, "next", where), 0, 0xFFFF, where + ".next"));
    c.distance = FromMetres16(Req(o, "distance", where), where + ".distance");
    c.weightNext = uint16_t(IntIn(Req(o, "weightNext", where), 0, 0xFFFF, where + ".weightNext"));
    c.weightThis = uint16_t(IntIn(Req(o, "weightThis", where), 0, 0xFFFF, where + ".weightThis"));
    c.vcoord = int16_t(IntIn(Req(o, "vcoord", where), -32768, 32767, where + ".vcoord"));
    const Value& origin = ArrayOf(Req(o, "origin", where), where + ".origin", 3);
    const Value& direction = ArrayOf(Req(o, "direction", where), where + ".direction", 3);
    const Value& centre = ArrayOf(Req(o, "centre", where), where + ".centre", 3);
    for (size_t k = 0; k < 3; k++) {
        c.origin[k] = FromMetres16(origin.At(k), where + ".origin");
        c.direction[k] = int16_t(IntIn(direction.At(k), -32768, 32767, where + ".direction"));
        c.centre[k] = FromMetres16(centre.At(k), where + ".centre");
    }
    c.cellOrigin = {FloorCell(c.centre[0]), FloorCell(c.centre[2])};
    c.sceneryMask = uint32_t(IntIn(Req(o, "sceneryMask", where), 0, 0xFFFFFFFFll, where + ".sceneryMask"));
    const Value& render = ArrayOf(Req(o, "renderList", where), where + ".renderList");
    for (size_t k = 0; k < render.Size(); k++) c.renderList.push_back(uint16_t(IntIn(render.At(k), 0, 0xFFFF, where + ".renderList")));
    c.road = ShapeFromJson(Req(o, "road", where), c, where + ".road");
    const Value& walls = ArrayOf(Req(o, "walls", where), where + ".walls");
    for (size_t k = 0; k < walls.Size(); k++) {
        const std::string w = where + ".walls[" + std::to_string(k) + "]";
        const Value& a = ArrayOf(walls.At(k), w, 4);
        c.boundaries.push_back({uint16_t(IntIn(a.At(0), 0, 0xFFFF, w)), uint16_t(IntIn(a.At(1), 0, 0xFFFF, w)), int16_t(IntIn(a.At(2), -32768, 32767, w)),
                                int16_t(IntIn(a.At(3), -32768, 32767, w))});
    }
    const Value& grid = Req(o, "surfaceGrid", where);
    WarnUnknownKeys(grid, {"origin", "shift", "cells"}, where + ".surfaceGrid", warnings);
    const Value& go = ArrayOf(Req(grid, "origin", where), where + ".surfaceGrid.origin", 2);
    const Value& gs = ArrayOf(Req(grid, "shift", where), where + ".surfaceGrid.shift", 2);
    c.surfaceGrid.originX = LocalS16(IntIn(go.At(0), INT32_MIN, INT32_MAX, where), CellX(c), where + ".surfaceGrid.origin");
    c.surfaceGrid.originZ = LocalS16(IntIn(go.At(1), INT32_MIN, INT32_MAX, where), CellZ(c), where + ".surfaceGrid.origin");
    c.surfaceGrid.shiftX = int16_t(IntIn(gs.At(0), -32768, 32767, where + ".surfaceGrid.shift"));
    c.surfaceGrid.shiftZ = int16_t(IntIn(gs.At(1), -32768, 32767, where + ".surfaceGrid.shift"));
    const Value& cells = ArrayOf(Req(grid, "cells", where), where + ".surfaceGrid.cells");
    if (cells.Size() != 16) Fail(where + ".surfaceGrid.cells", "expected 16 cells");
    for (size_t k = 0; k < 16; k++) {
        const Value& cell = ArrayOf(cells.At(k), where + ".surfaceGrid.cells");
        for (size_t j = 0; j < cell.Size(); j++) c.surfaceGrid.cells[k].push_back(uint16_t(IntIn(cell.At(j), 0, 0xFFFF, where + ".surfaceGrid.cells")));
    }
    const Value& boards = ArrayOf(Req(o, "billboards", where), where + ".billboards");
    for (size_t k = 0; k < boards.Size(); k++) {
        const std::string w = where + ".billboards[" + std::to_string(k) + "]";
        const Value& a = ArrayOf(boards.At(k), w, 8);
        TrackBillboard b;
        b.position[0] = LocalS16(IntIn(a.At(0), INT32_MIN, INT32_MAX, w), CellX(c), w);
        b.position[1] = LocalS16(IntIn(a.At(2), INT32_MIN, INT32_MAX, w), CellZ(c), w);
        b.position[2] = int16_t(IntIn(a.At(1), -32768, 32767, w));
        b.uvIndex = uint16_t(IntIn(a.At(3), 0, 0xFFFF, w + " uv"));
        b.width = int16_t(IntIn(a.At(4), -32768, 32767, w));
        b.height = int16_t(IntIn(a.At(5), -32768, 32767, w));
        b.color = FromRgb(a.At(6), w);
        b.primCode = uint8_t(IntIn(a.At(7), 0, 255, w));
        c.billboards.push_back(b); // texture words: from the UV table (ReadCourseFile, after the table is known)
    }
    c.lightCount = uint16_t(IntIn(Req(o, "lightCount", where), 0, 0xFFFF, where + ".lightCount"));
    c.surround = ShapeFromJson(Req(o, "mirror", where), c, where + ".mirror");
    return c;
}

TrackSceneryModel ModelFromJson(const Value& o, const std::string& where, std::vector<std::string>& warnings) {
    WarnUnknownKeys(o, {"scaleExponent", "boundsMin", "boundsMax", "vertices", "polygons", "billboards", "lightCount"}, where, warnings);
    TrackSceneryModel m;
    m.scaleExponent = int16_t(IntIn(Req(o, "scaleExponent", where), 8, 31, where + ".scaleExponent"));
    const Value& mn = ArrayOf(Req(o, "boundsMin", where), where + ".boundsMin", 3);
    const Value& mx = ArrayOf(Req(o, "boundsMax", where), where + ".boundsMax", 3);
    for (size_t k = 0; k < 3; k++) {
        m.boundsMin[k] = int16_t(IntIn(mn.At(k), -32768, 32767, where + ".boundsMin"));
        m.boundsMax[k] = int16_t(IntIn(mx.At(k), -32768, 32767, where + ".boundsMax"));
    }
    const Value& verts = ArrayOf(Req(o, "vertices", where), where + ".vertices");
    if (verts.Size() % 3) Fail(where + ".vertices", "length is not a multiple of 3");
    for (size_t i = 0; i < verts.Size(); i += 3)
        m.vertices.push_back({int16_t(IntIn(verts.At(i), -32768, 32767, where + ".vertices")), int16_t(IntIn(verts.At(i + 1), -32768, 32767, where + ".vertices")),
                              int16_t(IntIn(verts.At(i + 2), -32768, 32767, where + ".vertices"))});
    const Value& polys = ArrayOf(Req(o, "polygons", where), where + ".polygons");
    for (size_t i = 0; i < polys.Size(); i++) {
        const std::string w = where + ".polygons[" + std::to_string(i) + "]";
        const Value& a = ArrayOf(polys.At(i), w, 7);
        TrackSceneryPolygon p;
        p.primCode = uint8_t(IntIn(a.At(0), 0x20, 0x3F, w + " code"));
        const int64_t f = IntIn(a.At(1), 0, 3, w + " flags");
        p.sortFarthest = (f & 1) != 0;
        p.cullBackface = (f & 2) != 0;
        for (size_t k = 0; k < 4; k++) p.vertex[k] = uint16_t(IntIn(a.At(2 + k), 0, 1023, w + " vertex"));
        size_t at = 6;
        if (p.IsTextured()) {
            if (a.Size() < at + 10) Fail(w, "textured polygon without its 8 uv values, clut and tpage");
            for (size_t k = 0; k < 4; k++) {
                p.u[k] = uint8_t(IntIn(a.At(at++), 0, 255, w + " u"));
                p.v[k] = uint8_t(IntIn(a.At(at++), 0, 255, w + " v"));
            }
            p.clut = uint16_t(IntIn(a.At(at++), 0, 0xFFFF, w + " clut"));
            p.tpage = uint16_t(IntIn(a.At(at++), 0, 0xFFFF, w + " tpage"));
        }
        const size_t corners = p.IsQuad() ? 4 : 3, colours = p.IsGouraud() ? corners : 1;
        if (a.Size() != at + colours) Fail(w, "expected " + std::to_string(at + colours) + " elements for code " + std::to_string(p.primCode));
        for (size_t c = 0; c < corners; c++) p.color[c] = FromRgb(a.At(at + (p.IsGouraud() ? c : 0)), w + " colour");
        m.polygons.push_back(p);
    }
    const Value& boards = ArrayOf(Req(o, "billboards", where), where + ".billboards");
    for (size_t k = 0; k < boards.Size(); k++) {
        const std::string w = where + ".billboards[" + std::to_string(k) + "]";
        const Value& a = ArrayOf(boards.At(k), w, 17);
        TrackBillboard b;
        b.position = {int32_t(IntIn(a.At(0), -32768, 32767, w)), int32_t(IntIn(a.At(1), -32768, 32767, w)), int32_t(IntIn(a.At(2), -32768, 32767, w))};
        b.width = int16_t(IntIn(a.At(3), -32768, 32767, w));
        b.height = int16_t(IntIn(a.At(4), -32768, 32767, w));
        b.color = FromRgb(a.At(5), w);
        b.primCode = uint8_t(IntIn(a.At(6), 0, 255, w));
        for (size_t j = 0; j < 4; j++) {
            b.u[j] = uint8_t(IntIn(a.At(7 + j * 2), 0, 255, w));
            b.v[j] = uint8_t(IntIn(a.At(8 + j * 2), 0, 255, w));
        }
        b.clut = uint16_t(IntIn(a.At(15), 0, 0xFFFF, w));
        b.tpage = uint16_t(IntIn(a.At(16), 0, 0xFFFF, w));
        m.billboards.push_back(b);
    }
    m.billboardCount = uint16_t(m.billboards.size());
    m.lightCount = uint16_t(IntIn(Req(o, "lightCount", where), 0, 0xFFFF, where + ".lightCount"));
    return m;
}

TrackSceneryInstance InstanceFromJson(const Value& o, const std::string& w, const std::string& section, std::vector<std::string>& warnings) {
    WarnUnknownKeys(o, {"list", "lod", "position", "angle", "scale", "lodDivisor"}, section, warnings);
    TrackSceneryInstance inst;
    inst.list = uint8_t(IntIn(Req(o, "list", w), 0, 32, w + ".list"));
    inst.lodList = uint16_t(IntIn(Req(o, "lod", w), 0, 0xFFFF, w + ".lod"));
    const Value& pos = ArrayOf(Req(o, "position", w), w + ".position", 3);
    for (size_t k = 0; k < 3; k++) inst.position[k] = FromMetres16(pos.At(k), w + ".position");
    if (const Value* a = o.Get("angle")) {
        ArrayOf(*a, w + ".angle", 3);
        for (size_t k = 0; k < 3; k++) inst.angle[k] = FromDegrees16(a->At(k), w + ".angle");
    }
    inst.scale = {4096, 4096, 4096};
    if (const Value* sc = o.Get("scale")) {
        ArrayOf(*sc, w + ".scale", 3);
        for (size_t k = 0; k < 3; k++) inst.scale[k] = int16_t(RoundChecked(sc->At(k).AsDouble() * 4096.0, -32768, 32767, w + ".scale"));
    }
    inst.lodDivisor = int16_t(o.Get("lodDivisor") ? IntIn(*o.Get("lodDivisor"), -32768, 32767, w + ".lodDivisor") : 4096);
    return inst;
}

// Race record: [type, x m, z m, radius m, heading] (world x / z: the simulation plane's (x, y) = (world X, -world Z)),
// + [height, chunk, surface, attribute, distance, slopeAcross, slopeAlong] (raw) when any of the loader-filled fields
// is non-zero in the file.
Value RaceRecordToJson(const TrackRaceRecord& r) {
    Value a = Value::Array();
    a.Push(Value::Int(r.type));
    a.Push(Metres12(r.x));
    a.Push(Value::Double(-(r.y / 4096.0)));
    a.Push(Metres12(r.radius));
    a.Push(Value::Int(r.heading));
    if (r.height || r.chunk || r.surface || r.attribute || r.distance || r.slopeAcross || r.slopeAlong)
        for (int64_t v : {int64_t(r.height), int64_t(r.chunk), int64_t(r.surface), int64_t(r.attribute), int64_t(r.distance), int64_t(r.slopeAcross), int64_t(r.slopeAlong)})
            a.Push(Value::Int(v));
    return a;
}
TrackRaceRecord RaceRecordFromJson(const Value& v, const std::string& where) {
    const Value& a = ArrayOf(v, where, 5);
    if (a.Size() != 5 && a.Size() != 12) Fail(where, "expected 5 or 12 elements");
    TrackRaceRecord r{};
    r.type = int32_t(IntIn(a.At(0), INT32_MIN, INT32_MAX, where));
    r.x = FromMetres12(a.At(1), where);
    r.y = -FromMetres12(a.At(2), where);
    r.radius = FromMetres12(a.At(3), where);
    r.heading = int32_t(IntIn(a.At(4), INT32_MIN, INT32_MAX, where));
    if (a.Size() == 12) {
        r.height = int32_t(IntIn(a.At(5), INT32_MIN, INT32_MAX, where));
        r.chunk = uint16_t(IntIn(a.At(6), 0, 0xFFFF, where));
        r.surface = int8_t(IntIn(a.At(7), -128, 127, where));
        r.attribute = int8_t(IntIn(a.At(8), -128, 127, where));
        r.distance = int32_t(IntIn(a.At(9), INT32_MIN, INT32_MAX, where));
        r.slopeAcross = int32_t(IntIn(a.At(10), INT32_MIN, INT32_MAX, where));
        r.slopeAlong = int32_t(IntIn(a.At(11), INT32_MIN, INT32_MAX, where));
    }
    return r;
}

// ---------------------------------------------------------------- canonical bytes

struct Out {
    std::vector<uint8_t> b;
    void U8(uint32_t v) { b.push_back(uint8_t(v)); }
    void U16(uint32_t v) { U8(v); U8(v >> 8); }
    void U32(uint32_t v) { U16(v); U16(v >> 16); }
    void S32(int32_t v) { U32(uint32_t(v)); }
    void Str(const std::string& s) { U32(uint32_t(s.size())); b.insert(b.end(), s.begin(), s.end()); }
};

void PutShape(Out& o, const TrackShape& s) {
    o.U32(uint32_t(s.vertices.size()));
    for (const TrackVertex& v : s.vertices) { o.U16(uint16_t(v.x)); o.U16(uint16_t(v.y)); o.U16(uint16_t(v.z)); }
    o.U32(uint32_t(s.polygons.size()));
    for (const TrackPolygon& p : s.polygons) {
        for (uint16_t v : p.vertex) o.U16(v);
        o.U8(p.renderOrder);
        o.U16(p.uvIndex);
        o.U32(p.flags);
        for (const auto& c : p.color) { o.U8(c[0]); o.U8(c[1]); o.U8(c[2]); }
        o.U8(p.primCode);
    }
}
void PutBillboard(Out& o, const TrackBillboard& b) {
    for (int32_t v : b.position) o.S32(v);
    o.U16(uint16_t(b.width));
    o.U16(uint16_t(b.height));
    o.U8(b.color[0]); o.U8(b.color[1]); o.U8(b.color[2]);
    o.U8(b.primCode);
    for (size_t k = 0; k < 4; k++) { o.U8(b.u[k]); o.U8(b.v[k]); }
    o.U16(b.clut);
    o.U16(b.tpage);
    o.U16(b.uvIndex);
}

} // namespace

// ---------------------------------------------------------------- cameras

std::vector<uint8_t> CourseCameras::Blob(uint32_t base) const {
    std::vector<uint8_t> out;
    if (!present) return out;
    Out o;
    o.U16(uint16_t(records.size()));
    o.U16(lapModulus);
    size_t at = 4 + records.size() * 4;
    at = (at + 3) & ~size_t(3);
    std::vector<uint32_t> offsets;
    for (const CourseCameraRecord& r : records) {
        offsets.push_back(uint32_t(at));
        at += (0x10 + r.data.size() + 3) & ~size_t(3);
    }
    for (uint32_t off : offsets) o.U32(base + off);
    while (o.b.size() < (4 + records.size() * 4 + 3) / 4 * 4) o.U8(0);
    for (const CourseCameraRecord& r : records) {
        o.U16(r.flags);
        o.U16(r.firstLap);
        o.U16(r.lastLap);
        o.U16(r.chunk);
        o.S32(r.start);
        o.S32(r.end);
        o.b.insert(o.b.end(), r.data.begin(), r.data.end());
        while (o.b.size() % 4) o.U8(0);
    }
    return o.b;
}

CourseCameras ParseCourseCameras(std::span<const uint8_t> tro) {
    if (tro.size() < 0x20) throw std::runtime_error("replay cameras: short .tro");
    const uint32_t list = uint32_t(tro[0x1C] | (tro[0x1D] << 8) | (tro[0x1E] << 16) | (uint32_t(tro[0x1F]) << 24));
    return ParseCameraList(tro, 0, list);
}

CourseCameras ParseCameraList(std::span<const uint8_t> bytes, uint32_t base, uint32_t list) {
    auto need = [&](uint32_t pointer, size_t n) {
        const size_t off = size_t(pointer - base);
        if (pointer < base || off > bytes.size() || n > bytes.size() - off) throw std::runtime_error("replay cameras: read outside the data at 0x" + std::to_string(pointer));
        return off;
    };
    auto u16 = [&](uint32_t p) { const size_t o = need(p, 2); return uint16_t(bytes[o] | (bytes[o + 1] << 8)); };
    auto u32 = [&](uint32_t p) { const size_t o = need(p, 4); return uint32_t(bytes[o] | (bytes[o + 1] << 8) | (bytes[o + 2] << 16) | (uint32_t(bytes[o + 3]) << 24)); };
    CourseCameras cams;
    if (list == 0) return cams;
    cams.present = true;
    const uint16_t count = u16(list);
    cams.lapModulus = u16(list + 2);
    std::vector<uint32_t> pointers;
    for (uint32_t i = 0; i < count; i++) pointers.push_back(u32(list + 4 + 4 * i));
    for (uint32_t p : pointers) {
        // The bytes the camera code reads (race_camera.cpp): kinds 0 / 3 carry an inline path at + 0x18 / + 0x1C
        // ({u16 kind, u16 count, count x 16 (polyline) or 56 (cubic) bytes}); kind 1 reads to + 0x26, kind 2 to + 0x14.
        CourseCameraRecord r;
        r.flags = u16(p);
        r.firstLap = u16(p + 2);
        r.lastLap = u16(p + 4);
        r.chunk = u16(p + 6);
        r.start = int32_t(u32(p + 8));
        r.end = int32_t(u32(p + 12));
        size_t size = 0x10;
        const uint32_t kind = r.flags & 0xF;
        auto pathEnd = [&](uint32_t path) {
            const uint16_t pathKind = u16(path), n = u16(path + 2);
            if (pathKind > 1) throw std::runtime_error("replay cameras: unknown path kind " + std::to_string(pathKind));
            return path - p + 4 + size_t(n) * (pathKind == 0 ? 16 : 56);
        };
        if (kind == 0) size = pathEnd(p + 0x18);
        else if (kind == 1) size = 0x26;
        else if (kind == 2) size = 0x14;
        else if (kind == 3) size = pathEnd(p + 0x1C);
        // The size rule above must not run into another record or the list (checked on every course of the disc).
        for (uint32_t other : pointers)
            if (other > p && other < p + size) throw std::runtime_error("replay cameras: record at 0x" + std::to_string(p) + " overlaps the next record");
        if (list > p && list < p + size) throw std::runtime_error("replay cameras: record overlaps the list");
        const size_t at = need(p, size);
        r.data.assign(bytes.begin() + std::ptrdiff_t(at + 0x10), bytes.begin() + std::ptrdiff_t(at + size));
        cams.records.push_back(std::move(r));
    }
    return cams;
}

// ---------------------------------------------------------------- validation

void ValidateTrack(const Track& t) {
    static constexpr uint8_t kCode[8] = {0x20, 0x28, 0x30, 0x38, 0x24, 0x2C, 0x34, 0x3C};
    auto listOf = [](uint8_t code) { return size_t(((code & 0x04) ? 4 : 0) | ((code & 0x10) ? 2 : 0) | ((code & 0x08) ? 1 : 0)); };
    if (t.chunks.empty()) throw std::runtime_error("track: no chunks");
    if (t.chunks.size() > 0x3FFF) throw std::runtime_error("track: more than 16383 chunks");
    if (t.uvTable.size() > 0x7FFF) throw std::runtime_error("track: UV table too large");
    auto checkShape = [&](const TrackShape& s, const std::string& where) {
        if (s.vertices.size() > 512) throw std::runtime_error(where + ": more than 512 vertices (9-bit indices)");
        size_t lastList = 0;
        for (size_t i = 0; i < s.polygons.size(); i++) {
            const TrackPolygon& p = s.polygons[i];
            const std::string w = where + " polygon " + std::to_string(i);
            if ((p.primCode & 0xE0) != 0x20) throw std::runtime_error(w + ": bad primitive code");
            const size_t list = listOf(p.primCode);
            if ((p.primCode & 0xFC) != kCode[list]) throw std::runtime_error(w + ": bad primitive code");
            if (list < lastList) throw std::runtime_error(w + ": polygons must be ordered by list (F3 F4 G3 G4 FT3 FT4 GT3 GT4)");
            lastList = list;
            for (size_t c = 0; c < (p.IsQuad() ? 4u : 3u); c++)
                if (p.vertex[c] >= s.vertices.size()) throw std::runtime_error(w + ": vertex index out of range");
            if (p.IsTextured() && p.uvIndex >= t.uvTable.size()) throw std::runtime_error(w + ": UV index out of range");
        }
    };
    for (size_t i = 0; i < t.chunks.size(); i++) {
        const TrackChunk& c = t.chunks[i];
        const std::string w = "track: chunk " + std::to_string(i);
        checkShape(c.road, w + " road");
        checkShape(c.surround, w + " mirror");
        if (c.prev >= t.chunks.size() || c.next >= t.chunks.size()) throw std::runtime_error(w + ": prev / next out of range");
        for (const TrackBoundary& b : c.boundaries)
            if (b.vertexA >= c.road.vertices.size() || b.vertexB >= c.road.vertices.size()) throw std::runtime_error(w + ": wall vertex out of range");
        if (c.boundaries.size() > 4096) throw std::runtime_error(w + ": too many walls");
        for (const auto& cell : c.surfaceGrid.cells)
            for (uint16_t p : cell)
                if (p >= c.road.polygons.size()) throw std::runtime_error(w + ": surface grid polygon out of range");
        if (c.renderList.size() > 1024) throw std::runtime_error(w + ": render list too long");
        for (uint16_t e : c.renderList)
            if ((e & 0x3FFF) >= t.chunks.size()) throw std::runtime_error(w + ": render list chunk out of range");
        for (const TrackBillboard& b : c.billboards)
            if (b.uvIndex >= t.uvTable.size()) throw std::runtime_error(w + ": billboard UV index out of range");
        const std::array<int32_t, 2> cell = {FloorCell(c.centre[0]), FloorCell(c.centre[2])};
        if (cell != c.cellOrigin) throw std::runtime_error(w + ": cell origin does not follow the centre");
    }
    for (size_t i = 0; i < t.sceneryModels.size(); i++) {
        const TrackSceneryModel& m = t.sceneryModels[i];
        const std::string w = "track: scenery model " + std::to_string(i);
        if (m.vertices.size() > 1024) throw std::runtime_error(w + ": more than 1024 vertices (10-bit indices)");
        if (m.scaleExponent < 8 || m.scaleExponent > 31) throw std::runtime_error(w + ": bad scale exponent");
        size_t lastList = 0;
        for (const TrackSceneryPolygon& p : m.polygons) {
            const size_t list = listOf(p.primCode);
            if ((p.primCode & 0xFC) != kCode[list]) throw std::runtime_error(w + ": bad primitive code");
            if (list < lastList) throw std::runtime_error(w + ": polygons must be ordered by list");
            lastList = list;
            for (size_t c = 0; c < (p.IsQuad() ? 4u : 3u); c++)
                if (p.vertex[c] >= m.vertices.size()) throw std::runtime_error(w + ": vertex index out of range");
        }
        if (m.billboardCount != m.billboards.size()) throw std::runtime_error(w + ": billboard count");
    }
    for (size_t i = 0; i < t.sceneryLods.size(); i++) {
        if (t.sceneryLods[i].empty() || t.sceneryLods[i].size() > 16) throw std::runtime_error("track: LOD list " + std::to_string(i) + ": 1..16 entries");
        for (const TrackLodEntry& e : t.sceneryLods[i])
            if (e.model >= t.sceneryModels.size()) throw std::runtime_error("track: LOD list " + std::to_string(i) + ": model out of range");
    }
    uint8_t lastList = 0;
    for (size_t i = 0; i < t.sceneryInstances.size(); i++) {
        const TrackSceneryInstance& s = t.sceneryInstances[i];
        if (s.lodList >= t.sceneryLods.size()) throw std::runtime_error("track: scenery instance " + std::to_string(i) + ": LOD list out of range");
        if (s.list > 32) throw std::runtime_error("track: scenery instance " + std::to_string(i) + ": list > 32");
        if (s.list < lastList) throw std::runtime_error("track: scenery instances must be ordered by list");
        lastList = s.list;
    }
    if (int16_t(t.courseLength >> 16) != t.lengthMetres) throw std::runtime_error("track: lengthMetres differs from the course length");
}

// ---------------------------------------------------------------- canonical bytes

std::vector<uint8_t> CanonicalTrackBytes(const Track& t) {
    Out o;
    o.S32(t.courseLength);
    o.U16(uint16_t(t.lengthMetres));
    o.U16(uint16_t(t.startAngle));
    for (const auto& g : t.startGrid) for (int32_t v : g) o.S32(v);
    o.U32(uint32_t(t.uvTable.size()));
    for (const TrackUvEntry& e : t.uvTable) {
        for (const TrackUvSet* s : {&e.nearSet, &e.farSet}) {
            for (size_t k = 0; k < 4; k++) { o.U8(s->u[k]); o.U8(s->v[k]); }
            o.U16(s->clut);
            o.U16(s->tpage);
        }
        o.U32(e.extra);
    }
    o.U32(uint32_t(t.chunks.size()));
    for (const TrackChunk& c : t.chunks) {
        o.U16(c.prev); o.U16(c.next);
        o.U32(uint32_t(c.boundaries.size()));
        for (const TrackBoundary& b : c.boundaries) { o.U16(b.vertexA); o.U16(b.vertexB); o.U16(uint16_t(b.normal0)); o.U16(uint16_t(b.normal1)); }
        o.S32(c.distance); o.U16(c.weightNext); o.U16(c.weightThis); o.U16(uint16_t(c.vcoord));
        for (int32_t v : c.origin) o.S32(v);
        for (int16_t v : c.direction) o.U16(uint16_t(v));
        for (int32_t v : c.centre) o.S32(v);
        for (int32_t v : c.cellOrigin) o.S32(v);
        PutShape(o, c.road);
        PutShape(o, c.surround);
        o.U32(uint32_t(c.billboards.size()));
        for (const TrackBillboard& b : c.billboards) PutBillboard(o, b);
        o.U16(c.lightCount);
        o.U16(uint16_t(c.surfaceGrid.originX)); o.U16(uint16_t(c.surfaceGrid.originZ));
        o.U16(uint16_t(c.surfaceGrid.shiftX)); o.U16(uint16_t(c.surfaceGrid.shiftZ));
        for (const auto& cell : c.surfaceGrid.cells) { o.U32(uint32_t(cell.size())); for (uint16_t p : cell) o.U16(p); }
        o.U32(c.sceneryMask);
        o.U32(uint32_t(c.renderList.size()));
        for (uint16_t e : c.renderList) o.U16(e);
    }
    o.U32(uint32_t(t.sceneryModels.size()));
    for (const TrackSceneryModel& m : t.sceneryModels) {
        o.U32(uint32_t(m.vertices.size()));
        for (const TrackVertex& v : m.vertices) { o.U16(uint16_t(v.x)); o.U16(uint16_t(v.y)); o.U16(uint16_t(v.z)); }
        o.U32(uint32_t(m.polygons.size()));
        for (const TrackSceneryPolygon& p : m.polygons) {
            for (uint16_t v : p.vertex) o.U16(v);
            o.U8((p.sortFarthest ? 1 : 0) | (p.cullBackface ? 2 : 0));
            for (const auto& c : p.color) { o.U8(c[0]); o.U8(c[1]); o.U8(c[2]); }
            o.U8(p.primCode);
            for (size_t k = 0; k < 4; k++) { o.U8(p.u[k]); o.U8(p.v[k]); }
            o.U16(p.clut); o.U16(p.tpage);
        }
        for (int16_t v : m.boundsMin) o.U16(uint16_t(v));
        for (int16_t v : m.boundsMax) o.U16(uint16_t(v));
        o.U16(uint16_t(m.scaleExponent));
        o.U16(m.billboardCount);
        o.U32(uint32_t(m.billboards.size()));
        for (const TrackBillboard& b : m.billboards) PutBillboard(o, b);
        o.U16(m.lightCount);
    }
    o.U32(uint32_t(t.sceneryLods.size()));
    for (const auto& l : t.sceneryLods) { o.U32(uint32_t(l.size())); for (const TrackLodEntry& e : l) { o.U32(e.threshold); o.U16(e.model); } }
    o.U32(uint32_t(t.sceneryInstances.size()));
    for (const TrackSceneryInstance& s : t.sceneryInstances) {
        for (int16_t v : s.angle) o.U16(uint16_t(v));
        o.U16(s.lodList);
        for (int16_t v : s.scale) o.U16(uint16_t(v));
        o.U16(uint16_t(s.lodDivisor));
        for (int32_t v : s.position) o.S32(v);
        o.U8(s.list);
    }
    return o.b;
}

std::vector<uint8_t> CanonicalRaceBytes(const TrackRaceData& r) {
    Out o;
    o.U32(uint32_t(r.startLineDistances.size()));
    for (int32_t d : r.startLineDistances) o.S32(d);
    o.U32(r.listCount);
    for (size_t i = 0; i < 7; i++) {
        o.U8(r.present[i] ? 1 : 0);
        o.U32(uint32_t(r.lists[i].size()));
        const uint8_t* p = reinterpret_cast<const uint8_t*>(r.lists[i].data());
        o.b.insert(o.b.end(), p, p + r.lists[i].size() * sizeof(TrackRaceRecord));
    }
    return o.b;
}

std::vector<uint8_t> CanonicalCameraBytes(const CourseCameras& c) {
    Out o;
    o.U8(c.present ? 1 : 0);
    o.U16(c.lapModulus);
    o.U32(uint32_t(c.records.size()));
    for (const CourseCameraRecord& r : c.records) {
        o.U16(r.flags); o.U16(r.firstLap); o.U16(r.lastLap); o.U16(r.chunk);
        o.S32(r.start); o.S32(r.end);
        o.U32(uint32_t(r.data.size()));
        o.b.insert(o.b.end(), r.data.begin(), r.data.end());
    }
    return o.b;
}

std::vector<uint8_t> CanonicalInfoBytes(const CourseInfo& i) {
    Out o;
    o.U8(i.hasEntry ? 1 : 0);
    o.Str(i.name);
    o.U16(i.flags);
    o.Str(i.backdrop);
    o.b.insert(o.b.end(), i.tail.begin(), i.tail.end());
    return o.b;
}

// ---------------------------------------------------------------- disc -> file

namespace {

std::vector<std::string> BackdropPaths(const GtfsVolume& vol) {
    std::vector<std::string> paths;
    for (const auto& f : vol.Files())
        if (f.path.rfind("bgsobj/", 0) == 0 && f.path.size() > 7 && f.path.compare(f.path.size() - 7, 7, ".bso.gz") == 0) paths.push_back(f.path);
    return paths;
}

CourseInfo DiscCourseInfo(const GtfsVolume& vol, const std::string& course, int* indexOut) {
    CourseInfo info;
    const CourseInfoTable table = ParseCourseInfo(vol.Read(".crsinfo"));
    const int index = table.FindByFileName(course);
    if (indexOut) *indexOut = index;
    if (index < 0) return info;
    const CourseInfoEntry& e = table.entries[size_t(index)];
    info.hasEntry = true;
    info.name = e.name;
    info.flags = e.flags;
    std::copy(e.rest.begin() + 2, e.rest.end(), info.tail.begin());
    info.backdrop = BackdropNameAt(BackdropPaths(vol), BackdropIndexOf(e.rest));
    return info;
}

} // namespace

CourseFile MakeCourseFile(const GtfsVolume& vol, const std::string& course) {
    const std::vector<uint8_t> tro = vol.Read("crsobj/" + course + ".tro");
    CourseFile f;
    f.course = course;
    f.baseCourse = course;
    f.textureCourse = course;
    f.courseMap = course;
    f.track = ParseTrack(tro);
    f.race = ParseTrackRaceData(tro);
    f.cameras = ParseCourseCameras(tro);
    f.info = DiscCourseInfo(vol, course, nullptr);
    f.blocks = {true, true, true, true, true, true, true};
    f.infoKeys = {true, true, true, true};
    return f;
}

// ---------------------------------------------------------------- file writer

void WriteCourseFile(const std::string& path, const CourseFile& f) {
    const Track& t = f.track;
    Value root = Value::Object();
    root.Set("format", Value::String(kFormat));
    root.Set("version", Value::Int(kVersion));
    root.Set("course", Value::String(f.course));
    if (!f.baseCourse.empty()) root.Set("baseCourse", Value::String(f.baseCourse));
    {
        Value assets = Value::Object();
        if (!f.meshFile.empty()) assets.Set("mesh", Value::String(f.meshFile));
        if (!f.texturePack.empty()) assets.Set("texturePack", Value::String(f.texturePack));
        if (!f.textureCourse.empty()) assets.Set("textureCourse", Value::String(f.textureCourse));
        if (!f.courseMap.empty()) assets.Set("courseMap", Value::String(f.courseMap));
        root.Set("assets", std::move(assets));
    }
    if (f.blocks.info) {
        Value info = Value::Object();
        if (f.info.hasEntry) {
            info.Set("name", Value::String(f.info.name));
            info.Set("flags", Value::Int(f.info.flags));
            info.Set("backdrop", Value::String(f.info.backdrop));
            info.Set("tail", Value::String(Hex(f.info.tail)));
        }
        root.Set("info", std::move(info));
    }
    if (f.blocks.start) {
        Value start = Value::Object();
        start.Set("angle", Degrees(t.startAngle));
        Value grid = Value::Array();
        for (const auto& g : t.startGrid) {
            Value p = Value::Array();
            for (int32_t v : g) p.Push(Metres16(v));
            grid.Push(std::move(p));
        }
        start.Set("grid", std::move(grid));
        root.Set("start", std::move(start));
    }
    if (f.blocks.uvTable) {
        Value uv = Value::Array();
        for (const TrackUvEntry& e : t.uvTable) {
            Value a = Value::Array();
            for (const TrackUvSet* s : {&e.nearSet, &e.farSet}) {
                for (size_t k = 0; k < 4; k++) { a.Push(Value::Int(s->u[k])); a.Push(Value::Int(s->v[k])); }
                a.Push(Value::Int(s->clut));
                a.Push(Value::Int(s->tpage));
                if (s == &e.nearSet) a.Push(Value::Int(e.extra));
            }
            uv.Push(std::move(a));
        }
        root.Set("uvTable", std::move(uv));
    }
    if (f.blocks.chunks) {
        root.Set("courseLength", Metres16(t.courseLength));
        Value chunks = Value::Array();
        for (const TrackChunk& c : t.chunks) chunks.Push(ChunkToJson(c));
        root.Set("chunks", std::move(chunks));
    }
    if (f.blocks.scenery) {
        Value scenery = Value::Object();
        Value models = Value::Array();
        for (const TrackSceneryModel& m : t.sceneryModels) models.Push(ModelToJson(m));
        scenery.Set("models", std::move(models));
        Value lods = Value::Array();
        for (const auto& l : t.sceneryLods) {
            Value a = Value::Array();
            for (const TrackLodEntry& e : l) a.Push(IntArray({e.threshold, e.model}));
            lods.Push(std::move(a));
        }
        scenery.Set("lods", std::move(lods));
        Value instances = Value::Array();
        for (const TrackSceneryInstance& s : t.sceneryInstances) {
            Value o = Value::Object();
            o.Set("list", Value::Int(s.list));
            o.Set("lod", Value::Int(s.lodList));
            Value pos = Value::Array(), angle = Value::Array(), scale = Value::Array();
            for (int32_t v : s.position) pos.Push(Metres16(v));
            for (int16_t v : s.angle) angle.Push(Degrees(v));
            for (int16_t v : s.scale) scale.Push(Value::Double(v / 4096.0));
            o.Set("position", std::move(pos));
            o.Set("angle", std::move(angle));
            o.Set("scale", std::move(scale));
            o.Set("lodDivisor", Value::Int(s.lodDivisor));
            instances.Push(std::move(o));
        }
        scenery.Set("instances", std::move(instances));
        root.Set("scenery", std::move(scenery));
    }
    if (f.blocks.race) {
        Value race = Value::Object();
        Value lines = Value::Array();
        for (int32_t d : f.race.startLineDistances) lines.Push(Metres16(d));
        race.Set("startLines", std::move(lines));
        race.Set("listCount", Value::Int(f.race.listCount));
        Value lists = Value::Array();
        for (size_t i = 0; i < 7; i++) {
            if (!f.race.present[i]) { lists.Push(Value::Null()); continue; }
            Value l = Value::Array();
            for (const TrackRaceRecord& r : f.race.lists[i]) l.Push(RaceRecordToJson(r));
            lists.Push(std::move(l));
        }
        race.Set("lists", std::move(lists));
        root.Set("race", std::move(race));
    }
    if (f.blocks.cameras) {
        if (!f.cameras.present) root.Set("replayCameras", Value::Null());
        else {
            Value c = Value::Object();
            c.Set("lapModulus", Value::Int(f.cameras.lapModulus));
            Value records = Value::Array();
            for (const CourseCameraRecord& r : f.cameras.records) {
                Value o = Value::Object();
                o.Set("kind", Value::Int(r.flags & 0xF));
                o.Set("flags", Value::Int(r.flags));
                o.Set("laps", IntArray({r.firstLap, r.lastLap}));
                o.Set("chunk", Value::Int(r.chunk));
                o.Set("start", Metres16(r.start));
                o.Set("end", Metres16(r.end));
                o.Set("data", Value::String(Hex(r.data)));
                records.Push(std::move(o));
            }
            c.Set("records", std::move(records));
            root.Set("replayCameras", std::move(c));
        }
    }
    if (!f.objects.empty()) {
        Value objects = Value::Array();
        for (const CourseObject& ob : f.objects) {
            Value o = Value::Object();
            o.Set("mesh", Value::String(ob.mesh));
            o.Set("scale", Value::Double(ob.scale));
            Value pos = Value::Array(), rot = Value::Array();
            for (double v : ob.position) pos.Push(Value::Double(v));
            for (double v : ob.rotation) rot.Push(Value::Double(v));
            o.Set("position", std::move(pos));
            o.Set("rotation", std::move(rot));
            if (ob.shadow) o.Set("shadow", Value::Bool(true));
            objects.Push(std::move(o));
        }
        root.Set("objects", std::move(objects));
    }
    json::WriteFile(path, root);
}

// ---------------------------------------------------------------- file reader

CourseFile ReadCourseFile(const std::string& path) {
    const Value root = json::ReadFile(path);
    const std::string where = path;
    if (!root.IsObject()) Fail(where, "not a JSON object");
    CourseFile f;
    const std::string format = root.StringOr("format", "");
    if (format != kFormat) {
        if (format.empty()) f.warnings.push_back(std::string("no \"format\" key (expected \"") + kFormat + "\")");
        else Fail(where, "format \"" + format + "\" is not " + kFormat);
    }
    f.version = int(root.IntOr("version", kVersion));
    if (f.version != kVersion) Fail(where, "unsupported version " + std::to_string(f.version));
    WarnUnknownKeys(root, {"format", "version", "course", "baseCourse", "assets", "info", "start", "uvTable", "courseLength", "chunks", "scenery", "race",
                           "replayCameras", "objects", "addSceneryInstances"},
                    "", f.warnings);
    f.course = root.StringOr("course", std::filesystem::path(path).stem().string());
    f.baseCourse = root.StringOr("baseCourse", "");
    if (const Value* a = root.Get("assets")) {
        WarnUnknownKeys(*a, {"mesh", "texturePack", "textureCourse", "courseMap"}, "assets", f.warnings);
        f.meshFile = a->StringOr("mesh", "");
        f.texturePack = a->StringOr("texturePack", "");
        f.textureCourse = a->StringOr("textureCourse", "");
        f.courseMap = a->StringOr("courseMap", "");
    }
    Track& t = f.track;
    if (const Value* info = root.Get("info")) {
        WarnUnknownKeys(*info, {"name", "flags", "backdrop", "tail"}, "info", f.warnings);
        f.blocks.info = true;
        f.info.hasEntry = info->Size() > 0;
        f.infoKeys = {info->Get("name") != nullptr, info->Get("flags") != nullptr, info->Get("backdrop") != nullptr, info->Get("tail") != nullptr};
        f.info.name = info->StringOr("name", "");
        f.info.flags = uint16_t(info->Get("flags") ? IntIn(*info->Get("flags"), 0, 0xFFFF, where + " info.flags") : 0);
        f.info.backdrop = info->StringOr("backdrop", "");
        if (const Value* tail = info->Get("tail")) {
            const std::vector<uint8_t> b = FromHex(*tail, where + " info.tail");
            if (b.size() != 12) Fail(where + " info.tail", "expected 12 bytes");
            std::copy(b.begin(), b.end(), f.info.tail.begin());
        }
    }
    if (const Value* s = root.Get("start")) {
        WarnUnknownKeys(*s, {"angle", "grid"}, "start", f.warnings);
        f.blocks.start = true;
        t.startAngle = FromDegrees16(Req(*s, "angle", where + " start"), where + " start.angle");
        const Value& grid = ArrayOf(Req(*s, "grid", where + " start"), where + " start.grid");
        if (grid.Size() > 16) Fail(where + " start.grid", "at most 16 slots");
        for (size_t i = 0; i < grid.Size(); i++) {
            const Value& p = ArrayOf(grid.At(i), where + " start.grid", 3);
            for (size_t k = 0; k < 3; k++) t.startGrid[i][k] = FromMetres16(p.At(k), where + " start.grid");
        }
    }
    if (const Value* uv = root.Get("uvTable")) {
        f.blocks.uvTable = true;
        ArrayOf(*uv, where + " uvTable");
        for (size_t i = 0; i < uv->Size(); i++) {
            const std::string w = where + " uvTable[" + std::to_string(i) + "]";
            const Value& a = ArrayOf(uv->At(i), w, 21);
            TrackUvEntry e;
            size_t at = 0;
            for (TrackUvSet* s : {&e.nearSet, &e.farSet}) {
                for (size_t k = 0; k < 4; k++) {
                    s->u[k] = uint8_t(IntIn(a.At(at++), 0, 255, w));
                    s->v[k] = uint8_t(IntIn(a.At(at++), 0, 255, w));
                }
                s->clut = uint16_t(IntIn(a.At(at++), 0, 0xFFFF, w));
                s->tpage = uint16_t(IntIn(a.At(at++), 0, 0xFFFF, w));
                if (s == &e.nearSet) e.extra = uint32_t(IntIn(a.At(at++), 0, 0xFFFFFFFFll, w));
            }
            t.uvTable.push_back(e);
        }
    }
    if (const Value* chunks = root.Get("chunks")) {
        f.blocks.chunks = true;
        ArrayOf(*chunks, where + " chunks");
        t.courseLength = FromMetres16(Req(root, "courseLength", where), where + " courseLength");
        t.lengthMetres = int16_t(t.courseLength >> 16);
        for (size_t i = 0; i < chunks->Size(); i++) t.chunks.push_back(ChunkFromJson(chunks->At(i), where + " chunks[" + std::to_string(i) + "]", f.warnings));
    } else if (root.Get("courseLength")) {
        f.warnings.push_back("courseLength without chunks ignored (the length belongs to the chunk block)");
    }
    if (const Value* s = root.Get("scenery")) {
        WarnUnknownKeys(*s, {"models", "lods", "instances"}, "scenery", f.warnings);
        f.blocks.scenery = true;
        const Value& models = ArrayOf(Req(*s, "models", where + " scenery"), where + " scenery.models");
        for (size_t i = 0; i < models.Size(); i++) t.sceneryModels.push_back(ModelFromJson(models.At(i), where + " scenery.models[" + std::to_string(i) + "]", f.warnings));
        const Value& lods = ArrayOf(Req(*s, "lods", where + " scenery"), where + " scenery.lods");
        for (size_t i = 0; i < lods.Size(); i++) {
            const std::string w = where + " scenery.lods[" + std::to_string(i) + "]";
            const Value& l = ArrayOf(lods.At(i), w);
            std::vector<TrackLodEntry> list;
            for (size_t k = 0; k < l.Size(); k++) {
                const Value& e = ArrayOf(l.At(k), w, 2);
                list.push_back({uint32_t(IntIn(e.At(0), 0, 0xFFFFFFFFll, w)), uint16_t(IntIn(e.At(1), 0, 0xFFFF, w))});
            }
            t.sceneryLods.push_back(std::move(list));
        }
        const Value& instances = ArrayOf(Req(*s, "instances", where + " scenery"), where + " scenery.instances");
        for (size_t i = 0; i < instances.Size(); i++)
            t.sceneryInstances.push_back(InstanceFromJson(instances.At(i), where + " scenery.instances[" + std::to_string(i) + "]",
                                                          "scenery.instances[" + std::to_string(i) + "]", f.warnings));
        // The game walks the 33 lists in order: keep the file's order within a list.
        std::stable_sort(t.sceneryInstances.begin(), t.sceneryInstances.end(), [](const TrackSceneryInstance& a, const TrackSceneryInstance& b) { return a.list < b.list; });
    }
    if (const Value* r = root.Get("race")) {
        WarnUnknownKeys(*r, {"startLines", "listCount", "lists"}, "race", f.warnings);
        f.blocks.race = true;
        const Value& lines = ArrayOf(Req(*r, "startLines", where + " race"), where + " race.startLines");
        if (lines.Size() > 12) Fail(where + " race.startLines", "at most 12 start lines");
        for (size_t i = 0; i < lines.Size(); i++) f.race.startLineDistances.push_back(FromMetres16(lines.At(i), where + " race.startLines"));
        f.race.listCount = uint32_t(IntIn(Req(*r, "listCount", where + " race"), 0, 7, where + " race.listCount"));
        const Value& lists = ArrayOf(Req(*r, "lists", where + " race"), where + " race.lists");
        if (lists.Size() > 7) Fail(where + " race.lists", "at most 7 lists");
        for (size_t i = 0; i < lists.Size(); i++) {
            if (lists.At(i).IsNull()) continue;
            if (i >= f.race.listCount) Fail(where + " race.lists", "list " + std::to_string(i) + " beyond listCount");
            const Value& l = ArrayOf(lists.At(i), where + " race.lists");
            f.race.present[i] = true;
            for (size_t k = 0; k < l.Size(); k++)
                f.race.lists[i].push_back(RaceRecordFromJson(l.At(k), where + " race.lists[" + std::to_string(i) + "][" + std::to_string(k) + "]"));
        }
    }
    if (const Value* c = root.Get("replayCameras")) {
        f.blocks.cameras = true;
        if (!c->IsNull()) {
            WarnUnknownKeys(*c, {"lapModulus", "records"}, "replayCameras", f.warnings);
            f.cameras.present = true;
            f.cameras.lapModulus = uint16_t(IntIn(Req(*c, "lapModulus", where), 0, 0xFFFF, where + " replayCameras.lapModulus"));
            const Value& records = ArrayOf(Req(*c, "records", where), where + " replayCameras.records");
            for (size_t i = 0; i < records.Size(); i++) {
                const std::string w = where + " replayCameras.records[" + std::to_string(i) + "]";
                const Value& o = records.At(i);
                WarnUnknownKeys(o, {"kind", "flags", "laps", "chunk", "start", "end", "data"}, "replayCameras.records[" + std::to_string(i) + "]", f.warnings);
                CourseCameraRecord r;
                r.flags = uint16_t(IntIn(Req(o, "flags", w), 0, 0xFFFF, w + ".flags"));
                if (const Value* k = o.Get("kind"))
                    if (IntIn(*k, 0, 15, w + ".kind") != (r.flags & 0xF)) Fail(w, "kind differs from flags & 15");
                const Value& laps = ArrayOf(Req(o, "laps", w), w + ".laps", 2);
                r.firstLap = uint16_t(IntIn(laps.At(0), 0, 0xFFFF, w + ".laps"));
                r.lastLap = uint16_t(IntIn(laps.At(1), 0, 0xFFFF, w + ".laps"));
                r.chunk = uint16_t(IntIn(Req(o, "chunk", w), 0, 0xFFFF, w + ".chunk"));
                r.start = FromMetres16(Req(o, "start", w), w + ".start");
                r.end = FromMetres16(Req(o, "end", w), w + ".end");
                r.data = FromHex(Req(o, "data", w), w + ".data");
                f.cameras.records.push_back(std::move(r));
            }
        }
    }
    if (const Value* add = root.Get("addSceneryInstances")) {
        ArrayOf(*add, where + " addSceneryInstances");
        for (size_t i = 0; i < add->Size(); i++)
            f.addInstances.push_back(InstanceFromJson(add->At(i), where + " addSceneryInstances[" + std::to_string(i) + "]",
                                                      "addSceneryInstances[" + std::to_string(i) + "]", f.warnings));
    }
    if (const Value* objects = root.Get("objects")) {
        ArrayOf(*objects, where + " objects");
        for (size_t i = 0; i < objects->Size(); i++) {
            const std::string w = where + " objects[" + std::to_string(i) + "]";
            const Value& o = objects->At(i);
            WarnUnknownKeys(o, {"mesh", "scale", "position", "rotation", "shadow"}, "objects[" + std::to_string(i) + "]", f.warnings);
            CourseObject ob;
            if (!Req(o, "mesh", w).IsString()) Fail(w + ".mesh", "expected a file name");
            ob.mesh = o.Require("mesh").AsString();
            ob.scale = o.DoubleOr("scale", 1.0);
            const Value& pos = ArrayOf(Req(o, "position", w), w + ".position", 3);
            for (size_t k = 0; k < 3; k++) ob.position[k] = pos.At(k).AsDouble();
            if (const Value* r = o.Get("rotation")) {
                ArrayOf(*r, w + ".rotation", 3);
                for (size_t k = 0; k < 3; k++) ob.rotation[k] = r->At(k).AsDouble();
            }
            ob.shadow = o.BoolOr("shadow", false);
            f.objects.push_back(std::move(ob));
        }
    }
    // Chunk billboards take their texture words from the UV table (the parser does the same: 0x80020110).
    if (f.blocks.chunks && f.blocks.uvTable)
        for (TrackChunk& c : t.chunks)
            for (TrackBillboard& b : c.billboards) {
                if (b.uvIndex >= t.uvTable.size()) Fail(where, "billboard UV index out of range");
                const TrackUvSet& s = t.uvTable[b.uvIndex].nearSet;
                b.u = s.u;
                b.v = s.v;
                b.clut = s.clut;
                b.tpage = s.tpage;
            }
    if (f.blocks.chunks && f.blocks.uvTable && f.blocks.scenery) ValidateTrack(t);
    return f;
}

// ---------------------------------------------------------------- resolution

ResolvedCourse ResolveCourseFile(const GtfsVolume& vol, const CourseFile& f, const std::string& jsonPath) {
    ResolvedCourse r;
    r.course = f.course;
    const std::filesystem::path dir = std::filesystem::path(jsonPath).parent_path();
    std::string base = f.baseCourse;
    auto discCourse = [&](const std::string& name) {
        return !name.empty() && (vol.Find("crsobj/" + name + ".tro.gz") != nullptr || vol.Find("crsobj/" + name + ".tro") != nullptr);
    };
    if (base.empty() && discCourse(f.course)) base = f.course;
    r.baseCourse = base;
    if (!f.blocks.All() && base.empty()) throw std::runtime_error(jsonPath + ": a partial course file needs \"baseCourse\" (a disc course)");
    CourseFile disc;
    if (!base.empty()) {
        if (!discCourse(base)) throw std::runtime_error(jsonPath + ": baseCourse \"" + base + "\" is not a course of the disc");
        disc = MakeCourseFile(vol, base);
        DiscCourseInfo(vol, base, &r.baseIndex);
    }
    // Blocks: the file's, else the base course's.
    const Track& ft = f.track;
    Track& t = r.track;
    t = disc.track;
    if (f.blocks.start) { t.startAngle = ft.startAngle; t.startGrid = ft.startGrid; }
    if (f.blocks.uvTable) t.uvTable = ft.uvTable;
    if (f.blocks.chunks) { t.chunks = ft.chunks; t.courseLength = ft.courseLength; t.lengthMetres = ft.lengthMetres; }
    if (f.blocks.scenery) { t.sceneryModels = ft.sceneryModels; t.sceneryLods = ft.sceneryLods; t.sceneryInstances = ft.sceneryInstances; }
    if (!f.addInstances.empty()) { // appended to their lists (the game walks the 33 lists in order)
        t.sceneryInstances.insert(t.sceneryInstances.end(), f.addInstances.begin(), f.addInstances.end());
        std::stable_sort(t.sceneryInstances.begin(), t.sceneryInstances.end(), [](const TrackSceneryInstance& a, const TrackSceneryInstance& b) { return a.list < b.list; });
    }
    // Chunk billboards follow the resolved UV table.
    for (TrackChunk& c : t.chunks)
        for (TrackBillboard& b : c.billboards)
            if (b.uvIndex < t.uvTable.size()) {
                const TrackUvSet& s = t.uvTable[b.uvIndex].nearSet;
                b.u = s.u; b.v = s.v; b.clut = s.clut; b.tpage = s.tpage;
            }
    ValidateTrack(t);
    r.race = f.blocks.race ? f.race : disc.race;
    r.cameras = f.blocks.cameras ? f.cameras : disc.cameras;
    r.info = disc.info;
    if (f.blocks.info) { // key by key over the base course's entry
        const CourseInfo& fi = f.info;
        if (f.infoKeys.name) r.info.name = fi.name;
        if (f.infoKeys.flags) r.info.flags = fi.flags;
        if (f.infoKeys.backdrop) r.info.backdrop = fi.backdrop;
        if (f.infoKeys.tail) r.info.tail = fi.tail;
        r.info.hasEntry = r.info.hasEntry || fi.hasEntry;
    }
    for (const CourseCameraRecord& c : r.cameras.records)
        if (c.chunk >= t.chunks.size()) throw std::runtime_error(jsonPath + ": replay camera chunk out of range");
    // Textures: an own TIM pack file, else a disc course's .trp.
    if (!f.texturePack.empty()) {
        const std::filesystem::path p = dir / f.texturePack;
        std::ifstream in(p, std::ios::binary);
        if (!in) throw std::runtime_error(jsonPath + ": cannot open texture pack " + p.string());
        r.texturePack.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        r.textureSource = p.string();
    } else {
        const std::string tc = !f.textureCourse.empty() ? f.textureCourse : base;
        if (tc.empty()) throw std::runtime_error(jsonPath + ": no texture source (assets.texturePack / assets.textureCourse / baseCourse)");
        r.texturePack = vol.Read("crsobj/" + tc + ".trp");
        r.textureSource = "disc crsobj/" + tc + ".trp";
    }
    r.courseMap = !f.courseMap.empty() ? f.courseMap : base;
    for (const CourseObject& o : f.objects) {
        CourseObject ob = o;
        ob.meshPath = (dir / o.mesh).string();
        ob.loaded = std::make_shared<GltfMesh>(ReadGltf(ob.meshPath));
        for (const std::string& w : ob.loaded->warnings) r.notes.push_back("object " + o.mesh + ": " + w);
        r.objects.push_back(std::move(ob));
    }
    if (!r.info.backdrop.empty()) {
        bool found = false;
        for (const std::string& p : BackdropPaths(vol)) found |= p == "bgsobj/" + r.info.backdrop + ".bso.gz";
        if (!found) throw std::runtime_error(jsonPath + ": backdrop \"" + r.info.backdrop + "\" is not a bgsobj/*.bso of the disc");
    }
    return r;
}

// ---------------------------------------------------------------- diff

std::vector<std::string> DiffCourses(const Track& a, const TrackRaceData& ra, const Track& b, const TrackRaceData& rb, size_t limit) {
    std::vector<std::string> out;
    auto add = [&](const std::string& s) { if (out.size() < limit) out.push_back(s); };
    if (a.courseLength != b.courseLength) add("courseLength " + std::to_string(a.courseLength / 65536.0) + " -> " + std::to_string(b.courseLength / 65536.0) + " m");
    if (a.startAngle != b.startAngle || a.startGrid != b.startGrid) add("start (angle / grid) differs");
    if (a.uvTable.size() != b.uvTable.size()) add("uvTable " + std::to_string(a.uvTable.size()) + " -> " + std::to_string(b.uvTable.size()) + " entries");
    else {
        Track x, y;
        x.uvTable = a.uvTable;
        y.uvTable = b.uvTable;
        if (CanonicalTrackBytes(x) != CanonicalTrackBytes(y)) add("uvTable entries differ");
    }
    if (a.chunks.size() != b.chunks.size()) add("chunks " + std::to_string(a.chunks.size()) + " -> " + std::to_string(b.chunks.size()));
    for (size_t i = 0; i < std::min(a.chunks.size(), b.chunks.size()); i++) {
        Track x, y;
        x.chunks.push_back(a.chunks[i]);
        y.chunks.push_back(b.chunks[i]);
        if (CanonicalTrackBytes(x) == CanonicalTrackBytes(y)) continue;
        const TrackChunk& p = a.chunks[i];
        const TrackChunk& q = b.chunks[i];
        std::string what;
        auto shapeDiff = [](const TrackShape& s, const TrackShape& t) {
            Track u, v;
            u.chunks.resize(1); v.chunks.resize(1);
            u.chunks[0].road = s; v.chunks[0].road = t;
            return CanonicalTrackBytes(u) != CanonicalTrackBytes(v);
        };
        if (shapeDiff(p.road, q.road)) {
            size_t moved = 0;
            for (size_t k = 0; k < std::min(p.road.vertices.size(), q.road.vertices.size()); k++)
                moved += (p.road.vertices[k].x != q.road.vertices[k].x || p.road.vertices[k].y != q.road.vertices[k].y || p.road.vertices[k].z != q.road.vertices[k].z) ? 1 : 0;
            what += " road (" + std::to_string(moved) + " vertices moved, polygons " + std::to_string(p.road.polygons.size()) + " -> " + std::to_string(q.road.polygons.size()) + ")";
        }
        if (shapeDiff(p.surround, q.surround)) what += " mirror";
        Track bw1, bw2;
        bw1.chunks.resize(1); bw2.chunks.resize(1);
        bw1.chunks[0].boundaries = p.boundaries; bw2.chunks[0].boundaries = q.boundaries;
        if (CanonicalTrackBytes(bw1) != CanonicalTrackBytes(bw2)) what += " walls";
        if (p.distance != q.distance || p.weightNext != q.weightNext || p.weightThis != q.weightThis || p.prev != q.prev || p.next != q.next) what += " chain/distance";
        if (p.origin != q.origin || p.centre != q.centre || p.direction != q.direction || p.vcoord != q.vcoord) what += " placement";
        if (p.renderList != q.renderList || p.sceneryMask != q.sceneryMask) what += " visibility";
        if (what.empty()) what = " other fields (grid / billboards)";
        add("chunk " + std::to_string(i) + ":" + what);
    }
    if (a.sceneryModels.size() != b.sceneryModels.size()) add("scenery models " + std::to_string(a.sceneryModels.size()) + " -> " + std::to_string(b.sceneryModels.size()));
    if (a.sceneryInstances.size() != b.sceneryInstances.size())
        add("scenery instances " + std::to_string(a.sceneryInstances.size()) + " -> " + std::to_string(b.sceneryInstances.size()));
    else
        for (size_t i = 0; i < a.sceneryInstances.size(); i++) {
            const TrackSceneryInstance& p = a.sceneryInstances[i];
            const TrackSceneryInstance& q = b.sceneryInstances[i];
            if (p.position != q.position || p.angle != q.angle || p.scale != q.scale || p.lodList != q.lodList || p.list != q.list || p.lodDivisor != q.lodDivisor)
                add("scenery instance " + std::to_string(i) + " differs");
        }
    {
        Track x, y;
        x.sceneryModels = a.sceneryModels; x.sceneryLods = a.sceneryLods;
        y.sceneryModels = b.sceneryModels; y.sceneryLods = b.sceneryLods;
        if (a.sceneryModels.size() == b.sceneryModels.size() && CanonicalTrackBytes(x) != CanonicalTrackBytes(y)) add("scenery models / LOD lists differ");
    }
    if (CanonicalRaceBytes(ra) != CanonicalRaceBytes(rb)) {
        if (ra.startLineDistances != rb.startLineDistances) add("race: start lines differ");
        for (size_t i = 0; i < 7; i++)
            if (ra.present[i] != rb.present[i] || ra.lists[i].size() != rb.lists[i].size() ||
                (ra.lists[i].size() && std::memcmp(ra.lists[i].data(), rb.lists[i].data(), ra.lists[i].size() * sizeof(TrackRaceRecord)) != 0))
                add("race: list " + std::to_string(i) + " differs");
    }
    return out;
}

} // namespace gt2
