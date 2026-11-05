# SPDX-License-Identifier: LGPL-2.0-or-later
#
# toe — the terminal library: VT parse · screen model · scrollback · shell
# integration · key encoding. It turns bytes into a grid of cells.
#
# One strict rule: toe links NOTHING. No window system, no GPU, no font stack,
# no image codec, no OS process machinery. Every dependency here is the C++
# standard library, and CMake keeps it that way — if a find_package or
# target_link_libraries ever appears below, the library has stopped being a
# terminal library.
#
# Drawing the grid, owning the PTY and running an event loop are the host's
# job. toe hands it a model to read.

add_library(toe
  term/screen.cpp
  term/update.cpp
  term/palette.cpp
  term/sbquery.cpp
  term/redact.cpp
  term/graphics.cpp
  input/keymap.cpp)

# Canonical namespaced alias, plus a back-compat alias for consumers that still
# say toe::core (there is only one target — the engine IS toe).
add_library(toe::toe  ALIAS toe)
add_library(toe::core ALIAS toe)

set_target_properties(toe PROPERTIES
  VERSION     ${PROJECT_VERSION}
  SOVERSION   ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON)

# Headers: source tree during build, <prefix>/include after install.
target_include_directories(toe PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(toe PUBLIC cxx_std_23)
target_compile_options(toe PRIVATE ${TOE_WARNINGS})

# Optional ISA baseline for the SIMD VT scanner (empty by default — see the
# root CMakeLists for why widening to v3 measured no win). PUBLIC because
# simd_scan.hpp is header-only and compiles into the consumer.
if(TOE_ARCH_FLAGS)
  target_compile_options(toe PUBLIC ${TOE_ARCH_FLAGS})
endif()
