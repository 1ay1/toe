// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Foundational value types. The terminal is a 2-D grid, and the single most
// common bug class in emulators is silently swapping a row for a column or a
// pixel for a cell. We make those categories distinct in the type system so
// the compiler rejects the confusion.

#ifndef TOE_CORE_TYPES_HPP
#define TOE_CORE_TYPES_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace toe {

// --- strong integer newtype ------------------------------------------------
// A phantom Tag makes each axis a distinct type. Arithmetic is deliberately
// limited to what makes sense on a coordinate (offset by an amount, compare,
// difference), not full ring arithmetic.

template <typename Tag>
struct Coord {
    using value_type = std::int32_t;
    value_type v{0};

    constexpr Coord() = default;
    constexpr explicit Coord(value_type value) noexcept : v{value} {}

    constexpr auto operator<=>(const Coord &) const = default;

    constexpr Coord &operator++() noexcept { ++v; return *this; }
    constexpr Coord &operator--() noexcept { --v; return *this; }
    constexpr Coord operator++(int) noexcept { Coord t = *this; ++v; return t; }
    constexpr Coord operator--(int) noexcept { Coord t = *this; --v; return t; }

    constexpr Coord operator+(value_type d) const noexcept { return Coord{v + d}; }
    constexpr Coord operator-(value_type d) const noexcept { return Coord{v - d}; }
    constexpr value_type operator-(Coord o) const noexcept { return v - o.v; }

    constexpr Coord &operator+=(value_type d) noexcept { v += d; return *this; }
    constexpr Coord &operator-=(value_type d) noexcept { v -= d; return *this; }

    [[nodiscard]] constexpr value_type get() const noexcept { return v; }
};

struct RowTag {};
struct ColTag {};

using Row = Coord<RowTag>; // 0-based line within the grid
using Col = Coord<ColTag>; // 0-based column within a line

// A grid position.
struct Pos {
    Row row{};
    Col col{};
    constexpr auto operator<=>(const Pos &) const = default;
};

// Grid dimensions, in cells. Deliberately distinct from Pos.
struct Extent {
    std::int32_t cols{0};
    std::int32_t rows{0};
    constexpr auto operator<=>(const Extent &) const = default;
    [[nodiscard]] constexpr std::size_t area() const noexcept {
        return static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows);
    }
};

// Pixel dimensions of the drawable surface.
struct PixelSize {
    std::int32_t w{0};
    std::int32_t h{0};
    constexpr auto operator<=>(const PixelSize &) const = default;
};

// --- color -----------------------------------------------------------------
struct Rgb {
    std::uint8_t r{}, g{}, b{};
    constexpr auto operator<=>(const Rgb &) const = default;
};

constexpr Rgb rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept { return {r, g, b}; }

// --- error channel ---------------------------------------------------------
// One error type for the whole app boundary. std::expected carries it out of
// any fallible constructor/factory instead of throwing across the C FFI seams.

struct Error {
    std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

[[nodiscard]] inline std::unexpected<Error> fail(std::string msg) {
    return std::unexpected(Error{std::move(msg)});
}

} // namespace toe

#endif // TOE_CORE_TYPES_HPP
