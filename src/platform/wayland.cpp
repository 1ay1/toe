// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Wayland + EGL backend for platform::Surface. Connects to the compositor,
// creates an xdg-shell toplevel, wraps a wl_egl_window in an EGL/GL 3.3
// context, and translates wl_keyboard events (decoded through xkbcommon) into
// the platform-neutral Event sum type.

#include "gvte/platform/surface.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <xkbcommon/xkbcommon.h>

// epoxy must be included before (or instead of) the system EGL/GL headers; it
// re-exports the EGL and GL symbols itself.
#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <sys/mman.h>
#include <unistd.h>

#include "xdg-shell-client-protocol.h"

namespace gvte::platform {

namespace {

class WaylandSurface final : public Surface {
public:
    static Result<std::unique_ptr<WaylandSurface>> open(std::string_view title, PixelSize initial);

    ~WaylandSurface() override;

    void swap() override;
    [[nodiscard]] PixelSize pixel_size() const override { return size_; }
    void poll_events(const std::function<void(const Event &)> &sink) override;
    [[nodiscard]] bool should_close() const override { return closed_; }

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
    static void kb_enter(void *, wl_keyboard *, uint32_t, wl_surface *, wl_array *) {}
    static void kb_leave(void *, wl_keyboard *, uint32_t, wl_surface *) {}
    static void kb_key(void *data, wl_keyboard *, uint32_t serial, uint32_t time, uint32_t key,
                       uint32_t state);
    static void kb_modifiers(void *data, wl_keyboard *, uint32_t serial, uint32_t dep,
                             uint32_t lat, uint32_t locked, uint32_t group);
    static void kb_repeat_info(void *, wl_keyboard *, int32_t, int32_t) {}

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
    wl_surface *surface_ = nullptr;
    xdg_surface *xdg_surface_ = nullptr;
    xdg_toplevel *toplevel_ = nullptr;
    wl_egl_window *egl_window_ = nullptr;

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

void WaylandSurface::kb_key(void *data, wl_keyboard *, uint32_t, uint32_t, uint32_t key,
                            uint32_t state) {
    auto *self = static_cast<WaylandSurface *>(data);
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !self->xkb_state_ || !self->sink_) {
        return;
    }
    // Wayland keycodes are evdev; xkb expects +8.
    const xkb_keycode_t kc = key + 8;
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(self->xkb_state_, kc);

    Modifiers mods;
    mods.ctrl = xkb_state_mod_name_is_active(self->xkb_state_, XKB_MOD_NAME_CTRL,
                                             XKB_STATE_MODS_EFFECTIVE) > 0;
    mods.alt = xkb_state_mod_name_is_active(self->xkb_state_, XKB_MOD_NAME_ALT,
                                            XKB_STATE_MODS_EFFECTIVE) > 0;
    mods.shift = xkb_state_mod_name_is_active(self->xkb_state_, XKB_MOD_NAME_SHIFT,
                                              XKB_STATE_MODS_EFFECTIVE) > 0;

    // Special keys first.
    auto special = [&](SpecialKey sk) {
        KeyEvent ev;
        ev.key = sk;
        ev.mods = mods;
        (*self->sink_)(Event{KeyPressed{ev}});
    };
    switch (sym) {
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: return special(SpecialKey::Enter);
    case XKB_KEY_BackSpace: return special(SpecialKey::Backspace);
    case XKB_KEY_Tab: return special(SpecialKey::Tab);
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
    default: break;
    }

    // Control-letter combos: emit as text so the terminal folds to a C0 control.
    if (mods.ctrl) {
        const xkb_keysym_t lower = xkb_keysym_to_lower(sym);
        if (lower >= XKB_KEY_a && lower <= XKB_KEY_z) {
            KeyEvent ev;
            ev.key = TextInput{std::string(1, static_cast<char>('a' + (lower - XKB_KEY_a)))};
            ev.mods = mods;
            (*self->sink_)(Event{KeyPressed{ev}});
            return;
        }
    }

    // Ordinary text: let xkb produce the UTF-8 for this keysym+state.
    char utf8[8] = {0};
    const int n = xkb_state_key_get_utf8(self->xkb_state_, kc, utf8, sizeof(utf8));
    if (n > 0 && !(mods.ctrl || mods.alt)) {
        (*self->sink_)(Event{TextEntered{std::string_view{utf8, static_cast<size_t>(n)}}});
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
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &kRegistryListener, this);
    wl_display_roundtrip(display_); // bind globals

    if (!compositor_ || !wm_base_) {
        return fail("wayland: missing wl_compositor or xdg_wm_base");
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

    const EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
    egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, ctx_attribs);
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
    if (xkb_state_) xkb_state_unref(xkb_state_);
    if (xkb_keymap_) xkb_keymap_unref(xkb_keymap_);
    if (xkb_ctx_) xkb_context_unref(xkb_ctx_);
    if (keyboard_) wl_keyboard_destroy(keyboard_);
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
    // Dispatch whatever is already queued, then flush + read new events without
    // blocking, so the render loop keeps running.
    wl_display_dispatch_pending(display_);
    wl_display_flush(display_);
    sink_ = nullptr;
}

} // namespace

// Backend entry point (Wayland only for now; X11 added behind the same call).
Result<std::unique_ptr<Surface>> open_surface(std::string_view title, PixelSize initial) {
    auto ws = WaylandSurface::open(title, initial);
    if (!ws) {
        return std::unexpected(ws.error());
    }
    return std::unique_ptr<Surface>(std::move(*ws));
}

} // namespace gvte::platform
