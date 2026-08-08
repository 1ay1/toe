// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Wayland + EGL backend for platform::Surface. Connects to the compositor,
// creates an xdg-shell toplevel, wraps a wl_egl_window in an EGL/GL 3.3
// context, and translates wl_keyboard events (decoded through xkbcommon) into
// the platform-neutral Event sum type.

#include "toe/platform/surface.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>

#include <fcntl.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#ifdef TOE_HAVE_TEXT_INPUT_V3
#include "text-input-unstable-v3-client-protocol.h"
#endif

// epoxy must be included before (or instead of) the system EGL/GL headers; it
// re-exports the EGL and GL symbols itself.
#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#include "xdg-shell-client-protocol.h"

namespace toe::platform {

namespace {

class WaylandSurface final {
public:
    static Result<std::unique_ptr<WaylandSurface>> open(std::string_view title, PixelSize initial);

    ~WaylandSurface();

    void swap();
    void swap_damaged(DamageRect d); // present, damaging only the changed region
    [[nodiscard]] PixelSize pixel_size() const { return size_; }
    [[nodiscard]] int event_fd() const { return wl_display_get_fd(display_); }
    [[nodiscard]] int repeat_fd() const { return repeat_fd_; }
    void flush() { wl_display_flush(display_); }
    void set_title(std::string_view title) {
        if (toplevel_) {
            xdg_toplevel_set_title(toplevel_, std::string{title}.c_str());
        }
    }
    void set_clipboard(std::string_view utf8);
    [[nodiscard]] std::string get_clipboard();
    void poll_events(const std::function<void(const Event &)> &sink);
    [[nodiscard]] bool should_close() const { return closed_; }

    // --- native listener callbacks (public so the C listener tables at
    // namespace scope can take their addresses) ---
    static void registry_global(void *data, wl_registry *reg, uint32_t name,
                                const char *iface, uint32_t version);
    static void registry_global_remove(void *, wl_registry *, uint32_t) {}
    static void xdg_wm_base_ping(void *, xdg_wm_base *base, uint32_t serial) {
        xdg_wm_base_pong(base, serial);
    }
    static void xdg_surface_configure(void *data, xdg_surface *surf, uint32_t serial);
    static void xdg_toplevel_configure(void *data, xdg_toplevel *, int32_t w, int32_t h,
                                       wl_array *);
    static void xdg_toplevel_close(void *data, xdg_toplevel *);
    static void seat_capabilities(void *data, wl_seat *seat, uint32_t caps);
    static void seat_name(void *, wl_seat *, const char *) {}
    static void kb_keymap(void *data, wl_keyboard *, uint32_t format, int fd, uint32_t size);
    static void kb_enter(void *data, wl_keyboard *, uint32_t, wl_surface *, wl_array *) {
        auto *self = static_cast<WaylandSurface *>(data);
        if (self->sink_) (*self->sink_)(Event{FocusChanged{true}});
    }
    static void kb_leave(void *data, wl_keyboard *, uint32_t, wl_surface *) {
        auto *self = static_cast<WaylandSurface *>(data);
        // Losing keyboard focus must cancel any in-flight key repeat.
        self->disarm_repeat();
        if (self->sink_) (*self->sink_)(Event{FocusChanged{false}});
    }
    static void kb_key(void *data, wl_keyboard *, uint32_t serial, uint32_t time, uint32_t key,
                       uint32_t state);
    static void kb_modifiers(void *data, wl_keyboard *, uint32_t serial, uint32_t dep,
                             uint32_t lat, uint32_t locked, uint32_t group);
    static void kb_repeat_info(void *data, wl_keyboard *, int32_t rate, int32_t delay) {
        auto *self = static_cast<WaylandSurface *>(data);
        self->repeat_rate_ = rate;   // keys/second; 0 disables
        self->repeat_delay_ = delay; // ms before first repeat
    }

    // Key repeat helpers (instance methods).
    void emit_key(uint32_t key, KeyEvent::Kind kind = KeyEvent::Kind::press);
    void arm_repeat(uint32_t key);
    void disarm_repeat();

    // data-device (clipboard) callbacks.
    static void dd_data_offer(void *data, wl_data_device *, wl_data_offer *offer);
    static void dd_selection(void *data, wl_data_device *, wl_data_offer *offer);
    static void dd_enter(void *, wl_data_device *, uint32_t, wl_surface *, wl_fixed_t, wl_fixed_t,
                         wl_data_offer *) {}
    static void dd_leave(void *, wl_data_device *) {}
    static void dd_motion(void *, wl_data_device *, uint32_t, wl_fixed_t, wl_fixed_t) {}
    static void dd_drop(void *, wl_data_device *) {}
    static void offer_mime(void *, wl_data_offer *, const char *) {}
    static void offer_source_actions(void *, wl_data_offer *, uint32_t) {}
    static void offer_action(void *, wl_data_offer *, uint32_t) {}
    static void source_target(void *, wl_data_source *, const char *) {}
    static void source_send(void *data, wl_data_source *, const char *mime, int32_t fd);
    static void source_cancelled(void *data, wl_data_source *source);
    static void source_dnd_drop(void *, wl_data_source *) {}
    static void source_dnd_finished(void *, wl_data_source *) {}
    static void source_action(void *, wl_data_source *, uint32_t) {}

    // pointer callbacks.
    static void ptr_enter(void *data, wl_pointer *, uint32_t serial, wl_surface *, wl_fixed_t sx,
                          wl_fixed_t sy);
    static void ptr_leave(void *, wl_pointer *, uint32_t, wl_surface *) {}
    static void ptr_motion(void *data, wl_pointer *, uint32_t time, wl_fixed_t sx, wl_fixed_t sy);
    static void ptr_button(void *data, wl_pointer *, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state);
    static void ptr_axis(void *data, wl_pointer *, uint32_t time, uint32_t axis, wl_fixed_t value);
    static void ptr_frame(void *, wl_pointer *) {}
    static void ptr_axis_source(void *, wl_pointer *, uint32_t) {}
    static void ptr_axis_stop(void *, wl_pointer *, uint32_t, uint32_t) {}
    static void ptr_axis_discrete(void *, wl_pointer *, uint32_t, int32_t) {}
    static void ptr_axis_value120(void *, wl_pointer *, uint32_t, int32_t) {}
    static void ptr_axis_relative_direction(void *, wl_pointer *, uint32_t, uint32_t) {}

#ifdef TOE_HAVE_TEXT_INPUT_V3
    // text-input-v3 (IME) callbacks — public so the C listener table can take
    // their addresses.
    static void ti_enter(void *, zwp_text_input_v3 *, wl_surface *);
    static void ti_leave(void *, zwp_text_input_v3 *, wl_surface *);
    static void ti_preedit(void *, zwp_text_input_v3 *, const char *, int32_t, int32_t);
    static void ti_commit_string(void *, zwp_text_input_v3 *, const char *);
    static void ti_delete_surrounding(void *, zwp_text_input_v3 *, uint32_t, uint32_t);
    static void ti_done(void *, zwp_text_input_v3 *, uint32_t);
#endif

private:
    WaylandSurface() = default;
    Result<void> init(std::string_view title, PixelSize initial);
    Result<void> init_egl();

    // Native handles.
    wl_display *display_ = nullptr;
    wl_registry *registry_ = nullptr;
    wl_compositor *compositor_ = nullptr;
    xdg_wm_base *wm_base_ = nullptr;
    wl_seat *seat_ = nullptr;
    wl_keyboard *keyboard_ = nullptr;
    wl_pointer *pointer_ = nullptr;
    wl_surface *surface_ = nullptr;
    xdg_surface *xdg_surface_ = nullptr;
    xdg_toplevel *toplevel_ = nullptr;
    wl_egl_window *egl_window_ = nullptr;

    // Clipboard via wl_data_device.
    wl_data_device_manager *data_mgr_ = nullptr;
    wl_data_device *data_device_ = nullptr;
    wl_data_source *data_source_ = nullptr;   // our outgoing offer (copy)
    wl_data_offer *selection_offer_ = nullptr; // current incoming selection (paste)
    std::string clipboard_owned_;             // text we currently offer
    uint32_t last_serial_ = 0;                // most recent input event serial

    // Key repeat. Wayland does NOT resend held keys — the client must synthesize
    // repeats from the compositor's advertised rate/delay via a timer.
    int repeat_fd_ = -1;         // timerfd, added to the host poll set
    int repeat_rate_ = 25;       // keys/second (0 disables repeat)
    int repeat_delay_ = 600;     // ms before the first repeat
    xkb_keycode_t repeat_key_ = 0; // the evdev keycode currently repeating (0 = none)

    // Pointer state / click-count tracking.
    std::int32_t ptr_x_ = 0, ptr_y_ = 0;
    bool ptr_down_ = false;
    uint32_t last_click_time_ = 0;
    std::int32_t last_click_x_ = -1, last_click_y_ = -1;
    int click_count_ = 0;

    // EGL.
    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLContext egl_context_ = EGL_NO_CONTEXT;
    EGLSurface egl_surface_ = EGL_NO_SURFACE;
    EGLConfig egl_config_ = nullptr;

    // xkb.
    xkb_context *xkb_ctx_ = nullptr;
    xkb_keymap *xkb_keymap_ = nullptr;
    xkb_state *xkb_state_ = nullptr;
    xkb_compose_table *compose_table_ = nullptr; // dead-key / Compose sequences
    xkb_compose_state *compose_state_ = nullptr;

#ifdef TOE_HAVE_TEXT_INPUT_V3
    zwp_text_input_manager_v3 *ti_manager_ = nullptr;
    zwp_text_input_v3 *text_input_ = nullptr;
    // Pending state accumulated between preedit/commit events and the `done`
    // event that applies them atomically (per the protocol).
    std::string ti_pending_preedit_{};
    std::string ti_pending_commit_{};
    int ti_pending_caret_ = -1;
    bool ti_active_ = false; // enabled (we have focus + a text_input)
    void ti_setup();
#endif

    PixelSize size_{960, 600};
    bool closed_ = false;
    bool configured_ = false;

    // Event sink is only valid during a poll_events() call.
    const std::function<void(const Event &)> *sink_ = nullptr;
};

// --- listener tables -------------------------------------------------------
const wl_registry_listener kRegistryListener = {
    &WaylandSurface::registry_global,
    &WaylandSurface::registry_global_remove,
};
const xdg_wm_base_listener kWmBaseListener = {&WaylandSurface::xdg_wm_base_ping};
const xdg_surface_listener kXdgSurfaceListener = {&WaylandSurface::xdg_surface_configure};
const xdg_toplevel_listener kToplevelListener = {
    &WaylandSurface::xdg_toplevel_configure,
    &WaylandSurface::xdg_toplevel_close,
    nullptr, // configure_bounds (newer)
    nullptr, // wm_capabilities (newer)
};
const wl_seat_listener kSeatListener = {&WaylandSurface::seat_capabilities,
                                        &WaylandSurface::seat_name};
const wl_keyboard_listener kKeyboardListener = {
    &WaylandSurface::kb_keymap,   &WaylandSurface::kb_enter,      &WaylandSurface::kb_leave,
    &WaylandSurface::kb_key,      &WaylandSurface::kb_modifiers,  &WaylandSurface::kb_repeat_info,
};
const wl_data_device_listener kDataDeviceListener = {
    &WaylandSurface::dd_data_offer, &WaylandSurface::dd_enter,     &WaylandSurface::dd_leave,
    &WaylandSurface::dd_motion,     &WaylandSurface::dd_drop,      &WaylandSurface::dd_selection,
};
const wl_data_offer_listener kDataOfferListener = {
    &WaylandSurface::offer_mime, &WaylandSurface::offer_source_actions, &WaylandSurface::offer_action,
};
const wl_data_source_listener kDataSourceListener = {
    &WaylandSurface::source_target,     &WaylandSurface::source_send,
    &WaylandSurface::source_cancelled,  &WaylandSurface::source_dnd_drop,
    &WaylandSurface::source_dnd_finished, &WaylandSurface::source_action,
};
const wl_pointer_listener kPointerListener = {
    &WaylandSurface::ptr_enter,        &WaylandSurface::ptr_leave,
    &WaylandSurface::ptr_motion,       &WaylandSurface::ptr_button,
    &WaylandSurface::ptr_axis,         &WaylandSurface::ptr_frame,
    &WaylandSurface::ptr_axis_source,  &WaylandSurface::ptr_axis_stop,
    &WaylandSurface::ptr_axis_discrete, &WaylandSurface::ptr_axis_value120,
    &WaylandSurface::ptr_axis_relative_direction,
};

// --- registry --------------------------------------------------------------
void WaylandSurface::registry_global(void *data, wl_registry *reg, uint32_t name,
                                     const char *iface, uint32_t version) {
    auto *self = static_cast<WaylandSurface *>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        self->compositor_ = static_cast<wl_compositor *>(
            wl_registry_bind(reg, name, &wl_compositor_interface, std::min(version, 4u)));
    } else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0) {
        self->wm_base_ = static_cast<xdg_wm_base *>(
            wl_registry_bind(reg, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(self->wm_base_, &kWmBaseListener, self);
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0) {
        self->seat_ = static_cast<wl_seat *>(
            wl_registry_bind(reg, name, &wl_seat_interface, std::min(version, 5u)));
        wl_seat_add_listener(self->seat_, &kSeatListener, self);
    } else if (std::strcmp(iface, wl_data_device_manager_interface.name) == 0) {
        self->data_mgr_ = static_cast<wl_data_device_manager *>(
            wl_registry_bind(reg, name, &wl_data_device_manager_interface, std::min(version, 3u)));
    }
#ifdef TOE_HAVE_TEXT_INPUT_V3
    else if (std::strcmp(iface, zwp_text_input_manager_v3_interface.name) == 0) {
        self->ti_manager_ = static_cast<zwp_text_input_manager_v3 *>(
            wl_registry_bind(reg, name, &zwp_text_input_manager_v3_interface, 1));
    }
#endif
}

// --- xdg -------------------------------------------------------------------
void WaylandSurface::xdg_surface_configure(void *data, xdg_surface *surf, uint32_t serial) {
    auto *self = static_cast<WaylandSurface *>(data);
    xdg_surface_ack_configure(surf, serial);
    self->configured_ = true;
}

void WaylandSurface::xdg_toplevel_configure(void *data, xdg_toplevel *, int32_t w, int32_t h,
                                            wl_array *) {
    auto *self = static_cast<WaylandSurface *>(data);
    if (w > 0 && h > 0) {
        self->size_ = PixelSize{w, h};
        if (self->egl_window_) {
            wl_egl_window_resize(self->egl_window_, w, h, 0, 0);
        }
        if (self->sink_) {
            (*self->sink_)(Event{Resized{self->size_}});
        }
    }
}

void WaylandSurface::xdg_toplevel_close(void *data, xdg_toplevel *) {
    auto *self = static_cast<WaylandSurface *>(data);
    self->closed_ = true;
    if (self->sink_) {
        (*self->sink_)(Event{CloseRequested{}});
    }
}

// --- seat / keyboard -------------------------------------------------------
void WaylandSurface::seat_capabilities(void *data, wl_seat *seat, uint32_t caps) {
    auto *self = static_cast<WaylandSurface *>(data);
    const bool has_kb = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
    if (has_kb && !self->keyboard_) {
        self->keyboard_ = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(self->keyboard_, &kKeyboardListener, self);
    } else if (!has_kb && self->keyboard_) {
        wl_keyboard_destroy(self->keyboard_);
        self->keyboard_ = nullptr;
    }
    const bool has_ptr = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (has_ptr && !self->pointer_) {
        self->pointer_ = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(self->pointer_, &kPointerListener, self);
    } else if (!has_ptr && self->pointer_) {
        wl_pointer_destroy(self->pointer_);
        self->pointer_ = nullptr;
    }
}

void WaylandSurface::kb_keymap(void *data, wl_keyboard *, uint32_t format, int fd, uint32_t size) {
    auto *self = static_cast<WaylandSurface *>(data);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    char *map = static_cast<char *>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }
    if (!self->xkb_ctx_) {
        self->xkb_ctx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    }
    if (self->xkb_keymap_) xkb_keymap_unref(self->xkb_keymap_);
    if (self->xkb_state_) xkb_state_unref(self->xkb_state_);
    self->xkb_keymap_ = xkb_keymap_new_from_string(self->xkb_ctx_, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                                   XKB_KEYMAP_COMPILE_NO_FLAGS);
    self->xkb_state_ = self->xkb_keymap_ ? xkb_state_new(self->xkb_keymap_) : nullptr;

    // Build a Compose table from the user's locale (dead keys, Compose-key
    // sequences). If the locale has none, compose stays disabled and typing is
    // unaffected. Rebuilt with each keymap so it tracks the current context.
    if (self->compose_state_) { xkb_compose_state_unref(self->compose_state_); self->compose_state_ = nullptr; }
    if (self->compose_table_) { xkb_compose_table_unref(self->compose_table_); self->compose_table_ = nullptr; }
    const char *locale = ::getenv("LC_ALL");
    if (!locale || !*locale) locale = ::getenv("LC_CTYPE");
    if (!locale || !*locale) locale = ::getenv("LANG");
    if (!locale || !*locale) locale = "C";
    self->compose_table_ = xkb_compose_table_new_from_locale(
        self->xkb_ctx_, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (self->compose_table_)
        self->compose_state_ = xkb_compose_state_new(self->compose_table_,
                                                     XKB_COMPOSE_STATE_NO_FLAGS);

    munmap(map, size);
    close(fd);
}

void WaylandSurface::kb_modifiers(void *data, wl_keyboard *, uint32_t, uint32_t dep, uint32_t lat,
                                  uint32_t locked, uint32_t group) {
    auto *self = static_cast<WaylandSurface *>(data);
    if (self->xkb_state_) {
        xkb_state_update_mask(self->xkb_state_, dep, lat, locked, 0, 0, group);
    }
}

// Translate one evdev keycode (already the raw wl key, +8 done here) into an
// Event and hand it to the sink. Shared by the initial press and by the repeat
// timer, so held keys repeat with identical semantics.
void WaylandSurface::emit_key(uint32_t key, KeyEvent::Kind kind) {
    if (!xkb_state_ || !sink_) return;
    const xkb_keycode_t kc = key + 8;
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_state_, kc);

    Modifiers mods;
    mods.ctrl = xkb_state_mod_name_is_active(xkb_state_, XKB_MOD_NAME_CTRL,
                                             XKB_STATE_MODS_EFFECTIVE) > 0;
    mods.alt = xkb_state_mod_name_is_active(xkb_state_, XKB_MOD_NAME_ALT,
                                            XKB_STATE_MODS_EFFECTIVE) > 0;
    mods.shift = xkb_state_mod_name_is_active(xkb_state_, XKB_MOD_NAME_SHIFT,
                                              XKB_STATE_MODS_EFFECTIVE) > 0;

    // Dead keys / Compose sequences: feed the keysym to the compose state on a
    // real press. While composing, emit an inline Preedit; on completion, emit
    // the composed text as TextEntered and clear the preedit. This is how a
    // host without a full IME still gets ´+e = é, Compose+<3 = ❤, etc.
    bool ime_owns_text = false;
#ifdef TOE_HAVE_TEXT_INPUT_V3
    ime_owns_text = ti_active_; // a real IME (fcitx/ibus) is driving input
#endif
    if (compose_state_ && !ime_owns_text && kind == KeyEvent::Kind::press && !mods.ctrl &&
        !mods.alt && sym != XKB_KEY_NoSymbol) {
        xkb_compose_state_feed(compose_state_, sym);
        const xkb_compose_status st = xkb_compose_state_get_status(compose_state_);
        if (st == XKB_COMPOSE_COMPOSING) {
            // Show a preedit hint (the compose feedback — a small marker, since
            // xkb gives no intermediate string). Non-empty so the caret shows.
            (*sink_)(Event{Preedit{std::string_view{"\xc2\xa8"}, -1}}); // ¨ combining hint
            return;
        }
        if (st == XKB_COMPOSE_COMPOSED) {
            char cbuf[16] = {0};
            const int cn = xkb_compose_state_get_utf8(compose_state_, cbuf, sizeof cbuf);
            xkb_compose_state_reset(compose_state_);
            (*sink_)(Event{Preedit{std::string_view{}, -1}}); // clear the hint
            if (cn > 0)
                (*sink_)(Event{TextEntered{std::string_view{cbuf, static_cast<size_t>(cn)}}});
            return;
        }
        if (st == XKB_COMPOSE_CANCELLED) {
            xkb_compose_state_reset(compose_state_);
            (*sink_)(Event{Preedit{std::string_view{}, -1}}); // clear the hint
            return;
        }
        // XKB_COMPOSE_NOTHING: fall through to normal handling.
    }

    auto special = [&](SpecialKey sk) {
        KeyEvent ev;
        ev.key = sk;
        ev.mods = mods;
        ev.kind = kind;
        (*sink_)(Event{KeyPressed{ev}});
    };
    switch (sym) {
    case XKB_KEY_Return: return special(SpecialKey::Enter);
    case XKB_KEY_KP_Enter: return special(SpecialKey::KpEnter);
    case XKB_KEY_BackSpace: return special(SpecialKey::Backspace);
    case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: return special(SpecialKey::Tab);
    case XKB_KEY_Escape: return special(SpecialKey::Escape);
    case XKB_KEY_Up: return special(SpecialKey::Up);
    case XKB_KEY_Down: return special(SpecialKey::Down);
    case XKB_KEY_Left: return special(SpecialKey::Left);
    case XKB_KEY_Right: return special(SpecialKey::Right);
    case XKB_KEY_Home: return special(SpecialKey::Home);
    case XKB_KEY_End: return special(SpecialKey::End);
    case XKB_KEY_Page_Up: return special(SpecialKey::PageUp);
    case XKB_KEY_Page_Down: return special(SpecialKey::PageDown);
    case XKB_KEY_Delete: return special(SpecialKey::Delete);
    case XKB_KEY_Insert: return special(SpecialKey::Insert);
    case XKB_KEY_F1: return special(SpecialKey::F1);
    case XKB_KEY_F2: return special(SpecialKey::F2);
    case XKB_KEY_F3: return special(SpecialKey::F3);
    case XKB_KEY_F4: return special(SpecialKey::F4);
    case XKB_KEY_F5: return special(SpecialKey::F5);
    case XKB_KEY_F6: return special(SpecialKey::F6);
    case XKB_KEY_F7: return special(SpecialKey::F7);
    case XKB_KEY_F8: return special(SpecialKey::F8);
    case XKB_KEY_F9: return special(SpecialKey::F9);
    case XKB_KEY_F10: return special(SpecialKey::F10);
    case XKB_KEY_F11: return special(SpecialKey::F11);
    case XKB_KEY_F12: return special(SpecialKey::F12);
    default: break;
    }

    // Ctrl / Alt combos and symbols: emit as a single-char TextInput with mods
    // set, and let the terminal's keymap encoder do the C0 / ESC-prefix work.
    char utf8[8] = {0};
    const int n = xkb_state_key_get_utf8(xkb_state_, kc, utf8, sizeof(utf8));
    if (mods.ctrl || mods.alt) {
        // Prefer the base (unshifted-ascii) character for control folding.
        const uint32_t cp = xkb_keysym_to_utf32(xkb_keysym_to_lower(sym));
        if (cp && cp < 0x80) {
            KeyEvent ev;
            ev.key = TextInput{std::string(1, static_cast<char>(cp))};
            ev.mods = mods;
            ev.kind = kind;
            (*sink_)(Event{KeyPressed{ev}});
        }
        return;
    }
    // A release event on a plain text key carries no legacy bytes, but the
    // terminal needs the KeyPressed{kind=release} form to encode it under the
    // Kitty protocol; a TextEntered has no kind, so route releases as KeyEvents.
    if (kind == KeyEvent::Kind::release) {
        if (n > 0) {
            KeyEvent ev;
            ev.key = TextInput{std::string{utf8, static_cast<size_t>(n)}};
            ev.mods = mods;
            ev.kind = kind;
            (*sink_)(Event{KeyPressed{ev}});
        }
        return;
    }
    if (n > 0) {
        (*sink_)(Event{TextEntered{std::string_view{utf8, static_cast<size_t>(n)}}});
    }
}

void WaylandSurface::arm_repeat(uint32_t key) {
    if (repeat_rate_ <= 0 || repeat_fd_ < 0) return;
    repeat_key_ = key;
    const long interval_ns = 1000000000L / repeat_rate_;
    itimerspec its{};
    its.it_value.tv_sec = repeat_delay_ / 1000;
    its.it_value.tv_nsec = (repeat_delay_ % 1000) * 1000000L;
    its.it_interval.tv_sec = interval_ns / 1000000000L;
    its.it_interval.tv_nsec = interval_ns % 1000000000L;
    timerfd_settime(repeat_fd_, 0, &its, nullptr);
}

void WaylandSurface::disarm_repeat() {
    repeat_key_ = 0;
    if (repeat_fd_ >= 0) {
        itimerspec its{};
        timerfd_settime(repeat_fd_, 0, &its, nullptr); // all-zero disarms
    }
}

void WaylandSurface::kb_key(void *data, wl_keyboard *, uint32_t serial, uint32_t, uint32_t key,
                            uint32_t state) {
    auto *self = static_cast<WaylandSurface *>(data);
    self->last_serial_ = serial; // needed for wl_data_device.set_selection

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        // Stop repeating iff the released key is the one repeating.
        if (self->repeat_key_ == key) self->disarm_repeat();
        // Emit a release event too. The terminal drops it unless the app opted
        // into the Kitty keyboard "report event types" flag (the encoder gates
        // this), so legacy apps never see spurious release input.
        self->emit_key(key, KeyEvent::Kind::release);
        return;
    }
    // Pressed: emit once, then (re)arm the repeat timer for this key. A new
    // press supersedes any previous repeat (last-key-wins, like every terminal).
    self->emit_key(key, KeyEvent::Kind::press);
    // Only repeat keys that actually produce output; modifiers/dead keys don't.
    // Crucially, NEVER repeat while Ctrl or Alt is held: those are shortcuts
    // (Ctrl+Shift+V paste, Ctrl+C copy, ...) and auto-repeating them fires the
    // action twice on a single press. Held plain text keys still repeat.
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(self->xkb_state_, key + 8);
    const bool ctrl = xkb_state_mod_name_is_active(self->xkb_state_, XKB_MOD_NAME_CTRL,
                                                   XKB_STATE_MODS_EFFECTIVE) > 0;
    const bool alt = xkb_state_mod_name_is_active(self->xkb_state_, XKB_MOD_NAME_ALT,
                                                  XKB_STATE_MODS_EFFECTIVE) > 0;
    const bool repeatable = self->xkb_keymap_ && !ctrl && !alt &&
                            xkb_keymap_key_repeats(self->xkb_keymap_, key + 8) &&
                            sym != XKB_KEY_NoSymbol;
    if (repeatable) {
        self->arm_repeat(key);
    } else {
        self->disarm_repeat();
    }
}

// --- construction ----------------------------------------------------------
Result<std::unique_ptr<WaylandSurface>> WaylandSurface::open(std::string_view title,
                                                             PixelSize initial) {
    auto s = std::unique_ptr<WaylandSurface>(new WaylandSurface());
    if (auto r = s->init(title, initial); !r) {
        return std::unexpected(r.error());
    }
    return s;
}

Result<void> WaylandSurface::init(std::string_view title, PixelSize initial) {
    size_ = initial;

    display_ = wl_display_connect(nullptr);
    if (!display_) {
        return fail("wayland: wl_display_connect failed (is a compositor running?)");
    }
    // A timerfd drives synthetic key repeat (Wayland doesn't resend held keys).
    repeat_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &kRegistryListener, this);
    wl_display_roundtrip(display_); // bind globals

    if (!compositor_ || !wm_base_) {
        return fail("wayland: missing wl_compositor or xdg_wm_base");
    }

    // Clipboard: bind a data device to the seat if the manager is present.
    if (data_mgr_ && seat_) {
        data_device_ = wl_data_device_manager_get_data_device(data_mgr_, seat_);
        wl_data_device_add_listener(data_device_, &kDataDeviceListener, this);
    }

    surface_ = wl_compositor_create_surface(compositor_);
    xdg_surface_ = xdg_wm_base_get_xdg_surface(wm_base_, surface_);
    xdg_surface_add_listener(xdg_surface_, &kXdgSurfaceListener, this);
    toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
    xdg_toplevel_add_listener(toplevel_, &kToplevelListener, this);
    xdg_toplevel_set_title(toplevel_, std::string{title}.c_str());
    xdg_toplevel_set_app_id(toplevel_, "toe");

    wl_surface_commit(surface_);
    wl_display_roundtrip(display_); // wait for first configure

#ifdef TOE_HAVE_TEXT_INPUT_V3
    ti_setup(); // create the text_input for IME (fcitx/ibus), if available
#endif

    if (auto r = init_egl(); !r) {
        return r;
    }
    return {};
}

Result<void> WaylandSurface::init_egl() {
    egl_display_ = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display_));
    if (egl_display_ == EGL_NO_DISPLAY) {
        return fail("egl: eglGetDisplay failed");
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(egl_display_, &major, &minor)) {
        return fail("egl: eglInitialize failed");
    }
    eglBindAPI(EGL_OPENGL_API);

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLint num = 0;
    if (!eglChooseConfig(egl_display_, cfg_attribs, &egl_config_, 1, &num) || num == 0) {
        return fail("egl: no matching config");
    }

    // Prefer a 4.4 core context (enables persistent-mapped buffers for the
    // fastest streaming path); fall back to 3.3 if the driver won't grant it.
    const EGLint ctx44[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 4,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
    const EGLint ctx33[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
    egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, ctx44);
    if (egl_context_ == EGL_NO_CONTEXT) {
        egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, ctx33);
    }
    if (egl_context_ == EGL_NO_CONTEXT) {
        return fail("egl: eglCreateContext (GL 3.3 core) failed");
    }

    egl_window_ = wl_egl_window_create(surface_, size_.w, size_.h);
    if (!egl_window_) {
        return fail("wayland: wl_egl_window_create failed");
    }
    egl_surface_ = eglCreateWindowSurface(egl_display_, egl_config_,
                                          reinterpret_cast<EGLNativeWindowType>(egl_window_),
                                          nullptr);
    if (egl_surface_ == EGL_NO_SURFACE) {
        return fail("egl: eglCreateWindowSurface failed");
    }

    if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_)) {
        return fail("egl: eglMakeCurrent failed");
    }
    // Swap interval 0: never let eglSwapBuffers block on the compositor's frame
    // callback. The host paces itself (its poll() loop + damage gate), and a
    // blocking swap here stalls Wayland dispatch between frames — under a
    // throttling compositor (e.g. Hyprland) that starves xdg ping/pong and gets
    // the window flagged "not responding". We present as soon as the frame is
    // ready and rely on the loop's own timing, like foot/kitty do.
    eglSwapInterval(egl_display_, 0);
    return {};
}

WaylandSurface::~WaylandSurface() {
    if (egl_display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_surface_ != EGL_NO_SURFACE) eglDestroySurface(egl_display_, egl_surface_);
        if (egl_context_ != EGL_NO_CONTEXT) eglDestroyContext(egl_display_, egl_context_);
        eglTerminate(egl_display_);
    }
    if (egl_window_) wl_egl_window_destroy(egl_window_);
    if (data_source_) wl_data_source_destroy(data_source_);
    if (selection_offer_) wl_data_offer_destroy(selection_offer_);
    if (data_device_) wl_data_device_destroy(data_device_);
    if (data_mgr_) wl_data_device_manager_destroy(data_mgr_);
    if (xkb_state_) xkb_state_unref(xkb_state_);
    if (compose_state_) xkb_compose_state_unref(compose_state_);
    if (compose_table_) xkb_compose_table_unref(compose_table_);
#ifdef TOE_HAVE_TEXT_INPUT_V3
    if (text_input_) zwp_text_input_v3_destroy(text_input_);
    if (ti_manager_) zwp_text_input_manager_v3_destroy(ti_manager_);
#endif
    if (xkb_keymap_) xkb_keymap_unref(xkb_keymap_);
    if (xkb_ctx_) xkb_context_unref(xkb_ctx_);
    if (keyboard_) wl_keyboard_destroy(keyboard_);
    if (pointer_) wl_pointer_destroy(pointer_);
    if (repeat_fd_ >= 0) ::close(repeat_fd_);
    if (toplevel_) xdg_toplevel_destroy(toplevel_);
    if (xdg_surface_) xdg_surface_destroy(xdg_surface_);
    if (surface_) wl_surface_destroy(surface_);
    if (seat_) wl_seat_destroy(seat_);
    if (wm_base_) xdg_wm_base_destroy(wm_base_);
    if (compositor_) wl_compositor_destroy(compositor_);
    if (registry_) wl_registry_destroy(registry_);
    if (display_) wl_display_disconnect(display_);
}

void WaylandSurface::swap() { swap_damaged(DamageRect::full(size_)); }

void WaylandSurface::swap_damaged(DamageRect d) {
    // Tell the compositor which buffer region changed so it recomposites only
    // that rectangle (VTE-style partial damage) instead of the whole surface.
    // eglSwapBuffers issues its own wl_surface.damage for the full area unless
    // we mark buffer damage first via the EGL_KHR_swap_buffers_with_damage path;
    // libwayland-egl forwards our wl_surface_damage_buffer with the swap.
    if (surface_ && !d.empty()) {
        wl_surface_damage_buffer(surface_, d.x, d.y, d.w, d.h);
    }
    eglSwapBuffers(egl_display_, egl_surface_);
}

void WaylandSurface::poll_events(const std::function<void(const Event &)> &sink) {
    sink_ = &sink;
    // Dispatch anything already decoded, then pull whatever is on the socket
    // WITHOUT blocking. This is called every frame regardless of what woke the
    // host's poll() (often the PTY, not Wayland), so read_events() must not be
    // allowed to block: we only read when the fd is actually readable, and
    // cancel the read otherwise. Blocking here starves xdg ping/pong and makes
    // the compositor flag the window "not responding".
    wl_display_dispatch_pending(display_);

    while (wl_display_prepare_read(display_) != 0) {
        wl_display_dispatch_pending(display_);
    }
    wl_display_flush(display_);

    // Is the Wayland fd readable right now? poll with a zero timeout.
    struct pollfd pfd{wl_display_get_fd(display_), POLLIN, 0};
    if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        wl_display_read_events(display_);
        wl_display_dispatch_pending(display_);
    } else {
        // Nothing to read — release the read intent instead of blocking on it.
        wl_display_cancel_read(display_);
    }

    // Fire synthetic key repeats for however many intervals elapsed.
    if (repeat_fd_ >= 0 && repeat_key_ != 0) {
        uint64_t expirations = 0;
        if (::read(repeat_fd_, &expirations, sizeof expirations) == sizeof expirations) {
            for (uint64_t i = 0; i < expirations && repeat_key_ != 0; ++i) {
                emit_key(repeat_key_, KeyEvent::Kind::repeat);
            }
        }
    }
    sink_ = nullptr;
}

// --- clipboard: incoming selection (paste) ---------------------------------
void WaylandSurface::dd_data_offer(void *, wl_data_device *, wl_data_offer *offer) {
    // A new offer is being introduced; listen for its mime types.
    wl_data_offer_add_listener(offer, &kDataOfferListener, nullptr);
}

void WaylandSurface::dd_selection(void *data, wl_data_device *, wl_data_offer *offer) {
    auto *self = static_cast<WaylandSurface *>(data);
    if (self->selection_offer_ && self->selection_offer_ != offer) {
        wl_data_offer_destroy(self->selection_offer_);
    }
    self->selection_offer_ = offer; // may be null (selection cleared)
}

// --- clipboard: outgoing source (copy) -------------------------------------
void WaylandSurface::source_send(void *data, wl_data_source *, const char *, int32_t fd) {
    auto *self = static_cast<WaylandSurface *>(data);
    const std::string &s = self->clipboard_owned_;
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t w = ::write(fd, s.data() + off, s.size() - off);
        if (w <= 0) break;
        off += static_cast<size_t>(w);
    }
    ::close(fd);
}

void WaylandSurface::source_cancelled(void *data, wl_data_source *source) {
    auto *self = static_cast<WaylandSurface *>(data);
    wl_data_source_destroy(source);
    if (self->data_source_ == source) self->data_source_ = nullptr;
}

void WaylandSurface::set_clipboard(std::string_view utf8) {
    if (!data_device_ || !data_mgr_) return;
    clipboard_owned_ = std::string{utf8};
    if (data_source_) {
        wl_data_source_destroy(data_source_);
        data_source_ = nullptr;
    }
    data_source_ = wl_data_device_manager_create_data_source(data_mgr_);
    wl_data_source_add_listener(data_source_, &kDataSourceListener, this);
    wl_data_source_offer(data_source_, "text/plain;charset=utf-8");
    wl_data_source_offer(data_source_, "text/plain");
    wl_data_device_set_selection(data_device_, data_source_, last_serial_);
    wl_display_flush(display_);
}

std::string WaylandSurface::get_clipboard() {
    // This pumps the display internally, which would re-dispatch a pending key
    // event (e.g. the very Ctrl+Shift+V that triggered this paste) back into the
    // sink and paste twice. Detach the sink for the duration so nested events
    // are parsed into wl state but NOT re-delivered to the host.
    const std::function<void(const Event &)> *saved_sink = sink_;
    sink_ = nullptr;
    struct SinkRestore {
        WaylandSurface *s;
        const std::function<void(const Event &)> *v;
        ~SinkRestore() { s->sink_ = v; }
    } restore{this, saved_sink};

    // Ensure any pending selection announcement has been processed so
    // selection_offer_ reflects the current clipboard owner.
    wl_display_roundtrip(display_);
    if (!selection_offer_) return {};
    int fds[2];
    if (::pipe(fds) != 0) return {};
    ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
    wl_data_offer_receive(selection_offer_, "text/plain;charset=utf-8", fds[1]);
    wl_display_flush(display_);
    ::close(fds[1]);

    std::string out;
    char buf[4096];
    // Bounded read: pump the display so the compositor delivers the pipe data.
    for (int spins = 0; spins < 200; ++spins) {
        wl_display_dispatch_pending(display_);
        wl_display_flush(display_);
        const ssize_t n = ::read(fds[0], buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<size_t>(n));
            spins = 0; // keep reading while data flows
        } else if (n == 0) {
            break; // EOF
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) break;
            usleep(1000);
        }
    }
    ::close(fds[0]);
    return out;
}

// --- pointer ---------------------------------------------------------------
void WaylandSurface::ptr_enter(void *data, wl_pointer *, uint32_t serial, wl_surface *,
                               wl_fixed_t sx, wl_fixed_t sy) {
    auto *self = static_cast<WaylandSurface *>(data);
    self->last_serial_ = serial;
    self->ptr_x_ = wl_fixed_to_int(sx);
    self->ptr_y_ = wl_fixed_to_int(sy);
}

void WaylandSurface::ptr_motion(void *data, wl_pointer *, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    auto *self = static_cast<WaylandSurface *>(data);
    self->ptr_x_ = wl_fixed_to_int(sx);
    self->ptr_y_ = wl_fixed_to_int(sy);
    if (self->sink_) {
        (*self->sink_)(Event{MouseMove{self->ptr_x_, self->ptr_y_, self->ptr_down_}});
    }
}

void WaylandSurface::ptr_button(void *data, wl_pointer *, uint32_t serial, uint32_t time,
                                uint32_t button, uint32_t state) {
    auto *self = static_cast<WaylandSurface *>(data);
    self->last_serial_ = serial;
    if (!self->sink_) return;

    // Linux input event codes.
    MouseButton btn = MouseButton::left;
    if (button == 0x111) btn = MouseButton::right;
    else if (button == 0x112) btn = MouseButton::middle;

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        const std::int32_t dx = self->ptr_x_ - self->last_click_x_;
        const std::int32_t dy = self->ptr_y_ - self->last_click_y_;
        if (time - self->last_click_time_ < 400 && dx * dx + dy * dy <= 8) {
            self->click_count_ = self->click_count_ % 3 + 1;
        } else {
            self->click_count_ = 1;
        }
        self->last_click_time_ = time;
        self->last_click_x_ = self->ptr_x_;
        self->last_click_y_ = self->ptr_y_;
        self->ptr_down_ = true;
        (*self->sink_)(
            Event{MouseDown{btn, self->ptr_x_, self->ptr_y_, self->click_count_, Modifiers{}}});
    } else {
        self->ptr_down_ = false;
        (*self->sink_)(Event{MouseUp{btn, self->ptr_x_, self->ptr_y_, Modifiers{}}});
    }
}

void WaylandSurface::ptr_axis(void *data, wl_pointer *, uint32_t, uint32_t axis,
                              wl_fixed_t value) {
    auto *self = static_cast<WaylandSurface *>(data);
    if (!self->sink_ || axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    // Positive axis value scrolls down; emit discrete steps.
    const int steps = wl_fixed_to_int(value) > 0 ? -1 : 1;
    (*self->sink_)(Event{MouseWheel{0, steps}});
}

#ifdef TOE_HAVE_TEXT_INPUT_V3
// --- text-input-v3: real IME (fcitx/ibus) --------------------------------
namespace {
// Some distros ship a text-input-v3 XML with vendor extension events (action/
// language/preedit_hint). We only need the standard six; leave the rest null.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const zwp_text_input_v3_listener kTextInputListener = {
    .enter = &WaylandSurface::ti_enter,
    .leave = &WaylandSurface::ti_leave,
    .preedit_string = &WaylandSurface::ti_preedit,
    .commit_string = &WaylandSurface::ti_commit_string,
    .delete_surrounding_text = &WaylandSurface::ti_delete_surrounding,
    .done = &WaylandSurface::ti_done,
};
#pragma GCC diagnostic pop
} // namespace

void WaylandSurface::ti_setup() {
    if (!ti_manager_ || !seat_) return;
    text_input_ = zwp_text_input_manager_v3_get_text_input(ti_manager_, seat_);
    if (text_input_) zwp_text_input_v3_add_listener(text_input_, &kTextInputListener, this);
}

// The IME gained/lost the text field (our surface). enable + commit tells the
// compositor we accept input methods; a serial handshake follows via done().
void WaylandSurface::ti_enter(void *data, zwp_text_input_v3 *ti, wl_surface *) {
    auto *self = static_cast<WaylandSurface *>(data);
    self->ti_active_ = true;
    zwp_text_input_v3_enable(ti);
    zwp_text_input_v3_set_content_type(
        ti, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL);
    zwp_text_input_v3_commit(ti);
}
void WaylandSurface::ti_leave(void *data, zwp_text_input_v3 *ti, wl_surface *) {
    auto *self = static_cast<WaylandSurface *>(data);
    self->ti_active_ = false;
    zwp_text_input_v3_disable(ti);
    zwp_text_input_v3_commit(ti);
    if (self->sink_) (*self->sink_)(Event{Preedit{std::string_view{}, -1}}); // clear
}

// Preedit/commit arrive as a batch terminated by done(); accumulate here.
void WaylandSurface::ti_preedit(void *data, zwp_text_input_v3 *, const char *text,
                                int32_t cursor_begin, int32_t) {
    auto *self = static_cast<WaylandSurface *>(data);
    self->ti_pending_preedit_ = text ? text : "";
    // cursor_begin is a byte offset; the engine wants a cell offset. Leave -1
    // (caret at end) unless the IME put it at the start.
    self->ti_pending_caret_ = (cursor_begin == 0) ? 0 : -1;
}
void WaylandSurface::ti_commit_string(void *data, zwp_text_input_v3 *, const char *text) {
    auto *self = static_cast<WaylandSurface *>(data);
    self->ti_pending_commit_ = text ? text : "";
}
void WaylandSurface::ti_delete_surrounding(void *, zwp_text_input_v3 *, uint32_t, uint32_t) {
    // Terminals don't expose surrounding text, so there's nothing to delete.
}

// Apply the accumulated batch atomically: commit text first (as TextEntered),
// then set the new preedit (or clear it).
void WaylandSurface::ti_done(void *data, zwp_text_input_v3 *ti, uint32_t serial) {
    auto *self = static_cast<WaylandSurface *>(data);
    (void)ti;
    (void)serial;
    if (!self->sink_) { self->ti_pending_commit_.clear(); self->ti_pending_preedit_.clear(); return; }
    if (!self->ti_pending_commit_.empty()) {
        (*self->sink_)(Event{TextEntered{std::string_view{self->ti_pending_commit_}}});
    }
    (*self->sink_)(Event{Preedit{std::string_view{self->ti_pending_preedit_},
                                 self->ti_pending_caret_}});
    self->ti_pending_commit_.clear();
    self->ti_pending_preedit_.clear();
}
#endif // TOE_HAVE_TEXT_INPUT_V3

} // namespace

// Exposed to the backend selector in surface.cpp.
Result<AnySurface> open_wayland_surface(std::string_view title, PixelSize initial) {
    auto ws = WaylandSurface::open(title, initial);
    if (!ws) {
        return std::unexpected(ws.error());
    }
    return AnySurface{std::move(*ws)};
}

} // namespace toe::platform
