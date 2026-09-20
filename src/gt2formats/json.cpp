#include "gt2formats/json.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace gt2::json {

// ---------------------------------------------------------------- Value

bool Value::AsBool() const {
    if (type_ != Type::Bool) throw std::runtime_error("json: not a boolean");
    return bool_;
}

double Value::AsDouble() const {
    if (type_ != Type::Number) throw std::runtime_error("json: not a number");
    return number_;
}

int64_t Value::AsInt() const {
    if (type_ != Type::Number) throw std::runtime_error("json: not a number");
    if (isInt_) return int_;
    if (number_ != std::floor(number_) || std::fabs(number_) > 9.2e18) throw std::runtime_error("json: not an integer");
    return int64_t(number_);
}

const std::string& Value::AsString() const {
    if (type_ != Type::String) throw std::runtime_error("json: not a string");
    return string_;
}

const Value& Value::At(size_t i) const {
    if (type_ != Type::Array) throw std::runtime_error("json: not an array");
    if (i >= array_.size()) throw std::runtime_error("json: array index out of range");
    return array_[i];
}

Value& Value::Push(Value v) {
    if (type_ != Type::Array) throw std::runtime_error("json: not an array");
    array_.push_back(std::move(v));
    return array_.back();
}

const Value* Value::Get(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& [k, v] : object_)
        if (k == key) return &v;
    return nullptr;
}

const Value& Value::Require(std::string_view key) const {
    const Value* v = Get(key);
    if (!v) throw std::runtime_error("json: missing key \"" + std::string(key) + "\"");
    return *v;
}

Value& Value::Set(std::string key, Value v) {
    if (type_ != Type::Object) throw std::runtime_error("json: not an object");
    for (auto& [k, existing] : object_)
        if (k == key) { existing = std::move(v); return existing; }
    object_.emplace_back(std::move(key), std::move(v));
    return object_.back().second;
}

int64_t Value::IntOr(std::string_view key, int64_t fallback) const {
    const Value* v = Get(key);
    return v && !v->IsNull() ? v->AsInt() : fallback;
}
double Value::DoubleOr(std::string_view key, double fallback) const {
    const Value* v = Get(key);
    return v && !v->IsNull() ? v->AsDouble() : fallback;
}
std::string Value::StringOr(std::string_view key, std::string fallback) const {
    const Value* v = Get(key);
    return v && !v->IsNull() ? v->AsString() : fallback;
}
bool Value::BoolOr(std::string_view key, bool fallback) const {
    const Value* v = Get(key);
    return v && !v->IsNull() ? v->AsBool() : fallback;
}

// ---------------------------------------------------------------- parser

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Value ParseDocument() {
        SkipSpace();
        Value v = ParseValue(0);
        SkipSpace();
        if (pos_ != text_.size()) Fail("trailing characters after the document");
        return v;
    }

private:
    [[noreturn]] void Fail(const std::string& message) const {
        size_t line = 1, col = 1;
        for (size_t i = 0; i < pos_ && i < text_.size(); i++) {
            if (text_[i] == '\n') { line++; col = 1; }
            else col++;
        }
        throw std::runtime_error("json: " + message + " at line " + std::to_string(line) + ", column " + std::to_string(col));
    }
    bool More() const { return pos_ < text_.size(); }
    char Peek() const { return More() ? text_[pos_] : '\0'; }
    void SkipSpace() {
        while (More() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) pos_++;
    }
    void Expect(char c) {
        if (Peek() != c) Fail(std::string("expected '") + c + "'");
        pos_++;
    }

    Value ParseValue(int depth) {
        if (depth > 200) Fail("nesting too deep");
        switch (Peek()) {
        case '{': return ParseObject(depth);
        case '[': return ParseArray(depth);
        case '"': return Value::String(ParseString());
        case 't': Literal("true"); return Value::Bool(true);
        case 'f': Literal("false"); return Value::Bool(false);
        case 'n': Literal("null"); return Value::Null();
        default: return ParseNumber();
        }
    }

    void Literal(const char* word) {
        const size_t n = std::strlen(word);
        if (text_.substr(pos_, n) != word) Fail("unexpected token");
        pos_ += n;
    }

    Value ParseObject(int depth) {
        Expect('{');
        Value obj = Value::Object();
        SkipSpace();
        if (Peek() == '}') { pos_++; return obj; }
        for (;;) {
            SkipSpace();
            if (Peek() != '"') Fail("expected a string key");
            std::string key = ParseString();
            SkipSpace();
            Expect(':');
            SkipSpace();
            Value v = ParseValue(depth + 1);
            if (obj.Get(key)) Fail("duplicate key \"" + key + "\"");
            obj.Set(std::move(key), std::move(v));
            SkipSpace();
            if (Peek() == ',') { pos_++; continue; }
            if (Peek() == '}') { pos_++; return obj; }
            Fail("expected ',' or '}'");
        }
    }

    Value ParseArray(int depth) {
        Expect('[');
        Value arr = Value::Array();
        SkipSpace();
        if (Peek() == ']') { pos_++; return arr; }
        for (;;) {
            SkipSpace();
            arr.Push(ParseValue(depth + 1));
            SkipSpace();
            if (Peek() == ',') { pos_++; continue; }
            if (Peek() == ']') { pos_++; return arr; }
            Fail("expected ',' or ']'");
        }
    }

    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out.push_back(char(cp));
        else if (cp < 0x800) { out.push_back(char(0xC0 | (cp >> 6))); out.push_back(char(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { out.push_back(char(0xE0 | (cp >> 12))); out.push_back(char(0x80 | ((cp >> 6) & 0x3F))); out.push_back(char(0x80 | (cp & 0x3F))); }
        else { out.push_back(char(0xF0 | (cp >> 18))); out.push_back(char(0x80 | ((cp >> 12) & 0x3F))); out.push_back(char(0x80 | ((cp >> 6) & 0x3F))); out.push_back(char(0x80 | (cp & 0x3F))); }
    }

    uint32_t Hex4() {
        if (pos_ + 4 > text_.size()) Fail("truncated \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = text_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else Fail("bad \\u escape");
        }
        return v;
    }

    std::string ParseString() {
        Expect('"');
        std::string out;
        for (;;) {
            if (!More()) Fail("unterminated string");
            const char c = text_[pos_++];
            if (c == '"') return out;
            if (uint8_t(c) < 0x20) Fail("control character in string");
            if (c != '\\') { out.push_back(c); continue; }
            if (!More()) Fail("unterminated escape");
            const char e = text_[pos_++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp = Hex4();
                if (cp >= 0xD800 && cp < 0xDC00) { // surrogate pair
                    if (text_.substr(pos_, 2) != "\\u") Fail("lone high surrogate");
                    pos_ += 2;
                    const uint32_t lo = Hex4();
                    if (lo < 0xDC00 || lo > 0xDFFF) Fail("bad low surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                AppendUtf8(out, cp);
                break;
            }
            default: Fail("bad escape");
            }
        }
    }

    Value ParseNumber() {
        const size_t start = pos_;
        bool isInt = true;
        if (Peek() == '-') pos_++;
        if (!(Peek() >= '0' && Peek() <= '9')) Fail("unexpected character");
        while (Peek() >= '0' && Peek() <= '9') pos_++;
        if (Peek() == '.') {
            isInt = false;
            pos_++;
            if (!(Peek() >= '0' && Peek() <= '9')) Fail("bad fraction");
            while (Peek() >= '0' && Peek() <= '9') pos_++;
        }
        if (Peek() == 'e' || Peek() == 'E') {
            isInt = false;
            pos_++;
            if (Peek() == '+' || Peek() == '-') pos_++;
            if (!(Peek() >= '0' && Peek() <= '9')) Fail("bad exponent");
            while (Peek() >= '0' && Peek() <= '9') pos_++;
        }
        const std::string literal(text_.substr(start, pos_ - start));
        if (isInt && literal.size() < 19) return Value::Int(std::strtoll(literal.c_str(), nullptr, 10));
        return Value::Double(std::strtod(literal.c_str(), nullptr));
    }

    std::string_view text_;
    size_t pos_ = 0;
};

// ---------------------------------------------------------------- writer

void WriteString(std::string& out, const std::string& s) {
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (uint8_t(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(uint8_t(c)));
                out += buf;
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

void WriteNumber(std::string& out, const Value& v) {
    if (v.IsInt()) { out += std::to_string(v.AsInt()); return; }
    const double d = v.AsDouble();
    if (!std::isfinite(d)) throw std::runtime_error("json: cannot write a non-finite number");
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", d);
    if (std::strtod(buf, nullptr) != d) std::snprintf(buf, sizeof(buf), "%.17g", d);
    out += buf;
    if (std::strpbrk(buf, ".eE") == nullptr) out += ".0"; // keep it a float literal on re-read
}

bool AllScalars(const Value& arr) {
    for (size_t i = 0; i < arr.Size(); i++)
        if (arr.At(i).IsArray() || arr.At(i).IsObject()) return false;
    return true;
}

void WriteValue(std::string& out, const Value& v, int indent) {
    auto newline = [&](int level) {
        out.push_back('\n');
        out.append(size_t(level) * 2, ' ');
    };
    switch (v.GetType()) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.AsBool() ? "true" : "false"; break;
    case Value::Type::Number: WriteNumber(out, v); break;
    case Value::Type::String: WriteString(out, v.AsString()); break;
    case Value::Type::Array: {
        if (v.Size() == 0) { out += "[]"; break; }
        const bool inline_ = AllScalars(v);
        out.push_back('[');
        for (size_t i = 0; i < v.Size(); i++) {
            if (i) out.push_back(',');
            if (inline_) { if (i) out.push_back(' '); }
            else newline(indent + 1);
            WriteValue(out, v.At(i), indent + 1);
        }
        if (!inline_) newline(indent);
        out.push_back(']');
        break;
    }
    case Value::Type::Object: {
        if (v.Size() == 0) { out += "{}"; break; }
        out.push_back('{');
        bool first = true;
        for (const auto& [k, member] : v.Members()) {
            if (!first) out.push_back(',');
            first = false;
            newline(indent + 1);
            WriteString(out, k);
            out += ": ";
            WriteValue(out, member, indent + 1);
        }
        newline(indent);
        out.push_back('}');
        break;
    }
    }
}

} // namespace

Value Parse(std::string_view text) { return Parser(text).ParseDocument(); }

std::string Write(const Value& value) {
    std::string out;
    WriteValue(out, value, 0);
    out.push_back('\n');
    return out;
}

Value ReadFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("json: cannot open " + path);
    std::string text;
    char buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    std::fclose(f);
    if (text.size() >= 3 && uint8_t(text[0]) == 0xEF && uint8_t(text[1]) == 0xBB && uint8_t(text[2]) == 0xBF) text.erase(0, 3); // UTF-8 BOM
    try {
        return Parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(path + ": " + e.what());
    }
}

void WriteFile(const std::string& path, const Value& value) {
    const std::string text = Write(value);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("json: cannot create " + path);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

} // namespace gt2::json
