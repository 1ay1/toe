# SPDX-License-Identifier: LGPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(toe VERSION 0.1.0 LANGUAGES CXX C)

# ---------------------------------------------------------------------------
# toe — a GPU terminal ENGINE.
#
#   toe::core   the whole terminal: pty · vt · screen · update · graphics ·
#               renderer · keymap. It turns bytes into pixels (into a GL
#               framebuffer the host provides via a RenderContext token) and
#               knows NOTHING about any window system. This is the library.
#
# toe has no window-system dependency at all. A host (hand, or Qt/GLFW/SDL/
# Win32/macOS) owns the window, input and GL-context creation, and drives
# toe::Terminal. The old toe::platform Linux backends now live in `hand`.
#
# Toggles:
#   TOE_BUILD_TESTS   build the test suite         (default ON when top-level)
#   TOE_INSTALL       emit install/find_package     (default ON when top-level)
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

option(TOE_BUILD_TESTS    "Build the toe test suite" ${_toe_top})
option(TOE_INSTALL        "Emit install & export (find_package) rules" ${_toe_top})

if(NOT CMAKE_BUILD_TYPE AND _toe_top)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(GNUInstallDirs)

# Warning flags applied to toe's own TUs (not propagated to consumers).
set(TOE_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)

add_subdirectory(src)

if(TOE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(TOE_INSTALL)
  include(cmake/install.cmake)
endif()
