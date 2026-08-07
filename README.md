# Termite GPU — a from-scratch, GPU-accelerated terminal emulator

This is a **ground-up rewrite** of a VT terminal emulator with no VTE and no
GTK. It owns the whole stack: PTY, the escape-sequence parser, the grid model,
font shaping, and an OpenGL glyph-atlas renderer.

## Stack

- **C++23** (GCC 16 / Clang 18+)
- **SDL3** — window, input, GL context, clipboard (Wayland + X11)
- **OpenGL 3.3 core** — instanced textured-quad glyph rendering
- **FreeType + HarfBuzz + Fontconfig** — glyph rasterization and text shaping
- glibc **forkpty** — child shell / PTY

## Design ("type-theoretic modern C++")

- Strong coordinate types (`Row`, `Col`, `Extent`) so grid math can't mix axes.
- `std::expected<T, Error>` for every fallible boundary (PTY, GL, fonts).
- `enum class` + `std::variant` for the parser state machine and cell state.
- RAII handle wrappers over every C resource; `std::span` / `std::string_view`
  over raw pointer+length pairs.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/termite-gpu
```

## Status

Early. Built in runnable vertical slices — see `TODO` / git history.
