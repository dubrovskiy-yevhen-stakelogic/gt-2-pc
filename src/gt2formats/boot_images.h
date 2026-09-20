#pragma once
#include <cstdint>
#include <vector>

namespace gt2 {
struct GuestImage;
struct BootImage {
    int width = 0, height = 0;
    std::vector<uint8_t> rgb;
};
// Reads the first indexed TIM from a named gzip member of the resident executable.
BootImage LoadBootImage(const GuestImage& exe, const char* name);
BootImage DecodeBootTim(const std::vector<uint8_t>& bytes);
}
