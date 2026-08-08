// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Headless / offscreen surface. Creates a valid EGL GL context with NO window
// and NO compositor — via EGL_PLATFORM_SURFACELESS_MESA when available, else a
// tiny pbuffer on the default display. This is what lets the render/cache tests
// (and any batch/CI job) exercise the real GL renderer without a Wayland or X11
// server. Event, clipboard and title methods are inert stubs.

#include "toe/platform/surface.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

namespace toe::platform {

namespace {

class OffscreenSurface final {
public:
    static Result<std::unique_ptr<OffscreenSurface>> open(PixelSize size) {
        auto s = std::unique_ptr<OffscreenSurface>(new OffscreenSurface());
        s->size_ = size;

        // Use the default display + a pbuffer draw surface. This works on every
        // EGL implementation (Mesa's swrast included) without needing the EGL
        // 1.5 eglGetPlatformDisplay entry point, which epoxy will abort on if
        // the driver doesn't provide it.
        EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (dpy == EGL_NO_DISPLAY) {
            return fail("offscreen: no EGL display (need a working libEGL, e.g. Mesa)");
        }
        if (!eglInitialize(dpy, nullptr, nullptr)) {
            return fail("offscreen: eglInitialize failed");
        }
        s->dpy_ = dpy;

        eglBindAPI(EGL_OPENGL_API);
        const EGLint cfg_attribs[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE,
        };
        EGLConfig cfg{};
        EGLint num = 0;
        if (!eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &num) || num == 0) {
            return fail("offscreen: no matching EGL config");
        }

        // Match the on-screen backends: request 4.4 core, fall back to 3.3.
        const EGLint ctx44[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 4,
                                EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
        const EGLint ctx33[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                                EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
        s->ctx_ = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx44);
        if (s->ctx_ == EGL_NO_CONTEXT) {
            s->ctx_ = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx33);
        }
        if (s->ctx_ == EGL_NO_CONTEXT) {
            return fail("offscreen: eglCreateContext failed");
        }

        // A pbuffer draw surface sized to the request.
        {
            const EGLint pb[] = {EGL_WIDTH, size.w > 0 ? size.w : 1,
                                 EGL_HEIGHT, size.h > 0 ? size.h : 1, EGL_NONE};
            s->surf_ = eglCreatePbufferSurface(dpy, cfg, pb);
            if (s->surf_ == EGL_NO_SURFACE) {
                return fail("offscreen: eglCreatePbufferSurface failed");
            }
        }

        if (!eglMakeCurrent(dpy, s->surf_, s->surf_, s->ctx_)) {
            return fail("offscreen: eglMakeCurrent failed");
        }
        return s;
    }

    ~OffscreenSurface() {
        if (dpy_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surf_ != EGL_NO_SURFACE) eglDestroySurface(dpy_, surf_);
            if (ctx_ != EGL_NO_CONTEXT) eglDestroyContext(dpy_, ctx_);
            eglTerminate(dpy_);
        }
    }

    void swap() {
        if (surf_ != EGL_NO_SURFACE) eglSwapBuffers(dpy_, surf_);
        else glFlush();
    }
    [[nodiscard]] PixelSize pixel_size() const { return size_; }
    void set_title(std::string_view) {}
    void set_clipboard(std::string_view) {}
    [[nodiscard]] std::string get_clipboard() { return {}; }
    void poll_events(const std::function<void(const Event &)> &) {}
    [[nodiscard]] int event_fd() const { return -1; }
    [[nodiscard]] bool should_close() const { return false; }

private:
    OffscreenSurface() = default;
    PixelSize size_{};
    EGLDisplay dpy_ = EGL_NO_DISPLAY;
    EGLContext ctx_ = EGL_NO_CONTEXT;
    EGLSurface surf_ = EGL_NO_SURFACE;
};

} // namespace

Result<AnySurface> open_offscreen_surface(PixelSize size) {
    auto s = OffscreenSurface::open(size);
    if (!s) {
        return std::unexpected(s.error());
    }
    return AnySurface{std::move(*s)};
}

} // namespace toe::platform
