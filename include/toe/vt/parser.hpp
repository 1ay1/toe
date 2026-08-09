// SPDX-License-Identifier: LGPL-2.0-or-later
//
// VT / ANSI escape-sequence parser.
//
// A byte-driven state machine after Paul Williams' DEC ANSI parser
// (https://vt100.net/emu/dec_ansi_parser). We keep the state set explicit and
// emit strongly-typed Actions via a visitor callback rather than mutating a
// terminal directly — the parser stays a pure function of (state, byte), which
// makes it unit-testable in isolation from rendering and the PTY.

#ifndef TOE_VT_PARSER_HPP
#define TOE_VT_PARSER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "toe/vt/simd_scan.hpp"
#include <vector>

namespace toe::vt {

// --- typed actions ---------------------------------------------------------
// A decoded UTF-8 codepoint to place at the cursor.
struct Print {
    char32_t cp{};
};

// A contiguous run of printable ASCII (0x20-0x7E) to place at the cursor — the
// hot path under any flood. Batching a whole run into one action avoids a
// per-character variant construction + visit + apply() dispatch. The view is
// valid only for the duration of the sink call (it points into the fed bytes).
struct PrintRun {
    std::string_view text;
};

// A C0/C1 control (BS, LF, CR, BEL, HT, ...). Raw byte value.
struct Execute {
    std::uint8_t byte{};
};

// A final CSI dispatch, e.g. ESC [ 1 ; 31 m  ->  {params:{1,31}, final:'m'}.
struct CsiDispatch {
    std::span<const int> params;
    // Parallel to params: sub[i] is true when param i was joined to param i-1
    // by a COLON (a sub-parameter), e.g. SGR 4:3 (curly underline) or
    // 38:2:r:g:b. Distinguishes ':' from ';' so those can be handled correctly.
    std::span<const std::uint8_t> sub;
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

// A completed DCS (Device Control String), e.g. ESC P + q <hex> ST for
// XTGETTCAP. `prefix` is the intermediate/final that selects the DCS kind
// (e.g. "+q"), `data` is the payload up to ST.
struct DcsDispatch {
    std::string_view prefix; // intermediates + final byte, e.g. "+q", "$q"
    std::string_view data;   // payload between the final and ST
};

// A completed APC (Application Program Command) string: ESC _ <data> ST. The
// kitty graphics protocol lives here (ESC _ G <control> ; <payload> ST).
struct ApcDispatch {
    std::string_view data; // everything between ESC _ and ST
};

using Action =
    std::variant<Print, PrintRun, Execute, CsiDispatch, EscDispatch, OscDispatch, DcsDispatch,
                 ApcDispatch>;

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
        DcsEntry,        // just saw ESC P
        DcsPassthrough,  // collecting the DCS payload until ST
        DcsIgnore,
        ApcString,       // ESC _ ... ST (kitty graphics)
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
    std::vector<std::uint8_t> param_sub_{}; // colon-joined flag per param
    bool param_started_{false};
    std::string intermediates_{};
    char marker_{0};
    bool private_marker_{false};

    // OSC accumulation
    std::string osc_{};

    // DCS accumulation
    std::string dcs_prefix_{}; // intermediates + final selecting the DCS kind
    std::string dcs_data_{};   // payload until ST
    bool dcs_saw_esc_{false};  // saw ESC inside DCS (waiting for the ST '\\')

    // APC accumulation (kitty graphics: ESC _ ... ST)
    std::string apc_{};
    bool apc_saw_esc_{false};

    // UTF-8 decoding of printable text in Ground.
    char32_t utf8_cp_{0};
    int utf8_remaining_{0};
};

} // namespace toe::vt

#include "toe/vt/parser.inl"

#endif // TOE_VT_PARSER_HPP
