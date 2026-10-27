#pragma once
// GT4 SpecDB tables (specdb/GT4_US2560): GTDB rows, GTID labels, GTST strings. Derived from CORE.GT4 US 1.01
// (0x004490f8 table accessor, 0x00449440 Huffman stage, 0x00449600 template-delta stage, 0x00449968 labels,
// 0x00449cd8 strings). Format description: docs/formats/specdb.md.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gt4::specdb {

class Table {
public:
    // Takes ownership of the file bytes. Returns false (with `error`) on malformed input.
    bool Load(std::vector<uint8_t> data, std::string* error);

    uint32_t rowCount() const { return count_; }
    uint32_t rowSize() const { return rowSize_; }
    bool compressed() const { return compressed_; }
    uint32_t rowId(uint32_t index) const;
    uint32_t rowOffset(uint32_t index) const;    // relative to the data block
    uint32_t dataBlock() const { return dataBlock_; }
    const std::vector<uint8_t>& bytes() const { return data_; }

    // Decodes row `index` into `out` (rowSize bytes). Returns false on corrupt data.
    bool Row(uint32_t index, std::vector<uint8_t>* out, std::string* error) const;
    // Binary search by id, like 0x00449190. Returns -1 when absent.
    int Find(uint32_t id) const;

private:
    bool Symbol(uint32_t bits, uint8_t* value, int* length) const;
    uint32_t U32(uint32_t offset) const;

    std::vector<uint8_t> data_;
    uint32_t count_ = 0, rowSize_ = 0, dataBlock_ = 0;
    bool compressed_ = false;
    uint32_t shortCodes_ = 0, longCodes_ = 0, longCount_ = 0, templates_ = 0;
};

// GTID: id -> label.
bool LoadLabels(const std::vector<uint8_t>& data, std::map<uint32_t, std::string>* labels, std::string* error);
// GTST: index -> string (UTF-8 for the localized string DBs).
bool LoadStrings(const std::vector<uint8_t>& data, std::vector<std::string>* strings, std::string* error);

} // namespace gt4::specdb
