#include "specdb/specdb.h"

#include <cstring>

namespace gt4::specdb {

uint32_t Table::U32(uint32_t offset) const {
    if (offset + 4 > data_.size()) return 0;
    uint32_t v;
    std::memcpy(&v, data_.data() + offset, 4);
    return v;
}

bool Table::Load(std::vector<uint8_t> data, std::string* error) {
    data_ = std::move(data);
    if (data_.size() < 16 || std::memcmp(data_.data(), "GTDB", 4) != 0) { *error = "not a GTDB table"; return false; }
    const uint16_t flags = static_cast<uint16_t>(data_[4] | (data_[5] << 8));
    count_ = U32(8);
    rowSize_ = U32(12);
    compressed_ = (flags & 1) != 0;
    const uint64_t indexEnd = 0x10 + uint64_t{count_} * 8;
    if (indexEnd > data_.size()) { *error = "row index outside file"; return false; }
    const uint32_t p = static_cast<uint32_t>(indexEnd);
    if (count_ == 0) { dataBlock_ = p; return true; }   // empty tables carry a truncated code book
    if (!compressed_) { dataBlock_ = p; return true; }
    shortCodes_ = p + 8;
    longCodes_ = p + 0x208;
    longCount_ = U32(p + 4);
    const uint32_t templateBlock = p + U32(p);
    if (templateBlock + 8 > data_.size()) { *error = "template block outside file"; return false; }
    templates_ = templateBlock + 8;
    dataBlock_ = templateBlock + U32(templateBlock);
    if (dataBlock_ > data_.size() || longCodes_ + uint64_t{longCount_} * 8 > data_.size()) {
        *error = "code book outside file";
        return false;
    }
    return true;
}

uint32_t Table::rowId(uint32_t index) const { return U32(0x10 + index * 8); }
uint32_t Table::rowOffset(uint32_t index) const { return U32(0x14 + index * 8); }

int Table::Find(uint32_t id) const {
    int lo = -1, hi = static_cast<int>(count_);
    while (lo + 1 != hi) {
        const int mid = (lo + hi) >> 1;
        const uint32_t key = rowId(static_cast<uint32_t>(mid));
        if (key == id) return mid;
        if (id <= key) hi = mid; else lo = mid;
    }
    return -1;
}

bool Table::Symbol(uint32_t bits, uint8_t* value, int* length) const {
    const uint32_t entry = shortCodes_ + (bits & 0xff) * 2;
    if (data_[entry + 1] != 0) {
        *value = data_[entry];
        *length = data_[entry + 1];
        return true;
    }
    for (int n = 9; n <= 32; ++n) {   // long codes, sorted by (length, code), as in 0x00449558
        const uint64_t key = (uint64_t{static_cast<uint32_t>(n)} << 32) |
                             (n == 32 ? bits : (bits & ((1u << n) - 1)));
        int lo = -1, hi = static_cast<int>(longCount_);
        while (lo + 1 != hi) {
            const int mid = (lo + hi) >> 1;
            const uint32_t e = longCodes_ + static_cast<uint32_t>(mid) * 8;
            const uint64_t k = (uint64_t{data_[e]} << 32) | U32(e + 4);
            if (k == key) { *value = data_[e + 1]; *length = n; return true; }
            if (key <= k) hi = mid; else lo = mid;
        }
    }
    return false;
}

bool Table::Row(uint32_t index, std::vector<uint8_t>* out, std::string* error) const {
    if (index >= count_) { *error = "row index out of range"; return false; }
    const uint32_t start = dataBlock_ + rowOffset(index);
    out->assign(rowSize_, 0);
    if (!compressed_) {
        if (start + uint64_t{rowSize_} > data_.size()) { *error = "row outside file"; return false; }
        std::memcpy(out->data(), data_.data() + start, rowSize_);
        return true;
    }
    if (start >= data_.size()) { *error = "row outside file"; return false; }
    // Stage 1: Huffman-coded record (first byte = decoded length).
    const uint32_t n = data_[start];
    std::vector<uint8_t> record;
    record.reserve(n);
    uint32_t bit = 0;
    for (uint32_t k = 0; k < n; ++k) {
        uint8_t raw[4] = {0, 0, 0, 0};
        const uint32_t at = start + 1 + (bit >> 3);
        for (uint32_t b = 0; b < 4 && at + b < data_.size(); ++b) raw[b] = data_[at + b];
        uint32_t word;
        std::memcpy(&word, raw, 4);
        uint8_t value = 0;
        int length = 0;
        if (!Symbol(word >> (bit & 7), &value, &length)) { *error = "undecodable code"; return false; }
        record.push_back(value);
        bit += static_cast<uint32_t>(length);
    }
    if (record.empty()) { *error = "empty record"; return false; }
    // Stage 2: template delta.
    const uint32_t mode = record[0] >> 6, templateIndex = record[0] & 0x3f;
    const uint32_t templateOffset = templates_ + rowSize_ * templateIndex;
    auto copyTemplate = [&]() {
        if (templateOffset + uint64_t{rowSize_} > data_.size()) return false;
        std::memcpy(out->data(), data_.data() + templateOffset, rowSize_);
        return true;
    };
    if (mode == 1) {
        if (record.size() < 1 + uint64_t{rowSize_}) { *error = "short raw record"; return false; }
        std::memcpy(out->data(), record.data() + 1, rowSize_);
        return true;
    }
    if (mode == 0) {
        if (!copyTemplate()) { *error = "template outside file"; return false; }
        return true;
    }
    if (mode == 2) {
        if (!copyTemplate()) { *error = "template outside file"; return false; }
        const uint32_t maskBytes = (rowSize_ + 7) / 8;
        uint32_t src = 1 + maskBytes;
        for (uint32_t b = 0; b < rowSize_; ++b) {
            if ((record.at(1 + (b >> 3)) >> (b & 7)) & 1) {
                if (src >= record.size()) { *error = "short delta record"; return false; }
                (*out)[b] = record[src++];
            }
        }
        return true;
    }
    *error = "unknown record mode";
    return false;
}

bool LoadLabels(const std::vector<uint8_t>& data, std::map<uint32_t, std::string>* labels, std::string* error) {
    if (data.size() < 16 || std::memcmp(data.data(), "GTID", 4) != 0) { *error = "not GTID"; return false; }
    uint32_t count;
    std::memcpy(&count, data.data() + 4, 4);
    const uint64_t base = 0x10 + uint64_t{count} * 8;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t stringOffset, id;
        std::memcpy(&stringOffset, data.data() + 0x10 + i * 8, 4);
        std::memcpy(&id, data.data() + 0x14 + i * 8, 4);
        const uint64_t at = base + stringOffset;
        if (at + 2 > data.size()) { *error = "label outside file"; return false; }
        const uint32_t length = data[at] | (data[at + 1] << 8);   // includes the terminator
        if (length == 0 || at + 2 + length - 1 > data.size()) { *error = "bad label length"; return false; }
        (*labels)[id] = std::string(reinterpret_cast<const char*>(data.data() + at + 2), length - 1);
    }
    return true;
}

bool LoadStrings(const std::vector<uint8_t>& data, std::vector<std::string>* strings, std::string* error) {
    if (data.size() < 16 || std::memcmp(data.data(), "GTST", 4) != 0) { *error = "not GTST"; return false; }
    uint32_t count;
    std::memcpy(&count, data.data() + 4, 4);
    const uint64_t base = 0x10 + uint64_t{count} * 4;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t offset;
        std::memcpy(&offset, data.data() + 0x10 + i * 4, 4);
        const uint64_t at = base + offset;
        if (at + 2 > data.size()) { *error = "string outside file"; return false; }
        const uint32_t length = data[at] | (data[at + 1] << 8);
        if (at + 2 + length > data.size()) { *error = "bad string length"; return false; }
        std::string s(reinterpret_cast<const char*>(data.data() + at + 2), length);
        s = s.c_str();   // stop at the first NUL
        strings->push_back(std::move(s));
    }
    return true;
}

} // namespace gt4::specdb
