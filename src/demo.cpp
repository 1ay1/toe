// SPDX-License-Identifier: LGPL-2.0-or-later
//
// gvte-demo: a minimal host that drives a gvte::Terminal through the public
// state-machine API, using the direct platform surface (Wayland/X11, no SDL).
// It owns the window + event loop; gvte owns everything from PTY to pixels.

#include <cstdio>

#include <epoxy/gl.h>

#include "gvte/gfx/render_target.hpp"
#include "gvte/platform/backend.hpp"
#include "gvte/terminal.hpp"

int main() {
    auto surface = gvte::platform::open_surface("gvte-demo", gvte::PixelSize{960, 600});
    if (!surface) {
        std::fprintf(stderr, "surface: %s\n", surface.error().message.c_str());
        return 1;
    }
    gvte::platform::AnySurface &surf = *surface;

    gvte::PixelSize px = surf.pixel_size();

    gvte::Config cfg;
    auto term = gvte::Terminal::create(cfg, px);
    if (!term) {
        std::fprintf(stderr, "terminal: %s\n", term.error().message.c_str());
        return 1;
    }

    bool running = true;
    while (running && !surf.should_close()) {
        // The state machine's only transition: Running -> (Running | Exited).
        gvte::Terminal::Poll p = term->poll();
        if (p.exited) {
            return p.exited->code; // dead terminal: nothing to render or type into
        }
        gvte::Session &session = *p.running;

        // Drain native events; the platform layer hands us the neutral sum type.
        surf.poll_events([&](const gvte::platform::Event &ev) {
            std::visit(
                [&](auto &&e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, gvte::platform::CloseRequested>) {
                        running = false;
                    } else if constexpr (std::is_same_v<T, gvte::platform::Resized>) {
                        px = e.size;
                        session.resize(px);
                    } else if constexpr (std::is_same_v<T, gvte::platform::KeyPressed>) {
                        session.send_key(e.key);
                    } else if constexpr (std::is_same_v<T, gvte::platform::TextEntered>) {
                        gvte::KeyEvent k;
                        k.key = gvte::TextInput{std::string{e.utf8}};
                        session.send_key(k);
                    }
                },
                ev);
        });

        glViewport(0, 0, px.w, px.h);
        // The GL context is current after open_surface; adopt it as the render
        // capability. Default target = FBO 0 (the window's back buffer).
        auto rc = gvte::gfx::RenderContext::adopt_current();
        session.render(rc, px);
        surf.swap();
    }

    return 0;
}
