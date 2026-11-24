# SPDX-License-Identifier: LGPL-2.0-or-later
#
# Every test links only toe::toe and runs headless: no window, no GL context,
# no PTY, no fonts. That is a property of the library, not of the tests — the
# terminal is a pure function of (state, bytes), so asserting on it needs
# nothing but the model.

add_executable(parser_test parser_test.cpp)
target_link_libraries(parser_test PRIVATE toe::toe)
add_test(NAME parser COMMAND parser_test)

add_executable(screen_test screen_test.cpp)
target_link_libraries(screen_test PRIVATE toe::toe)
add_test(NAME screen COMMAND screen_test)

add_executable(widechar_test widechar_test.cpp)
target_link_libraries(widechar_test PRIVATE toe::toe)
add_test(NAME widechar COMMAND widechar_test)

add_executable(utf8_test utf8_test.cpp)
target_link_libraries(utf8_test PRIVATE toe::toe)
add_test(NAME utf8 COMMAND utf8_test)

add_executable(caps_test caps_test.cpp)
target_link_libraries(caps_test PRIVATE toe::toe)
add_test(NAME caps COMMAND caps_test)

add_executable(sgr_test sgr_test.cpp)
target_link_libraries(sgr_test PRIVATE toe::toe)
add_test(NAME sgr COMMAND sgr_test)

add_executable(graphics_test graphics_test.cpp)
target_link_libraries(graphics_test PRIVATE toe::toe)
add_test(NAME graphics COMMAND graphics_test)

add_executable(sbquery_test sbquery_test.cpp)
target_link_libraries(sbquery_test PRIVATE toe::toe)
add_test(NAME sbquery COMMAND sbquery_test)

add_executable(region_test region_test.cpp)
target_link_libraries(region_test PRIVATE toe::toe)
add_test(NAME region COMMAND region_test)

# Robustness: no byte stream may crash, hang or corrupt the model. Most
# valuable under -DTOE_SANITIZE=ON, where a bad cell write becomes a failure.
add_executable(fuzz_test fuzz_test.cpp)
target_link_libraries(fuzz_test PRIVATE toe::toe)
add_test(NAME fuzz COMMAND fuzz_test)
set_tests_properties(fuzz PROPERTIES TIMEOUT 300)

# The damage keys (row_version, history_epoch) never hold still over a change:
# every cache built on them (hand's row memo, the minimap) relies on it.
add_executable(damage_test damage_test.cpp)
target_link_libraries(damage_test PRIVATE toe::toe)
add_test(NAME damage COMMAND damage_test)

add_executable(resize_test resize_test.cpp)
target_link_libraries(resize_test PRIVATE toe::toe)
add_test(NAME resize COMMAND resize_test)

# A resize is a pure function: thousands of random screens x random resizes,
# every observable bit folded into one digest checked against a golden from
# the reference reflow. Speed work on reflow must leave it unchanged.
add_executable(reflow_oracle_test reflow_oracle_test.cpp)
target_link_libraries(reflow_oracle_test PRIVATE toe::toe)
add_test(NAME reflow_oracle COMMAND reflow_oracle_test)

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

# simd_scan.hpp is header-only; include the tree directly.
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
add_executable(yes_bench EXCLUDE_FROM_ALL yes_bench.cpp)
target_link_libraries(yes_bench PRIVATE toe::toe)
add_executable(resize_bench EXCLUDE_FROM_ALL resize_bench.cpp)
target_link_libraries(resize_bench PRIVATE toe::toe)
add_executable(prof_bench EXCLUDE_FROM_ALL prof_bench.cpp)
target_link_libraries(prof_bench PRIVATE toe::toe)
