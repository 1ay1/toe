# SPDX-License-Identifier: LGPL-2.0-or-later
#
# Install + export rules: make toe consumable via find_package(toe).
#
#   find_package(toe CONFIG REQUIRED COMPONENTS Core)            # core only
#   find_package(toe CONFIG REQUIRED COMPONENTS Core Platform)   # + backends
#   target_link_libraries(app PRIVATE toe::core [toe::platform])
#
# Platform is a COMPONENT: a consumer that brought its own window requires only
# Core and never drags in the Linux windowing stack.

include(CMakePackageConfigHelpers)

set(TOE_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/toe"
    CACHE STRING "Install location for toe's CMake package files")

# --- headers ---------------------------------------------------------------
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/toe"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# --- targets ---------------------------------------------------------------
set(_toe_targets toe-core)
install(TARGETS toe-core
        EXPORT toeTargets
        LIBRARY  DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE  DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME  DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(TOE_BUILD_PLATFORM)
  list(APPEND _toe_targets toe-platform)
  install(TARGETS toe-platform
          EXPORT toeTargets
          LIBRARY  DESTINATION "${CMAKE_INSTALL_LIBDIR}"
          ARCHIVE  DESTINATION "${CMAKE_INSTALL_LIBDIR}"
          RUNTIME  DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

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
