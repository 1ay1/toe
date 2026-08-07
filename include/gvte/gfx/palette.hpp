// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The 256-color palette and Color -> Rgb resolution. Separating this from the
// screen model keeps SGR handling pure (it only records *which* color, as a
// sum type) and localizes the "what pixels" decision to the renderer.

#ifndef GVTE_GFX_PALETTE_HPP
#define GVTE_GFX_PALETTE_HPP

#include <array>

#include "gvte/core/types.hpp"
#include "gvte/term/cell.hpp"

namespace gvte::gfx {

class Palette {
public:
    // Build the standard xterm 256-color table (16 base + 6x6x6 cube + grays).
    Palette();

    [[nodiscard]] Rgb by_index(std::uint8_t i) const noexcept { return table_[i]; }

    [[nodiscard]] Rgb default_fg() const noexcept { return fg_; }
    [[nodiscard]] Rgb default_bg() const noexcept { return bg_; }

    // Resolve a terminal Color to concrete RGB. `is_fg` selects which default
    // to substitute for DefaultColor.
    [[nodiscard]] Rgb resolve(const term::Color &c, bool is_fg) const noexcept;

private:
    std::array<Rgb, 256> table_{};
    Rgb fg_{rgb(220, 220, 220)};
    Rgb bg_{rgb(23, 23, 28)};
};

} // namespace gvte::gfx

#endif // GVTE_GFX_PALETTE_HPP
