// SPDX-License-Identifier: LGPL-2.0-or-later
//
// EventRouter<S> — the engine's input policy, expressed as an exhaustive
// visitor over the surface's closed Event sum type, MONOMORPHIC over the
// concrete surface type S.
//
// Each event kind is a small, named `operator()` overload. `std::visit(router,
// ev)` dispatches at compile time — no `if constexpr` ladder, no std::function,
// no per-keystroke heap churn. Because the surface is a template parameter (not
// a type-erased AnySurface), every surface call — clipboard, title — inlines;
// optional refinements resolve through the uniform free-function accessors,
// which `if constexpr` away on backends that don't provide them.

#ifndef TOE_CORE_EVENT_ROUTER_HPP
#define TOE_CORE_EVENT_ROUTER_HPP

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "toe/app.hpp"
#include "toe/terminal.hpp"

namespace toe {

// Routes translated window events into Session actions. One instance is reused
// across the whole session; `take_wrote_input()` reports (and resets) whether
// this batch handed bytes to the child, so the host can coalesce the echo into
// one frame. Templated on the concrete surface so nothing goes through a vtable.
template <App S>
class EventRouter {
public:
    EventRouter(Session &session, S &surface, PixelSize &px, bool &running)
        : s_(session), surf_(surface), px_(px), running_(running) {}

    // The visitor entry point: `std::visit(router, event)`.
    template <class E> void operator()(const E &e) { handle(e); }

    // True (and cleared) if this poll batch sent input to the child. The host
    // uses it to wait briefly for the echo before rendering — same-frame echo.
    [[nodiscard]] bool take_wrote_input() noexcept {
        return std::exchange(wrote_input_, false);
    }

private:
    // --- window lifecycle --------------------------------------------------
    void handle(const win::CloseRequested &) { running_ = false; }

    void handle(const win::Resized &e) {
        px_ = e.size;
        // TEA: a resize is a Msg; update() reflows the grid and returns a
        // ResizePty Cmd that run() applies.
        s_.run(s_.update(toe::Resized{px_}));
    }

    // --- keyboard ----------------------------------------------------------
    void handle(const win::KeyPressed &e) {
        // Local shortcuts (paste/copy/scroll) act on PRESS only. Release and
        // repeat events still flow to the child below (the encoder gates them on
        // the Kitty protocol), but must never re-trigger a shortcut — otherwise
        // Ctrl+Shift+V pastes on both press and release.
        const bool press = e.key.kind == toe::KeyEvent::Kind::press;

        if (press) {
            // Shift+PageUp/Down scroll the scrollback on the primary screen; on
            // the alt screen they go to the app (which owns its own paging).
            if (const auto *sk = std::get_if<toe::SpecialKey>(&e.key.key);
                sk && e.key.mods.shift &&
                (*sk == toe::SpecialKey::PageUp || *sk == toe::SpecialKey::PageDown)) {
                if (!s_.on_alt_screen()) {
                    const int page = s_.grid_size().rows - 1;
                    s_.scroll(*sk == toe::SpecialKey::PageUp ? page : -page);
                    return;
                }
            }

            // Clipboard shortcuts intercept before the key reaches the child.
            if (const auto *txt = std::get_if<toe::TextInput>(&e.key.key);
                txt && e.key.mods.ctrl && e.key.mods.shift) {
                if (txt->utf8 == "v" || txt->utf8 == "V") return paste_clipboard();
                if (txt->utf8 == "c" || txt->utf8 == "C") return copy_selection();
            }
        }

        // Everything else is child input: encode + write via the TEA pipeline.
        // (The encoder drops release/repeat unless the app enabled Kitty event
        // reporting, so legacy apps still see press-only input.)
        send_to_child(toe::Key{e.key});
    }

    void handle(const win::TextEntered &e) {
        // Ordinary typed/composed text — a Key (never bracketed-paste).
        toe::KeyEvent k;
        k.key = toe::TextInput{std::string{e.utf8}};
        send_to_child(toe::Key{std::move(k)});
    }

    void handle(const win::Preedit &e) {
        // IME composition in progress: show it inline at the cursor. An empty
        // string clears it on commit/cancel.
        s_.set_preedit(e.utf8, e.cursor);
    }

    // --- pointer -----------------------------------------------------------
    void handle(const win::MouseDown &e) {
        const auto [col, vrow] = cell_of(e.x, e.y);
        // Ctrl+Click (or plain click when the app isn't tracking the mouse)
        // opens an OSC 8 hyperlink under the pointer, if any.
        if (e.button == MouseButton::left && (e.mods.ctrl || !s_.wants_mouse())) {
            if (std::string_view uri = s_.link_at(vrow, col); !uri.empty()) {
                open_url(surf_, uri);
                return;
            }
        }
        if (app_owns_mouse(e.mods.shift)) {
            report(toe::Session::MouseEvent::press, button_code(e.button), col, vrow, e.mods);
        } else if (e.button == MouseButton::left) {
            if (e.click_count >= 3) s_.select_line(vrow, col);
            else if (e.click_count == 2) s_.select_word(vrow, col);
            else s_.select_begin(vrow, col, 0);
        } else if (e.button == MouseButton::middle) {
            paste_clipboard(); // primary-selection paste
        }
    }

    void handle(const win::MouseMove &e) {
        const auto [col, vrow] = cell_of(e.x, e.y);
        if (s_.wants_mouse() &&
            (s_.wants_mouse_motion() || (e.button_down && s_.wants_mouse_drag()))) {
            report(toe::Session::MouseEvent::motion, e.button_down ? 0 : 3, col, vrow, {});
        } else if (e.button_down) {
            s_.select_extend(vrow, col);
        } else {
            // Idle pointer motion: track the OSC 8 link under it for hover
            // highlighting. The Session bumps damage when the link changes.
            s_.set_hover(vrow, col);
        }
    }

    void handle(const win::MouseUp &e) {
        const auto [col, vrow] = cell_of(e.x, e.y);
        if (app_owns_mouse(e.mods.shift)) {
            report(toe::Session::MouseEvent::release, button_code(e.button), col, vrow, e.mods);
        } else if (e.button == MouseButton::left && s_.has_selection()) {
            // Copy on release so a plain drag-select fills the clipboard.
            if (std::string sel = s_.selected_text(); !sel.empty()) set_clipboard(sel);
        }
    }

    void handle(const win::MouseWheel &e) {
        const auto [col, vrow] = cell_of(0, 0);
        if (s_.wants_mouse()) {
            // Wheel maps to buttons 64 (up) / 65 (down) in X10/SGR mouse.
            for (int i = 0; i < std::abs(e.dy); ++i)
                report(toe::Session::MouseEvent::press, e.dy > 0 ? 64 : 65, col, vrow, {});
        } else if (!s_.on_alt_screen()) {
            s_.scroll(e.dy * 3); // 3 lines per notch
        }
    }

    void handle(const win::FocusChanged &e) {
        // Report focus in/out to the app when it enabled DEC 1004 (vim/tmux use
        // this to pause rendering / update their status line).
        s_.report_focus(e.focused);
    }

    void handle(const win::FontZoom &e) {
        // Live font zoom (macOS Cmd +/- /0). Compute the target pixel size and
        // rebuild the atlas; the grid re-flows to the current surface size.
        const int cur = s_.font_pixel_size();
        int target = cur;
        if (e.absolute >= 0) {
            target = e.absolute; // 0 => host asked for reset; clamp keeps it sane
        } else if (e.delta != 0) {
            // Step ~6% per notch so zoom feels proportional, min 1px per step.
            const int step = std::max(1, cur / 16);
            target = cur + (e.delta > 0 ? step : -step);
        }
        s_.set_font_pixel_size(target, px_);
    }

    // --- shared helpers ----------------------------------------------------
    struct CellPos { int col, vrow; };
    [[nodiscard]] CellPos cell_of(int x, int y) const noexcept {
        return {x / std::max(1, s_.cell_width()), y / std::max(1, s_.cell_height())};
    }

    [[nodiscard]] bool app_owns_mouse(bool shift_held) const noexcept {
        return s_.wants_mouse() && !shift_held; // Shift always forces local select
    }

    static int button_code(MouseButton b) noexcept {
        switch (b) {
        case MouseButton::right: return 2;
        case MouseButton::middle: return 1;
        case MouseButton::left: default: return 0;
        }
    }

    void report(toe::Session::MouseEvent kind, int btn, int col, int vrow, toe::Modifiers m) {
        s_.report_mouse(kind, btn, col, vrow, m.shift, m.alt, m.ctrl);
    }

    void send_to_child(toe::Msg &&m) {
        // Typing (or pasting) while scrolled up into history snaps the view back
        // to the live bottom, so you always SEE what you send. Scroll shortcuts
        // (Shift+PageUp/Down) return before reaching here, so paging still works.
        s_.scroll_to_bottom();
        s_.run(s_.update(m));
        wrote_input_ = true;
    }

    // Clipboard through the uniform accessors: these `if constexpr` to a no-op /
    // empty string on a surface that doesn't model the clipboard refinement.
    void set_clipboard(std::string_view t) { toe::clipboard_set(surf_, t); }
    [[nodiscard]] std::string get_clipboard() { return toe::clipboard_get(surf_); }

    void paste_clipboard() {
        if (std::string clip = get_clipboard(); !clip.empty())
            send_to_child(toe::Paste{std::move(clip)});
    }

    void copy_selection() {
        if (!s_.has_selection()) return;
        if (std::string sel = s_.selected_text(); !sel.empty()) set_clipboard(sel);
    }

    // Open an OSC 8 link via the host's URL opener (a UrlOpenerApp refinement).
    // Opening a URL is an OS action — like the clipboard — so the host owns it;
    // toe holds no fork/exec, no xdg-open, no platform binary name.
    void open_url(S &surf, std::string_view uri) { toe::open_url(surf, uri); }

    toe::Session &s_;
    S &surf_;
    toe::PixelSize &px_;
    bool &running_;
    bool wrote_input_ = false;
};

} // namespace toe

#endif // TOE_CORE_EVENT_ROUTER_HPP
