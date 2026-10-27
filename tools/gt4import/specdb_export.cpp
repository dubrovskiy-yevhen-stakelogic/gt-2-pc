// Export decoded GTDB rows and GTID labels for the model-set preparation scripts.
#include "specdb/specdb.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace {
std::vector<uint8_t> Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open " + path.string());
    return {(std::istreambuf_iterator<char>(file)), {}};
}
void Quote(std::ostream& out, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c < 32) out << "\\u00" << hex[c >> 4] << hex[c & 15];
        else out << char(c);
    }
    out << '"';
}
void ExportTable(const fs::path& input, const fs::path& output) {
    gt4::specdb::Table table;
    std::string error;
    if (!table.Load(Read(input), &error)) throw std::runtime_error(input.string() + ": " + error);
    auto labelPath = input;
    labelPath.replace_extension(".idi");
    std::map<uint32_t, std::string> labels;
    if (fs::exists(labelPath) && !gt4::specdb::LoadLabels(Read(labelPath), &labels, &error))
        throw std::runtime_error(labelPath.string() + ": " + error);
    std::ofstream out(output, std::ios::binary);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << "{\n\"row_size\": " << table.rowSize() << ",\n\"rows\": [\n";
    constexpr char hex[] = "0123456789abcdef";
    std::vector<uint8_t> row;
    for (uint32_t i = 0; i < table.rowCount(); ++i) {
        if (!table.Row(i, &row, &error)) throw std::runtime_error(input.string() + ": " + error);
        const auto id = table.rowId(i);
        if (i) out << ",\n";
        out << "{\"id\":" << id << ",\"label\":";
        const auto label = labels.find(id);
        Quote(out, label == labels.end() ? std::string_view{} : std::string_view(label->second));
        out << ",\"hex\":\"";
        for (uint8_t byte : row) out << hex[byte >> 4] << hex[byte & 15];
        out << "\"}";
    }
    out << "\n]}\n";
    out.close();
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Usage: gt4specdbexport <raw-specdb-directory> <new-json-directory>");
        const fs::path input = argv[1], output = argv[2];
        if (!fs::is_directory(input)) throw std::runtime_error("SpecDB directory is missing");
        if (fs::exists(output)) throw std::runtime_error("Choose a new output directory");
        std::vector<fs::path> tables;
        for (const auto& entry : fs::directory_iterator(input))
            if (entry.is_regular_file() && entry.path().extension() == ".dbt") tables.push_back(entry.path());
        if (tables.empty()) throw std::runtime_error("No .dbt tables found");
        std::sort(tables.begin(), tables.end());
        fs::create_directories(output);
        for (const auto& table : tables) ExportTable(table, output / (table.stem().string() + ".json"));
        std::cout << "Exported " << tables.size() << " SpecDB tables\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
