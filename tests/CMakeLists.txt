# SPDX-License-Identifier: LGPL-2.0-or-later
#
# Pure logic tests link only toe::toe (no window system needed). The renderer
# tests that need a live GL context (render_test, cache_test) moved to the
# `hand` frontend, which owns the offscreen GL backend — toe itself no longer
# links any window system.

# --- pure core tests (no GL, no windowing) --------------------------------
add_executable(parser_test parser_test.cpp)
target_link_libraries(parser_test PRIVATE toe::toe)
add_test(NAME parser COMMAND parser_test)

add_executable(screen_test screen_test.cpp)
target_link_libraries(screen_test PRIVATE toe::toe)
add_test(NAME screen COMMAND screen_test)

add_executable(tea_test tea_test.cpp)
target_link_libraries(tea_test PRIVATE toe::toe)
add_test(NAME tea COMMAND tea_test)

add_executable(keymap_test keymap_test.cpp)
target_link_libraries(keymap_test PRIVATE toe::toe)
add_test(NAME keymap COMMAND keymap_test)
