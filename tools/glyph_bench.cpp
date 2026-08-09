// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Microbenchmark for the glyph rasterizer (Face::rasterize). Loads a font and
// rasterizes a large set of unique glyphs, reporting glyphs/s and total heap
// allocations (via an operator new/delete counter) to prove the zero-copy
// rasterize + scratch reuse. Build:
//   c++ -std=c++23 -O3 -I include tools/glyph_bench.cpp src/gfx/face.cpp \
//       src/gfx/face_color.cpp src/gfx/stb_impl.cpp -o /tmp/gb && /tmp/gb <font.ttf>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "toe/gfx/face.hpp"

// --- allocation counter ----------------------------------------------------
static std::atomic<std::size_t> g_allocs{0};
static std::atomic<std::size_t> g_bytes{0};
void *operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(n, std::memory_order_relaxed);
    if (void *p = std::malloc(n)) return p;
    throw std::bad_alloc{};
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

using namespace toe::gfx;

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/usr/share/fonts/TTF/DejaVuSansMono.ttf";
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
    std::printf("font: %s (%zu bytes)\n", path, data.size());

    auto face = Face::load(std::move(data), 32);
    if (!face) { std::fprintf(stderr, "Face::load failed\n"); return 1; }

    // Rasterize every glyph index in the font, several passes.
    const int reps = 20;
    // Reset counters after setup so we measure only the rasterize loop.
    const std::size_t a0 = g_allocs.load(), b0 = g_bytes.load();
    std::size_t total = 0, painted = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < reps; ++r) {
        for (std::uint32_t gi = 0; gi < 2000; ++gi) {
            GlyphBitmap g = face->rasterize(gi);
            total++;
            if (!g.pixels.empty()) painted++;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const std::size_t da = g_allocs.load() - a0, db = g_bytes.load() - b0;

    std::printf("rasterized %zu glyphs (%zu painted) in %.4fs => %.0f glyphs/s\n",
                total, painted, secs, total / secs);
    std::printf("heap allocations in loop: %zu (%.2f per glyph), %zu bytes\n",
                da, double(da) / double(total), db);
    return 0;
}
