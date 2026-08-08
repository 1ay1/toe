// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The 256-color palette and Color -> Rgb resolution. Separating this from the
// screen model keeps SGR handling pure (it only records *which* color, as a
// sum type) and localizes the "what pixels" decision to the renderer.

#ifndef TOE_GFX_PALETTE_HPP
#define TOE_GFX_PALETTE_HPP

#include <array>
#include <optional>
#include <variant>

#include "toe/core/types.hpp"
#include "toe/term/cell.hpp"

namespace toe::gfx {

class Palette {
public:
    // Build the standard xterm 256-color table (16 base + 6x6x6 cube + grays).
    Palette();

    [[nodiscard]] Rgb by_index(std::uint8_t i) const noexcept { return table_[i]; }

    [[nodiscard]] Rgb default_fg() const noexcept { return fg_; }
    [[nodiscard]] Rgb default_bg() const noexcept { return bg_; }
    [[nodiscard]] Rgb cursor_color() const noexcept { return cursor_ ? *cursor_ : fg_; }

    // Dynamic colour control (OSC 4/104, 10/11, 12/112). Setters return true
    // when the value actually changed, so the renderer can damage on change.
    bool set_index(std::uint8_t i, Rgb c) noexcept {
        if (table_[i] == c) return false;
        table_[i] = c;
        return true;
    }
    bool set_default_fg(Rgb c) noexcept { return assign(fg_, c); }
    bool set_default_bg(Rgb c) noexcept { return assign(bg_, c); }
    bool set_cursor_color(std::optional<Rgb> c) noexcept {
        if (cursor_ == c) return false;
        cursor_ = c;
        return true;
    }
    // Restore the built-in defaults (OSC 104/110/111/112 with no params).
    void reset() { *this = Palette{}; }
    bool reset_index(std::uint8_t i) noexcept { return set_index(i, Palette{}.by_index(i)); }

    // Resolve a terminal Color to concrete RGB. `is_fg` selects which default
    // to substitute for DefaultColor. Inline + branch-on-index (not std::visit)
    // — this is the renderer's hottest per-cell call, so it must inline into
    // the draw loop.
    [[nodiscard]] Rgb resolve(const term::Color &c, bool is_fg) const noexcept {
        switch (c.index()) {
        case 1: // IndexedColor
            return table_[std::get_if<term::IndexedColor>(&c)->index];
        case 2: // TrueColor
            return std::get_if<term::TrueColor>(&c)->rgb;
        default: // DefaultColor (0) or valueless
            return is_fg ? fg_ : bg_;
        }
    }

private:
    static bool assign(Rgb &dst, Rgb v) noexcept {
        if (dst == v) return false;
        dst = v;
        return true;
    }
    std::array<Rgb, 256> table_{};
    Rgb fg_{rgb(220, 220, 220)};
    Rgb bg_{rgb(23, 23, 28)};
    std::optional<Rgb> cursor_{}; // OSC 12; nullopt => follow fg
};

} // namespace toe::gfx

#endif // TOE_GFX_PALETTE_HPP
