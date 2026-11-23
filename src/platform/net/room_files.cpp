#include "platform/net/room_files.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace gt2::net {
namespace {
std::string Hex(std::span<const uint8_t> bytes) {
    constexpr char chars[] = "0123456789abcdef";
    std::string text; for (const auto b : bytes) { text += chars[b >> 4]; text += chars[b & 15]; } return text;
}
void Unhex(const std::string& text, std::span<uint8_t> bytes) {
    if (text.size() != bytes.size() * 2) throw std::invalid_argument("invalid room key length");
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return uint8_t(c - '0');
        if (c >= 'a' && c <= 'f') return uint8_t(c - 'a' + 10);
        throw std::invalid_argument("invalid room key encoding");
    };
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(nibble(text[2*i]) * 16 + nibble(text[2*i+1]));
    if (std::all_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b == 0; })) throw std::invalid_argument("empty room key");
}
std::istringstream Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() <= 0 || file.tellg() > 1024) throw std::runtime_error("cannot read room file or invalid size");
    std::string text(size_t(file.tellg()), '\0'); file.seekg(0); file.read(text.data(), std::streamsize(text.size()));
    if (!file) throw std::runtime_error("room file read failed");
    return std::istringstream(text);
}
void WriteNew(const std::filesystem::path& path, const std::string& text) {
#ifdef _WIN32
    auto* file = _wfopen(path.c_str(), L"wbx");
#else
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    auto* file = descriptor < 0 ? nullptr : ::fdopen(descriptor, "wb");
    if (descriptor >= 0 && !file) ::close(descriptor);
#endif
    if (!file) throw std::runtime_error("cannot create room file; choose a new writable path");
    const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) throw std::runtime_error("room file write failed");
}
void CheckAddress(const std::string& address) {
    if (address.empty() || address.size() > 253 || !std::all_of(address.begin(), address.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
    })) throw std::invalid_argument("invalid host name in invitation");
}
}
RoomInvitation ReadInvitation(const std::filesystem::path& path) {
    auto file = Read(path); RoomInvitation value; std::string magic, port, pk, psk, extra;
    if (!(file >> magic >> value.address >> port >> pk >> psk) || magic != "GT2-INVITE-1" || file >> extra)
        throw std::invalid_argument("invalid invitation file");
    CheckAddress(value.address);
    size_t used = 0; const auto number = std::stoul(port, &used);
    if (used != port.size() || !number || number > 65535) throw std::invalid_argument("invalid invitation port");
    value.port = uint16_t(number); Unhex(pk, value.security.hostPublic); Unhex(psk, value.security.invitation);
    return value;
}
void WriteInvitation(const std::filesystem::path& path, const RoomInvitation& value) {
    CheckAddress(value.address);
    if (!value.port) throw std::invalid_argument("invalid invitation port");
    WriteNew(path, "GT2-INVITE-1\n" + value.address + "\n" + std::to_string(value.port) + "\n" +
        Hex(value.security.hostPublic) + "\n" + Hex(value.security.invitation) + "\n");
}
RoomSecurity ReadHostKey(const std::filesystem::path& path) {
    auto file = Read(path); RoomSecurity value; std::string magic, pk, sk, psk, extra;
    if (!(file >> magic >> pk >> sk >> psk) || magic != "GT2-HOST-1" || file >> extra) throw std::invalid_argument("invalid host key file");
    Unhex(pk, value.hostPublic); Unhex(sk, value.hostSecret); Unhex(psk, value.invitation); return value;
}
void WriteHostKey(const std::filesystem::path& path, const RoomSecurity& value) {
    if (!value.Hosting()) throw std::invalid_argument("host key has no private identity");
    WriteNew(path, "GT2-HOST-1\n" + Hex(value.hostPublic) + "\n" + Hex(value.hostSecret) + "\n" + Hex(value.invitation) + "\n");
}
SavedResume ReadResume(const std::filesystem::path& path) {
    auto file = Read(path); SavedResume value; std::string magic, pk, content, token, extra;
    unsigned port = 0, slot = 255;
    if (!(file >> magic >> value.address >> port >> pk >> content >> value.identity.session >> slot >> token) ||
        magic != "GT2-RESUME-1" || file >> extra || !port || port > 65535 || slot >= kSlots || !value.identity.session)
        throw std::invalid_argument("invalid saved room identity");
    CheckAddress(value.address); value.port = uint16_t(port); value.identity.slot = uint8_t(slot);
    Unhex(pk, value.hostPublic); Unhex(content, value.content); Unhex(token, value.identity.token); return value;
}
void WriteResume(const std::filesystem::path& path, const SavedResume& value) {
    CheckAddress(value.address);
    if (!value.port || value.identity.slot >= kSlots || !value.identity.session) throw std::invalid_argument("invalid saved room identity");
    auto temporary = path; temporary += "." + std::to_string(NewSessionId()) + ".new";
    std::ostringstream file;
    file << "GT2-RESUME-1\n" << value.address << '\n' << value.port << '\n' << Hex(value.hostPublic) << '\n'
         << Hex(value.content) << '\n' << value.identity.session << '\n' << unsigned(value.identity.slot) << '\n'
         << Hex(value.identity.token) << '\n';
    WriteNew(temporary, file.str());
    // Replace only this credential atomically; a failed write preserves the old identity.
    try {
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot replace saved room identity");
#else
        std::filesystem::rename(temporary, path);
#endif
    } catch (...) { std::error_code ignored; std::filesystem::remove(temporary, ignored); throw; }
}
} // namespace gt2::net
