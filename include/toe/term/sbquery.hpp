// SPDX-License-Identifier: LGPL-2.0-or-later
//
// sbquery.hpp — DEC Private Mode 2034: the Semantic Block Query.
//
// A discoverable, machine-readable way for a programmatic consumer (an AI agent,
// an accessibility tool, an automation script) to pull the terminal's command
// blocks out as JSON, without scraping the byte stream. It is built on the
// OSC 133 command log (see command_log.hpp).
//
// Protocol (after Contour's extension):
//
//   Enable   CSI ? 2034 h   -> the terminal mints a 64-bit session token and
//                              replies  DCS > 2034 ; 1 b T1;T2;T3;T4 ST
//                              (Ti are the token's four 16-bit words). Tracking
//                              of blocks is on.
//   Disable  CSI ? 2034 l   -> tracked data is discarded, the token invalidated.
//   Verify   CSI ? 2034 $ p -> DECRQM reply CSI ? 2034 ; Ps $ y  (1 set/2 reset)
//   Query    CSI > Ps ; Pn ; T1 ; T2 ; T3 ; T4 b
//                 Ps = 1  the last completed block
//                 Ps = 2  the last Pn completed blocks
//                 Ps = 3  the current in-progress command
//            -> DCS > <status> b {json} ST     status: 1 ok, 2 no token,
//                                              3 bad token, 4 no data
//
// The JSON payload never contains a raw ST: control chars in string fields are
// \u-escaped. This module is pure — it takes a CommandLog + a text resolver and
// returns the bytes to write back; the wiring lives in update.cpp.

#ifndef TOE_TERM_SBQUERY_HPP
#define TOE_TERM_SBQUERY_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "toe/term/command_log.hpp"

namespace toe::term {

// Resolves a block's command line / output text on demand (backed by the live
// Screen in the real terminal; a stub in tests). `want_output=false` asks only
// for the command line (cheaper — output can be large).
struct BlockTextResolver {
    // Returns {command, output}. output is left empty when want_output is false.
    std::function<std::pair<std::string, std::string>(const CommandBlock &, bool want_output)> fn;
};

// The 2034 session state: whether tracking is enabled and the current token.
class SemanticBlockQuery {
public:
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] std::uint64_t token() const noexcept { return token_; }

    // CSI ? 2034 h — enable + mint a token. Returns the DCS handshake reply.
    std::string enable() {
        enabled_ = true;
        token_ = mint_token();
        return handshake_reply();
    }
    // CSI ? 2034 l — disable + invalidate the token.
    void disable() {
        enabled_ = false;
        token_ = 0;
    }

    // Handle a query sequence body (the params of `CSI > ... b`). `blocks` is
    // the live log; `res` resolves block text. Returns the DCS reply bytes, or
    // empty if the sequence isn't a 2034 query. Never throws.
    [[nodiscard]] std::string query(std::string_view params, const CommandLog &log,
                                    const BlockTextResolver &res) const;

    // Build the DECRQM reply for CSI ? 2034 $ p.
    [[nodiscard]] std::string decrqm_reply() const {
        std::string r = "\x1b[?2034;";
        r += enabled_ ? '1' : '2';
        r += "$y";
        return r;
    }

private:
    static std::uint64_t mint_token();
    [[nodiscard]] std::string handshake_reply() const;

    bool enabled_{false};
    std::uint64_t token_{0};
};

// JSON-encode one block (command/output resolved via `res`). Exposed for tests.
[[nodiscard]] std::string block_to_json(const CommandBlock &b, const BlockTextResolver &res,
                                        bool with_output);

// Escape a string for embedding in JSON (control chars -> \uXXXX, quotes/back-
// slashes escaped). No surrounding quotes.
[[nodiscard]] std::string json_escape(std::string_view s);

} // namespace toe::term

#endif // TOE_TERM_SBQUERY_HPP
