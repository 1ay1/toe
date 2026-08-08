# SPDX-License-Identifier: LGPL-2.0-or-later
#
# One library, one strict rule: `toe` must never link or include a window-system
# dependency. The compiler enforces the API contract (the RenderContext token);
# CMake enforces the DEPENDENCY contract.

find_package(PkgConfig REQUIRED)

# ===========================================================================
# toe — the terminal engine: pty · vt · screen · update · graphics · renderer ·
# keymap. It turns bytes into pixels (into a GL framebuffer the host supplies
# via a RenderContext token) and knows nothing about any window system.
#
# Fonts are handled by vendored stb_truetype (include/stb) + a small in-tree
# GSUB shaper (opentype.hpp) — no FreeType, HarfBuzz, or Fontconfig, so no
# font-cache threads and a far smaller dependency surface. Every remaining dep
# is a content decoder (png) or the GL loader (epoxy). epoxy is PRIVATE: no
# public header names a GL type.
# ===========================================================================
pkg_check_modules(PNG        REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(EPOXY      REQUIRED IMPORTED_TARGET epoxy)

add_library(toe
  terminal.cpp
  pty/pty.cpp
  pty/child.cpp
  gfx/font.cpp
  gfx/face.cpp
  gfx/stb_impl.cpp
  gfx/shader.cpp
  gfx/palette.cpp
  gfx/renderer.cpp
  input/keymap.cpp
  term/screen.cpp
  term/update.cpp
  term/graphics.cpp)

# Canonical namespaced alias, plus a back-compat alias for consumers that still
# say toe::core (there is only one target now — the engine IS toe).
add_library(toe::toe  ALIAS toe)
add_library(toe::core ALIAS toe)

set_target_properties(toe PROPERTIES
  VERSION     ${PROJECT_VERSION}
  SOVERSION   ${PROJECT_VERSION_MAJOR}
  EXPORT_NAME toe
  OUTPUT_NAME toe)

# Public include dir: source tree during build, <prefix>/include after install.
target_include_directories(toe PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# All GL/decoder deps are PRIVATE — implementation detail, not part of the ABI.
target_link_libraries(toe PRIVATE PkgConfig::PNG PkgConfig::EPOXY)

target_compile_features(toe PUBLIC cxx_std_23)
target_compile_options(toe PRIVATE ${TOE_WARNINGS})

# stb_truetype is vendored third-party C: don't hold it to our strict warnings.
set_source_files_properties(gfx/stb_impl.cpp PROPERTIES
  COMPILE_OPTIONS "-w")

# NOTE: toe has NO window-system target. The Wayland/X11/offscreen Surface
# backends (formerly toe::platform) now live entirely in the `hand` frontend,
# which owns every windowing dependency (wayland, xcb, egl, xkbcommon). toe
# renders into a GL framebuffer the host supplies via a RenderContext token.
