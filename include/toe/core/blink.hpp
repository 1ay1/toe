// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Time & blink phases, as values instead of scattered magic arithmetic.
//
// A terminal frame is driven by a single monotonic clock reading. From it we
// derive two square-wave phases (cursor blink, SGR-5 text blink), each a
// distinct type so they can't be swapped, and each carrying its own period.
// The old code open-coded `(ms / 530) % 2 == 0` inline in the loop; here the
// period is a named property of the phase and the wave is one total function.

#ifndef TOE_CORE_BLINK_HPP
#define TOE_CORE_BLINK_HPP

#include <chrono>
#include <compare>
#include <cstdint>

namespace toe {

// A monotonic timestamp in milliseconds since the steady-clock epoch. A strong
// newtype so it never gets confused with a duration, a deadline, or a raw int.
struct Millis {
    std::uint64_t value{0};

    [[nodiscard]] static Millis now() noexcept {
        using namespace std::chrono;
        return Millis{static_cast<std::uint64_t>(
            duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count())};
    }

    constexpr auto operator<=>(const Millis &) const = default;
};

// A square wave: `on()` is true for the first half of every `period`. Two
// phantom-tagged instantiations below make Cursor and Text blink incompatible
// types, so a caller can't pass one where the other is meant.
template <typename Tag, std::uint64_t PeriodMs>
struct SquareWave {
    static constexpr std::uint64_t period = PeriodMs;
    [[nodiscard]] static bool on(Millis t) noexcept {
        return (t.value / PeriodMs) % 2 == 0;
    }
};

// Cursor blinks on a 530ms half-period; the SGR-5 "blink" attribute on a 750ms one.
struct CursorTag {};
struct TextTag {};
using CursorBlink = SquareWave<CursorTag, 530>;
using TextBlink = SquareWave<TextTag, 750>;

// The blink state of one frame: both phases, captured once from a single clock
// reading so the whole frame is internally consistent.
struct BlinkState {
    bool cursor_on{true};
    bool text_on{true};

    [[nodiscard]] static BlinkState at(Millis t) noexcept {
        return {CursorBlink::on(t), TextBlink::on(t)};
    }

    // Runtime cursor half-period (from config). 0 disables cursor blink (steady
    // on). The SGR-5 text-blink phase keeps its fixed cadence. Used so
    // cursor.blink / cursor.blink_ms are real, not compile-time constants.
    [[nodiscard]] static BlinkState at(Millis t, std::uint64_t cursor_ms) noexcept {
        const bool cursor = (cursor_ms == 0) ? true : ((t.value / cursor_ms) % 2 == 0);
        return {cursor, TextBlink::on(t)};
    }

    constexpr auto operator<=>(const BlinkState &) const = default;
};

} // namespace toe

#endif // TOE_CORE_BLINK_HPP
