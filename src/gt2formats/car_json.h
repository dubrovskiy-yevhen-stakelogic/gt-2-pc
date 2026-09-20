#pragma once
// The editable car file (docs/formats/car_json.md): one JSON per car with the car's configuration
// (gt2::CarConfig - part rows and menu settings), the resulting race record (sim::CarParams - what the physics
// setup reads) and the descriptive data a mod needs (mesh, wheel geometry, sound set). Both records are
// described by field tables (CarParamsFields / CarConfigFields) that drive the writer, the reader, the
// override layer and the diff, so that a round trip is byte-identical and a partial file overrides exactly
// the fields it names. No dependency beyond json.h.
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "game/sim/car_setup.h"
#include "gt2formats/car_params.h"

namespace gt2 {

// ---------------------------------------------------------------- field tables

enum class CarFieldKind : uint8_t { U8, S8, U16, S16, Hex };

struct CarFieldDesc {
    const char* path;      // dotted JSON path inside "params" / "config", e.g. "engine.torque"
    uint16_t offset;       // byte offset in the record
    CarFieldKind kind;
    uint16_t count;        // elements (Hex: bytes, written as one hex string)
    const char* const* enumNames = nullptr; // optional value -> name table (index = value), nullptr = numbers only
    uint8_t enumCount = 0;
    size_t ElementSize() const { return kind == CarFieldKind::U16 || kind == CarFieldKind::S16 ? 2 : 1; }
};

std::span<const CarFieldDesc> CarParamsFields(); // covers all 0x1C0 bytes of sim::CarParams exactly once
std::span<const CarFieldDesc> CarConfigFields(); // covers all 0x84 bytes of gt2::CarConfig exactly once

// Throws std::logic_error unless both tables cover their records exactly once (called by the tools at start).
void CheckCarJsonDescriptors();

// Which fields (by index into the field table) a file named.
struct CarFieldMask {
    std::vector<bool> present;
    bool Any() const;
    bool All() const;
    void SetAll(size_t count, bool value);
};

// ---------------------------------------------------------------- the file

struct CarJsonPaint {
    uint8_t id = 0;         // paint id of the .cdp / .carinfo
    uint16_t chipColor = 0; // PS1 15-bit colour chip of .carinfo (informational)
};

struct CarJson {
    int version = 1;
    std::string carId;      // the id this file describes (file name without .json for mods)
    std::string baseCar;    // optional: the disc car whose tables build the base record (mods; default = carId when it is a disc car)
    std::string name;       // display name (informational)
    std::string modelId;    // body model id of the configured racing-modify row (informational)
    std::vector<CarJsonPaint> paints;

    // "mesh": an external glTF body (mods). Absent = the disc's carobj/<id>.cdo.
    bool hasMesh = false;
    std::string meshFile;   // relative to the JSON's directory
    double meshScale = 1.0; // multiplies the glTF positions (metres after scaling)
    uint32_t meshPaint = 0; // paint index the exported texture was decoded with (informational)
    // "reflection": which triangles of the mesh take the game's reflection pass (the environment map, like the .cdo
    // polygons with renderFlags bit 15): "materials" (default: the materials whose extras say gt2pc.reflection), "all",
    // "none". The pass needs vertex normals (NORMAL; face normals are used when the mesh has none).
    enum class MeshReflection : uint8_t { None, Materials, All };
    MeshReflection meshReflection = MeshReflection::Materials;

    // "body": wheel geometry in metres, car axes (+X right, +Y up, -Z front); the left wheels, right side mirrored.
    bool hasBody = false;
    std::array<double, 3> wheelFront{}, wheelRear{};
    std::array<double, 2> wheelRadius{}, wheelWidth{}; // front, rear

    // "sound": the engine sound set (engine/<set>_*.es), exhaust byte (0..3, +4 turbo), turbo flag.
    bool hasSound = false;
    uint16_t engineSoundId = 0;
    uint8_t exhaustByte = 0;
    bool turbo = false;

    bool hasConfig = false;
    CarConfig config{};
    CarFieldMask configMask;
    bool hasParams = false;
    sim::CarParams params{};
    CarFieldMask paramsMask;

    std::vector<std::string> warnings; // unknown keys and other non-fatal findings of the reader
};

// A complete file for a disc car: every field of both records present.
CarJson MakeCarJson(const std::string& carId, const CarConfig& config, const sim::CarParams& params);

void WriteCarJson(const std::string& path, const CarJson& car);
void WriteCarJson(const std::string& path, const std::string& carId, const CarConfig& config, const sim::CarParams& params);

// Throws std::runtime_error with the path and the offending key on malformed input.
CarJson ReadCarJson(const std::string& path);

// The override layer: copies the fields the file names onto `target`, leaves the rest.
void ApplyParamOverrides(sim::CarParams& target, const CarJson& car);
void ApplyConfigOverrides(CarConfig& target, const CarJson& car);

// Human-readable per-element differences ("engine.torque[3]: 100 -> 120").
std::vector<std::string> DiffCarParams(const sim::CarParams& before, const sim::CarParams& after);
std::vector<std::string> DiffCarConfig(const CarConfig& before, const CarConfig& after);

// The body-model inputs of the record builder for an external mesh: bounding box z extent (metres, -Z front)
// and the lateral (x) position of the front / rear left wheel centres (metres); the same derivation the
// original applies to the .cdo (car_params.h CarBodyDimensions, 1/4096 m units with scale shift 16).
CarBodyDimensions BodyDimensionsOfMesh(double minZ, double maxZ, double wheelLateralFront, double wheelLateralRear);

// ---------------------------------------------------------------- resolution over the disc

class GtfsVolume;
struct GltfMesh;

// A car file resolved against the disc's tables with the precedence of docs/formats/car_json.md:
// base car's stock config < the file's "config" fields -> record built by the game's builder (body dimensions
// from the .cdo, or from the external mesh) < the file's "params" fields. Without a base car (an id that is not
// in the tables and no "baseCar") the record is the file's "params" alone.
struct ResolvedCar {
    std::string baseCar;            // empty = stand-alone file
    std::string modelId;            // .cdo body model to draw when there is no external mesh (the configured racing-modify model)
    CarConfig stockConfig{}, config{};
    sim::CarParams stockParams{}, params{};
    bool externalMesh = false;
    std::string meshPath;           // absolute / relative path of the glTF when externalMesh
    std::shared_ptr<GltfMesh> mesh; // loaded when externalMesh
    double meshScale = 1.0;         // the file's mesh.scale
    CarJson::MeshReflection meshReflection = CarJson::MeshReflection::Materials; // the file's mesh.reflection
    CarBodyDimensions dims{};
    // Wheel geometry for the renderer (metres; left wheels) - the file's "body" block or the .cdo / a bounding-box estimate.
    std::array<double, 3> wheelFront{}, wheelRear{};
    std::array<double, 2> wheelRadius{}, wheelWidth{};
    uint16_t engineSoundId = 0;
    uint8_t exhaustByte = 0;
    bool turbo = false;
    std::vector<std::string> notes;  // what was derived / defaulted (printed by the tools)
};
ResolvedCar ResolveCarJson(const GtfsVolume& vol, const CarParamTables& tables, const CarJson& car, const std::string& jsonPath);

} // namespace gt2
