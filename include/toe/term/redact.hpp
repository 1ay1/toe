// SPDX-License-Identifier: LGPL-2.0-or-later
//
// redact.hpp — scrub secrets from terminal text before it leaves for an agent.
//
// A "terminal for AI" surface (snapshot / command blocks / DEC 2034 query) is a
// data-exfiltration risk: the agent should never see the API key you just
// exported, the token in a curl -H header, or a password prompt echo. Redactor
// masks those on the way out. It is deliberately conservative and fast (a fixed
// set of anchored patterns + a high-entropy token heuristic); it runs on the
// resolved TEXT, not the byte stream, so it never corrupts the live screen.
//
// This is a best-effort guard, not a security boundary — pair it with real
// sandboxing. But it removes the most common footguns for free.

#ifndef TOE_TERM_REDACT_HPP
#define TOE_TERM_REDACT_HPP

#include <string>
#include <string_view>

namespace toe::term {

class Redactor {
public:
    // The mask substituted for a detected secret.
    static constexpr std::string_view kMask = "«redacted»";

    Redactor() = default;
    explicit Redactor(bool enabled) : enabled_(enabled) {}

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    void set_enabled(bool e) noexcept { enabled_ = e; }

    // Return `text` with detected secrets replaced by the mask. When disabled,
    // returns the input unchanged (cheap identity). Never throws.
    [[nodiscard]] std::string apply(std::string_view text) const;

private:
    bool enabled_{false};
};

// Exposed for testing: true if `token` looks like a high-entropy secret (long
// mixed-charset run with no spaces — API keys, hashes, base64 blobs).
[[nodiscard]] bool looks_like_secret(std::string_view token) noexcept;

} // namespace toe::term

#endif // TOE_TERM_REDACT_HPP
