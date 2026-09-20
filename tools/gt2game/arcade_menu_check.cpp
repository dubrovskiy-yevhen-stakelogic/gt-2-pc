#include "arcade_menu_check.h"

#include <cstdio>
#include <stdexcept>

namespace gt2game {

void WriteMenuPrimListing(const std::string& path, const std::vector<gt2::MenuPrim>& prims) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) throw std::runtime_error("cannot write " + path);
    int mode = -1;
    for (const gt2::MenuPrim& p : prims) {
        if (p.kind == gt2::MenuPrim::kPolyFT4) mode = p.tpage; // set by the primitive itself
        if (int(p.tpage) != mode) {
            mode = p.tpage;
            std::fprintf(f, "E1 tpage=%03X blend=%u\n", unsigned(p.tpage & 0x7FF), unsigned((p.tpage >> 5) & 3));
        }
        const unsigned rgb = unsigned(p.colour[0] & 0xFFFFFF);
        if (p.kind == gt2::MenuPrim::kPolyFT4) { // the primitive's texture page replaces the draw mode's page bits (no E1 packet)
            std::fprintf(f, "POLY %02X quad tex%s rgb=%06X v0=(%d,%d) uv0=(%u,%u) clut=%04X v1=(%d,%d) uv1=(%u,%u) tpage=%04X v2=(%d,%d) uv2=(%u,%u) v3=(%d,%d) uv3=(%u,%u)\n",
                         p.semi ? 0x2E : 0x2C, p.semi ? " semi" : "", rgb, p.x[0], p.y[0], p.tu[0], p.tv[0], p.clut, p.x[1], p.y[1], p.tu[1], p.tv[1], p.tpage, p.x[2], p.y[2],
                         p.tu[2], p.tv[2], p.x[3], p.y[3], p.tu[3], p.tv[3]);
            continue;
        }
        switch (p.kind) {
        case gt2::MenuPrim::kSprite:
            std::fprintf(f, "RECT %02X rgb=%06X xy=(%d,%d) uv=(%u,%u) clut=%04X size=(%d,%d)%s\n", p.semi ? 0x66 : 0x64, rgb, p.x[0], p.y[0], p.u, p.v, p.clut, p.w,
                         p.h, p.semi ? " semi" : "");
            break;
        case gt2::MenuPrim::kTile:
            std::fprintf(f, "RECT %02X rgb=%06X xy=(%d,%d) size=(%d,%d)%s\n", p.semi ? 0x62 : 0x60, rgb, p.x[0], p.y[0], p.w, p.h, p.semi ? " semi" : "");
            break;
        case gt2::MenuPrim::kLine:
            std::fprintf(f, "LINE %02X rgb=%06X v=(%d,%d) v=(%d,%d)\n", p.semi ? 0x42 : 0x40, rgb, p.x[0], p.y[0], p.x[1], p.y[1]);
            break;
        case gt2::MenuPrim::kPolyF4:
            if (p.x[2] == p.x[3] && p.y[2] == p.y[3])
                std::fprintf(f, "POLY %02X tri%s rgb=%06X v0=(%d,%d) v1=(%d,%d) v2=(%d,%d)\n", p.semi ? 0x22 : 0x20, p.semi ? " semi" : "", rgb, p.x[0], p.y[0], p.x[1],
                             p.y[1], p.x[2], p.y[2]);
            else
                std::fprintf(f, "POLY %02X quad%s rgb=%06X v0=(%d,%d) v1=(%d,%d) v2=(%d,%d) v3=(%d,%d)\n", p.semi ? 0x2A : 0x28, p.semi ? " semi" : "", rgb, p.x[0], p.y[0],
                             p.x[1], p.y[1], p.x[2], p.y[2], p.x[3], p.y[3]);
            break;
        case gt2::MenuPrim::kPolyG4:
            std::fprintf(f, "POLY %02X quad gouraud%s rgb=%06X v0=(%d,%d) rgb1=%06X v1=(%d,%d) rgb2=%06X v2=(%d,%d) rgb3=%06X v3=(%d,%d)\n", p.semi ? 0x3A : 0x38,
                         p.semi ? " semi" : "", rgb, p.x[0], p.y[0], unsigned(p.colour[1] & 0xFFFFFF), p.x[1], p.y[1], unsigned(p.colour[2] & 0xFFFFFF), p.x[2], p.y[2],
                         unsigned(p.colour[3] & 0xFFFFFF), p.x[3], p.y[3]);
            break;
        case gt2::MenuPrim::kPolyFT4: break; // above
        }
    }
    std::fclose(f);
}

} // namespace gt2game
