// SPDX-License-Identifier: LGPL-2.0-or-later
//
// X11 (Xlib + XCB) + EGL backend for platform::Surface. Xlib owns the Display
// (so eglGetDisplay works portably), XCB drives the event loop, and
// xkbcommon-x11 decodes keys into the neutral Event sum type. The window,
// event translation and EGL setup mirror the Wayland backend so both satisfy
// the exact same interface.

#include "gvte/platform/surface.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <xcb/xcb.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-x11.h>

#include <unistd.h>

namespace gvte::platform {

namespace {

class X11Surface final {
public:
    static Result<std::unique_ptr<X11Surface>> open(std::string_view title, PixelSize initial);

    ~X11Surface();

    void swap();
    [[nodiscard]] PixelSize pixel_size() const { return size_; }
    [[nodiscard]] int event_fd() const { return xcb_get_file_descriptor(xcb_); }
    void flush() { xcb_flush(xcb_); }
    void set_title(std::string_view title) {
        if (display_ && window_) {
            XStoreName(display_, static_cast<Window>(window_), std::string{title}.c_str());
            XFlush(display_);
        }
    }
    void set_clipboard(std::string_view utf8);
    [[nodiscard]] std::string get_clipboard();
    void poll_events(const std::function<void(const Event &)> &sink);
    [[nodiscard]] bool should_close() const { return closed_; }

private:
    X11Surface() = default;
    Result<void> init(std::string_view title, PixelSize initial);
    Result<void> init_egl();
    Result<void> create_egl_surface();
    Result<void> init_xkb();
    void handle_key(xcb_keycode_t code, const std::function<void(const Event &)> &sink);

    Display *display_ = nullptr;
    xcb_connection_t *xcb_ = nullptr;
    xcb_window_t window_ = 0;
    xcb_atom_t wm_delete_ = 0;

    // Clipboard (CLIPBOARD selection) state.
    Atom a_clipboard_ = 0;
    Atom a_utf8_ = 0;
    Atom a_targets_ = 0;
    Atom a_prop_ = 0; // property used for transfers ("GVTE_CLIP")
    std::string clipboard_owned_; // text we currently offer

    // Pointer / click-count tracking.
    bool button_down_ = false;
    uint32_t last_click_time_ = 0;
    int16_t last_click_x_ = -1, last_click_y_ = -1;
    int click_count_ = 0;

    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLContext egl_context_ = EGL_NO_CONTEXT;
    EGLSurface egl_surface_ = EGL_NO_SURFACE;
    EGLConfig egl_config_ = nullptr;

    xkb_context *xkb_ctx_ = nullptr;
    xkb_keymap *xkb_keymap_ = nullptr;
    xkb_state *xkb_state_ = nullptr;
    int32_t xkb_device_ = -1;

    PixelSize size_{960, 600};
    bool closed_ = false;
};

xcb_atom_t intern_atom(xcb_connection_t *c, const char *name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(c, 0, static_cast<uint16_t>(std::strlen(name)),
                                                      name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(c, cookie, nullptr);
    const xcb_atom_t atom = reply ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    free(reply);
    return atom;
}

Result<std::unique_ptr<X11Surface>> X11Surface::open(std::string_view title, PixelSize initial) {
    auto s = std::unique_ptr<X11Surface>(new X11Surface());
    if (auto r = s->init(title, initial); !r) {
        return std::unexpected(r.error());
    }
    return s;
}

Result<void> X11Surface::init(std::string_view title, PixelSize initial) {
    size_ = initial;

    display_ = XOpenDisplay(nullptr);
    if (!display_) {
        return fail("x11: XOpenDisplay failed (no X server / DISPLAY unset)");
    }
    xcb_ = XGetXCBConnection(display_);
    if (!xcb_ || xcb_connection_has_error(xcb_)) {
        return fail("x11: XGetXCBConnection failed");
    }
    // Let XCB own the event queue.
    XSetEventQueueOwner(display_, XCBOwnsEventQueue);

    // Initialize EGL first: the window must be created with the visual that
    // matches the chosen EGL config, or eglCreateWindowSurface fails.
    if (auto r = init_egl(); !r) {
        return r;
    }
    EGLint visual_id = 0;
    eglGetConfigAttrib(egl_display_, egl_config_, EGL_NATIVE_VISUAL_ID, &visual_id);

    const int scr = DefaultScreen(display_);
    Window root = RootWindow(display_, scr);

    // Resolve the EGL visual to an Xlib Visual*; fall back to the default.
    Visual *visual = DefaultVisual(display_, scr);
    int depth = DefaultDepth(display_, scr);
    if (visual_id != 0) {
        XVisualInfo tmpl{};
        tmpl.visualid = static_cast<VisualID>(visual_id);
        int count = 0;
        if (XVisualInfo *vi = XGetVisualInfo(display_, VisualIDMask, &tmpl, &count); vi) {
            visual = vi->visual;
            depth = vi->depth;
            XFree(vi);
        }
    }

    Colormap colormap = XCreateColormap(display_, root, visual, AllocNone);
    XSetWindowAttributes swa{};
    swa.colormap = colormap;
    swa.background_pixel = 0;
    swa.border_pixel = 0;
    swa.event_mask = KeyPressMask | StructureNotifyMask | ExposureMask | ButtonPressMask |
                     ButtonReleaseMask | PointerMotionMask | FocusChangeMask;

    Window win = XCreateWindow(display_, root, 0, 0, static_cast<unsigned>(size_.w),
                               static_cast<unsigned>(size_.h), 0, depth, InputOutput, visual,
                               CWColormap | CWBackPixel | CWBorderPixel | CWEventMask, &swa);
    window_ = static_cast<xcb_window_t>(win);

    XStoreName(display_, win, std::string{title}.c_str());

    // WM close-button handling (WM_DELETE_WINDOW).
    wm_delete_ = intern_atom(xcb_, "WM_DELETE_WINDOW");
    Atom del = static_cast<Atom>(wm_delete_);
    XSetWMProtocols(display_, win, &del, 1);

    // Clipboard atoms.
    a_clipboard_ = XInternAtom(display_, "CLIPBOARD", False);
    a_utf8_ = XInternAtom(display_, "UTF8_STRING", False);
    a_targets_ = XInternAtom(display_, "TARGETS", False);
    a_prop_ = XInternAtom(display_, "GVTE_CLIP", False);

    XMapWindow(display_, win);
    // Detectable auto-repeat: the server sends only KeyPress on repeat (no
    // phantom KeyRelease/KeyPress pair), so held keys repeat cleanly.
    {
        Bool supported = False;
        XkbSetDetectableAutoRepeat(display_, True, &supported);
    }
    XSync(display_, False);

    if (auto r = create_egl_surface(); !r) {
        return r;
    }
    if (auto r = init_xkb(); !r) {
        return r;
    }
    return {};
}

Result<void> X11Surface::init_egl() {
    // Force the X11 EGL platform explicitly. On mixed Wayland/Xwayland setups
    // the legacy eglGetDisplay(Display*) can pick the wrong platform and then
    // reject the X window with EGL_BAD_NATIVE_WINDOW. The EXT entry point is a
    // client extension resolvable without an initialized display.
    const char *client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (client_exts && std::strstr(client_exts, "EGL_EXT_platform_x11")) {
        auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (get_platform_display) {
            egl_display_ = get_platform_display(EGL_PLATFORM_X11_EXT, display_, nullptr);
        }
    }
    if (egl_display_ == EGL_NO_DISPLAY) {
        egl_display_ = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display_));
    }
    if (egl_display_ == EGL_NO_DISPLAY) {
        return fail("egl(x11): could not obtain an EGL display");
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(egl_display_, &major, &minor)) {
        return fail("egl(x11): eglInitialize failed");
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
        return fail("egl(x11): no matching config");
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
        return fail("egl(x11): eglCreateContext (GL 3.3 core) failed");
    }
    return {};
}

Result<void> X11Surface::create_egl_surface() {
    egl_surface_ = eglCreateWindowSurface(
        egl_display_, egl_config_, static_cast<EGLNativeWindowType>(window_), nullptr);
    if (egl_surface_ == EGL_NO_SURFACE) {
        const EGLint err = eglGetError();
        return fail("egl(x11): eglCreateWindowSurface failed (0x" +
                    [](EGLint e) { char b[8]; std::snprintf(b, sizeof b, "%x", e); return std::string{b}; }(err) +
                    ")");
    }
    if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_)) {
        return fail("egl(x11): eglMakeCurrent failed");
    }
    eglSwapInterval(egl_display_, 1);
    return {};
}

Result<void> X11Surface::init_xkb() {
    if (xkb_x11_setup_xkb_extension(xcb_, XKB_X11_MIN_MAJOR_XKB_VERSION,
                                    XKB_X11_MIN_MINOR_XKB_VERSION,
                                    XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, nullptr, nullptr, nullptr,
                                    nullptr) == 0) {
        return fail("xkb(x11): failed to set up XKB extension");
    }
    xkb_ctx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!xkb_ctx_) {
        return fail("xkb(x11): xkb_context_new failed");
    }
    xkb_device_ = xkb_x11_get_core_keyboard_device_id(xcb_);
    if (xkb_device_ == -1) {
        return fail("xkb(x11): no core keyboard device");
    }
    xkb_keymap_ = xkb_x11_keymap_new_from_device(xkb_ctx_, xcb_, xkb_device_,
                                                 XKB_KEYMAP_COMPILE_NO_FLAGS);
    xkb_state_ = xkb_keymap_ ? xkb_x11_state_new_from_device(xkb_keymap_, xcb_, xkb_device_)
                             : nullptr;
    if (!xkb_state_) {
        return fail("xkb(x11): failed to build keymap/state");
    }
    return {};
}

void X11Surface::handle_key(xcb_keycode_t code, const std::function<void(const Event &)> &sink) {
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_state_, code);

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
        sink(Event{KeyPressed{ev}});
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

    if (mods.ctrl) {
        const xkb_keysym_t lower = xkb_keysym_to_lower(sym);
        if (lower >= XKB_KEY_a && lower <= XKB_KEY_z) {
            KeyEvent ev;
            ev.key = TextInput{std::string(1, static_cast<char>('a' + (lower - XKB_KEY_a)))};
            ev.mods = mods;
            sink(Event{KeyPressed{ev}});
            return;
        }
    }

    char utf8[8] = {0};
    const int n = xkb_state_key_get_utf8(xkb_state_, code, utf8, sizeof(utf8));
    if (n > 0 && !(mods.ctrl || mods.alt)) {
        sink(Event{TextEntered{std::string_view{utf8, static_cast<size_t>(n)}}});
    }
}

void X11Surface::set_clipboard(std::string_view utf8) {
    clipboard_owned_ = std::string{utf8};
    // Claim ownership of the CLIPBOARD selection. Subsequent SelectionRequest
    // events (handled in poll_events) will serve `clipboard_owned_`.
    XSetSelectionOwner(display_, a_clipboard_, static_cast<Window>(window_), CurrentTime);
    XFlush(display_);
}

std::string X11Surface::get_clipboard() {
    const Window owner = XGetSelectionOwner(display_, a_clipboard_);
    if (owner == None) {
        return {};
    }
    if (owner == static_cast<Window>(window_)) {
        return clipboard_owned_; // we own it; short-circuit
    }
    XConvertSelection(display_, a_clipboard_, a_utf8_, a_prop_, static_cast<Window>(window_),
                      CurrentTime);
    XFlush(display_);

    // Wait for the SelectionNotify through XCB (which owns the event queue).
    // Non-selection events seen during this brief window are dropped — paste is
    // a rare, explicit operation.
    for (int spins = 0; spins < 200; ++spins) {
        xcb_generic_event_t *ev = nullptr;
        while ((ev = xcb_poll_for_event(xcb_)) != nullptr) {
            const uint8_t type = static_cast<uint8_t>(ev->response_type & 0x7f);
            if (type == XCB_SELECTION_NOTIFY) {
                auto *sn = reinterpret_cast<xcb_selection_notify_event_t *>(ev);
                const xcb_atom_t prop = sn->property;
                free(ev);
                if (prop == XCB_ATOM_NONE) {
                    return {};
                }
                Atom actual_type = 0;
                int actual_format = 0;
                unsigned long nitems = 0, bytes_after = 0;
                unsigned char *data = nullptr;
                // long_length is in 32-bit units; 0x1fffffff covers any sane
                // clipboard payload without overflowing the server's math.
                XGetWindowProperty(display_, static_cast<Window>(window_), a_prop_, 0, 0x1fffffff,
                                   True, AnyPropertyType, &actual_type, &actual_format, &nitems,
                                   &bytes_after, &data);
                std::string out;
                if (data) {
                    out.assign(reinterpret_cast<char *>(data), nitems);
                    XFree(data);
                }
                return out;
            }
            free(ev);
        }
        usleep(1000);
    }
    return {};
}

void X11Surface::poll_events(const std::function<void(const Event &)> &sink) {
    xcb_generic_event_t *ev = nullptr;
    while ((ev = xcb_poll_for_event(xcb_)) != nullptr) {
        switch (ev->response_type & ~0x80) {
        case XCB_KEY_PRESS: {
            auto *k = reinterpret_cast<xcb_key_press_event_t *>(ev);
            handle_key(k->detail, sink);
            break;
        }
        case XCB_CONFIGURE_NOTIFY: {
            auto *c = reinterpret_cast<xcb_configure_notify_event_t *>(ev);
            if (c->width > 0 && c->height > 0 &&
                (c->width != size_.w || c->height != size_.h)) {
                size_ = PixelSize{c->width, c->height};
                sink(Event{Resized{size_}});
            }
            break;
        }
        case XCB_CLIENT_MESSAGE: {
            auto *m = reinterpret_cast<xcb_client_message_event_t *>(ev);
            if (m->data.data32[0] == wm_delete_) {
                closed_ = true;
                sink(Event{CloseRequested{}});
            }
            break;
        }
        case XCB_BUTTON_PRESS: {
            auto *b = reinterpret_cast<xcb_button_press_event_t *>(ev);
            Modifiers mods;
            mods.ctrl = (b->state & XCB_MOD_MASK_CONTROL) != 0;
            mods.shift = (b->state & XCB_MOD_MASK_SHIFT) != 0;
            mods.alt = (b->state & XCB_MOD_MASK_1) != 0;
            if (b->detail == 4 || b->detail == 5) { // wheel up/down
                sink(Event{MouseWheel{0, b->detail == 4 ? 1 : -1}});
            } else {
                // Click-count: same button, within 400ms and ~2px.
                const int dx = b->event_x - last_click_x_;
                const int dy = b->event_y - last_click_y_;
                if (b->time - last_click_time_ < 400 && dx * dx + dy * dy <= 8) {
                    click_count_ = click_count_ % 3 + 1;
                } else {
                    click_count_ = 1;
                }
                last_click_time_ = b->time;
                last_click_x_ = b->event_x;
                last_click_y_ = b->event_y;
                button_down_ = true;
                MouseButton btn = (b->detail == 3)   ? MouseButton::right
                                  : (b->detail == 2) ? MouseButton::middle
                                                     : MouseButton::left;
                sink(Event{MouseDown{btn, b->event_x, b->event_y, click_count_, mods}});
            }
            break;
        }
        case XCB_BUTTON_RELEASE: {
            auto *b = reinterpret_cast<xcb_button_release_event_t *>(ev);
            if (b->detail != 4 && b->detail != 5) {
                button_down_ = false;
                Modifiers mods;
                mods.ctrl = (b->state & XCB_MOD_MASK_CONTROL) != 0;
                mods.shift = (b->state & XCB_MOD_MASK_SHIFT) != 0;
                MouseButton btn = (b->detail == 3)   ? MouseButton::right
                                  : (b->detail == 2) ? MouseButton::middle
                                                     : MouseButton::left;
                sink(Event{MouseUp{btn, b->event_x, b->event_y, mods}});
            }
            break;
        }
        case XCB_MOTION_NOTIFY: {
            auto *m = reinterpret_cast<xcb_motion_notify_event_t *>(ev);
            sink(Event{MouseMove{m->event_x, m->event_y, button_down_}});
            break;
        }
        case XCB_SELECTION_REQUEST: {
            // Another client is pasting from our CLIPBOARD: serve the text.
            auto *r = reinterpret_cast<xcb_selection_request_event_t *>(ev);
            XSelectionEvent notify{};
            notify.type = SelectionNotify;
            notify.display = display_;
            notify.requestor = r->requestor;
            notify.selection = r->selection;
            notify.target = r->target;
            notify.time = r->time;
            notify.property = None;
            if (r->target == a_utf8_ || r->target == XA_STRING) {
                XChangeProperty(display_, r->requestor, r->property, r->target, 8, PropModeReplace,
                                reinterpret_cast<const unsigned char *>(clipboard_owned_.data()),
                                static_cast<int>(clipboard_owned_.size()));
                notify.property = r->property;
            } else if (r->target == a_targets_) {
                Atom targets[] = {a_targets_, a_utf8_, XA_STRING};
                XChangeProperty(display_, r->requestor, r->property, XA_ATOM, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char *>(targets), 3);
                notify.property = r->property;
            }
            XSendEvent(display_, r->requestor, False, 0, reinterpret_cast<XEvent *>(&notify));
            XFlush(display_);
            break;
        }
        case XCB_FOCUS_IN:
            sink(Event{FocusChanged{true}});
            break;
        case XCB_FOCUS_OUT:
            sink(Event{FocusChanged{false}});
            break;
        default:
            break;
        }
        free(ev);
    }
    if (xcb_connection_has_error(xcb_)) {
        closed_ = true;
    }
}

void X11Surface::swap() { eglSwapBuffers(egl_display_, egl_surface_); }

X11Surface::~X11Surface() {
    if (egl_display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_surface_ != EGL_NO_SURFACE) eglDestroySurface(egl_display_, egl_surface_);
        if (egl_context_ != EGL_NO_CONTEXT) eglDestroyContext(egl_display_, egl_context_);
        eglTerminate(egl_display_);
    }
    if (xkb_state_) xkb_state_unref(xkb_state_);
    if (xkb_keymap_) xkb_keymap_unref(xkb_keymap_);
    if (xkb_ctx_) xkb_context_unref(xkb_ctx_);
    if (xcb_ && window_) {
        xcb_destroy_window(xcb_, window_);
        xcb_flush(xcb_);
    }
    if (display_) XCloseDisplay(display_);
}

} // namespace

// Exposed to the backend selector in surface.cpp.
Result<AnySurface> open_x11_surface(std::string_view title, PixelSize initial) {
    auto s = X11Surface::open(title, initial);
    if (!s) {
        return std::unexpected(s.error());
    }
    return AnySurface{std::move(*s)};
}

} // namespace gvte::platform
