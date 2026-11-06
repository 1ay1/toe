// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Inline image protocol conformance: kitty graphics (APC _G) and sixel.
//
// toe handles the PROTOCOL -- transmit, store, place, delete, animate -- and
// leaves decoding to the host. So what's tested here is the bookkeeping: that
// a placement lands with the right cell size, that re-placing an id replaces
// rather than duplicates, that an unknown id is refused, and that animation
// timing is both correct and *reportable*.
//
// Animation timing had two real bugs: an unseeded animation reported deadline
// 0 ("nothing scheduled"), so a host polling it to decide when to wake would
// never start the animation at all; and each frame was scheduled from now_ms
// instead of from the deadline it passed, so every late tick permanently
// stretched the loop.

#include "toe/term/update.hpp"

#include <cstdio>
#include <string>
#include <string_view>

using namespace toe;

static int failures = 0;

static void expect(bool cond, const char *what) {
    std::printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) ++failures;
}

static void expect_eq(std::uint64_t got, std::uint64_t want, const char *what) {
    if (got != want) {
        std::printf("FAIL %s\n       got  %llu\n       want %llu\n", what,
                    static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
        ++failures;
    } else {
        std::printf("ok   %s\n", what);
    }
}

static std::string b64(std::string_view in) {
    static constexpr char kT[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    int v = 0, bits = 0;
    for (unsigned char c : in) {
        v = (v << 8) | c;
        bits += 8;
        while (bits >= 6) { o += kT[(v >> (bits - 6)) & 63]; bits -= 6; }
    }
    if (bits) o += kT[(v << (6 - bits)) & 63];
    while (o.size() % 4) o += '=';
    return o;
}

// A model with cell metrics set, as a host would.
static void feed(term::Model &m, std::string_view s) { (void)term::feed_output(m, s); }

int main() {
    // --- kitty: transmit, place, replace, refuse, delete -------------------
    {
        term::Config cfg;
        term::Model m{cfg, Extent{20, 6}};
        m.screen.set_cell_size(8, 16);
        // a=t stores WITHOUT displaying: 2x2 RGBA (f=32) = 16 bytes.
        const std::string px(2 * 2 * 4, '\xff');
        feed(m, "\033_Ga=t,i=1,f=32,s=2,v=2;" + b64(px) + "\033\\");
        expect(m.screen.graphics().image(1) != nullptr, "a=t stores the image");
        expect(m.screen.graphics().placements().empty(), "a=t does not display it");

        // a=p places the stored image.
        feed(m, "\033_Ga=p,i=1,p=7\033\\");
        expect(m.screen.graphics().placements().size() == 1, "a=p places a stored image");

        // The same placement id must REPLACE, not accumulate.
        feed(m, "\033_Ga=p,i=1,p=7\033\\");
        expect(m.screen.graphics().placements().size() == 1,
               "a=p with the same placement id replaces it");

        // An unknown image id must be refused rather than placing nothing-ness.
        feed(m, "\033_Ga=p,i=99,p=1\033\\");
        expect(m.screen.graphics().placements().size() == 1,
               "a=p with an unknown image id is refused");

        // a=d clears placements.
        feed(m, "\033_Ga=d\033\\");
        expect(m.screen.graphics().placements().empty(), "a=d deletes placements");
    }

    // --- kitty: a=T transmits and displays in one go -----------------------
    {
        term::Config cfg;
        term::Model m{cfg, Extent{20, 6}};
        m.screen.set_cell_size(8, 16);
        const std::string px(4 * 4 * 3, '\x7f'); // f=24 is RGB
        feed(m, "\033_Ga=T,i=2,f=24,s=4,v=4;" + b64(px) + "\033\\");
        expect(m.screen.graphics().image(2) != nullptr, "a=T stores the image");
        expect(m.screen.graphics().placements().size() == 1, "a=T also displays it");
        if (!m.screen.graphics().placements().empty()) {
            const auto &p = m.screen.graphics().placements()[0];
            // 4x4 px in 8x16 cells rounds up to one cell.
            expect(p.cols >= 1 && p.rows >= 1, "the placement covers at least one cell");
        }
    }

    // --- PNG needs a host decoder -----------------------------------------
    {
        term::Config cfg;
        term::Model m{cfg, Extent{20, 6}};
        m.screen.set_cell_size(8, 16);
        // f=100 is PNG. With no decoder installed it must be refused cleanly,
        // not stored as garbage.
        term::set_png_decoder({});
        feed(m, "\033_Ga=T,i=3,f=100;" + b64("\x89PNG\r\n\x1a\n not really a png") + "\033\\");
        expect(m.screen.graphics().image(3) == nullptr,
               "PNG is refused when no host decoder is installed");

        // With a decoder, the same transmission succeeds.
        term::set_png_decoder([](const std::uint8_t *, std::size_t, int &w, int &h,
                                 std::vector<std::uint8_t> &rgba) {
            w = h = 2;
            rgba.assign(2 * 2 * 4, 0x40);
            return true;
        });
        feed(m, "\033_Ga=T,i=4,f=100;" + b64("whatever") + "\033\\");
        expect(m.screen.graphics().image(4) != nullptr, "PNG works with a host decoder");
        term::set_png_decoder({}); // leave no global state behind
    }

    // --- sixel -------------------------------------------------------------
    {
        term::Config cfg;
        term::Model m{cfg, Extent{20, 6}};
        m.screen.set_cell_size(8, 16);
        // Raster 6x6, one red colour register, two sixel bands.
        feed(m, "\033Pq\"1;1;6;6#0;2;100;0;0#0~~~~~~$-~~~~~~\033\\");
        expect(!m.screen.graphics().placements().empty(), "sixel produces a placement");
        expect(m.screen.graphics().has_images(), "sixel produces an image");
    }
    {
        // Sixel run-length (!Pn) and a second colour register must parse.
        term::Config cfg;
        term::Model m{cfg, Extent{20, 6}};
        m.screen.set_cell_size(8, 16);
        feed(m, "\033Pq#1;2;0;100;0!12~$-!6?\033\\");
        expect(!m.screen.graphics().placements().empty(), "sixel repeat count parses");
    }

    // --- animation timing --------------------------------------------------
    {
        term::Config cfg;
        term::Model m{cfg, Extent{20, 6}};
        m.screen.set_cell_size(8, 16);

        expect_eq(m.screen.graphics().next_animation_deadline(), 0,
                  "no images: deadline 0 means idle");

        feed(m, "\033_Ga=T,i=1,f=24,s=1,v=1;AAAA\033\\");
        expect(!m.screen.graphics().has_animations(), "a single frame is not an animation");
        expect_eq(m.screen.graphics().next_animation_deadline(), 0,
                  "one frame: still idle");
        expect(!m.screen.tick_animations(5000), "a single frame never advances");

        // a=f appends a frame with a 50ms gap -> now it animates.
        feed(m, "\033_Ga=f,i=1,f=24,s=1,v=1,r=50;////\033\\");
        expect(m.screen.graphics().has_animations(), "two frames is an animation");
        // THE BUG: this used to report 0, which a host reads as "idle", so it
        // would never tick and the animation would sit on frame 0 forever.
        expect(m.screen.graphics().next_animation_deadline() != 0,
               "an unseeded animation does not report itself idle");

        // The first tick seeds the clock without flipping a frame.
        expect(!m.screen.tick_animations(1000), "the first tick seeds, does not flip");
        // Frame 0's gap is the 40ms default, so the next flip is due at 1040.
        expect_eq(m.screen.graphics().next_animation_deadline(), 1040,
                  "after seeding, the deadline is now + frame 0's gap");
        expect(!m.screen.tick_animations(1020), "a tick before the deadline does nothing");
        expect(m.screen.tick_animations(1040), "a tick at the deadline flips the frame");
        // Frame 1's gap is 50ms, scheduled from 1040 (not from now).
        expect_eq(m.screen.graphics().next_animation_deadline(), 1090,
                  "the next deadline follows the previous one, not now_ms");
    }
    {
        // No drift: ticking consistently LATE must not stretch the loop.
        // Scheduling from now_ms made each late tick push the whole animation
        // back, so 500ms of 50ms frames produced fewer than 10 flips.
        term::Config cfg;
        term::Model m{cfg, Extent{20, 6}};
        m.screen.set_cell_size(8, 16);
        feed(m, "\033_Ga=T,i=1,f=24,s=1,v=1;AAAA\033\\");
        feed(m, "\033_Ga=f,i=1,f=24,s=1,v=1,r=50;////\033\\");
        (void)m.screen.tick_animations(1000); // seed
        int flips = 0;
        for (std::uint64_t t = 1060; t <= 1550; t += 50)
            if (m.screen.tick_animations(t)) ++flips;
        expect(flips == 10, "50ms frames ticked late still advance once per frame");
    }

    std::printf(failures ? "graphics: %d FAILURES\n" : "graphics: PASS\n", failures);
    return failures ? 1 : 0;
}
