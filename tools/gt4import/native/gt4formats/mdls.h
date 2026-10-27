// GT4 'MDLS' model sets (ModelSet2). Format: docs/formats/mdls.md.
// Independent re-implementation of scripts/gt4_mdls.py (agreement checked by `gt4tool digest`).
#pragma once

#include <array>
#include <map>
#include <optional>
#include <vector>

#include "gt4formats/tex1.h"

namespace gt4fmt {

// Raw contents of the VU1 slots a vertex uses after the VIF unpacks of its packet.
struct VuValue {
    std::array<std::uint32_t, 4> v{};
    std::uint8_t vn = 0;     // components of the unpack that last wrote X (0 = never written)
    std::uint8_t bits = 0;   // 32 / 16 / 8
    std::uint8_t usn = 0;    // 1 = unsigned (with 32 bits: IEEE float), 0 = signed integer
    std::uint8_t mask = 0;   // bit e = component e written
};

struct VertexRaw {
    VuValue pos;   // slot i        (0x00..)
    VuValue bank1; // slot 0x40 + i (UV V2, or normal V3 for vertex type 2)
    VuValue bank2; // slot 0x80 + i (normal V3 or colour V4-8)
};
struct SkinVertex {
    std::array<uint8_t,4> bones{};
    std::array<uint32_t,4> weights{}, position{}, normal{};
};

struct VifPacket {
    std::uint32_t offset = 0;  // absolute offset in the input data
    std::uint16_t qwc = 0, tex = 0, mat = 0;
    std::array<std::uint32_t, 3> gif{};  // GIF tag written to 0xC0 (NLOOP|EOP, PRE/PRIM/FLG/NREG, REGS)
    std::uint32_t nloop = 0;
    std::vector<std::uint8_t> strips;    // strip lengths (vertices)
    std::vector<VertexRaw> verts;        // nloop entries
    uint32_t skinScale = 0x3f800000;
    bool skinPacked = false, skinNormal = false;
    std::vector<SkinVertex> skin; // original0049da60 input stream after qwc VIF quadwords
    std::vector<std::uint8_t> canon;     // canonical stream of this packet (mdls.md)
};
// Standalone native-generated display packets use the same data encoding as MDLS shapes.
VifPacket decodeVifPacket(Bytes data, std::size_t offset, std::uint32_t qwords, std::uint32_t initialMask);

struct Shape {
    std::uint32_t offset = 0;
    std::uint32_t scaleBits = 0;  // +4: f32 position scale for packed positions (0 = float positions)
    std::uint8_t flags8 = 0, flags9 = 0;
    std::uint16_t totalVerts = 0, totalTris = 0;
    std::vector<VifPacket> packets;
    float scale() const { return bitsToFloat(scaleBits); }
};

struct Command {
    std::uint32_t offset;  // absolute offset in the input data
    std::uint8_t op;
    std::uint16_t length;  // including the opcode byte
};

struct Model {
    std::uint8_t unk0 = 0, nBounds = 0, unk2 = 0, unk3 = 0;
    float radius = 0;
    std::array<float, 3> origin{};
    std::uint32_t cmdOffset = 0;  // relative to the MDLS start, 0 = none
    std::array<std::int32_t, 3> vm{};
    std::vector<std::array<float, 3>> bounds;
    std::vector<Command> commands;  // every reachable command, sorted by offset
};

struct Material {
    std::array<float, 16> colors;  // 4 x RGBA (ambient/diffuse/specular/emission-like, see mdls.md)
    float f40;
    std::uint32_t flags;
    float f48, f4c;
};
struct ModelBinding {
    std::array<uint32_t,16> matrix{};
    uint32_t paletteIndex = 0;
    std::string name;
};
struct ModelSymbol { std::string name; uint32_t model = 0; };

struct ModelSet {
    std::size_t base = 0;  // offset of the MDLS in the input data
    std::uint32_t size = 0;
    std::uint8_t unk14 = 0, instanceFlags = 0, nVariations = 0, curVariation = 0;
    std::uint16_t nTexsets = 0, nTexlists = 0, nBindMatrices = 0;
    std::array<std::uint32_t, 18> ptrs{};  // +0x38 .. +0x7C
    std::vector<Material> materials;
    std::vector<std::vector<Material>> variations;
    std::vector<Model> models;
    std::vector<ModelBinding> bindings; // +48,80-byte entries,00454970
    std::vector<ModelSymbol> symbols; // +58,8-byte entries,004548a8
    std::vector<std::uint32_t> shapeOffsets;  // relative to the MDLS start
    std::vector<Shape> shapes;                // empty when parsed without shapes
    std::vector<std::vector<std::optional<Tex1>>> texsets;  // [list][index]
    std::map<std::uint32_t, std::vector<std::uint32_t>> params;  // +0x68 tagged list
};

ModelSet parseModelSet(Bytes data, std::size_t offset, bool decodeShapes = true);

// Render-command helpers (FUN_00455708).
std::size_t commandLength(Bytes data, std::size_t p);

// Render state tracked by the static walk. Defaults = ModelSet2::begin FUN_00455370: alpha test on with
// GS TEST.ATST 6 (GREATER) ref 0x20, ALPHA 0x44 ((Cs-Cd)*As+Cd), FBMSK 0, depth write on.
struct Draw {
    std::uint32_t shape;
    int texset;  // -1 = none bound yet
    int model;
    int blend0 = 0x44, blend1 = 0;   // cmd 0x20: GS ALPHA register byte, FIX
    bool alphaTest = true, depthMask = true;
    int alphaFunc = 6, alphaRef = 0x20;  // cmd 0x1C: TEST.ATST, TEST.AREF
    int alphaFail = 0;                   // cmd 0x2F: TEST.AFAIL
    std::uint32_t fbmsk = 0;             // cmd 0x24 operand = FBMSK (FUN_004a2980 stores ~arg; cmd passes ~u32)
    int cull = -1;                       // cmd 0x2D/0x2E: 1 / 0, -1 = untouched
    bool environmentDefault = true;      // cmd0x56 default factors;0x57 disables Fresnel ramp
    // Model matrix from the render commands (FUN_00455708 cases 0x0C/0x0D push/pop, 0x0F load, 0x10 multiply,
    // 0x11 translate, 0x12 scale, 0x14-0x16 rotate X/Y/Z), row-vector convention as VU0 vf28..vf31:
    // p' = x * row0 + y * row1 + z * row2 + row3, applied to the scaled shape positions. hasMatrix = not identity.
    bool hasMatrix = false;
    std::array<float, 16> matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};
// wheel = the set is drawn as a CarModel wheel: callback_branch ids select the texture set placed in external
// table slot 2 (GT4Model::CarModel::WheelCallback 0x00453e68 via FUN_004568f0(2, ...)); external slot 2 then binds
// that set. Other external tables (slot 0 = environment map) give texset -2.
std::vector<Draw> drawList(Bytes data, const ModelSet& ms, int model, int lod = 0, int texlist = 0, int depth = 0,
                           bool wheel = false);
// The same with CarModel callback values (callback_branch id -> branch index, 0x30 entries), e.g. id 0x00 = brake
// lamps (RaceCarModel +0x18 +0x38, docs/research/renderer.md "Car lamps").
std::vector<Draw> drawList(Bytes data, const ModelSet& ms, int model, int lod, int texlist, int depth, bool wheel,
                           const int* callbackValues);

// Semantic attributes of a vertex (see mdls.md): missing attributes have has* = false.
struct Vertex {
    bool hasPos = false, hasUv = false, hasNrm = false, hasCol = false;
    float pos[3]{}, uv[2]{}, nrm[3]{};
    std::uint8_t col[4]{};
};
std::vector<Vertex> skinPacket(const VifPacket& packet,
    std::span<const std::array<uint32_t,16>> palette); //0049d580/d728/db08/dcb0 vertex paths
Vertex interpretVertex(const VertexRaw& r, float scale);
std::vector<std::array<std::uint16_t, 3>> stripTriangles(const std::vector<std::uint8_t>& strips);

// Canonical stream of a whole model set (mdls.md "Canonical stream").
void mdlsStream(Bytes data, const ModelSet& ms, ByteWriter& out, DigestStats& stats);

}  // namespace gt4fmt
