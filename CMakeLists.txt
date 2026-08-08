# SPDX-License-Identifier: LGPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(toe VERSION 0.1.0 LANGUAGES CXX C)

# ---------------------------------------------------------------------------
# toe — a GPU terminal engine, as a *layered* library.
#
#   toe::core       the whole terminal (pty · vt · screen · update · graphics ·
#                    renderer · keymap) + the abstract `Surface` INTERFACE.
#                    Knows nothing about Wayland/X11/EGL. This is the library.
#
#   toe::platform   OPTIONAL, batteries-included Surface backends for Linux
#                    (Wayland/X11/offscreen) + open_surface(). A host that
#                    brings its own window (Qt/GLFW/SDL/Win32/macOS) simply
#                    does NOT link this and implements toe::Surface itself.
#
# The dependency arrow only ever points core <- platform <- host. core never
# refers to platform; platform is just one implementation of core's interface.
#
# Toggles (all give the host full control):
#   TOE_BUILD_PLATFORM   build the Linux Surface backends        (default ON)
#   TOE_BUILD_DEMO       build toe-demo (needs platform)        (default ON)
#   TOE_BUILD_TESTS      build the test suite                    (default ON when top-level)
#   TOE_INSTALL          emit install + find_package(toe) rules (default ON when top-level)
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Are we the top-level project, or add_subdirectory()'d by a host (hand)?
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_toe_top ON)
else()
  set(_toe_top OFF)
endif()

option(TOE_BUILD_PLATFORM "Build the Linux Wayland/X11 Surface backends" ON)
option(TOE_BUILD_DEMO     "Build the toe-demo host (implies TOE_BUILD_PLATFORM)" ${_toe_top})
option(TOE_BUILD_TESTS    "Build the toe test suite" ${_toe_top})
option(TOE_INSTALL        "Emit install & export (find_package) rules" ${_toe_top})

if(TOE_BUILD_DEMO AND NOT TOE_BUILD_PLATFORM)
  message(FATAL_ERROR "TOE_BUILD_DEMO requires TOE_BUILD_PLATFORM=ON")
endif()

if(NOT CMAKE_BUILD_TYPE AND _toe_top)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(GNUInstallDirs)

# Warning flags applied to toe's own TUs (not propagated to consumers).
set(TOE_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)

add_subdirectory(src)

if(TOE_BUILD_DEMO)
  add_executable(toe-demo src/demo.cpp)
  target_link_libraries(toe-demo PRIVATE toe::core toe::platform)
endif()

if(TOE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(TOE_INSTALL)
  include(cmake/install.cmake)
endif()
