# SPDX-License-Identifier: LGPL-2.0-or-later
#
# Pure logic tests link only gvte::core (no window system needed). Tests that
# need a live GL context to exercise the renderer link gvte::platform too and
# run against the offscreen EGL backend.

# --- pure core tests (no GL, no windowing) --------------------------------
add_executable(parser_test parser_test.cpp)
target_link_libraries(parser_test PRIVATE gvte::core)
add_test(NAME parser COMMAND parser_test)

add_executable(screen_test screen_test.cpp)
target_link_libraries(screen_test PRIVATE gvte::core)
add_test(NAME screen COMMAND screen_test)

add_executable(tea_test tea_test.cpp)
target_link_libraries(tea_test PRIVATE gvte::core)
add_test(NAME tea COMMAND tea_test)

add_executable(keymap_test keymap_test.cpp)
target_link_libraries(keymap_test PRIVATE gvte::core)
add_test(NAME keymap COMMAND keymap_test)

# --- GL/renderer tests (need a context; use the offscreen platform backend) -
if(GVTE_BUILD_PLATFORM)
  add_executable(render_test render_test.cpp)
  target_link_libraries(render_test PRIVATE gvte::core gvte::platform)
  add_test(NAME render COMMAND render_test)

  add_executable(cache_test cache_test.cpp)
  target_link_libraries(cache_test PRIVATE gvte::core gvte::platform)
  add_test(NAME cache COMMAND cache_test)

  # Force the offscreen EGL backend so these run headless (CI, no compositor).
  set_tests_properties(render cache PROPERTIES
    ENVIRONMENT "GVTE_HEADLESS=1")
endif()
