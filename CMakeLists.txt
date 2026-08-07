# SPDX-License-Identifier: LGPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(gvte VERSION 0.1 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-egl wayland-cursor)
pkg_check_modules(XKB REQUIRED IMPORTED_TARGET xkbcommon)

# --- generate xdg-shell protocol from wayland-protocols -------------------
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
pkg_get_variable(WLPROTO_DIR wayland-protocols pkgdatadir)
set(XDG_XML "${WLPROTO_DIR}/stable/xdg-shell/xdg-shell.xml")
set(GEN "${CMAKE_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GEN}")

add_custom_command(
  OUTPUT "${GEN}/xdg-shell-client-protocol.h"
  COMMAND ${WAYLAND_SCANNER} client-header "${XDG_XML}" "${GEN}/xdg-shell-client-protocol.h"
  DEPENDS "${XDG_XML}" VERBATIM)
add_custom_command(
  OUTPUT "${GEN}/xdg-shell-protocol.c"
  COMMAND ${WAYLAND_SCANNER} private-code "${XDG_XML}" "${GEN}/xdg-shell-protocol.c"
  DEPENDS "${XDG_XML}" VERBATIM)

# ---------------------------------------------------------------------------
# libgvte — the GPU terminal library. This is what a host (termite) links.
# ---------------------------------------------------------------------------
add_library(gvte
  src/terminal.cpp
  src/pty/pty.cpp
  src/platform/wayland.cpp
  "${GEN}/xdg-shell-protocol.c"
  "${GEN}/xdg-shell-client-protocol.h"
  src/gfx/font.cpp
  src/gfx/shader.cpp
  src/gfx/palette.cpp
  src/gfx/renderer.cpp
  src/term/screen.cpp)

target_include_directories(gvte PUBLIC include PRIVATE "${GEN}")

target_compile_options(gvte PRIVATE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)

target_link_libraries(gvte PUBLIC
  PkgConfig::EPOXY
  PkgConfig::FREETYPE
  PkgConfig::HARFBUZZ
  PkgConfig::FONTCONFIG
  PkgConfig::EGL
  PkgConfig::WAYLAND
  PkgConfig::XKB
  util)  # forkpty

add_library(gvte::gvte ALIAS gvte)

# ---------------------------------------------------------------------------
# gvte-demo — a minimal host that drives the library (window + event loop).
# ---------------------------------------------------------------------------
add_executable(gvte-demo src/demo.cpp)
target_link_libraries(gvte-demo PRIVATE gvte)

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(parser_test tests/parser_test.cpp)
target_include_directories(parser_test PRIVATE include)
target_compile_options(parser_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME parser COMMAND parser_test)

add_executable(screen_test tests/screen_test.cpp src/term/screen.cpp)
target_include_directories(screen_test PRIVATE include)
target_compile_options(screen_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME screen COMMAND screen_test)

add_executable(render_test tests/render_test.cpp)
target_link_libraries(render_test PRIVATE gvte)
add_test(NAME render COMMAND render_test)
