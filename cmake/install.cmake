# SPDX-License-Identifier: LGPL-2.0-or-later
#
# Install + export rules: make gvte consumable via find_package(gvte).
#
#   find_package(gvte CONFIG REQUIRED COMPONENTS Core)            # core only
#   find_package(gvte CONFIG REQUIRED COMPONENTS Core Platform)   # + backends
#   target_link_libraries(app PRIVATE gvte::core [gvte::platform])
#
# Platform is a COMPONENT: a consumer that brought its own window requires only
# Core and never drags in the Linux windowing stack.

include(CMakePackageConfigHelpers)

set(GVTE_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/gvte"
    CACHE STRING "Install location for gvte's CMake package files")

# --- headers ---------------------------------------------------------------
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/gvte"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# --- targets ---------------------------------------------------------------
set(_gvte_targets gvte-core)
install(TARGETS gvte-core
        EXPORT gvteTargets
        LIBRARY  DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE  DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME  DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(GVTE_BUILD_PLATFORM)
  list(APPEND _gvte_targets gvte-platform)
  install(TARGETS gvte-platform
          EXPORT gvteTargets
          LIBRARY  DESTINATION "${CMAKE_INSTALL_LIBDIR}"
          ARCHIVE  DESTINATION "${CMAKE_INSTALL_LIBDIR}"
          RUNTIME  DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

# Export the target set (namespaced gvte::) for the build & install trees.
install(EXPORT gvteTargets
        FILE gvteTargets.cmake
        NAMESPACE gvte::
        DESTINATION "${GVTE_INSTALL_CMAKEDIR}")

# --- package config --------------------------------------------------------
configure_package_config_file(
  "${PROJECT_SOURCE_DIR}/cmake/gvteConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/gvteConfig.cmake"
  INSTALL_DESTINATION "${GVTE_INSTALL_CMAKEDIR}")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/gvteConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/gvteConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/gvteConfigVersion.cmake"
  DESTINATION "${GVTE_INSTALL_CMAKEDIR}")

# --- pkg-config ------------------------------------------------------------
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/gvte.pc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/gvte.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/gvte.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
