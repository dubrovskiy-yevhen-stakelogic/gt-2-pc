#pragma once
#include <cstdint>
#include <span>
#include <string>

namespace gt2 {

// SHA-1 (FIPS 180-4) of a byte range as 40 lower-case hex digits. Used to identify the build of a disc's executable
// (db/*.yaml key the facts by the EXE's SHA-1; gt2formats/exe_profile.h selects the address profile by it).
std::string Sha1Hex(std::span<const uint8_t> bytes);

} // namespace gt2
