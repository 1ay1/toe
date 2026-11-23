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
#include <optional>
#include <type_traits>

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

// One of the three, in FOUR bytes on every platform: which (index()) and its
// value. This was a std::variant<DefaultColor, IndexedColor, TrueColor>, and
// a variant's index is a library choice -- one byte on libstdc++, four on
// libc++ -- so on the Mac every Color was 8 bytes, every Pen 28 and every
// Cell 36 instead of 24: half again the memory for the same scrollback, and
// a rewrap / render / copy that much slower. Plain bytes, every one always
// written (a value, never padding), so equality is one compare and the
// layout is the same everywhere; `static_assert(sizeof(Cell) == 24)` below.
//
//   c.index()           0 default, 1 indexed, 2 true colour (as the variant)
//   c.is<TrueColor>()   which one
//   c.as<TrueColor>()   the value, or nullopt
class Color {
public:
    constexpr Color() noexcept = default;
    constexpr Color(DefaultColor) noexcept {}
    constexpr Color(IndexedColor c) noexcept : kind_{1}, a_{c.index} {}
    constexpr Color(TrueColor c) noexcept : kind_{2}, a_{c.rgb.r}, b_{c.rgb.g}, c_{c.rgb.b} {}

    [[nodiscard]] constexpr std::size_t index() const noexcept { return kind_; }
    template <class T> [[nodiscard]] constexpr bool is() const noexcept { return kind_ == kind_of<T>(); }
    template <class T> [[nodiscard]] constexpr std::optional<T> as() const noexcept {
        if (!is<T>()) return std::nullopt;
        if constexpr (std::is_same_v<T, IndexedColor>) return IndexedColor{a_};
        else if constexpr (std::is_same_v<T, TrueColor>) return TrueColor{Rgb{a_, b_, c_}};
        else return DefaultColor{};
    }
    // kind first, then the value: the order std::variant had
    constexpr auto operator<=>(const Color &) const = default;

private:
    template <class T> static constexpr std::uint8_t kind_of() noexcept {
        static_assert(std::is_same_v<T, DefaultColor> || std::is_same_v<T, IndexedColor> || std::is_same_v<T, TrueColor>,
                      "a Color is a DefaultColor, an IndexedColor or a TrueColor");
        return std::is_same_v<T, DefaultColor> ? 0 : std::is_same_v<T, IndexedColor> ? 1 : 2;
    }
    std::uint8_t kind_ = 0, a_ = 0, b_ = 0, c_ = 0;
};
static_assert(sizeof(Color) == 4 && std::is_trivially_copyable_v<Color>);

[[nodiscard]] constexpr bool same_color(const Color &a, const Color &b) noexcept { return a == b; }

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
[[nodiscard]] constexpr Attr operator^(Attr a, Attr b) noexcept {
    return static_cast<Attr>(static_cast<std::uint16_t>(a) ^ static_cast<std::uint16_t>(b));
}
constexpr Attr &operator|=(Attr &a, Attr b) noexcept { return a = a | b; }
constexpr Attr &operator&=(Attr &a, Attr b) noexcept { return a = a & b; }
constexpr Attr &operator^=(Attr &a, Attr b) noexcept { return a = a ^ b; }

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
    [[nodiscard]] constexpr bool operator==(const Pen &o) const noexcept {
        return attr == o.attr && underline == o.underline && same_color(fg, o.fg) && same_color(bg, o.bg) &&
               same_color(underline_color, o.underline_color);
    }
    /// == Pen{}, without building one: the blank test runs on every trailing
    /// cell a reflow trims.
    [[nodiscard]] constexpr bool is_default() const noexcept {
        return attr == Attr::None && underline == Underline::None && fg.index() == 0 && bg.index() == 0 &&
               underline_color.index() == 0;
    }
};

// --- a grid cell -----------------------------------------------------------
struct Cell {
    char32_t cp{U' '}; // the base glyph; U' ' == blank
    Pen pen{};
    // Display width: 1 = normal, 2 = lead cell of a double-width (CJK/emoji)
    // glyph, 0 = the trailing spacer cell it occupies (renderer skips it).
    std::uint8_t width{1};
    // Combining marks attached to `cp` (accents, ZWJ emoji sequences), as a
    // 1-based index into the Screen's mark table; 0 = none. Lives in the
    // cell's existing padding byte, so Cell stays 24 bytes.
    std::uint8_t marks{0};
    // OSC 8 hyperlink id (0 = none). Indexes into the Screen's link table; kept
    // small so it fits the cell's existing padding — Cell stays 24 bytes.
    std::uint16_t link{0};

    [[nodiscard]] constexpr bool blank() const noexcept {
        return cp == U' ' && marks == 0 && pen.is_default();
    }
    [[nodiscard]] constexpr bool spacer() const noexcept { return width == 0; }
    constexpr auto operator<=>(const Cell &) const = default;
};
// The scrollback is an array of these (10k lines x 200 columns = 2M cells):
// every byte here is 2 MB there. Same size on every platform.
static_assert(sizeof(Cell) == 24, "toe::term::Cell grew");
static_assert(std::is_trivially_copyable_v<Cell>);

} // namespace toe::term

#endif // TOE_TERM_CELL_HPP
