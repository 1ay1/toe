# toe API Reference

The public surface of toe, grouped by header. All symbols are in namespace
`toe` (or `toe::gfx` / `toe::platform` as noted). For the design behind these
types see [ARCHITECTURE.md](ARCHITECTURE.md); for a working host see
[INTEGRATION.md](INTEGRATION.md).

Everything fallible returns `Result<T> = std::expected<T, Error>` where
`Error{ std::string message; }`. No API throws across a boundary.

---

## `toe/terminal.hpp` — the facade

The main entry point. A `Terminal` is a two-state machine; you only ever touch a
`Session` (the live state).

### `struct Config`

| Field | Type | Default | Meaning |
|-------|------|---------|---------|
| `font_family` | `std::string` | `"monospace"` | family substring; empty → system monospace |
| `font_file` | `std::string` | `""` | explicit font-file path; when set, bypasses discovery (a host on macOS/Windows sets this) |
| `font_pixel_size` | `int` | `18` | glyph size in device pixels |
| `default_fg` / `default_bg` | `Rgb` | light-on-dark | default palette colours |
| `source` | `AdoptFd` | `{}` | the child PTY the host opened — toe never forks (see `pty_source.hpp`) |

### `class Terminal`

```cpp
static Result<Terminal> create(const Config &cfg, PixelSize px);
```
Builds the renderer, resolves the font, and obtains the child per `cfg.source`.
Requires a current GL context (the atlas/renderer allocate GL objects).

```cpp
struct Poll { Session *running; const Exited *exited; };  // exactly one non-null
Poll poll();                    // the sole Running -> Exited transition
bool running() const noexcept;
```

`struct Exited { int code; };`

### `class Session` — the live terminal

Grouped by purpose. All are no-throw unless they return `Result`.

**Frame:**
```cpp
void render(gfx::RenderContext &rc, PixelSize px, bool cursor_on = true, bool blink_on = true);
void resize(PixelSize px);
```

**Input:**
```cpp
void send_key(const KeyEvent &ev);      // encodes & writes to the child
void send_text(std::string_view utf8);  // committed IME/compose text
```

**TEA (advanced / testing):**
```cpp
Cmds update(const Msg &msg);   // pure: mutate model, return effects as data
void run(const Cmds &cmds);    // interpret effects (the runtime)
```

**Child I/O & idle:**
```cpp
bool pump_output();                 // read PTY -> feed model; true if it produced output
bool output_pending() const noexcept;
int  pty_fd() const noexcept;       // poll() this to sleep until the child speaks
std::uint64_t generation() const noexcept;  // monotonic damage counter; render only when it moves
```

**Scrollback & selection:**
```cpp
void scroll(int lines); void scroll_to_bottom();
void select_begin(int vrow, int col, int mode); // mode: 0 char, 1 word, 2 line, 3 block
void select_extend(int vrow, int col);
void select_word(int vrow, int col);   // double-click
void select_line(int vrow, int col);   // triple-click
void select_clear();
bool has_selection() const noexcept;
std::string selected_text() const;
std::string_view link_at(int vrow, int col) const noexcept;  // OSC 8 hyperlink
```

**Mouse (when the app grabs the mouse):**
```cpp
enum class MouseEvent { press, release, motion };
bool wants_mouse() const noexcept;
bool wants_mouse_motion() const noexcept;
bool wants_mouse_drag() const noexcept;
void report_mouse(MouseEvent kind, int button, int col, int row,
                  bool shift, bool alt, bool ctrl, int wheel = 0);
```

**Host-side effects & queries:**
```cpp
std::optional<std::string> take_clipboard_request();  // OSC 52 -> set system clipboard
std::string window_title() const;                     // OSC 0/2
void report_focus(bool focused);
std::uint64_t next_animation_deadline() const noexcept;  // inline-image animation timing
Extent grid_size() const noexcept;
Pos    cursor() const noexcept;
int    cell_width() const noexcept;
int    cell_height() const noexcept;
bool   bracketed_paste() const noexcept;
bool   on_alt_screen() const noexcept;
```

---

## `toe/pty/pty_source.hpp` — boundary 3

```cpp
struct AdoptFd {
    int   master_fd = -1;   // an open PTY master (>= 0) the host opened
    pid_t child     = -1;   // child pid for exit/reaping, or -1 (host manages it)
    bool  owns_fd   = true; // toe close()s the fd on teardown when true
};
```

toe **never creates the process**. The host opens the PTY master by whatever
native means (`forkpty` on Linux/macOS, ConPTY on Windows, an SSH/tmux channel,
a replay fd) and hands it to toe via `Config::source`. argv/`$SHELL`, `TERM`,
and any `pre_exec` child hook are host policy, above this boundary.

---

## `toe/gfx/render_target.hpp` — boundary 2

```cpp
struct Framebuffer { std::uint32_t id = 0; };            // strong newtype; 0 = default FBO
inline constexpr Framebuffer default_framebuffer{0};

class RenderContext {                                     // move-only, non-owning
    static RenderContext adopt_current(Framebuffer target = default_framebuffer) noexcept;
    Framebuffer target() const noexcept;
    RenderContext &retarget(Framebuffer fb) noexcept;
};
```

`adopt_current()` is the host's claim that a GL context is current on this
thread; `Session::render()` consumes the token and binds `target()`.

---

## `toe/gfx/renderer.hpp` — the view (advanced)

Most hosts never touch this directly — `Session` owns a `Renderer`. Exposed
tunable:

```cpp
static void Renderer::set_persistent_mapping(bool enabled) noexcept;
```
Opt out of the GL 4.4 persistent-mapped instance ring (driver workaround).
Call before `Terminal::create`. Replaces the former `TOE_NO_PERSISTENT` env var.

---

## `toe/platform/surface.hpp` — boundary 1 (in `toe::platform`)

Part of **core** (it's the contract).

**Events** — the platform-neutral closed sum a host produces:
```cpp
CloseRequested, Resized{PixelSize}, KeyPressed{KeyEvent}, TextEntered{string_view},
MouseDown{button,x,y,click_count,mods}, MouseUp{...}, MouseMove{x,y,button_down},
MouseWheel{dx,dy}, FocusChanged{focused}
using Event    = std::variant<...>;
using EventSink = std::function<void(const Event&)>;
```

**The concept** and its refinements:
```cpp
template <class S> concept Surface;            // swap, pixel_size, poll_events, event_fd, should_close
template <class S> concept TitledSurface;      // + set_title
template <class S> concept ClipboardSurface;   // + set_clipboard, get_clipboard
template <class S> concept RepeatingSurface;   // + repeat_fd
template <class S> concept FlushableSurface;   // + flush
```

**Uniform accessors** (work on any model; no-op defaults for absent options):
```cpp
platform::title(s, sv);  platform::clipboard_set(s, sv);  platform::clipboard_get(s);
platform::repeat_fd(s);  platform::flush(s);
```

**`class AnySurface`** — type-erased wrapper; constructible from any `Surface`
model (by value or `unique_ptr`), and itself models `Surface`.

---

## `toe/platform/backend.hpp` — the Linux backends (in `toe::platform`)

Only in the optional `toe::platform` target.

```cpp
enum class Backend { automatic, wayland, x11, offscreen };

Result<AnySurface> open_surface(std::string_view title, PixelSize initial,
                                Backend backend = Backend::automatic);
```

`automatic` picks Wayland → X11 → offscreen (honouring `TOE_HEADLESS` for CI).
On success a GL context is current on the calling thread. This is the *only*
toe function that talks to a window system.

---

## `toe/input.hpp` — key events

```cpp
enum class SpecialKey { Enter, Backspace, Tab, Escape, Up, Down, Left, Right,
                        Home, End, PageUp, PageDown, Delete, Insert,
                        F1..F12, KpEnter };
struct TextInput  { std::string utf8; };
struct Modifiers  { bool ctrl, alt, shift; };
struct KeyEvent   { std::variant<TextInput, SpecialKey> key; Modifiers mods; };
```

Construct a `KeyEvent` and hand it to `Session::send_key`; toe encodes the
correct escape sequence (respecting application-cursor / keypad modes).

---

## `toe/core/types.hpp` — foundations

```cpp
template <class Tag> struct Coord;      // strong int newtype
using Row = Coord<...>; using Col = Coord<...>;
struct Extent    { int cols, rows; };   // grid size in cells
struct PixelSize { int w, h; };         // size in device pixels
struct Pos       { int row, col; };
struct Rgb       { std::uint8_t r, g, b; };  Rgb rgb(r,g,b);
struct Error     { std::string message; };
template <class T> using Result = std::expected<T, Error>;
```

---

## `toe/core/tea.hpp` — the architecture (advanced / testing)

```cpp
// Msg — every input, as data:
ChildOutput, ChildExited, Key, Paste, Resized, MouseDown, MouseUp, MouseDrag, MouseWheel, Tick
using Msg = std::variant<...>;

// Cmd — every effect, as data:
WriteChild{bytes}, SetTitle{s}, SetClipboard{s}, ResizePty{extent}, RingBell, Quit
using Cmd  = std::variant<...>;
using Cmds = std::vector<Cmd>;

// The pure reducer over raw child bytes:
namespace term { Cmds feed_output(Model &m, std::string_view bytes); }
```

Use these to test terminal behaviour with zero I/O — see the `tea_test` suite.
