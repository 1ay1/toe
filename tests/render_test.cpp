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
#include "gvte/platform/surface.hpp"
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
    (*surface)->swap(); // realize surface

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
    // A line of text and a red-on-nothing 'X', plus a green-background space.
    std::string_view input = "Hello, GPU!\r\n\x1b[31mXXXX\x1b[0m \x1b[42m \x1b[0m";
    parser.feed(std::span<const char>{input.data(), input.size()},
                [&](const vt::Action &a) { screen.apply(a); });

    renderer->draw(screen, PixelSize{W, H});
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

    int fails = 0;
    if (!bg_ok) { std::printf("FAIL background color\n"); ++fails; }
    if (nonbg < 200) { std::printf("FAIL glyph coverage too low\n"); ++fails; }
    if (redish < 5) { std::printf("FAIL red SGR glyphs missing\n"); ++fails; }
    if (greenish < 5) { std::printf("FAIL green SGR background missing\n"); ++fails; }

    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);

    if (fails == 0) {
        std::printf("headless render test passed\n");
        return 0;
    }
    return 1;
}
