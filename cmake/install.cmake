# SPDX-License-Identifier: LGPL-2.0-or-later
#
# Install + export rules: make toe consumable via find_package(toe).
#
#   find_package(toe CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE toe::toe)
#
# toe exports a single target, toe::toe (the engine). toe::core remains as a
# back-compat alias. The window system lives in the host (e.g. hand), never toe.

include(CMakePackageConfigHelpers)

set(TOE_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/toe"
    CACHE STRING "Install location for toe's CMake package files")

# --- headers ---------------------------------------------------------------
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/toe"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# --- targets ---------------------------------------------------------------
set(_toe_targets toe)
install(TARGETS toe
        EXPORT toeTargets
        LIBRARY  DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE  DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME  DESTINATION "${CMAKE_INSTALL_BINDIR}")


# Export the target set (namespaced toe::) for the build & install trees.
install(EXPORT toeTargets
        FILE toeTargets.cmake
        NAMESPACE toe::
        DESTINATION "${TOE_INSTALL_CMAKEDIR}")

# --- package config --------------------------------------------------------
configure_package_config_file(
  "${PROJECT_SOURCE_DIR}/cmake/toeConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/toeConfig.cmake"
  INSTALL_DESTINATION "${TOE_INSTALL_CMAKEDIR}")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/toeConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/toeConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/toeConfigVersion.cmake"
  DESTINATION "${TOE_INSTALL_CMAKEDIR}")

# --- pkg-config ------------------------------------------------------------
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/toe.pc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/toe.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/toe.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
