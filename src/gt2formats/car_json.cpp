#include "gt2formats/car_json.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>

#include <filesystem>

#include "gt2formats/car_info.h"
#include "gt2formats/gltf_reader.h"
#include "gt2formats/json.h"
#include "gt2vfs/gtfs.h"

namespace gt2 {
namespace {

using json::Value;

constexpr const char* kFormat = "gt2pc-car";
constexpr int kVersion = 1;

// ---------------------------------------------------------------- the tables

constexpr const char* kDriveTypeNames[] = {"rear", "front", "four_wheel", "rear_type3", "rear_type4"};

#define PF(path, member, kind, count) CarFieldDesc{path, uint16_t(offsetof(sim::CarParams, member)), CarFieldKind::kind, count}
#define PFE(path, member, kind, count, names) CarFieldDesc{path, uint16_t(offsetof(sim::CarParams, member)), CarFieldKind::kind, count, names, uint8_t(std::size(names))}
#define PS(path, axle, index) CarFieldDesc{path, uint16_t(offsetof(sim::CarParams, suspension) + axle * 12 + index), CarFieldKind::U8, 1}

const CarFieldDesc kParamsFields[] = {
    // body (mm, kg, percent; the lengths and tracks come from the body model at race start)
    PF("body.frontLength", frontLength, S16, 1),
    PF("body.rearLength", rearLength, S16, 1),
    PF("body.width", width, S16, 1),
    PF("body.height", height, S16, 1),
    PF("body.wheelbase", wheelbase, S16, 1),
    PF("body.frontTrack", frontTrack, S16, 1),
    PF("body.rearTrack", rearTrack, S16, 1),
    PF("body.weightKg", weightKg, S16, 1),
    PF("body.frontWeightPercent", frontWeightPercent, U8, 1),
    PF("body.yawInertiaCode", yawInertiaCode, U8, 1),
    PF("body.pitchInertiaCode", pitchInertiaCode, U8, 1),
    PF("body.rollInertiaCode", rollInertiaCode, U8, 1),
    PF("body.dragCoefficient100", dragCoefficient100, U8, 1),
    PF("body.downforce", downforce, U8, 2),
    PF("body.rideHeightMm", rideHeightMm, U8, 2),
    // engine
    PF("engine.idleRpm10", idleRpm10, U8, 1),
    PF("engine.revLimitRpm100", revLimitRpm100, U8, 1),
    PF("engine.upshiftRpm100", upshiftRpm100, U8, 1),
    PF("engine.downshiftFloorRpm10", downshiftFloorRpm10, U8, 1),
    PF("engine.torquePointCount", torquePointCount, U8, 1),
    PF("engine.torqueRpm100", torqueRpm100, U8, 16),
    PF("engine.torque", torque, U16, 16),
    PF("engine.torqueMultiplier1000", torqueMultiplier1000, U16, 1),
    PF("engine.powerPercent", powerPercent, U8, 1),
    PF("engine.powerPercentTop", powerPercentTop, U16, 1),
    PF("engine.engineInertia", engineInertia, U8, 1),
    PF("engine.engineBrake", engineBrake, U8, 1),
    PF("engine.turboBoostCap10", turboBoostCap10, U8, 1),
    PF("engine.turboSpoolRpm100", turboSpoolRpm100, U8, 1),
    PF("engine.turboBoost10", turboBoost10, U8, 1),
    PF("engine.turboSpoolRate10", turboSpoolRate10, U8, 1),
    PF("engine.turboSpoolRpm100Second", turboSpoolRpm100Second, U8, 1),
    PF("engine.turboBoost10Second", turboBoost10Second, U8, 1),
    PF("engine.turboSpoolRate10Second", turboSpoolRate10Second, U8, 1),
    PF("engine.turboModel", turboModel, U8, 2),
    // gearbox
    PF("gearbox.gearCount", gearCount, U8, 1),
    PF("gearbox.gearRatio", gearRatio, S16, 8),
    PF("gearbox.finalDrive", finalDrive, S16, 1),
    PF("gearbox.gearAutoSet", gearAutoSet, U8, 1),
    PF("gearbox.gearAutoFinal", gearAutoFinal, U8, 1),
    PF("gearbox.clutchCode", clutchCode, U8, 1),
    // drivetrain
    PFE("drivetrain.driveType", driveType, U8, 1, kDriveTypeNames),
    PF("drivetrain.fourWheelType", fourWheelType, U8, 1),
    PF("drivetrain.centreSplitPercent", centreSplitPercent, U8, 1),
    PF("drivetrain.wheelInertia", wheelInertia, U8, 2),
    PF("drivetrain.axleInertiaCode", axleInertiaCode, U8, 2),
    PF("drivetrain.diffTypeCode", diffTypeCode, S8, 2),
    PF("drivetrain.diffInitialTorque", diffInitialTorque, U8, 2),
    PF("drivetrain.diffAccel", diffAccel, U8, 2),
    PF("drivetrain.diffDecel", diffDecel, U8, 2),
    // brakes
    PF("brakes.brakeFront", brakeFront, U8, 1),
    PF("brakes.brakeRear", brakeRear, U8, 1),
    PF("brakes.handbrake", handbrake, U8, 1),
    PF("brakes.absGain", absGain, U8, 2),
    // steering
    PF("steering.steerLockDeg", steerLockDeg, U8, 1),
    PF("steering.steerRateDeg", steerRateDeg, U8, 1),
    PF("steering.steerMaxRateCode", steerMaxRateCode, U8, 1),
    PF("steering.steerLimitCount", steerLimitCount, U8, 1),
    PF("steering.steerLimitXs", steerLimitXs, U8, 6),
    PF("steering.steerLimitYs", steerLimitYs, U8, 6),
    // suspension
    PF("suspension.camberFront10", camberFront10, U8, 1),
    PF("suspension.camberRear10", camberRear10, U8, 1),
    PF("suspension.toeCode", toeCode, U8, 2),
    PF("suspension.damperScaleDivisor", damperScaleDivisor, U8, 2),
    PF("suspension.bumpTravelMm", bumpTravelMm, U8, 2),
    PF("suspension.droopTravelMm", droopTravelMm, U8, 2),
    PS("suspension.front.springCode", 0, 0),
    PS("suspension.front.antiRollCode", 0, 1),
    PS("suspension.front.bumpStopCode", 0, 2),
    PS("suspension.front.unsprungMassKg", 0, 3),
    PS("suspension.front.bumpLowKnee", 0, 4),
    PS("suspension.front.bumpLowForce", 0, 5),
    PS("suspension.front.bumpHighKnee", 0, 6),
    PS("suspension.front.bumpHighForce", 0, 7),
    PS("suspension.front.reboundLowKnee", 0, 8),
    PS("suspension.front.reboundLowForce", 0, 9),
    PS("suspension.front.reboundHighKnee", 0, 10),
    PS("suspension.front.reboundHighForce", 0, 11),
    PS("suspension.rear.springCode", 1, 0),
    PS("suspension.rear.antiRollCode", 1, 1),
    PS("suspension.rear.bumpStopCode", 1, 2),
    PS("suspension.rear.unsprungMassKg", 1, 3),
    PS("suspension.rear.bumpLowKnee", 1, 4),
    PS("suspension.rear.bumpLowForce", 1, 5),
    PS("suspension.rear.bumpHighKnee", 1, 6),
    PS("suspension.rear.bumpHighForce", 1, 7),
    PS("suspension.rear.reboundLowKnee", 1, 8),
    PS("suspension.rear.reboundLowForce", 1, 9),
    PS("suspension.rear.reboundHighKnee", 1, 10),
    PS("suspension.rear.reboundHighForce", 1, 11),
    // tyres
    PF("tyres.tyreWidthCode", tyreWidthCode, U8, 2),
    PF("tyres.rimCode", rimCode, U8, 2),
    PF("tyres.tyreAspectCode", tyreAspectCode, U8, 2),
    PF("tyres.tyreGripPercent", tyreGripPercent, U8, 2),
    PF("tyres.tyreGripModifier", tyreGripModifier, U8, 2),
    PF("tyres.surfaceGripPercent", surfaceGripPercent, U8, 8),
    PF("tyres.front.slipAngleCount", slipAngleCount, U8, 1),
    PF("tyres.front.slipAngleXs", slipAngleXs, U8, 8),
    PF("tyres.front.slipAngleYs", slipAngleYs, U8, 8),
    PF("tyres.front.slipRatioNegCount", slipRatioNegCount, U8, 1),
    PF("tyres.front.slipRatioNegXs", slipRatioNegXs, U8, 6),
    PF("tyres.front.slipRatioNegYs", slipRatioNegYs, U8, 6),
    PF("tyres.front.slipRatioNegYs2", slipRatioNegYs2, U8, 6),
    PF("tyres.front.slipRatioPosCount", slipRatioPosCount, U8, 1),
    PF("tyres.front.slipRatioPosXs", slipRatioPosXs, U8, 6),
    PF("tyres.front.slipRatioPosYs", slipRatioPosYs, U8, 6),
    PF("tyres.front.slipRatioPosYs2", slipRatioPosYs2, U8, 6),
    PF("tyres.front.loadGripCount", loadGripCount, U8, 1),
    PF("tyres.front.loadGripXs", loadGripXs, U8, 4),
    PF("tyres.front.loadGripYs", loadGripYs, U8, 4),
    PF("tyres.front.camberGripCount", camberGripCount, U8, 1),
    PF("tyres.front.camberGripXs", camberGripXs, U8, 4),
    PF("tyres.front.camberGripYs", camberGripYs, U8, 4),
    PF("tyres.rear.slipAngleCount", slipAngleCountRear, U8, 1),
    PF("tyres.rear.slipAngleXs", slipAngleXsRear, U8, 8),
    PF("tyres.rear.slipAngleYs", slipAngleYsRear, U8, 8),
    PF("tyres.rear.slipRatioNegCount", slipRatioNegCountRear, U8, 1),
    PF("tyres.rear.slipRatioNegXs", slipRatioNegXsRear, U8, 6),
    PF("tyres.rear.slipRatioNegYs", slipRatioNegYsRear, U8, 6),
    PF("tyres.rear.slipRatioNegYs2", slipRatioNegYs2Rear, U8, 6),
    PF("tyres.rear.slipRatioPosCount", slipRatioPosCountRear, U8, 1),
    PF("tyres.rear.slipRatioPosXs", slipRatioPosXsRear, U8, 6),
    PF("tyres.rear.slipRatioPosYs", slipRatioPosYsRear, U8, 6),
    PF("tyres.rear.slipRatioPosYs2", slipRatioPosYs2Rear, U8, 6),
    PF("tyres.rear.loadGripCount", loadGripCountRear, U8, 1),
    PF("tyres.rear.loadGripXs", loadGripXsRear, U8, 4),
    PF("tyres.rear.loadGripYs", loadGripYsRear, U8, 4),
    PF("tyres.rear.camberGripCount", camberGripCountRear, U8, 1),
    PF("tyres.rear.camberGripXs", camberGripXsRear, U8, 4),
    PF("tyres.rear.camberGripYs", camberGripYsRear, U8, 4),
    // driver aids
    PF("assists.tcsGain10", tcsGain10, U8, 1),
    PF("assists.tcsFalloffGain10", tcsFalloffGain10, U8, 1),
    PF("assists.tcsSteerGain100", tcsSteerGain100, U8, 1),
    PF("assists.asmYawGain100", asmYawGain100, U8, 1),
    PF("assists.asmYawThreshold100", asmYawThreshold100, U8, 1),
    // bytes the setup does not read (kept for the byte-identical round trip)
    PF("raw.reserved000", reserved000, Hex, 8),
    PF("raw.reserved030", reserved030, Hex, 1),
    PF("raw.reserved033", reserved033, Hex, 1),
    PF("raw.reserved052", reserved052, Hex, 4),
    PF("raw.reserved05C", reserved05C, Hex, 1),
    PF("raw.reserved08C", reserved08C, Hex, 2),
    PF("raw.reserved0FC", reserved0FC, Hex, 0x1A),
    PF("raw.reserved15E", reserved15E, Hex, 0x1A),
    PF("raw.reserved180", reserved180, Hex, 2),
    PF("raw.reserved190", reserved190, Hex, 2),
    PF("raw.reserved19A", reserved19A, Hex, 2),
    PF("raw.reserved1A7", reserved1A7, Hex, 3),
    PF("raw.reserved1AD", reserved1AD, Hex, 1),
    PF("raw.reserved1B2", reserved1B2, Hex, 0x0E),
};
#undef PF
#undef PFE
#undef PS

#define CF(path, member, kind, count) CarFieldDesc{path, uint16_t(offsetof(CarConfig, member)), CarFieldKind::kind, count}
const CarFieldDesc kConfigFields[] = {
    // rows of the part tables (car_params.h CarParamTable)
    CF("parts.brakes", brakes, U16, 1),
    CF("parts.brakeController", brakeController, U16, 1),
    CF("parts.steering", steering, U16, 1),
    CF("parts.chassis", chassis, U16, 1),
    CF("parts.engine", engine, U16, 1),
    CF("parts.drivetrain", drivetrain, U16, 1),
    CF("parts.gearbox", gearbox, U16, 1),
    CF("parts.suspension", suspension, U16, 1),
    CF("parts.lsd", lsd, U16, 1),
    CF("parts.tyresFront", tyresFront, U16, 1),
    CF("parts.tyresRear", tyresRear, U16, 1),
    CF("parts.lightweight", lightweight, U16, 1),
    CF("parts.racingModify", racingModify, U16, 1),
    CF("parts.portPolish", portPolish, U16, 1),
    CF("parts.engineBalance", engineBalance, U16, 1),
    CF("parts.displacement", displacement, U16, 1),
    CF("parts.computer", computer, U16, 1),
    CF("parts.naTune", naTune, U16, 1),
    CF("parts.turboKit", turboKit, U16, 1),
    CF("parts.flywheel", flywheel, U16, 1),
    CF("parts.clutch", clutch, U16, 1),
    CF("parts.propellerShaft", propellerShaft, U16, 1),
    CF("parts.muffler", muffler, U16, 1),
    CF("parts.intercooler", intercooler, U16, 1),
    CF("parts.activeStability", activeStability, U16, 1),
    CF("parts.tractionControl", tractionControl, U16, 1),
    // the menu settings
    CF("settings.torqueMultiplier100", torqueMultiplier100, U16, 1),
    CF("settings.gearRatio", gearRatio, S16, 8),
    CF("settings.finalDrive", finalDrive, S16, 1),
    CF("settings.gearAutoFinal", gearAutoFinal, U8, 1),
    CF("settings.brakeBalance", brakeBalance, U8, 2),
    CF("settings.downforce", downforce, U8, 2),
    CF("settings.turboBoost10", turboBoost10, U8, 1),
    CF("settings.turboSpoolRpm100", turboSpoolRpm100, U8, 1),
    CF("settings.turboSpoolRate10", turboSpoolRate10, U8, 1),
    CF("settings.turboBoost10Second", turboBoost10Second, U8, 1),
    CF("settings.turboSpoolRpm100Second", turboSpoolRpm100Second, U8, 1),
    CF("settings.turboSpoolRate10Second", turboSpoolRate10Second, U8, 1),
    CF("settings.camber10", camber10, U8, 2),
    CF("settings.rideHeightMm", rideHeightMm, U8, 2),
    CF("settings.toeCode", toeCode, U8, 2),
    CF("settings.springCode", springCode, U8, 2),
    CF("settings.damperScaleDivisor", damperScaleDivisor, U8, 2),
    CF("settings.damperLevel", damperLevel, U8, 8),
    CF("settings.antiRollLevel", antiRollLevel, U8, 2),
    CF("settings.diffInitial", diffInitial, U8, 2),
    CF("settings.diffAccel", diffAccel, U8, 2),
    CF("settings.diffDecel", diffDecel, U8, 2),
    CF("settings.asmLevel", asmLevel, U8, 1),
    CF("settings.tcsLevel", tcsLevel, U8, 1),
    CF("settings.flags", flags, U8, 1),
    // written back by the record builder
    CF("builder.engineWord", engineWord, U16, 1),
    CF("builder.exhaustByte", exhaustByte, U8, 1),
    // not read by the builder
    CF("raw.word00", word00, Hex, 4),
    CF("raw.word38", word38, Hex, 2),
    CF("raw.byte4F", byte4F, Hex, 1),
    CF("raw.byte79", byte79, Hex, 1),
    CF("raw.byte7B", byte7B, Hex, 1),
    CF("raw.words7C", word7C, Hex, 8),
};
#undef CF

void CheckCoverage(std::span<const CarFieldDesc> fields, size_t recordSize, const char* what) {
    std::vector<int> hits(recordSize, 0);
    std::set<std::string> paths;
    for (const CarFieldDesc& f : fields) {
        if (!paths.insert(f.path).second) throw std::logic_error(std::string("car_json: duplicate path ") + f.path);
        const size_t bytes = size_t(f.count) * f.ElementSize();
        if (f.offset + bytes > recordSize) throw std::logic_error(std::string("car_json: field past the record: ") + f.path);
        for (size_t i = 0; i < bytes; i++) hits[f.offset + i]++;
    }
    for (size_t i = 0; i < recordSize; i++)
        if (hits[i] != 1) throw std::logic_error(std::string("car_json: ") + what + " byte 0x" + [&] { char b[8]; std::snprintf(b, sizeof(b), "%03zX", i); return std::string(b); }() +
                                                 (hits[i] ? " covered twice" : " not covered"));
}

// ---------------------------------------------------------------- element access

int64_t ReadElement(const uint8_t* rec, const CarFieldDesc& f, size_t i) {
    const uint8_t* p = rec + f.offset + i * f.ElementSize();
    switch (f.kind) {
    case CarFieldKind::U8: case CarFieldKind::Hex: return p[0];
    case CarFieldKind::S8: return int8_t(p[0]);
    case CarFieldKind::U16: return uint16_t(p[0] | (p[1] << 8));
    case CarFieldKind::S16: return int16_t(uint16_t(p[0] | (p[1] << 8)));
    }
    return 0;
}

void WriteElement(uint8_t* rec, const CarFieldDesc& f, size_t i, int64_t v, const std::string& where) {
    uint8_t* p = rec + f.offset + i * f.ElementSize();
    int64_t lo = 0, hi = 255;
    switch (f.kind) {
    case CarFieldKind::U8: case CarFieldKind::Hex: break;
    case CarFieldKind::S8: lo = -128; hi = 127; break;
    case CarFieldKind::U16: hi = 65535; break;
    case CarFieldKind::S16: lo = -32768; hi = 32767; break;
    }
    if (v < lo || v > hi) throw std::runtime_error(where + ": value " + std::to_string(v) + " out of range " + std::to_string(lo) + ".." + std::to_string(hi));
    if (f.ElementSize() == 1) p[0] = uint8_t(v);
    else { p[0] = uint8_t(uint16_t(v) & 0xFF); p[1] = uint8_t(uint16_t(v) >> 8); }
}

Value ElementValue(const CarFieldDesc& f, int64_t v) {
    if (f.enumNames && v >= 0 && v < f.enumCount && f.enumNames[v]) return Value::String(f.enumNames[v]);
    return Value::Int(v);
}

int64_t ElementFromValue(const CarFieldDesc& f, const Value& v, const std::string& where) {
    if (v.IsString()) {
        for (int i = 0; i < f.enumCount; i++)
            if (f.enumNames && f.enumNames[i] && v.AsString() == f.enumNames[i]) return i;
        throw std::runtime_error(where + ": unknown name \"" + v.AsString() + "\"");
    }
    if (!v.IsNumber()) throw std::runtime_error(where + ": expected a number");
    try {
        return v.AsInt();
    } catch (const std::exception&) {
        throw std::runtime_error(where + ": expected an integer");
    }
}

std::string HexString(const uint8_t* rec, const CarFieldDesc& f) {
    std::string s;
    for (size_t i = 0; i < f.count; i++) {
        char b[4];
        std::snprintf(b, sizeof(b), "%02X", rec[f.offset + i]);
        if (i) s.push_back(' ');
        s += b;
    }
    return s;
}

void ParseHex(uint8_t* rec, const CarFieldDesc& f, const std::string& text, const std::string& where) {
    std::vector<uint8_t> bytes;
    int nibbles = 0, acc = 0;
    for (const char c : text) {
        int v;
        if (c == ' ' || c == ',' || c == '\t') continue;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else throw std::runtime_error(where + ": bad hex string");
        acc = (acc << 4) | v;
        if (++nibbles == 2) { bytes.push_back(uint8_t(acc)); nibbles = 0; acc = 0; }
    }
    if (nibbles) throw std::runtime_error(where + ": odd number of hex digits");
    if (bytes.size() != f.count) throw std::runtime_error(where + ": expected " + std::to_string(f.count) + " bytes, got " + std::to_string(bytes.size()));
    std::memcpy(rec + f.offset, bytes.data(), bytes.size());
}

// ---------------------------------------------------------------- record <-> JSON tree

std::vector<std::string> SplitPath(const char* path) {
    std::vector<std::string> parts;
    std::string cur;
    for (const char* p = path; *p; p++) {
        if (*p == '.') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(*p);
    }
    parts.push_back(cur);
    return parts;
}

Value RecordToJson(const uint8_t* rec, std::span<const CarFieldDesc> fields, const CarFieldMask* mask) {
    Value root = Value::Object();
    for (size_t index = 0; index < fields.size(); index++) {
        if (mask && (index >= mask->present.size() || !mask->present[index])) continue;
        const CarFieldDesc& f = fields[index];
        Value leaf;
        if (f.kind == CarFieldKind::Hex) leaf = Value::String(HexString(rec, f));
        else if (f.count == 1) leaf = ElementValue(f, ReadElement(rec, f, 0));
        else {
            leaf = Value::Array();
            for (size_t i = 0; i < f.count; i++) leaf.Push(ElementValue(f, ReadElement(rec, f, i)));
        }
        // Walk / create the group objects along the path, insert the leaf.
        const std::vector<std::string> parts = SplitPath(f.path);
        Value* node = &root;
        for (size_t i = 0; i + 1 < parts.size(); i++) {
            const Value* existing = node->Get(parts[i]);
            node = existing ? const_cast<Value*>(existing) : &node->Set(parts[i], Value::Object());
        }
        node->Set(parts.back(), std::move(leaf));
    }
    return root;
}

const Value* Lookup(const Value& root, const std::vector<std::string>& parts) {
    const Value* node = &root;
    for (const std::string& p : parts) {
        node = node->Get(p);
        if (!node) return nullptr;
    }
    return node;
}

void CollectPaths(const Value& node, const std::string& prefix, std::vector<std::string>& out) {
    if (!node.IsObject()) return;
    for (const auto& [k, v] : node.Members()) {
        const std::string path = prefix.empty() ? k : prefix + "." + k;
        out.push_back(path);
        CollectPaths(v, path, out);
    }
}

void JsonToRecord(const Value& root, std::span<const CarFieldDesc> fields, uint8_t* rec, CarFieldMask& mask, const std::string& section,
                  std::vector<std::string>& warnings) {
    mask.SetAll(fields.size(), false);
    std::set<std::string> known;
    for (size_t index = 0; index < fields.size(); index++) {
        const CarFieldDesc& f = fields[index];
        const std::vector<std::string> parts = SplitPath(f.path);
        std::string prefix;
        for (const std::string& p : parts) { prefix = prefix.empty() ? p : prefix + "." + p; known.insert(prefix); }
        const Value* v = Lookup(root, parts);
        if (!v || v->IsNull()) continue;
        const std::string where = section + "." + f.path;
        if (f.kind == CarFieldKind::Hex) {
            if (!v->IsString()) throw std::runtime_error(where + ": expected a hex string");
            ParseHex(rec, f, v->AsString(), where);
        } else if (f.count == 1) {
            WriteElement(rec, f, 0, ElementFromValue(f, *v, where), where);
        } else {
            if (!v->IsArray()) throw std::runtime_error(where + ": expected an array of " + std::to_string(f.count));
            if (v->Size() != f.count) throw std::runtime_error(where + ": expected " + std::to_string(f.count) + " elements, got " + std::to_string(v->Size()));
            for (size_t i = 0; i < f.count; i++) WriteElement(rec, f, i, ElementFromValue(f, v->At(i), where + "[" + std::to_string(i) + "]"), where);
        }
        mask.present[index] = true;
    }
    std::vector<std::string> paths;
    CollectPaths(root, "", paths);
    for (const std::string& p : paths)
        if (!known.count(p)) warnings.push_back("unknown key " + section + "." + p + " ignored");
}

std::vector<std::string> DiffRecords(const uint8_t* a, const uint8_t* b, std::span<const CarFieldDesc> fields) {
    std::vector<std::string> out;
    for (const CarFieldDesc& f : fields) {
        if (f.kind == CarFieldKind::Hex) {
            if (std::memcmp(a + f.offset, b + f.offset, f.count) != 0) out.push_back(std::string(f.path) + ": " + HexString(a, f) + " -> " + HexString(b, f));
            continue;
        }
        for (size_t i = 0; i < f.count; i++) {
            const int64_t x = ReadElement(a, f, i), y = ReadElement(b, f, i);
            if (x == y) continue;
            auto text = [&](int64_t v) {
                const Value e = ElementValue(f, v);
                return e.IsString() ? e.AsString() + " (" + std::to_string(v) + ")" : std::to_string(v);
            };
            out.push_back(std::string(f.path) + (f.count > 1 ? "[" + std::to_string(i) + "]" : "") + ": " + text(x) + " -> " + text(y));
        }
    }
    return out;
}

std::string ChipToHtml(uint16_t c) {
    char b[10];
    std::snprintf(b, sizeof(b), "#%02X%02X%02X", (c & 31) * 255 / 31, ((c >> 5) & 31) * 255 / 31, ((c >> 10) & 31) * 255 / 31);
    return b;
}

uint16_t HtmlToChip(const std::string& s) {
    if (s.size() != 7 || s[0] != '#') throw std::runtime_error("paints[].chip: expected #RRGGBB");
    const long v = std::strtol(s.c_str() + 1, nullptr, 16);
    const int r = int((v >> 16) & 255), g = int((v >> 8) & 255), b = int(v & 255);
    return uint16_t((r * 31 / 255) | ((g * 31 / 255) << 5) | ((b * 31 / 255) << 10));
}

Value Triple(const std::array<double, 3>& a) {
    Value v = Value::Array();
    for (const double d : a) v.Push(Value::Double(d));
    return v;
}
Value Pair(const std::array<double, 2>& a) {
    Value v = Value::Array();
    for (const double d : a) v.Push(Value::Double(d));
    return v;
}
template <size_t N>
std::array<double, N> Numbers(const Value& v, const std::string& where) {
    if (!v.IsArray() || v.Size() != N) throw std::runtime_error(where + ": expected " + std::to_string(N) + " numbers");
    std::array<double, N> out{};
    for (size_t i = 0; i < N; i++) out[i] = v.At(i).AsDouble();
    return out;
}

void WarnUnknownKeys(const Value& obj, const std::set<std::string>& known, const std::string& section, std::vector<std::string>& warnings) {
    for (const auto& [k, v] : obj.Members())
        if (!known.count(k)) warnings.push_back("unknown key " + (section.empty() ? k : section + "." + k) + " ignored");
}

} // namespace

// ---------------------------------------------------------------- public

std::span<const CarFieldDesc> CarParamsFields() { return kParamsFields; }
std::span<const CarFieldDesc> CarConfigFields() { return kConfigFields; }

void CheckCarJsonDescriptors() {
    CheckCoverage(kParamsFields, sizeof(sim::CarParams), "CarParams");
    CheckCoverage(kConfigFields, sizeof(CarConfig), "CarConfig");
}

bool CarFieldMask::Any() const { return std::any_of(present.begin(), present.end(), [](bool b) { return b; }); }
bool CarFieldMask::All() const { return !present.empty() && std::all_of(present.begin(), present.end(), [](bool b) { return b; }); }
void CarFieldMask::SetAll(size_t count, bool value) { present.assign(count, value); }

CarJson MakeCarJson(const std::string& carId, const CarConfig& config, const sim::CarParams& params) {
    CarJson c;
    c.carId = carId;
    c.hasConfig = true;
    c.config = config;
    c.configMask.SetAll(std::size(kConfigFields), true);
    c.hasParams = true;
    c.params = params;
    c.paramsMask.SetAll(std::size(kParamsFields), true);
    c.hasSound = true;
    c.engineSoundId = config.engineWord;
    c.exhaustByte = config.exhaustByte;
    c.turbo = (config.flags & 2) != 0;
    return c;
}

void WriteCarJson(const std::string& path, const CarJson& car) {
    Value root = Value::Object();
    root.Set("format", Value::String(kFormat));
    root.Set("version", Value::Int(kVersion));
    root.Set("carId", Value::String(car.carId));
    if (!car.baseCar.empty()) root.Set("baseCar", Value::String(car.baseCar));
    if (!car.name.empty()) root.Set("name", Value::String(car.name));
    if (!car.modelId.empty()) root.Set("model", Value::String(car.modelId));
    if (!car.paints.empty()) {
        Value paints = Value::Array();
        for (const CarJsonPaint& p : car.paints) {
            Value e = Value::Object();
            e.Set("id", Value::Int(p.id));
            e.Set("chip", Value::String(ChipToHtml(p.chipColor)));
            paints.Push(std::move(e));
        }
        root.Set("paints", std::move(paints));
    }
    if (car.hasMesh) {
        Value m = Value::Object();
        m.Set("file", Value::String(car.meshFile));
        m.Set("scale", Value::Double(car.meshScale));
        m.Set("paint", Value::Int(car.meshPaint));
        if (car.meshReflection != CarJson::MeshReflection::Materials)
            m.Set("reflection", Value::String(car.meshReflection == CarJson::MeshReflection::All ? "all" : "none"));
        root.Set("mesh", std::move(m));
    }
    if (car.hasBody) {
        Value b = Value::Object();
        b.Set("wheelFront", Triple(car.wheelFront));
        b.Set("wheelRear", Triple(car.wheelRear));
        b.Set("wheelRadius", Pair(car.wheelRadius));
        b.Set("wheelWidth", Pair(car.wheelWidth));
        root.Set("body", std::move(b));
    }
    if (car.hasSound) {
        Value s = Value::Object();
        s.Set("engineSet", Value::Int(car.engineSoundId));
        s.Set("exhaust", Value::Int(car.exhaustByte));
        s.Set("turbo", Value::Bool(car.turbo));
        root.Set("sound", std::move(s));
    }
    if (car.hasConfig) root.Set("config", RecordToJson(reinterpret_cast<const uint8_t*>(&car.config), kConfigFields, &car.configMask));
    if (car.hasParams) root.Set("params", RecordToJson(reinterpret_cast<const uint8_t*>(&car.params), kParamsFields, &car.paramsMask));
    json::WriteFile(path, root);
}

void WriteCarJson(const std::string& path, const std::string& carId, const CarConfig& config, const sim::CarParams& params) {
    WriteCarJson(path, MakeCarJson(carId, config, params));
}

CarJson ReadCarJson(const std::string& path) {
    const Value root = json::ReadFile(path);
    CarJson c;
    try {
        if (!root.IsObject()) throw std::runtime_error("the document is not an object");
        const std::string format = root.StringOr("format", "");
        if (format.empty()) c.warnings.push_back("no \"format\" key (expected \"" + std::string(kFormat) + "\")");
        else if (format != kFormat) throw std::runtime_error("format \"" + format + "\" is not \"" + kFormat + "\"");
        c.version = int(root.IntOr("version", kVersion));
        if (c.version > kVersion) throw std::runtime_error("version " + std::to_string(c.version) + " is newer than this reader (" + std::to_string(kVersion) + ")");
        c.carId = root.StringOr("carId", "");
        c.baseCar = root.StringOr("baseCar", "");
        c.name = root.StringOr("name", "");
        c.modelId = root.StringOr("model", "");
        if (const Value* paints = root.Get("paints")) {
            if (!paints->IsArray()) throw std::runtime_error("paints: expected an array");
            for (size_t i = 0; i < paints->Size(); i++) {
                const Value& e = paints->At(i);
                CarJsonPaint p;
                p.id = uint8_t(e.IntAt("id"));
                if (const Value* chip = e.Get("chip")) p.chipColor = HtmlToChip(chip->AsString());
                c.paints.push_back(p);
            }
        }
        if (const Value* m = root.Get("mesh")) {
            if (!m->IsObject()) throw std::runtime_error("mesh: expected an object");
            c.hasMesh = true;
            c.meshFile = m->Require("file").AsString();
            c.meshScale = m->DoubleOr("scale", 1.0);
            c.meshPaint = uint32_t(m->IntOr("paint", 0));
            if (!(c.meshScale > 0)) throw std::runtime_error("mesh.scale must be positive");
            const std::string reflection = m->StringOr("reflection", "materials");
            if (reflection == "materials") c.meshReflection = CarJson::MeshReflection::Materials;
            else if (reflection == "all") c.meshReflection = CarJson::MeshReflection::All;
            else if (reflection == "none") c.meshReflection = CarJson::MeshReflection::None;
            else throw std::runtime_error("mesh.reflection must be \"materials\", \"all\" or \"none\"");
            WarnUnknownKeys(*m, {"file", "scale", "paint", "reflection"}, "mesh", c.warnings);
        }
        if (const Value* b = root.Get("body")) {
            if (!b->IsObject()) throw std::runtime_error("body: expected an object");
            c.hasBody = true;
            c.wheelFront = Numbers<3>(b->Require("wheelFront"), "body.wheelFront");
            c.wheelRear = Numbers<3>(b->Require("wheelRear"), "body.wheelRear");
            c.wheelRadius = Numbers<2>(b->Require("wheelRadius"), "body.wheelRadius");
            c.wheelWidth = Numbers<2>(b->Require("wheelWidth"), "body.wheelWidth");
            WarnUnknownKeys(*b, {"wheelFront", "wheelRear", "wheelRadius", "wheelWidth"}, "body", c.warnings);
        }
        if (const Value* s = root.Get("sound")) {
            if (!s->IsObject()) throw std::runtime_error("sound: expected an object");
            c.hasSound = true;
            const int64_t set = s->IntAt("engineSet"), exhaust = s->IntOr("exhaust", 0);
            if (set < 0 || set > 65535) throw std::runtime_error("sound.engineSet out of range");
            if (exhaust < 0 || exhaust > 255) throw std::runtime_error("sound.exhaust out of range");
            c.engineSoundId = uint16_t(set);
            c.exhaustByte = uint8_t(exhaust);
            c.turbo = s->BoolOr("turbo", false);
            WarnUnknownKeys(*s, {"engineSet", "exhaust", "turbo"}, "sound", c.warnings);
        }
        if (const Value* cfg = root.Get("config")) {
            if (!cfg->IsObject()) throw std::runtime_error("config: expected an object");
            c.hasConfig = true;
            std::memset(&c.config, 0, sizeof(c.config));
            JsonToRecord(*cfg, kConfigFields, reinterpret_cast<uint8_t*>(&c.config), c.configMask, "config", c.warnings);
        }
        if (const Value* p = root.Get("params")) {
            if (!p->IsObject()) throw std::runtime_error("params: expected an object");
            c.hasParams = true;
            std::memset(&c.params, 0, sizeof(c.params));
            JsonToRecord(*p, kParamsFields, reinterpret_cast<uint8_t*>(&c.params), c.paramsMask, "params", c.warnings);
        }
        WarnUnknownKeys(root, {"format", "version", "carId", "baseCar", "name", "model", "paints", "mesh", "body", "sound", "config", "params"}, "", c.warnings);
    } catch (const std::exception& e) {
        throw std::runtime_error(path + ": " + e.what());
    }
    return c;
}

void ApplyParamOverrides(sim::CarParams& target, const CarJson& car) {
    if (!car.hasParams) return;
    uint8_t* dst = reinterpret_cast<uint8_t*>(&target);
    const uint8_t* src = reinterpret_cast<const uint8_t*>(&car.params);
    for (size_t i = 0; i < std::size(kParamsFields) && i < car.paramsMask.present.size(); i++) {
        if (!car.paramsMask.present[i]) continue;
        const CarFieldDesc& f = kParamsFields[i];
        std::memcpy(dst + f.offset, src + f.offset, size_t(f.count) * f.ElementSize());
    }
}

void ApplyConfigOverrides(CarConfig& target, const CarJson& car) {
    if (!car.hasConfig) return;
    uint8_t* dst = reinterpret_cast<uint8_t*>(&target);
    const uint8_t* src = reinterpret_cast<const uint8_t*>(&car.config);
    for (size_t i = 0; i < std::size(kConfigFields) && i < car.configMask.present.size(); i++) {
        if (!car.configMask.present[i]) continue;
        const CarFieldDesc& f = kConfigFields[i];
        std::memcpy(dst + f.offset, src + f.offset, size_t(f.count) * f.ElementSize());
    }
}

std::vector<std::string> DiffCarParams(const sim::CarParams& before, const sim::CarParams& after) {
    return DiffRecords(reinterpret_cast<const uint8_t*>(&before), reinterpret_cast<const uint8_t*>(&after), kParamsFields);
}

std::vector<std::string> DiffCarConfig(const CarConfig& before, const CarConfig& after) {
    return DiffRecords(reinterpret_cast<const uint8_t*>(&before), reinterpret_cast<const uint8_t*>(&after), kConfigFields);
}

CarBodyDimensions BodyDimensionsOfMesh(double minZ, double maxZ, double wheelLateralFront, double wheelLateralRear) {
    auto units = [](double metres) {
        const double v = std::round(metres * 4096.0);
        return int16_t(std::clamp(v, -32768.0, 32767.0));
    };
    CarBodyDimensions d;
    d.scaleShift = 16; // shift 0: 1/4096 m units
    d.bboxFront = units(minZ);
    d.bboxRear = units(maxZ);
    d.wheelLateralFront = units(wheelLateralFront);
    d.wheelLateralRear = units(wheelLateralRear);
    return d;
}

ResolvedCar ResolveCarJson(const GtfsVolume& vol, const CarParamTables& tables, const CarJson& car, const std::string& jsonPath) {
    ResolvedCar r;
    auto inTables = [&](const std::string& id) { return id.size() == 5 && !tables.RowsOfCar(kTableChassis, PackCarId(id)).empty(); };
    if (!car.baseCar.empty()) {
        if (!inTables(car.baseCar)) throw std::runtime_error(jsonPath + ": baseCar \"" + car.baseCar + "\" is not in the parameter tables");
        r.baseCar = car.baseCar;
    } else if (inTables(car.carId)) {
        r.baseCar = car.carId;
    }

    // External mesh first: its bounding box feeds the record builder like the .cdo does.
    if (car.hasMesh) {
        r.externalMesh = true;
        r.meshScale = car.meshScale;
        r.meshReflection = car.meshReflection;
        r.meshPath =(std::filesystem::path(jsonPath).parent_path() / std::filesystem::path(car.meshFile)).string();
        r.mesh = std::make_shared<GltfMesh>(ReadGltf(r.meshPath));
        for (const std::string& w : r.mesh->warnings) r.notes.push_back("mesh: " + w);
        if (r.mesh->primitives.empty()) throw std::runtime_error(r.meshPath + ": no triangles");
    }

    // The configuration and the model.
    CarModel model;
    bool haveModel = false;
    if (!r.baseCar.empty()) {
        r.stockConfig = StockCarConfig(tables, PackCarId(r.baseCar));
        r.config = r.stockConfig;
        ApplyConfigOverrides(r.config, car);
        const RacingModifyRow& rm = tables.RowAs<RacingModifyRow>(kTableRacingModify, r.config.racingModify);
        r.modelId = UnpackCarId(rm.modelId);
        if (!r.externalMesh) {
            model = ParseCarModel(vol.Read("carobj/" + r.modelId + ".cdo"));
            haveModel = true;
        }
    } else if (!r.externalMesh) {
        throw std::runtime_error(jsonPath + ": \"" + car.carId + "\" is not a disc car and the file names no \"baseCar\" and no \"mesh\"");
    }

    // Wheel geometry: the file's body block, else the .cdo, else an estimate from the mesh bounds.
    if (car.hasBody) {
        r.wheelFront = car.wheelFront;
        r.wheelRear = car.wheelRear;
        r.wheelRadius = car.wheelRadius;
        r.wheelWidth = car.wheelWidth;
    } else if (haveModel) {
        const double ws = 1.0 / 4096.0; // .cdo wheel entries: 1/4096 m (car_mesh.h kCarWheelMetresPerUnit)
        r.wheelFront = {model.wheels[0].z * ws, model.wheels[0].y * ws, model.wheels[0].x * ws};
        r.wheelRear = {model.wheels[2].z * ws, model.wheels[2].y * ws, model.wheels[2].x * ws};
        r.wheelRadius = {model.wheelRadiusFront * ws, model.wheelRadiusRear * ws};
        r.wheelWidth = {model.wheelWidthFront * ws, model.wheelWidthRear * ws};
    } else {
        const GltfMesh& m = *r.mesh;
        const double s = car.meshScale;
        const double halfWidth = (m.boundsMax[0] - m.boundsMin[0]) * s / 2, length = (m.boundsMax[2] - m.boundsMin[2]) * s;
        const double wheelbase = length * 0.6, radius = 0.32, width = 0.24;
        const double y = m.boundsMin[1] * s + radius * 0.5;
        r.wheelFront = {-(halfWidth - width / 2 - 0.02), y, -wheelbase / 2};
        r.wheelRear = {-(halfWidth - width / 2 - 0.02), y, wheelbase / 2};
        r.wheelRadius = {radius, radius};
        r.wheelWidth = {width, width};
        r.notes.push_back("no \"body\" block: wheel positions estimated from the mesh bounds (add body.wheelFront / wheelRear / wheelRadius / wheelWidth)");
    }

    // Body dimensions for the builder.
    if (r.externalMesh) {
        const GltfMesh& m = *r.mesh;
        r.dims = BodyDimensionsOfMesh(m.boundsMin[2] * car.meshScale, m.boundsMax[2] * car.meshScale, std::fabs(r.wheelFront[0]) + r.wheelWidth[0] / 2,
                                      std::fabs(r.wheelRear[0]) + r.wheelWidth[1] / 2);
    } else {
        r.dims = BodyDimensionsOf(model);
    }

    // The record.
    if (!r.baseCar.empty()) {
        CarConfig stock = r.stockConfig;
        r.stockParams = BuildCarParams(vol, tables, stock); // the reference: stock parts, the .cdo's dimensions
        r.stockConfig = stock;                              // with the builder's write-backs (engineWord, exhaustByte, flags)
        r.params = BuildCarParams(tables, r.config, r.dims);
        if (r.externalMesh) r.notes.push_back("body lengths / tracks derived from the mesh bounding box and the wheel positions (params.body.* in the file override them)");
    } else {
        std::memset(&r.stockParams, 0, sizeof(r.stockParams));
        std::memset(&r.params, 0, sizeof(r.params));
        if (!car.hasParams || !car.paramsMask.Any()) throw std::runtime_error(jsonPath + ": a stand-alone car needs a complete \"params\" block");
        if (!car.paramsMask.All()) r.notes.push_back("stand-alone car with an incomplete \"params\" block: the missing fields are zero");
    }
    ApplyParamOverrides(r.params, car);
    if (r.baseCar.empty() && r.externalMesh) {
        // Stand-alone: the builder did not run, derive the body lengths / tracks from the mesh unless the file has them.
        auto has = [&](const char* path) {
            const std::span<const CarFieldDesc> fields = CarParamsFields();
            for (size_t i = 0; i < fields.size(); i++)
                if (std::strcmp(fields[i].path, path) == 0) return i < car.paramsMask.present.size() && car.paramsMask.present[i];
            return false;
        };
        if (!has("body.frontLength")) r.params.frontLength = int16_t(std::lround(-r.mesh->boundsMin[2] * car.meshScale * 1000));
        if (!has("body.rearLength")) r.params.rearLength = int16_t(std::lround(r.mesh->boundsMax[2] * car.meshScale * 1000));
        if (!has("body.width")) r.params.width = int16_t(std::lround((r.mesh->boundsMax[0] - r.mesh->boundsMin[0]) * car.meshScale * 1000));
        if (!has("body.height")) r.params.height = int16_t(std::lround((r.mesh->boundsMax[1] - r.mesh->boundsMin[1]) * car.meshScale * 1000));
        if (!has("body.wheelbase")) r.params.wheelbase = int16_t(std::lround(std::fabs(r.wheelRear[2] - r.wheelFront[2]) * 1000));
        if (!has("body.frontTrack")) r.params.frontTrack = int16_t(std::lround(std::fabs(r.wheelFront[0]) * 2000));
        if (!has("body.rearTrack")) r.params.rearTrack = int16_t(std::lround(std::fabs(r.wheelRear[0]) * 2000));
        r.notes.push_back("stand-alone car: body dimensions not in the file were taken from the mesh");
    }

    // Sound.
    if (car.hasSound) {
        r.engineSoundId = car.engineSoundId;
        r.exhaustByte = car.exhaustByte;
        r.turbo = car.turbo;
    } else if (!r.baseCar.empty()) {
        r.engineSoundId = r.config.engineWord;
        r.exhaustByte = r.config.exhaustByte;
        r.turbo = (r.config.flags & 2) != 0;
    } else {
        r.notes.push_back("no \"sound\" block: engine sound set 0");
    }
    return r;
}

} // namespace gt2
