// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The Surface contract, as a *concept* — the seam between toe's engine and a
// window system. This header lives in toe::core and names NO platform type
// (no wl_*, no xcb_*, no EGL). It defines only:
//
//   1. the platform-neutral `Event` closed sum type,
//   2. a C++23 `concept Surface` — the STRUCTURAL contract a host's window
//      must satisfy (by shape, not by inheritance),
//   3. `AnySurface` — an optional type-erased adapter for hosts that want
//      runtime polymorphism over the concept.
//
// Design: a host brings its own window (GLFW, Qt, SDL, Win32, Cocoa) by making
// a type that *models* `Surface`. There is no base class to inherit, no vtable
// forced on the host's type, and no toe header pulled into the host's window
// class beyond this one. The shipped Wayland/X11 backends (toe::platform) are
// merely one set of models of this same concept.

#ifndef TOE_PLATFORM_SURFACE_HPP
#define TOE_PLATFORM_SURFACE_HPP

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "toe/core/types.hpp"
#include "toe/input.hpp"

namespace toe::platform {

// --- windowing events (platform-neutral) ----------------------------------
struct CloseRequested {};

struct Resized {
    PixelSize size;
};

struct KeyPressed {
    KeyEvent key;
};

struct TextEntered {
    // UTF-8 committed text (from the IME / compose). Distinct from KeyPressed
    // so the host can route ordinary typing separately from special keys.
    std::string_view utf8;
};

struct Preedit {
    // UTF-8 composition (preedit) string the IME is still assembling, shown
    // inline at the cursor. An empty string clears the preedit (commit/cancel).
    // `cursor` is the caret's cell offset within the string (-1 = at the end).
    // Valid only during the sink call, like TextEntered.
    std::string_view utf8;
    int cursor = -1;
};

// --- pointer events (in pixels, top-left origin) ---------------------------
enum class MouseButton { left, middle, right };

struct MouseDown {
    MouseButton button;
    std::int32_t x, y;   // pixels
    int click_count;     // 1 = single, 2 = double, 3 = triple
    Modifiers mods;
};
struct MouseUp {
    MouseButton button;
    std::int32_t x, y;
    Modifiers mods;
};
struct MouseMove {
    std::int32_t x, y;
    bool button_down;    // true while a button is held (a drag)
};
struct MouseWheel {
    std::int32_t dx, dy; // discrete steps; dy>0 = up
};
struct FocusChanged {
    bool focused; // window gained (true) or lost (false) keyboard focus
};

// A closed sum type: a host's dispatch is an exhaustive std::visit, so a new
// event kind can't be silently ignored. There is no "empty"/"invalid" event.
using Event = std::variant<CloseRequested, Resized, KeyPressed, TextEntered, Preedit, MouseDown,
                           MouseUp, MouseMove, MouseWheel, FocusChanged>;

// The callback shape toe hands to a surface to receive drained events.
using EventSink = std::function<void(const Event &)>;

// ===========================================================================
// concept Surface — the structural contract.
//
// A type S models Surface iff it provides these operations with these shapes.
// Optional operations (title, clipboard, timers, flush) are expressed via
// separate refinement concepts so a minimal host need not implement them.
// ===========================================================================
template <typename S>
concept Surface = requires(S s, const S cs, const EventSink &sink) {
    // Present the back buffer (the host's swap-buffers / present).
    { s.swap() } -> std::same_as<void>;

    // Current drawable size in pixels.
    { cs.pixel_size() } -> std::same_as<PixelSize>;

    // Drain pending native events, invoking `sink` for each translated Event.
    // Non-blocking: returns after dispatching whatever is queued.
    { s.poll_events(sink) } -> std::same_as<void>;

    // The fd the windowing connection multiplexes on (-1 if none), so a host
    // — or toe's own reference loop — can poll() it to block idle.
    { cs.event_fd() } -> std::convertible_to<int>;

    // True once the server/compositor has closed the connection.
    { cs.should_close() } -> std::convertible_to<bool>;
};

// --- optional refinements --------------------------------------------------
// A surface that can carry the window title (OSC 0/2).
template <typename S>
concept TitledSurface = Surface<S> && requires(S s, std::string_view t) {
    { s.set_title(t) } -> std::same_as<void>;
};

// A surface with a CLIPBOARD selection.
template <typename S>
concept ClipboardSurface = Surface<S> && requires(S s, std::string_view t) {
    { s.set_clipboard(t) } -> std::same_as<void>;
    { s.get_clipboard() } -> std::convertible_to<std::string>;
};

// A surface that needs a key-repeat timer fd folded into the host's poll set.
template <typename S>
concept RepeatingSurface = Surface<S> && requires(const S cs) {
    { cs.repeat_fd() } -> std::convertible_to<int>;
};

// A surface that buffers outgoing protocol and must flush before blocking.
template <typename S>
concept FlushableSurface = Surface<S> && requires(S s) {
    { s.flush() } -> std::same_as<void>;
};

// --- uniform accessors -----------------------------------------------------
// Free functions that work on ANY model, filling in sensible no-op defaults
// for the optional refinements. toe's runtime calls these, so it never has to
// know whether a given host implemented the optional bits.

template <Surface S>
inline void title(S &s, std::string_view t) {
    if constexpr (TitledSurface<S>) s.set_title(t);
}
template <Surface S>
inline void clipboard_set(S &s, std::string_view t) {
    if constexpr (ClipboardSurface<S>) s.set_clipboard(t);
}
template <Surface S>
[[nodiscard]] inline std::string clipboard_get(S &s) {
    if constexpr (ClipboardSurface<S>) return s.get_clipboard();
    else return {};
}
template <Surface S>
[[nodiscard]] inline int repeat_fd(const S &s) {
    if constexpr (RepeatingSurface<S>) return s.repeat_fd();
    else return -1;
}
template <Surface S>
inline void flush(S &s) {
    if constexpr (FlushableSurface<S>) s.flush();
}

// ===========================================================================
// AnySurface — optional type-erased adapter for hosts that want RUNTIME
// polymorphism (e.g. choose the backend at runtime, store surfaces in a
// container). Any model of `Surface` can be wrapped; the wrapper itself also
// models `Surface`, so it flows anywhere the concept is accepted.
//
// Hosts that stay fully static never touch this and pay nothing for it.
// ===========================================================================
class AnySurface {
public:
    // Wrap a value model (moved in).
    template <Surface S>
        requires(!std::same_as<std::remove_cvref_t<S>, AnySurface>)
    explicit AnySurface(S &&s)
        : self_{std::make_unique<Model<std::remove_cvref_t<S>>>(std::forward<S>(s))} {}

    // Wrap a heap-pinned model. Backends whose native callbacks capture `this`
    // (Wayland/X11) are not movable, so they live behind a unique_ptr; the
    // adapter forwards concept operations through the pointer.
    template <Surface S>
    explicit AnySurface(std::unique_ptr<S> p)
        : self_{std::make_unique<PtrModel<S>>(std::move(p))} {}

    void swap() { self_->swap(); }
    [[nodiscard]] PixelSize pixel_size() const { return self_->pixel_size(); }
    void poll_events(const EventSink &sink) { self_->poll_events(sink); }
    [[nodiscard]] int event_fd() const { return self_->event_fd(); }
    [[nodiscard]] bool should_close() const { return self_->should_close(); }
    void set_title(std::string_view t) { self_->set_title(t); }
    void set_clipboard(std::string_view t) { self_->set_clipboard(t); }
    [[nodiscard]] std::string get_clipboard() { return self_->get_clipboard(); }
    [[nodiscard]] int repeat_fd() const { return self_->repeat_fd(); }
    void flush() { self_->flush(); }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual void swap() = 0;
        virtual PixelSize pixel_size() const = 0;
        virtual void poll_events(const EventSink &) = 0;
        virtual int event_fd() const = 0;
        virtual bool should_close() const = 0;
        virtual void set_title(std::string_view) = 0;
        virtual void set_clipboard(std::string_view) = 0;
        virtual std::string get_clipboard() = 0;
        virtual int repeat_fd() const = 0;
        virtual void flush() = 0;
    };
    template <Surface S>
    struct Model final : Concept {
        explicit Model(S s) : s_{std::move(s)} {}
        void swap() override { s_.swap(); }
        PixelSize pixel_size() const override { return s_.pixel_size(); }
        void poll_events(const EventSink &sink) override { s_.poll_events(sink); }
        int event_fd() const override { return s_.event_fd(); }
        bool should_close() const override { return s_.should_close(); }
        void set_title(std::string_view t) override { platform::title(s_, t); }
        void set_clipboard(std::string_view t) override { platform::clipboard_set(s_, t); }
        std::string get_clipboard() override { return platform::clipboard_get(s_); }
        int repeat_fd() const override { return platform::repeat_fd(s_); }
        void flush() override { platform::flush(s_); }
        S s_;
    };
    template <Surface S>
    struct PtrModel final : Concept {
        explicit PtrModel(std::unique_ptr<S> p) : p_{std::move(p)} {}
        void swap() override { p_->swap(); }
        PixelSize pixel_size() const override { return p_->pixel_size(); }
        void poll_events(const EventSink &sink) override { p_->poll_events(sink); }
        int event_fd() const override { return p_->event_fd(); }
        bool should_close() const override { return p_->should_close(); }
        void set_title(std::string_view t) override { platform::title(*p_, t); }
        void set_clipboard(std::string_view t) override { platform::clipboard_set(*p_, t); }
        std::string get_clipboard() override { return platform::clipboard_get(*p_); }
        int repeat_fd() const override { return platform::repeat_fd(*p_); }
        void flush() override { platform::flush(*p_); }
        std::unique_ptr<S> p_;
    };
    std::unique_ptr<Concept> self_;
};

static_assert(Surface<AnySurface>, "AnySurface must itself model Surface");

} // namespace toe::platform

#endif // TOE_PLATFORM_SURFACE_HPP
