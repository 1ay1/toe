// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Headless render test. Creates a hidden GL context, renders a known Screen to
// an offscreen framebuffer, reads the pixels back, and asserts that (a) glyph
// coverage is non-trivial and (b) an SGR-colored cell produces its color. This
// verifies the whole font+renderer path without a visible window or PTY.

#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

#include <epoxy/gl.h>

#include "gvte/gfx/font.hpp"
#include "gvte/gfx/renderer.hpp"
#include "gvte/platform/backend.hpp"
#include "gvte/term/screen.hpp"
#include "gvte/vt/parser.hpp"

using namespace gvte;

int main() {
    // Open a real surface (Wayland/X11 + EGL) to get a current GL context, then
    // render into an offscreen framebuffer and read the pixels back.
    auto surface = platform::open_surface("render-test", PixelSize{64, 64});
    if (!surface) {
        std::fprintf(stderr, "surface: %s\n", surface.error().message.c_str());
        return 77; // skip when no display / compositor
    }
    (*surface).swap(); // realize surface

    constexpr int W = 400, H = 120;

    // Offscreen framebuffer.
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "FBO incomplete\n");
        return 1;
    }
    glViewport(0, 0, W, H);

    // Build the render stack.
    auto atlas = gfx::FontAtlas::create("monospace", 18);
    if (!atlas) {
        std::fprintf(stderr, "font: %s\n", atlas.error().message.c_str());
        return 1;
    }
    const int cw = atlas->cell_width(), ch = atlas->cell_height();
    auto renderer = gfx::Renderer::create(std::move(*atlas));
    if (!renderer) {
        std::fprintf(stderr, "renderer: %s\n", renderer.error().message.c_str());
        return 1;
    }

    Extent grid = renderer->cells_for(PixelSize{W, H});
    term::Screen screen{grid};
    vt::Parser parser;
    // A line of text and a red-on-nothing 'X', a green-background space, then a
    // row of FULL BLOCK glyphs (U+2588) to verify they tile without seams.
    std::string_view input =
        "Hello, GPU!\r\n\x1b[31mXXXX\x1b[0m \x1b[42m \x1b[0m\r\n\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88";
    parser.feed(std::span<const char>{input.data(), input.size()},
                [&](const vt::Action &a) {
                    gvte::Cmds out;
                    screen.apply(a, out);
                });

    renderer->draw(screen, PixelSize{W, H}, /*cursor_on=*/false);
    glFinish();

    // Read back.
    std::vector<unsigned char> px(static_cast<std::size_t>(W) * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

    auto at = [&](int x, int y) {
        const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4;
        return std::array<int, 3>{px[i], px[i + 1], px[i + 2]};
    };

    // Background should be our (23,23,28).
    auto corner = at(1, 1);
    const bool bg_ok = corner[0] == 23 && corner[1] == 23 && corner[2] == 28;

    // Count non-background pixels — glyph coverage from "Hello, GPU!" + XXXX.
    std::size_t nonbg = 0, redish = 0, greenish = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            auto c = at(x, y);
            if (!(c[0] == 23 && c[1] == 23 && c[2] == 28)) ++nonbg;
            if (c[0] > 120 && c[1] < 80 && c[2] < 80) ++redish;
            if (c[1] > 100 && c[0] < 90 && c[2] < 90) ++greenish;
        }
    }

    std::printf("bg_ok=%d nonbg=%zu redish=%zu greenish=%zu (cell %dx%d, grid %dx%d)\n",
                bg_ok, nonbg, redish, greenish, cw, ch, grid.cols, grid.rows);

    // Block-vs-glyph guard: a correctly-rendered glyph covers only PART of its
    // cell; if the atlas sample or UVs collapse, every glyph fills its whole
    // cell as a solid block (the exact symptom of the packed-UV regression).
    // Scan the TEXT rows (0..1) for the densest cell — the block row (2) is
    // intentionally full and excluded. Text glyphs never fill a cell; a solid
    // block is ~100%. Skip the green-background cell (\x1b[42m).
    int worst_cell_cov = 0;
    for (int gr = 0; gr < 2 && gr < grid.rows; ++gr) {
        for (int gc = 0; gc < grid.cols; ++gc) {
            int cov = 0, tot = 0, greenbg = 0;
            for (int y = gr * ch; y < (gr + 1) * ch && y < H; ++y) {
                for (int x = gc * cw; x < (gc + 1) * cw && x < W; ++x) {
                    ++tot;
                    auto c = at(x, y);
                    const bool isbg = (c[0] == 23 && c[1] == 23 && c[2] == 28);
                    if (!isbg) ++cov;
                    if (c[1] > 100 && c[0] < 100 && c[2] < 100) ++greenbg; // green fill
                }
            }
            if (tot == 0 || greenbg * 2 > tot) continue; // skip the green-bg cell
            const int pct = 100 * cov / tot;
            if (pct > worst_cell_cov) worst_cell_cov = pct;
        }
    }
    std::printf("densest glyph cell coverage = %d%% (a solid block would be ~100%%)\n",
                worst_cell_cov);

    int fails = 0;
    if (!bg_ok) { std::printf("FAIL background color\n"); ++fails; }
    if (nonbg < 200) { std::printf("FAIL glyph coverage too low\n"); ++fails; }
    if (redish < 5) { std::printf("FAIL red SGR glyphs missing\n"); ++fails; }
    if (greenish < 5) { std::printf("FAIL green SGR background missing\n"); ++fails; }
    if (worst_cell_cov >= 70) {
        std::printf("FAIL glyphs render as solid blocks (is_glyph/UV/atlas-sample bug)\n");
        ++fails;
    }

    // Block-tiling guard: the FULL BLOCK (U+2588) glyphs must join into a
    // seamless bar. Instead of guessing the block row's pixel band (glyph
    // baseline offsets make that fragile), scan every scanline for one that is
    // fully filled across the first 4 cells (that's the block bar), then check
    // the interior cell boundaries on it for background seams.
    {
        int seams = -1; // -1 = no fully-filled block scanline found yet
        for (int y = 0; y < H && seams != 0; ++y) {
            int filled = 0;
            for (int x = 0; x < 4 * cw && x < W; ++x) {
                auto c = at(x, y);
                if (!(c[0] == 23 && c[1] == 23 && c[2] == 28)) ++filled;
            }
            if (filled < 4 * cw - 4) continue; // this scanline isn't the block bar
            // Found the bar: count interior-boundary background gaps.
            int s = 0;
            for (int gc = 1; gc < 4; ++gc) {
                const int bx = gc * cw;
                if (bx <= 0 || bx >= W) continue;
                auto m = at(bx, y);
                if (m[0] == 23 && m[1] == 23 && m[2] == 28) ++s;
            }
            seams = s;
        }
        std::printf("block-tiling seams = %d (want 0; -1 = block bar not found)\n", seams);
        if (seams != 0) {
            std::printf("FAIL block glyphs don't tile seamlessly\n");
            ++fails;
        }
    }

    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);

    if (fails == 0) {
        std::printf("headless render test passed\n");
        return 0;
    }
    return 1;
}
