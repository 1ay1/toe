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

add_executable(widechar_test widechar_test.cpp)
target_link_libraries(widechar_test PRIVATE toe::toe)
add_test(NAME widechar COMMAND widechar_test)

add_executable(resize_test resize_test.cpp)
target_link_libraries(resize_test PRIVATE toe::toe)
add_test(NAME resize COMMAND resize_test)

add_executable(scrollback_test scrollback_test.cpp)
target_link_libraries(scrollback_test PRIVATE toe::toe)
add_test(NAME scrollback COMMAND scrollback_test)

add_executable(tea_test tea_test.cpp)
target_link_libraries(tea_test PRIVATE toe::toe)
add_test(NAME tea COMMAND tea_test)

add_executable(command_log_test command_log_test.cpp)
target_link_libraries(command_log_test PRIVATE toe::toe)
add_test(NAME command_log COMMAND command_log_test)

add_executable(redact_test redact_test.cpp)
target_link_libraries(redact_test PRIVATE toe::toe)
add_test(NAME redact COMMAND redact_test)

add_executable(keymap_test keymap_test.cpp)
target_link_libraries(keymap_test PRIVATE toe::toe)
add_test(NAME keymap COMMAND keymap_test)

# Procedural TUI glyphs (box drawing / blocks / quadrants / braille). Header-
# only, no lib link needed.
add_executable(box_glyphs_test box_glyphs_test.cpp)
target_include_directories(box_glyphs_test PRIVATE "${PROJECT_SOURCE_DIR}/include")
add_test(NAME box_glyphs COMMAND box_glyphs_test)

# Universal Unicode coverage: FaceStack + FontDiscovery lazy fallback. Uses the
# system's fonts, so it may SKIP (exit 77) on a machine with no usable primary.
add_executable(coverage_test coverage_test.cpp)
target_link_libraries(coverage_test PRIVATE toe::toe)
add_test(NAME coverage COMMAND coverage_test)
# Both font tests exit 77 when the machine has no usable fonts. Without this
# property CTest reports that as a FAILURE rather than a skip.
set_tests_properties(coverage PROPERTIES SKIP_RETURN_CODE 77)

# Per-script fallback resolution: asserts FontDiscovery finds a real system font
# for Latin/Greek/Cyrillic/CJK/Hangul/box-drawing/symbols on THIS platform. This
# is what catches a port whose font roots or priority list are wrong (e.g. the
# POSIX-only roots that made every fallback glyph render as tofu on Windows).
# SKIPs (77) when no fonts are discoverable at all.
add_executable(font_fallback_test font_fallback_test.cpp)
target_link_libraries(font_fallback_test PRIVATE toe::toe)
add_test(NAME font_fallback COMMAND font_fallback_test)
set_tests_properties(font_fallback PROPERTIES SKIP_RETURN_CODE 77)

# Emoji must render in COLOUR, not as flat monochrome outlines. Guards both
# colour formats (CBDT/CBLC bitmaps on Linux, COLR/CPAL layered vector on
# Windows) and the discovery rule that prefers a colour font for emoji
# codepoints a monochrome symbol font also covers.
add_executable(emoji_color_test emoji_color_test.cpp)
target_link_libraries(emoji_color_test PRIVATE toe::toe)
add_test(NAME emoji_color COMMAND emoji_color_test)
set_tests_properties(emoji_color PROPERTIES SKIP_RETURN_CODE 77)

# The SIMD printable-ASCII scanner must agree with the scalar reference on every
# input. Header-only, so it compiles against whatever ISA the build selected --
# run it after changing TOE_ARCH. (An AVX2 bug caught here was returning "no
# control byte found" for ESC, which would have corrupted every escape sequence.)
add_executable(simd_scan_test simd_scan_test.cpp)
target_include_directories(simd_scan_test PRIVATE "${PROJECT_SOURCE_DIR}/include")
add_test(NAME simd_scan COMMAND simd_scan_test)

# Throughput benchmarks. NOT tests (they assert nothing) -- built so a change
# can be measured rather than guessed at. Best-of-N internally, because
# run-to-run noise on a laptop exceeds most real wins.
add_executable(throughput_bench EXCLUDE_FROM_ALL throughput_bench.cpp)
target_link_libraries(throughput_bench PRIVATE toe::toe)
add_executable(scroll_bench EXCLUDE_FROM_ALL scroll_bench.cpp)
target_link_libraries(scroll_bench PRIVATE toe::toe)
