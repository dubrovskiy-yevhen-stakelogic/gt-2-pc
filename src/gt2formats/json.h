#pragma once
// Tiny dependency-free JSON (RFC 8259) value, parser and pretty-printer for the editable data layer
// (car_json.h, gltf_reader.h). Own code; no external licence involved.
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gt2::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    static Value Null() { return Value(); }
    static Value Bool(bool b) { Value v; v.type_ = Type::Bool; v.bool_ = b; return v; }
    static Value Int(int64_t i) { Value v; v.type_ = Type::Number; v.isInt_ = true; v.int_ = i; v.number_ = double(i); return v; }
    static Value Double(double d) { Value v; v.type_ = Type::Number; v.number_ = d; return v; }
    static Value String(std::string s) { Value v; v.type_ = Type::String; v.string_ = std::move(s); return v; }
    static Value Array() { Value v; v.type_ = Type::Array; return v; }
    static Value Object() { Value v; v.type_ = Type::Object; return v; }

    Type GetType() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsInt() const { return type_ == Type::Number && isInt_; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    bool AsBool() const;
    double AsDouble() const;
    int64_t AsInt() const; // exact integers only (throws for fractions)
    const std::string& AsString() const;

    // Arrays.
    size_t Size() const { return type_ == Type::Array ? array_.size() : type_ == Type::Object ? object_.size() : 0; }
    const Value& At(size_t i) const;
    Value& Push(Value v);

    // Objects (insertion order kept: the writer emits the keys in the order they were set).
    const Value* Get(std::string_view key) const;        // nullptr when absent
    const Value& Require(std::string_view key) const;    // throws when absent
    Value& Set(std::string key, Value v);                // replaces an existing key in place
    const std::vector<std::pair<std::string, Value>>& Members() const { return object_; }

    // Convenience: the number at `key` (throws when absent / not a number).
    int64_t IntAt(std::string_view key) const { return Require(key).AsInt(); }
    double DoubleAt(std::string_view key) const { return Require(key).AsDouble(); }
    // Optional variants.
    int64_t IntOr(std::string_view key, int64_t fallback) const;
    double DoubleOr(std::string_view key, double fallback) const;
    std::string StringOr(std::string_view key, std::string fallback) const;
    bool BoolOr(std::string_view key, bool fallback) const;

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    bool isInt_ = false;
    int64_t int_ = 0;
    double number_ = 0;
    std::string string_;
    std::vector<Value> array_;
    std::vector<std::pair<std::string, Value>> object_;
};

// Throws std::runtime_error("json: <message> at line L, column C").
Value Parse(std::string_view text);

// Pretty-printed with two-space indents; arrays whose elements are all scalars are written on one line.
std::string Write(const Value& value);

// File helpers (throw on I/O errors).
Value ReadFile(const std::string& path);
void WriteFile(const std::string& path, const Value& value);

} // namespace gt2::json
