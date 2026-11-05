# SPDX-License-Identifier: LGPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(toe VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# toe — a terminal LIBRARY.
#
#   toe::toe   the terminal itself: vt · screen · scrollback · shell
#              integration · keymap. It turns bytes into a grid of cells, and
#              links nothing at all (standard library only).
#
# toe does not draw, does not own a PTY, and does not run an event loop. A host
# (e.g. hand) owns the window, the GPU, the font stack, the child process and
# the loop; it feeds toe bytes and reads the grid back out.
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

# --- sanitizers --------------------------------------------------------------
# A terminal parses bytes from an untrusted child, so out-of-bounds cell writes
# and signed overflow are security bugs, not just crashes. TOE_SANITIZE=ON
# turns them into immediate failures; tests/fuzz_test.cpp exists to be run this
# way. Applied to everything (library and tests) so the whole path is covered.
option(TOE_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(TOE_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# --- optimisation policy -----------------------------------------------------
# A terminal is a byte pump: the VT scanner, the UTF-8 decoder and the cell
# writes are the whole program. Both knobs below were chosen by MEASUREMENT
# (tests/throughput_bench.cpp, best-of-7 to see past ~10% run-to-run noise),
# not by assumption.
#
# TOE_ARCH: the ISA baseline. Raising it to x86-64-v3 widens the SIMD scanner
# from 16- to 32-byte compares -- and measured NO reliable win: real VT runs
# between control bytes are short (a shell line, an SGR-delimited token), so the
# wider window rarely fills, while the AVX2 path slightly hurt escape-heavy
# workloads. The default therefore stays at the portable x86-64 baseline, which
# also means the binary runs on any x86-64 CPU. Set -DTOE_ARCH=x86-64-v3 or
# =native if you are building only for your own machine and want to re-measure.
if(NOT DEFINED TOE_ARCH)
  set(TOE_ARCH "" CACHE STRING "ISA baseline (-march); empty | x86-64-v3 | native")
endif()

include(CheckCXXCompilerFlag)
set(TOE_ARCH_FLAGS "")
if(NOT MSVC AND NOT TOE_ARCH STREQUAL "")
  check_cxx_compiler_flag("-march=${TOE_ARCH}" TOE_HAVE_ARCH_FLAG)
  if(TOE_HAVE_ARCH_FLAG)
    set(TOE_ARCH_FLAGS -march=${TOE_ARCH})
  endif()
endif()

# TOE_LTO: cross-TU inlining. The parser, the screen and the row ring live in
# different TUs, so the hot loop is split across translation units that -O3
# alone cannot inline through. This is the change that actually paid: measured
# +5% SGR, +9% CJK, +8% cursor-addressing throughput. Costs link time only.
option(TOE_LTO "Link-time optimisation for Release builds" ON)
if(TOE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _toe_ipo OUTPUT _toe_ipo_msg)
  if(_toe_ipo)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  endif()
endif()

add_subdirectory(src)

if(TOE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(TOE_INSTALL)
  include(cmake/install.cmake)
endif()
