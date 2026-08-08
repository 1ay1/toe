// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Wayland + EGL backend for platform::Surface. Connects to the compositor,
// creates an xdg-shell toplevel, wraps a wl_egl_window in an EGL/GL 3.3
// context, and translates wl_keyboard events (decoded through xkbcommon) into
// the platform-neutral Event sum type.

#include "gvte/platform/surface.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>

#include <fcntl.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <xkbcommon/xkbcommon.h>

// epoxy must be included before (or instead of) the system EGL/GL headers; it
// re-exports the EGL and GL symbols itself.
#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "xdg-shell-client-protocol.h"

namespace gvte::platform {

namespace {

class WaylandSurface final {
public:
    static Result<std::unique_ptr<WaylandSurface>> open(std::string_view title, PixelSize initial);

    ~WaylandSurface();

    void swap();
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
    void emit_key(uint32_t key);
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
void WaylandSurface::emit_key(uint32_t key) {
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

    auto special = [&](SpecialKey sk) {
        KeyEvent ev;
        ev.key = sk;
        ev.mods = mods;
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
        return;
    }
    // Pressed: emit once, then (re)arm the repeat timer for this key. A new
    // press supersedes any previous repeat (last-key-wins, like every terminal).
    self->emit_key(key);
    // Only repeat keys that actually produce output; modifiers/dead keys don't.
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(self->xkb_state_, key + 8);
    const bool repeatable = self->xkb_keymap_ &&
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
    xdg_toplevel_set_app_id(toplevel_, "gvte");

    wl_surface_commit(surface_);
    wl_display_roundtrip(display_); // wait for first configure

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
    eglSwapInterval(egl_display_, 1);
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

void WaylandSurface::swap() {
    eglSwapBuffers(egl_display_, egl_surface_);
}

void WaylandSurface::poll_events(const std::function<void(const Event &)> &sink) {
    sink_ = &sink;
    // Dispatch anything already decoded, then read whatever is on the socket
    // (non-blocking) and dispatch that too. The host is expected to have polled
    // the fd, so a read here won't block meaningfully; prepare_read/read_events
    // is the race-free way to pull new events after a poll wakeup.
    wl_display_dispatch_pending(display_);
    while (wl_display_prepare_read(display_) != 0) {
        wl_display_dispatch_pending(display_);
    }
    wl_display_flush(display_);
    wl_display_read_events(display_);
    wl_display_dispatch_pending(display_);

    // Fire synthetic key repeats for however many intervals elapsed.
    if (repeat_fd_ >= 0 && repeat_key_ != 0) {
        uint64_t expirations = 0;
        if (::read(repeat_fd_, &expirations, sizeof expirations) == sizeof expirations) {
            for (uint64_t i = 0; i < expirations && repeat_key_ != 0; ++i) {
                emit_key(repeat_key_);
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

} // namespace

// Exposed to the backend selector in surface.cpp.
Result<AnySurface> open_wayland_surface(std::string_view title, PixelSize initial) {
    auto ws = WaylandSurface::open(title, initial);
    if (!ws) {
        return std::unexpected(ws.error());
    }
    return AnySurface{std::move(*ws)};
}

} // namespace gvte::platform
