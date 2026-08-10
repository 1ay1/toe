// Does an emoji actually render in COLOUR on this machine?
//
// Fallback DISCOVERY finding seguiemj.ttf is not the same as RENDERING it:
// Windows' emoji font uses the COLR/CPAL layered-vector format, while toe's
// colour backend reads CBDT/CBLC (bitmap) only. If COLR is unsupported the
// glyph silently degrades to a monochrome outline or .notdef -- which looks
// broken, not "beautiful out of the box".
//
// This loads the face the fallback chain would pick and reports, per codepoint,
// whether we get a COLOUR bitmap, a mono glyph, or nothing.
#include "toe/gfx/face.hpp"
#include "toe/gfx/font_discovery.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static std::vector<std::uint8_t> slurp(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

int main() {
    toe::gfx::FontDiscovery fd;
    const std::vector<std::string> none;

    struct Probe { char32_t cp; const char *what; };
    const Probe probes[] = {
        {0x1F600, "grinning face"},
        {0x1F680, "rocket"},
        {0x2764,  "heavy heart"},
        {0x1F44D, "thumbs up"},
        {0x1F3F3, "waving flag"},
        {0x26A1,  "high voltage"},
    };

    int color_ok = 0, mono = 0, missing = 0;
    for (const auto &p : probes) {
        const auto path = fd.resolve(p.cp, none);
        if (!path) {
            std::printf("  U+%05X %-16s -> NO FONT\n", (unsigned)p.cp, p.what);
            ++missing;
            continue;
        }
        auto face = toe::gfx::Face::load(slurp(*path), 18);
        if (!face) {
            std::printf("  U+%05X %-16s -> font unreadable (%s)\n", (unsigned)p.cp, p.what,
                        path->c_str());
            ++missing;
            continue;
        }
        const bool face_color = face->is_color();
        const auto bmp = face->rasterize(face->glyph_index(p.cp));
        const char *verdict = bmp.pixels.empty() ? "BLANK"
                              : bmp.is_color     ? "colour"
                                                 : "MONO";
        if (bmp.is_color) ++color_ok;
        else if (!bmp.pixels.empty()) ++mono;
        else ++missing;
        std::printf("  U+%05X %-16s -> %-6s  (face is_color=%d)  %s\n", (unsigned)p.cp, p.what,
                    verdict, (int)face_color, path->c_str());
    }

    std::printf("colour=%d mono=%d missing=%d\n", color_ok, mono, missing);
    // A machine with no colour emoji font installed (a minimal container, a
    // stripped Linux image) can't satisfy this — skip rather than fail.
    if (color_ok == 0 && mono == 0) {
        std::printf("SKIP: no emoji font discoverable\n");
        return 77;
    }
    if (color_ok == 0) {
        std::printf("FAIL: emoji resolve but render MONOCHROME — the fallback "
                    "font's colour format (CBDT/CBLC or COLR/CPAL) is unsupported\n");
        return 1;
    }
    std::printf("ALL EMOJI RENDER IN COLOUR\n");
    return 0;
}
