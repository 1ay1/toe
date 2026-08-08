// SPDX-License-Identifier: LGPL-2.0-or-later
//
// toe::App — the WHOLE host boundary, as one contract.
//
// The engine owns every interface the system speaks in; a frontend (hand, or a
// Qt/GLFW/SDL/Win32/Cocoa shell) is nothing but an IMPLEMENTATION of toe's
// declared contracts. This header names the single aggregate a frontend must
// satisfy to be driven by `toe::run`. It bundles the sub-contracts toe already
// owns:
//
//   • Surface  (toe/core/surface.hpp) — the window: present, input, GL context,
//              and the optional clipboard/title/timer refinements.
//   • Config   (toe/terminal.hpp)     — HOW to build the terminal: fonts,
//              colours, and the PtySource (spawn $SHELL, or adopt an fd). The
//              host fills this value; it invents none of the types inside it.
//
// A host does NOT invent a vocabulary toe adapts to — it PRODUCES toe's types
// (PixelSize, Event, Config) through this one contract. "toe owns the contract
// of everything; hosts just implement" — this concept is that sentence in code.
//
// Like `Surface`, `App` is STRUCTURAL (a C++23 concept), so a frontend models it
// by shape — no base class, no vtable forced on the frontend's type. `toe::run`
// is templated on the concrete App, so every call inlines and the whole loop is
// monomorphic.

#ifndef TOE_APP_HPP
#define TOE_APP_HPP

#include "toe/core/surface.hpp"
#include "toe/terminal.hpp"

namespace toe {

// A type A models App iff it exposes the two things toe needs to run a terminal:
// the window to draw into (a Surface) and the config to build the engine with.
// `surface()` returns a reference whose type models `Surface`; `config()`
// returns the build recipe. Both are queried once at startup by `toe::run`.
template <typename A>
concept App = requires(A a, const A ca) {
    // The window this frontend brought. Its type must itself model Surface.
    { a.surface() } -> std::same_as<typename A::SurfaceType &>;
    requires Surface<typename A::SurfaceType>;

    // How to build the terminal (fonts, colours, PtySource). A value toe owns
    // the type of; the host merely fills it in.
    { ca.config() } -> std::convertible_to<const Config &>;
};

} // namespace toe

#endif // TOE_APP_HPP
