// SPDX-License-Identifier: LGPL-2.0-or-later
//
// sbquery.cpp — DEC 2034 Semantic Block Query implementation. See sbquery.hpp.

#include "toe/term/sbquery.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <random>
#include <tuple>
#include <vector>

namespace toe::term {

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

std::string block_to_json(const CommandBlock &b, const BlockTextResolver &res, bool with_output) {
    std::string command, output;
    if (res.fn) std::tie(command, output) = res.fn(b, with_output);

    std::string j = "{";
    j += "\"id\":" + std::to_string(b.id);
    j += ",\"command\":\"" + json_escape(command) + "\"";
    if (with_output) {
        j += ",\"output\":\"" + json_escape(output) + "\"";
        // Line count is cheap and useful for an agent to decide whether to
        // re-read with pagination.
        std::int64_t lines = output.empty() ? 0 : 1;
        for (char ch : output) if (ch == '\n') ++lines;
        j += ",\"outputLineCount\":" + std::to_string(lines);
    }
    j += ",\"cwd\":\"" + json_escape(b.cwd) + "\"";
    if (b.exit_code.has_value())
        j += ",\"exitCode\":" + std::to_string(*b.exit_code);
    else
        j += ",\"exitCode\":null";
    j += ",\"finished\":" + std::string(b.finished() ? "true" : "false");
    j += ",\"durationMs\":" + std::to_string(b.duration_ms());
    j += "}";
    return j;
}

std::uint64_t SemanticBlockQuery::mint_token() {
    // A non-zero, hard-to-guess 64-bit token. Seeded from a random_device mixed
    // with the clock so two sessions in the same process differ.
    std::random_device rd;
    std::uint64_t hi = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::uint64_t t = hi ^ static_cast<std::uint64_t>(now);
    return t ? t : 0x9E3779B97F4A7C15ull; // never 0 (0 means "no token")
}

std::string SemanticBlockQuery::handshake_reply() const {
    // DCS > 2034 ; 1 b T1;T2;T3;T4 ST, tokens as four 16-bit decimal words.
    const auto w = [](std::uint64_t v, int shift) {
        return static_cast<unsigned>((v >> shift) & 0xFFFF);
    };
    char buf[96];
    std::snprintf(buf, sizeof buf, "\x1bP>2034;1b%u;%u;%u;%u\x1b\\", w(token_, 48), w(token_, 32),
                  w(token_, 16), w(token_, 0));
    return buf;
}

namespace {
// Parse a ';'-separated list of unsigned ints from `s` into `out` (up to max).
// Returns the count parsed. Empty fields become 0.
std::size_t parse_params(std::string_view s, std::uint32_t *out, std::size_t max) {
    std::size_t n = 0, start = 0;
    while (n < max) {
        const std::size_t semi = s.find(';', start);
        std::string_view tok =
            s.substr(start, semi == std::string_view::npos ? std::string_view::npos : semi - start);
        std::uint32_t v = 0;
        for (char c : tok) {
            if (c < '0' || c > '9') { v = 0; break; }
            v = v * 10 + static_cast<std::uint32_t>(c - '0');
        }
        out[n++] = v;
        if (semi == std::string_view::npos) break;
        start = semi + 1;
    }
    return n;
}

std::string dcs_reply(int status, std::string_view json) {
    std::string r = "\x1bP>";
    r += std::to_string(status);
    r += 'b';
    r += json;
    r += "\x1b\\";
    return r;
}
} // namespace

std::string SemanticBlockQuery::query(std::string_view params, const CommandLog &log,
                                      const BlockTextResolver &res) const {
    // params: Ps ; Pn ; T1 ; T2 ; T3 ; T4
    std::array<std::uint32_t, 6> p{};
    parse_params(params, p.data(), p.size());
    const std::uint32_t ps = p[0], pn = p[1];

    if (token_ == 0 || !enabled_) return dcs_reply(2, "{}"); // no active token

    const std::uint64_t supplied = (static_cast<std::uint64_t>(p[2]) << 48) |
                                   (static_cast<std::uint64_t>(p[3]) << 32) |
                                   (static_cast<std::uint64_t>(p[4]) << 16) |
                                   static_cast<std::uint64_t>(p[5]);
    if (supplied != token_) return dcs_reply(3, "{}"); // bad token

    auto emit_one = [&](const CommandBlock *b, bool with_output) {
        if (!b) return dcs_reply(4, "{}"); // no data
        std::string j = "{\"version\":1,\"blocks\":[";
        j += block_to_json(*b, res, with_output);
        j += "]}";
        return dcs_reply(1, j);
    };

    switch (ps) {
    case 1: // last completed block (with output)
        return emit_one(log.last_completed(), true);
    case 3: // current in-progress command (command line only; output is partial)
        return emit_one(log.in_progress(), true);
    case 2: { // last Pn completed blocks
        const auto &blocks = log.blocks();
        // Collect the most-recent completed blocks, newest last.
        std::vector<const CommandBlock *> picked;
        const std::size_t want = pn ? pn : 1;
        for (auto it = blocks.rbegin(); it != blocks.rend() && picked.size() < want; ++it)
            if (it->finished()) picked.push_back(&*it);
        if (picked.empty()) return dcs_reply(4, "{}");
        std::string j = "{\"version\":1,\"blocks\":[";
        // Emit oldest-first for readability.
        for (auto it = picked.rbegin(); it != picked.rend(); ++it) {
            if (it != picked.rbegin()) j += ",";
            j += block_to_json(**it, res, true);
        }
        j += "]}";
        return dcs_reply(1, j);
    }
    default:
        return dcs_reply(4, "{}");
    }
}

} // namespace toe::term
