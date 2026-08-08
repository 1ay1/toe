// SPDX-License-Identifier: LGPL-2.0-or-later
//
// toe-demo: a minimal host that drives a toe::Terminal through the public
// state-machine API, using the direct platform surface (Wayland/X11, no SDL).
// It owns the window + event loop; toe owns everything from PTY to pixels.

#include <cstdio>

#include <epoxy/gl.h>

#include "toe/gfx/render_target.hpp"
#include "toe/platform/backend.hpp"
#include "toe/terminal.hpp"

int main() {
    auto surface = toe::platform::open_surface("toe-demo", toe::PixelSize{960, 600});
    if (!surface) {
        std::fprintf(stderr, "surface: %s\n", surface.error().message.c_str());
        return 1;
    }
    toe::platform::AnySurface &surf = *surface;

    toe::PixelSize px = surf.pixel_size();

    toe::Config cfg;
    auto term = toe::Terminal::create(cfg, px);
    if (!term) {
        std::fprintf(stderr, "terminal: %s\n", term.error().message.c_str());
        return 1;
    }

    bool running = true;
    while (running && !surf.should_close()) {
        // The state machine's only transition: Running -> (Running | Exited).
        toe::Terminal::Poll p = term->poll();
        if (p.exited) {
            return p.exited->code; // dead terminal: nothing to render or type into
        }
        toe::Session &session = *p.running;

        // Drain native events; the platform layer hands us the neutral sum type.
        surf.poll_events([&](const toe::platform::Event &ev) {
            std::visit(
                [&](auto &&e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, toe::platform::CloseRequested>) {
                        running = false;
                    } else if constexpr (std::is_same_v<T, toe::platform::Resized>) {
                        px = e.size;
                        session.resize(px);
                    } else if constexpr (std::is_same_v<T, toe::platform::KeyPressed>) {
                        session.send_key(e.key);
                    } else if constexpr (std::is_same_v<T, toe::platform::TextEntered>) {
                        toe::KeyEvent k;
                        k.key = toe::TextInput{std::string{e.utf8}};
                        session.send_key(k);
                    }
                },
                ev);
        });

        glViewport(0, 0, px.w, px.h);
        // The GL context is current after open_surface; adopt it as the render
        // capability. Default target = FBO 0 (the window's back buffer).
        auto rc = toe::gfx::RenderContext::adopt_current();
        session.render(rc, px);
        surf.swap();
    }

    return 0;
}
