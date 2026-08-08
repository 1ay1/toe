// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Correctness: after a sequence of mutations, a cache-warmed renderer must
// produce byte-identical pixels to a fresh renderer rendering the same final
// screen. Exercises the per-row damage cache: incremental rebuild, cursor
// moves, colored/reverse cells, and a full clear+rewrite.
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>
#include <epoxy/gl.h>
#include "toe/gfx/font.hpp"
#include "toe/gfx/renderer.hpp"
#include "toe/platform/backend.hpp"
#include "toe/term/screen.hpp"
#include "toe/vt/parser.hpp"
using namespace toe;

static std::vector<unsigned char> render_to_pixels(gfx::Renderer& r, term::Screen& s,
                                                    int W, int H, GLuint fbo) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0,0,W,H);
    r.draw(s, PixelSize{W,H});
    glFinish();
    std::vector<unsigned char> px(static_cast<size_t>(W)*H*4);
    glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,px.data());
    return px;
}

int main() {
    constexpr int W=640,H=384;
    auto surface = platform::open_surface("cachetest", PixelSize{64,64});
    if(!surface){ std::fprintf(stderr,"skip: no surface\n"); return 77; }
    (*surface).swap();
    GLuint tex=0,fbo=0; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);

    auto mk_atlas=[]{ return gfx::FontAtlas::create("monospace",14); };
    auto a1=mk_atlas(); auto a2=mk_atlas();
    if(!a1||!a2){ std::fprintf(stderr,"no font\n"); return 1; }
    auto warm = gfx::Renderer::create(std::move(*a1));
    auto fresh_maker = std::move(*a2);

    term::Screen s{Extent{40,20}}; // small grid
    vt::Parser parser;
    auto feed=[&](const std::string& t){ parser.feed(std::span<const char>{t.data(),t.size()},
        [&](const vt::Action& act){ toe::Cmds out; s.apply(act,out); }); };

    // A sequence of mutations, rendering the warm renderer after each so its
    // cache is exercised incrementally. Deliberately covers the ops that bypass
    // the per-cell write funnel (scroll, insert/delete lines, erase-display,
    // alt-screen swap) — exactly where a missed per-row damage stamp would
    // leave the cache stale.
    std::vector<std::string> steps = {
        "hello \x1b[31mworld\x1b[0m",
        "\r\n\x1b[32msecond line\x1b[0m",
        "\r\nthird\r\nfourth\r\n",
        "\x1b[1;1Hchanged top",           // cursor jump + overwrite row 0
        "\x1b[5;5Hmid\x1b[44m block\x1b[0m",
        // Fill past the bottom so the region scrolls (content shifts up).
        "\x1b[20;1Hbottom\r\nscroll me 1\r\nscroll me 2\r\nscroll me 3",
        "\x1b[3;1H\x1b[Linserted line via IL",   // DL/IL path
        "\x1b[4;1H\x1b[Mdeleted line via DL",
        "\x1b[?1049h",                    // enter alt screen
        "\x1b[Halt screen \x1b[35mcontent\x1b[0m\r\nalt row 2",
        "\x1b[?1049l",                    // leave alt screen (restore primary)
        "\x1b[2J\x1b[Hcleared and rewritten\r\nline b\r\nline c",
    };
    for (auto& st : steps) { feed(st); (void)render_to_pixels(*warm, s, W, H, fbo); }

    // Final warm render.
    auto warm_px = render_to_pixels(*warm, s, W, H, fbo);

    // Fresh renderer, same final screen, cold cache.
    auto fresh = gfx::Renderer::create(std::move(fresh_maker));
    auto fresh_px = render_to_pixels(*fresh, s, W, H, fbo);

    if (warm_px.size()!=fresh_px.size() || std::memcmp(warm_px.data(),fresh_px.data(),warm_px.size())!=0) {
        // count differing pixels
        size_t diff=0; for(size_t i=0;i<warm_px.size();i+=4) if(memcmp(&warm_px[i],&fresh_px[i],4)) ++diff;
        std::printf("FAIL: %zu/%d pixels differ between cached and fresh render\n", diff, W*H);
        return 1;
    }
    std::printf("PASS: cache-warmed render is byte-identical to fresh render\n");
    return 0;
}
