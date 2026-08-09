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

# --- shader generation: sokol-shdc compiles shaders.glsl -> shaders.glsl.h ----
# The generated header is committed (so a fresh checkout builds without the
# tool), but regenerated here when the .glsl changes and the tool is present.
# We ALWAYS emit both backends (glsl410 for the Linux GLCORE build, metal_macos
# for the Apple build) so the single committed header serves every platform —
# regenerating on one host must not break the other. Pick whichever shdc binary
# matches the build host.
if(APPLE)
  set(_shdc "${PROJECT_SOURCE_DIR}/tools/bin/sokol-shdc-osx-arm64")
else()
  set(_shdc "${PROJECT_SOURCE_DIR}/tools/bin/sokol-shdc-linux")
endif()
set(_slang "glsl410:metal_macos")
if(EXISTS "${_shdc}")
  add_custom_command(
    OUTPUT  "${CMAKE_CURRENT_SOURCE_DIR}/gfx/shaders.glsl.h"
    COMMAND "${_shdc}" --input "${CMAKE_CURRENT_SOURCE_DIR}/gfx/shaders.glsl"
                       --output "${CMAKE_CURRENT_SOURCE_DIR}/gfx/shaders.glsl.h"
                       --slang "${_slang}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gfx/shaders.glsl"
    COMMENT "sokol-shdc: compiling shaders" VERBATIM)
endif()

# The single SOKOL_IMPL TU. On Apple it's Objective-C (Metal); on Linux a .c
# with the GL backend.
if(APPLE)
  set(_sokol_impl gfx/sokol_impl.m)
  set_source_files_properties(gfx/sokol_impl.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
else()
  set(_sokol_impl gfx/sokol_impl.c)
endif()

add_library(toe
  terminal.cpp
  pty/pty.cpp
  pty/child.cpp
  gfx/font.cpp
  gfx/face.cpp
  gfx/face_color.cpp
  gfx/font_discovery.cpp
  gfx/png.cpp
  gfx/stb_impl.cpp
  gfx/palette.cpp
  gfx/renderer.cpp
  gfx/gpu.cpp
  ${_sokol_impl}
  "${CMAKE_CURRENT_SOURCE_DIR}/gfx/shaders.glsl.h"
  input/keymap.cpp
  term/screen.cpp
  term/update.cpp
  term/sbquery.cpp
  term/redact.cpp
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

# Renderer sources include the sokol header; point them at include/sokol and the
# generated shader header dir.
target_include_directories(toe PRIVATE
  "${PROJECT_SOURCE_DIR}/include/sokol"
  "${CMAKE_CURRENT_SOURCE_DIR}/gfx")

# PNG stays a private decoder dep. On Apple, sokol's Metal backend needs the
# Metal + QuartzCore frameworks (the host still owns the CAMetalLayer/device).
target_link_libraries(toe PRIVATE PkgConfig::PNG)
if(APPLE)
  target_link_libraries(toe PRIVATE
    "-framework Metal" "-framework MetalKit" "-framework QuartzCore" "-framework Foundation")
else()
  # Linux: sokol GLCORE is fully native — it includes <GL/gl.h> with
  # GL_GLEXT_PROTOTYPES and calls the gl* entry points directly, so the real
  # libGL (core + GLX-dispatched extensions) is linked at link time. No epoxy:
  # sokol owns GL, and modern libGL exports every symbol sokol references
  # (glDrawElementsInstanced, glDispatchCompute, glTexStorage2D, ...).
  find_package(OpenGL REQUIRED)
  target_link_libraries(toe PRIVATE OpenGL::GL)
endif()

target_compile_features(toe PUBLIC cxx_std_23)
target_compile_options(toe PRIVATE ${TOE_WARNINGS})

# stb_truetype is vendored third-party C: don't hold it to our strict warnings.
set_source_files_properties(gfx/stb_impl.cpp PROPERTIES
  COMPILE_OPTIONS "-w")

# NOTE: toe has NO window-system target. The Wayland/X11/offscreen Surface
# backends (formerly toe::platform) now live entirely in the `hand` frontend,
# which owns every windowing dependency (wayland, xcb, egl, xkbcommon). toe
# renders into a GL framebuffer the host supplies via a RenderContext token.
