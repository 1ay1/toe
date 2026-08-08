# SPDX-License-Identifier: LGPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(gvte VERSION 0.1.0 LANGUAGES CXX C)

# ---------------------------------------------------------------------------
# gvte — a GPU terminal engine, as a *layered* library.
#
#   gvte::core       the whole terminal (pty · vt · screen · update · graphics ·
#                    renderer · keymap) + the abstract `Surface` INTERFACE.
#                    Knows nothing about Wayland/X11/EGL. This is the library.
#
#   gvte::platform   OPTIONAL, batteries-included Surface backends for Linux
#                    (Wayland/X11/offscreen) + open_surface(). A host that
#                    brings its own window (Qt/GLFW/SDL/Win32/macOS) simply
#                    does NOT link this and implements gvte::Surface itself.
#
# The dependency arrow only ever points core <- platform <- host. core never
# refers to platform; platform is just one implementation of core's interface.
#
# Toggles (all give the host full control):
#   GVTE_BUILD_PLATFORM   build the Linux Surface backends        (default ON)
#   GVTE_BUILD_DEMO       build gvte-demo (needs platform)        (default ON)
#   GVTE_BUILD_TESTS      build the test suite                    (default ON when top-level)
#   GVTE_INSTALL          emit install + find_package(gvte) rules (default ON when top-level)
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Are we the top-level project, or add_subdirectory()'d by a host (hand)?
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_gvte_top ON)
else()
  set(_gvte_top OFF)
endif()

option(GVTE_BUILD_PLATFORM "Build the Linux Wayland/X11 Surface backends" ON)
option(GVTE_BUILD_DEMO     "Build the gvte-demo host (implies GVTE_BUILD_PLATFORM)" ${_gvte_top})
option(GVTE_BUILD_TESTS    "Build the gvte test suite" ${_gvte_top})
option(GVTE_INSTALL        "Emit install & export (find_package) rules" ${_gvte_top})

if(GVTE_BUILD_DEMO AND NOT GVTE_BUILD_PLATFORM)
  message(FATAL_ERROR "GVTE_BUILD_DEMO requires GVTE_BUILD_PLATFORM=ON")
endif()

if(NOT CMAKE_BUILD_TYPE AND _gvte_top)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

include(GNUInstallDirs)

# Warning flags applied to gvte's own TUs (not propagated to consumers).
set(GVTE_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)

add_subdirectory(src)

if(GVTE_BUILD_DEMO)
  add_executable(gvte-demo src/demo.cpp)
  target_link_libraries(gvte-demo PRIVATE gvte::core gvte::platform)
endif()

if(GVTE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(GVTE_INSTALL)
  include(cmake/install.cmake)
endif()
