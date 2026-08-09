// SPDX-License-Identifier: LGPL-2.0-or-later
//
// redact.cpp — secret scrubbing for the AI read-out surface. See redact.hpp.

#include "toe/term/redact.hpp"

#include <array>
#include <cmath>
#include <regex>

namespace toe::term {

namespace {

// Anchored, high-precision patterns for well-known secret shapes. Ordered from
// most-specific to least; each match is replaced wholesale by the mask.
const std::regex &secret_patterns() {
    // One alternation, compiled once. Kept intentionally tight to avoid masking
    // ordinary output.
    static const std::regex re(
        // provider API keys: sk-..., ghp_/gho_/ghu_/ghs_..., xox[baprs]-...,
        // AWS AKIA..., Google AIza..., Slack/Stripe-ish prefixes.
        R"((sk-[A-Za-z0-9_\-]{16,})"
        R"(|(?:gh[pousr]|github_pat)_[A-Za-z0-9_]{16,})"
        R"(|xox[baprs]-[A-Za-z0-9\-]{10,})"
        R"(|AKIA[0-9A-Z]{12,})"
        R"(|AIza[0-9A-Za-z_\-]{20,})"
        // JWTs: three base64url segments.
        R"(|eyJ[A-Za-z0-9_\-]+\.[A-Za-z0-9_\-]+\.[A-Za-z0-9_\-]+)"
        // KEY/TOKEN/SECRET/PASSWORD = value  (env exports, config lines).
        R"(|(?:[A-Za-z0-9_]*(?:SECRET|TOKEN|PASSWORD|PASSWD|API[_-]?KEY|ACCESS[_-]?KEY)[A-Za-z0-9_]*)\s*[=:]\s*\S+)"
        // Authorization: Bearer <token> / -H 'Authorization: ...'
        R"(|[Aa]uthorization:\s*\S+\s+\S+)"
        // password prompt echoes.
        R"(|(?:[Pp]assword|[Pp]assphrase)\s*:\s*\S+))",
        std::regex::optimize);
    return re;
}

double shannon_entropy(std::string_view s) {
    if (s.empty()) return 0.0;
    std::array<int, 256> freq{};
    for (unsigned char c : s) ++freq[c];
    double h = 0.0;
    for (int f : freq) {
        if (!f) continue;
        const double p = static_cast<double>(f) / static_cast<double>(s.size());
        h -= p * std::log2(p);
    }
    return h; // bits per char
}

} // namespace

bool looks_like_secret(std::string_view token) noexcept {
    // Heuristic: a long, spaceless, mixed-charset run with high entropy. Tuned
    // to catch API keys / hashes / base64 blobs while sparing prose and paths.
    if (token.size() < 20) return false;
    bool has_lower = false, has_upper = false, has_digit = false;
    for (unsigned char c : token) {
        if (c == ' ' || c == '/' ) return false; // paths / phrases: skip
        if (c >= 'a' && c <= 'z') has_lower = true;
        else if (c >= 'A' && c <= 'Z') has_upper = true;
        else if (c >= '0' && c <= '9') has_digit = true;
    }
    const int classes = has_lower + has_upper + has_digit;
    if (classes < 2) return false;
    return shannon_entropy(token) >= 3.5; // bits/char; random b64 ~= 6
}

std::string Redactor::apply(std::string_view text) const {
    if (!enabled_ || text.empty()) return std::string{text};

    // 1) Pattern pass: replace well-known secret shapes wholesale.
    std::string s = std::regex_replace(std::string{text}, secret_patterns(),
                                       std::string{kMask});

    // 2) Entropy pass: mask any remaining high-entropy tokens (unknown key
    //    formats). Split on whitespace, test each token, rebuild preserving the
    //    original separators.
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (std::isspace(static_cast<unsigned char>(s[i]))) {
            out.push_back(s[i++]);
            continue;
        }
        std::size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        std::string_view tok{s.data() + start, i - start};
        if (looks_like_secret(tok)) out += kMask;
        else out.append(tok);
    }
    return out;
}

} // namespace toe::term
