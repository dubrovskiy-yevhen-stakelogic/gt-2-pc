#pragma once
#include <array>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gt2view {
// Palette expansion for hardware filtering. RGB expands five-bit channels
// to eight bits; zero-alpha texels are black for premultiplied mip filtering. Mixed STP pages use two
// layers so hardware filtering never blends opaque and semitransparent classes.
class DecodedTextureCache {
public:
    static constexpr uint32_t kLayers = 512, kTableSize = 4096, kTexels = 256 * 256;
    struct Entry { uint32_t page = 0, clutDepth = 0, slot = 0; bool dirty = true, prepared = false; };
    std::array<std::array<uint32_t, 4>, kTableSize> table{};
    std::vector<uint32_t> pixels;
    std::vector<uint32_t> uploads;
    uint32_t hits = 0, misses = 0;
    // Fold high address bits before reducing to the GPU table size. Palette
    // rows and texture depth live above bit 15 and must affect the bucket.
    static uint32_t Bucket(uint32_t page, uint32_t clutDepth) {
        uint32_t hash = (page * 73856093u) ^ (clutDepth * 19349663u);
        hash ^= hash >> 16;
        hash *= 0x7feb352du;
        return (hash ^ (hash >> 15)) & (kTableSize - 1);
    }
    void Begin() {
        uploads.clear(); hits = misses = 0; protected_.clear(); priority_.clear();
        for (auto& [key, entry] : entries_) { (void)key; entry.prepared = false; }
    }
    // Reserve every material of this submission before decoding any of them.
    // Slots used by earlier menus or courses may then be reused safely.
    void Protect(uint32_t page, uint32_t clutDepth, bool priority = false) {
        const auto key = Key(page, clutDepth);
        protected_.insert(key);
        if (priority) priority_.insert(key);
    }
    void Invalidate(uint32_t row, uint32_t count) {
        if (!count) return;
        if (row == 0 && count >= 512) { entries_.clear(); table = {}; uploads.clear(); return; }
        for (auto& [key, entry] : entries_) {
            (void)key;
            const uint32_t imageRow = entry.page >> 16, paletteRow = (entry.clutDepth & 0x0fffffffu) >> 16;
            if ((row < imageRow + 257 && row + count > imageRow) || (row <= paletteRow && row + count > paletteRow)) entry.dirty = true;
        }
    }
    template<class Word>
    void Prepare(uint32_t page, uint32_t clutDepth, const Word* vram, uint32_t rows, bool priority = false) {
        if (pixels.empty()) pixels.resize(size_t(kLayers) * kTexels);
        const uint64_t key = Key(page, clutDepth);
        protected_.insert(key);
        auto it = entries_.find(key);
        if (it == entries_.end()) {
            uint32_t slot = uint32_t(entries_.size()) * 2;
            if (entries_.size() >= kLayers / 2) {
                auto victim = entries_.begin();
                // Priority admission happens before ordinary draws are prepared.
                // Never invalidate a slot already referenced by this submission.
                while (victim != entries_.end() && (victim->second.prepared ||
                       (protected_.contains(victim->first) && (!priority || priority_.contains(victim->first))))) ++victim;
                if (victim == entries_.end()) { ++misses; return; }
                slot = victim->second.slot;
                // Keep a tombstone: shader lookup must still reach later keys
                // in the same probe chain. Its zero slot means cache fallback.
                for (auto& target : table)
                    if (target[3] == 1 && Key(target[0], target[1]) == victim->first) { target[2] = 0; target[3] = 2; }
                entries_.erase(victim);
            }
            it = entries_.emplace(key, Entry{page, clutDepth, slot, true}).first;
        }
        auto& entry = it->second;
        entry.prepared = true;
        uint32_t bucket = Bucket(page, clutDepth);
        uint32_t available = kTableSize;
        for (uint32_t probe = 0; probe < 16; ++probe, bucket = (bucket + 1) & (kTableSize - 1)) {
            const auto& candidate = table[bucket];
            if (candidate[3] == 1 && candidate[0] == page && candidate[1] == clutDepth) { available = bucket; break; }
            if (candidate[3] != 1 && available == kTableSize) available = bucket;
        }
        if (available != kTableSize) {
            auto& target = table[available];
            if (entry.dirty) {
                uint32_t classes = 0;
                const uint32_t px = page & 65535, py = page >> 16;
                const uint32_t cx = clutDepth & 65535, cy = (clutDepth & 0x0fffffffu) >> 16, depth = clutDepth >> 28;
                const auto word = [&](uint32_t x, uint32_t y) { const uint64_t at = uint64_t(y) * 1024 + x; return at < uint64_t(rows) * 1024 ? vram[at] : 0u; };
                for (uint32_t y = 0; y < 256; ++y) for (uint32_t x = 0; x < 256; ++x) {
                    uint32_t texel;
                    if (depth == 0) { const auto packed = word(px + x / 4, py + y); texel = word(cx + ((packed >> ((x & 3) * 4)) & 15), cy); }
                    else if (depth == 1) { const auto packed = word(px + x / 2, py + y); texel = word(cx + ((packed >> ((x & 1) * 8)) & 255), cy); }
                    else texel = word(px + x, py + y);
                    if (texel) classes |= (texel & 32768) ? 2 : 1;
                    pixels[size_t(entry.slot) * kTexels + y * 256 + x] = texel ?
                        Expand(texel & 31) | (Expand((texel >> 5) & 31) << 8) | (Expand((texel >> 10) & 31) << 16) | ((texel & 32768) ? 0x80000000u : 0xff000000u) : 0;
                }
                const size_t base = size_t(entry.slot) * kTexels;
                for (uint32_t i = 0; i < kTexels; ++i) {
                    auto& pixel = pixels[base + i];
                    if (classes == 3) {
                        pixels[base + kTexels + i] = (pixel >> 24) == 128 ? pixel | 0xff000000u : 0;
                        if ((pixel >> 24) == 128) pixel = 0;
                    } else if (pixel) pixel |= 0xff000000u;
                }
                target = {page, clutDepth, (entry.slot + 1) | (classes == 2 ? 65536u : 0u) | (classes == 3 ? 131072u : 0u), 1};
                uploads.push_back(entry.slot);
                if (classes == 3) uploads.push_back(entry.slot + 1);
                entry.dirty = false;
            }
            if (target[2]) ++hits; else ++misses;
            return;
        }
        ++misses;
    }
private:
    static uint32_t Expand(uint32_t v) { return (v << 3) | (v >> 2); }
    static uint64_t Key(uint32_t page, uint32_t clutDepth) { return (uint64_t(page) << 32) | clutDepth; }
    std::unordered_map<uint64_t, Entry> entries_;
    std::unordered_set<uint64_t> protected_, priority_;
};
}
