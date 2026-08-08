# SPDX-License-Identifier: LGPL-2.0-or-later
#
# Two libraries, one strict rule: toe::core must never link or include a
# window-system dependency. The compiler enforces the API contract (the Surface
# concept, the RenderContext token); CMake enforces the DEPENDENCY contract.

find_package(PkgConfig REQUIRED)

# ===========================================================================
# toe::core — the terminal engine + the abstract Surface contract.
#
# Every dependency here is either a content decoder/shaper (freetype, harfbuzz,
# fontconfig, png) or the GL *loader* (epoxy). NONE is a windowing system.
# epoxy is PRIVATE: no public header names a GL type, so consumers do not
# inherit GL headers — they only need a current GL context at render time,
# proven by gfx::RenderContext.
# ===========================================================================
pkg_check_modules(FREETYPE   REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ   REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(PNG        REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(EPOXY      REQUIRED IMPORTED_TARGET epoxy)

add_library(toe-core
  terminal.cpp
  pty/pty.cpp
  gfx/font.cpp
  gfx/shader.cpp
  gfx/palette.cpp
  gfx/renderer.cpp
  input/keymap.cpp
  term/screen.cpp
  term/update.cpp
  term/graphics.cpp)
add_library(toe::core ALIAS toe-core)

set_target_properties(toe-core PROPERTIES
  VERSION     ${PROJECT_VERSION}
  SOVERSION   ${PROJECT_VERSION_MAJOR}
  EXPORT_NAME core
  OUTPUT_NAME toe)

# Public include dir: source tree during build, <prefix>/include after install.
target_include_directories(toe-core PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# All GL/decoder deps are PRIVATE — implementation detail, not part of the ABI.
target_link_libraries(toe-core PRIVATE
  PkgConfig::FREETYPE PkgConfig::HARFBUZZ PkgConfig::FONTCONFIG
  PkgConfig::PNG PkgConfig::EPOXY)

target_compile_features(toe-core PUBLIC cxx_std_23)
target_compile_options(toe-core PRIVATE ${TOE_WARNINGS})

# ===========================================================================
# toe::platform — OPTIONAL Linux Surface backends (Wayland / X11 / offscreen).
#
# This is the ONLY target that links a window system. A host that brings its
# own window simply does not build/link this; it models toe::platform::Surface
# (the concept, which lives in core) with its own type.
# ===========================================================================
if(TOE_BUILD_PLATFORM)
  pkg_check_modules(EGL     REQUIRED IMPORTED_TARGET egl)
  pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-egl wayland-cursor)
  pkg_check_modules(XKB     REQUIRED IMPORTED_TARGET xkbcommon)
  pkg_check_modules(X11     REQUIRED IMPORTED_TARGET x11 x11-xcb xcb xkbcommon-x11)

  # Generate the xdg-shell protocol glue from wayland-protocols.
  find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
  pkg_get_variable(WLPROTO_DIR wayland-protocols pkgdatadir)
  set(XDG_XML "${WLPROTO_DIR}/stable/xdg-shell/xdg-shell.xml")
  set(GEN "${CMAKE_CURRENT_BINARY_DIR}/gen")
  file(MAKE_DIRECTORY "${GEN}")
  add_custom_command(
    OUTPUT "${GEN}/xdg-shell-client-protocol.h"
    COMMAND ${WAYLAND_SCANNER} client-header "${XDG_XML}" "${GEN}/xdg-shell-client-protocol.h"
    DEPENDS "${XDG_XML}" VERBATIM)
  add_custom_command(
    OUTPUT "${GEN}/xdg-shell-protocol.c"
    COMMAND ${WAYLAND_SCANNER} private-code "${XDG_XML}" "${GEN}/xdg-shell-protocol.c"
    DEPENDS "${XDG_XML}" VERBATIM)

  add_library(toe-platform
    platform/surface.cpp
    platform/wayland.cpp
    platform/x11.cpp
    platform/offscreen.cpp
    "${GEN}/xdg-shell-client-protocol.h"
    "${GEN}/xdg-shell-protocol.c")
  add_library(toe::platform ALIAS toe-platform)

  set_target_properties(toe-platform PROPERTIES
    VERSION     ${PROJECT_VERSION}
    SOVERSION   ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME platform
    OUTPUT_NAME toe-platform)

  target_include_directories(toe-platform
    PUBLIC
      $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE "${GEN}")

  # platform depends on core (uses the concept/types); windowing deps PRIVATE.
  target_link_libraries(toe-platform
    PUBLIC  toe::core
    PRIVATE PkgConfig::EGL PkgConfig::WAYLAND PkgConfig::XKB PkgConfig::X11 PkgConfig::EPOXY)

  target_compile_features(toe-platform PUBLIC cxx_std_23)
  target_compile_options(toe-platform PRIVATE ${TOE_WARNINGS})
endif()
