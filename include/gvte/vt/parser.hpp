// SPDX-License-Identifier: LGPL-2.0-or-later
//
// VT / ANSI escape-sequence parser.
//
// A byte-driven state machine after Paul Williams' DEC ANSI parser
// (https://vt100.net/emu/dec_ansi_parser). We keep the state set explicit and
// emit strongly-typed Actions via a visitor callback rather than mutating a
// terminal directly — the parser stays a pure function of (state, byte), which
// makes it unit-testable in isolation from rendering and the PTY.

#ifndef GVTE_VT_PARSER_HPP
#define GVTE_VT_PARSER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace gvte::vt {

// --- typed actions ---------------------------------------------------------
// A decoded UTF-8 codepoint to place at the cursor.
struct Print {
    char32_t cp{};
};

// A C0/C1 control (BS, LF, CR, BEL, HT, ...). Raw byte value.
struct Execute {
    std::uint8_t byte{};
};

// A final CSI dispatch, e.g. ESC [ 1 ; 31 m  ->  {params:{1,31}, final:'m'}.
struct CsiDispatch {
    std::span<const int> params;
    std::string_view intermediates; // collected intermediate bytes (0x20-0x2F)
    char final{};
    bool private_marker{}; // leading '?' etc. (0x3C-0x3F)
    char marker{};         // the private marker byte, if any
};

// A simple ESC dispatch that isn't a CSI, e.g. ESC c (RIS), ESC ( B.
struct EscDispatch {
    std::string_view intermediates;
    char final{};
};

// A completed OSC string, e.g. ESC ] 0 ; title BEL  ->  {"0;title"}.
struct OscDispatch {
    std::string_view data;
};

using Action = std::variant<Print, Execute, CsiDispatch, EscDispatch, OscDispatch>;

// --- the parser ------------------------------------------------------------
class Parser {
public:
    // Feed a chunk of PTY bytes. `sink(Action)` is invoked in order for every
    // decoded action. The parser retains partial-sequence state across calls,
    // so streaming works with arbitrary chunk boundaries.
    template <typename Sink>
    void feed(std::span<const char> bytes, Sink &&sink);

private:
    enum class State : std::uint8_t {
        Ground,
        Escape,
        EscapeIntermediate,
        CsiEntry,
        CsiParam,
        CsiIntermediate,
        CsiIgnore,
        OscString,
        // UTF-8 continuation is handled inline in Ground via utf8_.
    };

    template <typename Sink>
    void step(std::uint8_t b, Sink &sink);

    template <typename Sink>
    void ground_byte(std::uint8_t b, Sink &sink);

    void clear_csi() noexcept;
    template <typename Sink>
    void csi_dispatch(std::uint8_t final, Sink &sink);

    State state_{State::Ground};

    // CSI accumulation
    std::vector<int> params_{};
    bool param_started_{false};
    std::string intermediates_{};
    char marker_{0};
    bool private_marker_{false};

    // OSC accumulation
    std::string osc_{};

    // UTF-8 decoding of printable text in Ground.
    char32_t utf8_cp_{0};
    int utf8_remaining_{0};
};

} // namespace gvte::vt

#include "gvte/vt/parser.inl"

#endif // GVTE_VT_PARSER_HPP
