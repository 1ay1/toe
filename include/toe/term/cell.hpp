// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The atoms of the grid: a Cell is a codepoint plus visual attributes. We make
// every attribute a distinct type — no bare bools threaded positionally, no
// int color that could be an index or an RGB. Color is a sum type (default vs
// palette index vs true-color) so "which kind of color" is impossible to
// confuse with "which value".

#ifndef TOE_TERM_CELL_HPP
#define TOE_TERM_CELL_HPP

#include <cstdint>
#include <variant>

#include "toe/core/types.hpp"

namespace toe::term {

// --- color as a sum type ---------------------------------------------------
// The terminal's notion of a color is one of three disjoint things. Encoding
// that as a variant means the renderer must handle every case explicitly.

struct DefaultColor {
    constexpr auto operator<=>(const DefaultColor &) const = default;
};

struct IndexedColor {
    std::uint8_t index{}; // 0-255 palette slot
    constexpr auto operator<=>(const IndexedColor &) const = default;
};

struct TrueColor {
    Rgb rgb{};
    constexpr auto operator<=>(const TrueColor &) const = default;
};

using Color = std::variant<DefaultColor, IndexedColor, TrueColor>;

// --- text style flags ------------------------------------------------------
// A scoped-enum bitset. Operator overloads keep the bit-twiddling type-safe:
// you can only combine Attr with Attr, and the result is still an Attr.

enum class Attr : std::uint16_t {
    None = 0,
    Bold = 1u << 0,
    Faint = 1u << 1,
    Italic = 1u << 2,
    Underline = 1u << 3,
    Blink = 1u << 4,
    Reverse = 1u << 5,
    Hidden = 1u << 6,
    Strike = 1u << 7,
    Overline = 1u << 8,
};

[[nodiscard]] constexpr Attr operator|(Attr a, Attr b) noexcept {
    return static_cast<Attr>(static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
}
[[nodiscard]] constexpr Attr operator&(Attr a, Attr b) noexcept {
    return static_cast<Attr>(static_cast<std::uint16_t>(a) & static_cast<std::uint16_t>(b));
}
[[nodiscard]] constexpr Attr operator~(Attr a) noexcept {
    return static_cast<Attr>(~static_cast<std::uint16_t>(a));
}
constexpr Attr &operator|=(Attr &a, Attr b) noexcept { return a = a | b; }
constexpr Attr &operator&=(Attr &a, Attr b) noexcept { return a = a & b; }

[[nodiscard]] constexpr bool has(Attr set, Attr flag) noexcept {
    return (set & flag) != Attr::None;
}

// Underline style (SGR 4, 4:1..4:5, 21). Kept as a small enum in the Pen so a
// cell can be single / double / curly / dotted / dashed underlined. The
// Attr::Underline flag still gates whether ANY underline is drawn.
enum class Underline : std::uint8_t {
    None = 0,
    Single = 1,
    Double = 2,
    Curly = 3,
    Dotted = 4,
    Dashed = 5,
};

// --- the rendition (SGR state) ---------------------------------------------
// The "pen" the terminal draws with: current fg/bg and active style flags.
struct Pen {
    Color fg{DefaultColor{}};
    Color bg{DefaultColor{}};
    Color underline_color{DefaultColor{}}; // DefaultColor => draw underline in fg
    Attr attr{Attr::None};
    Underline underline{Underline::None};
    constexpr auto operator<=>(const Pen &) const = default;
};

// --- a grid cell -----------------------------------------------------------
struct Cell {
    char32_t cp{U' '}; // the glyph; U' ' == blank
    Pen pen{};
    // Display width: 1 = normal, 2 = lead cell of a double-width (CJK/emoji)
    // glyph, 0 = the trailing spacer cell it occupies (renderer skips it).
    std::uint8_t width{1};
    // OSC 8 hyperlink id (0 = none). Indexes into the Screen's link table; kept
    // small so it fits the cell's existing padding — Cell stays 24 bytes.
    std::uint16_t link{0};

    [[nodiscard]] constexpr bool blank() const noexcept {
        return cp == U' ' && pen == Pen{};
    }
    [[nodiscard]] constexpr bool spacer() const noexcept { return width == 0; }
    constexpr auto operator<=>(const Cell &) const = default;
};

} // namespace toe::term

#endif // TOE_TERM_CELL_HPP
