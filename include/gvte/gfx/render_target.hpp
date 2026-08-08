// SPDX-License-Identifier: LGPL-2.0-or-later
//
// RenderContext — a capability token that makes "a GL context is current on
// this thread" a fact the TYPE SYSTEM can see.
//
// The problem it solves: `Session::render()` issues GL calls. That has always
// carried an *unchecked* precondition — "a GL context must be current on the
// calling thread" — enforced only by a comment. Get it wrong and you get UB.
// It also drew into whatever framebuffer happened to be bound, so a host that
// brings its own window couldn't say WHERE to draw.
//
// The fix, in the spirit of the rest of gvte (illegal states unrepresentable):
// render() now REQUIRES a `RenderContext&`. You can only obtain one by making a
// claim the compiler then holds you to:
//
//   * RenderContext::adopt_current()  — "I promise a GL context is current on
//     this thread right now." Returns the token; you pass it to render().
//   * gvte::platform surfaces hand you one from their own make-current path.
//
// The token also carries the TARGET framebuffer, so the host controls exactly
// where the terminal is composited (default 0 = the current/default FBO). This
// is the type-checked "render into a texture I give you" path.
//
// The token is move-only (a capability is not freely copyable) and non-owning
// (it does not create or destroy the GL context — the host owns that).

#ifndef GVTE_GFX_RENDER_TARGET_HPP
#define GVTE_GFX_RENDER_TARGET_HPP

#include <cstdint>

#include "gvte/core/types.hpp"

namespace gvte::gfx {

// A GL framebuffer object name. Strong newtype so a raw int (a texture, a
// width, anything) can't be passed where an FBO is meant. 0 = the default /
// currently-bound framebuffer.
struct Framebuffer {
    std::uint32_t id{0};
    constexpr auto operator<=>(const Framebuffer &) const = default;
};

inline constexpr Framebuffer default_framebuffer{0};

// The capability token. Its mere existence is the proof render() consumes.
class RenderContext {
public:
    // Claim that a GL context is current on THIS thread, right now. The host
    // (or a gvte::platform surface, right after its make-current/swap setup)
    // makes this claim; gvte trusts it — that is the whole point of a
    // capability token. Optionally names the destination framebuffer.
    [[nodiscard]] static RenderContext adopt_current(
        Framebuffer target = default_framebuffer) noexcept {
        return RenderContext{target};
    }

    RenderContext(const RenderContext &) = delete;
    RenderContext &operator=(const RenderContext &) = delete;
    RenderContext(RenderContext &&) noexcept = default;
    RenderContext &operator=(RenderContext &&) noexcept = default;
    ~RenderContext() = default;

    // Where the terminal should be composited this frame.
    [[nodiscard]] Framebuffer target() const noexcept { return target_; }

    // Retarget within the same current context (e.g. draw into an offscreen
    // FBO, then a second pass to the default one). Returns *this for chaining.
    RenderContext &retarget(Framebuffer fb) noexcept {
        target_ = fb;
        return *this;
    }

private:
    explicit RenderContext(Framebuffer target) noexcept : target_{target} {}
    Framebuffer target_{default_framebuffer};
};

} // namespace gvte::gfx

#endif // GVTE_GFX_RENDER_TARGET_HPP
