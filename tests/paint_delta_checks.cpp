// Checks the GT4 paint delta reader (palette remap, rectangle and pixel list).
#include "gt2formats/paint_delta.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

uint32_t Pack(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24;
}

void Add32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(value >> (8 * i)));
}

std::vector<uint8_t> Header(uint32_t type) {
    std::vector<uint8_t> out{'G', 'T', '4', 'P'};
    Add32(out, 1);
    Add32(out, type);
    return out;
}

void Write(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

gt2::PngImage Base() {
    gt2::PngImage image;
    image.width = 3;
    image.height = 2;
    for (uint8_t i = 0; i < 6; ++i) {
        const uint8_t r = uint8_t(i * 4 + 1), g = uint8_t(i * 4 + 2), b = uint8_t(i * 4 + 3), a = uint8_t(i * 4 + 4);
        image.rgba.insert(image.rgba.end(), {r, g, b, a});
    }
    return image;
}

uint32_t Pixel(const gt2::PngImage& image, size_t index) {
    uint32_t value;
    std::memcpy(&value, &image.rgba[index * 4], 4);
    return value;
}

void Expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "gt2-paint-delta-checks";
    std::filesystem::create_directories(folder);
    try {
        const gt2::PngImage base = Base();

        auto remap = Header(0);
        Add32(remap, 1);
        Add32(remap, Pack(1, 2, 3, 4));
        Add32(remap, Pack(90, 91, 92, 93));
        Write(folder / "remap.delta.gz", remap);
        const auto painted = gt2::ReadPaintVariant(base, (folder / "remap.delta.gz").string());
        Expect(Pixel(painted, 0) == Pack(90, 91, 92, 93), "palette remap did not replace the colour");
        Expect(Pixel(painted, 1) == Pixel(base, 1), "palette remap changed another colour");

        auto rectangle = Header(1);
        Add32(rectangle, 1); Add32(rectangle, 1); Add32(rectangle, 2); Add32(rectangle, 1);
        rectangle.insert(rectangle.end(), {50, 51, 52, 53, 60, 61, 62, 63});
        Write(folder / "rect.delta.gz", rectangle);
        const auto cropped = gt2::ReadPaintVariant(base, (folder / "rect.delta.gz").string());
        Expect(Pixel(cropped, 4) == Pack(50, 51, 52, 53), "rectangle first pixel");
        Expect(Pixel(cropped, 5) == Pack(60, 61, 62, 63), "rectangle second pixel");
        Expect(Pixel(cropped, 0) == Pixel(base, 0), "rectangle changed a pixel outside its bounds");

        auto pixels = Header(2);
        Add32(pixels, 2);
        Add32(pixels, 0); Add32(pixels, Pack(70, 71, 72, 73));
        Add32(pixels, 5); Add32(pixels, Pack(80, 81, 82, 83));
        Write(folder / "pixels.delta.gz", pixels);
        const auto sparse = gt2::ReadPaintVariant(base, (folder / "pixels.delta.gz").string());
        Expect(Pixel(sparse, 0) == Pack(70, 71, 72, 73), "pixel list first pixel");
        Expect(Pixel(sparse, 5) == Pack(80, 81, 82, 83), "pixel list second pixel");

        auto outside = Header(1);
        Add32(outside, 2); Add32(outside, 0); Add32(outside, 2); Add32(outside, 1);
        outside.insert(outside.end(), 8, 0);
        Write(folder / "outside.delta.gz", outside);
        bool rejected = false;
        try { (void)gt2::ReadPaintVariant(base, (folder / "outside.delta.gz").string()); }
        catch (const std::runtime_error&) { rejected = true; }
        Expect(rejected, "rectangle outside the base image was accepted");
    } catch (...) {
        std::filesystem::remove_all(folder);
        throw;
    }
    std::filesystem::remove_all(folder);
    std::puts("PASS: paint delta remap, rectangle, pixel list and bounds");
    return 0;
}
