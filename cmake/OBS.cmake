# SPDX-License-Identifier: GPL-3.0-or-later
# Imported targets OBS::libobs and OBS::obs-frontend-api. macOS links the
# installed (or cached) OBS application against the pinned source headers;
# Windows uses the CMake package of the official OBS SDK.
if(APPLE)
  # BEAUTI_OBS_APP, BEAUTI_OBS_SOURCE, and BEAUTI_SIMDE come from cmake/Dependencies.cmake.
  foreach(header "${BEAUTI_OBS_SOURCE}/libobs/obs-module.h" "${BEAUTI_OBS_SOURCE}/frontend/api/obs-frontend-api.h" "${BEAUTI_SIMDE}/simde/x86/sse2.h")
    if(NOT EXISTS "${header}")
      message(FATAL_ERROR "Missing header ${header}. Check buildspec.json and cmake/Dependencies.cmake.")
    endif()
  endforeach()
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/obsconfig.h" "#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")
  add_library(OBS::libobs SHARED IMPORTED)
  set_target_properties(OBS::libobs PROPERTIES IMPORTED_LOCATION "${BEAUTI_OBS_APP}/Contents/Frameworks/libobs.framework/libobs" INTERFACE_INCLUDE_DIRECTORIES "${BEAUTI_OBS_SOURCE}/libobs;${BEAUTI_SIMDE}"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${BEAUTI_OBS_SOURCE}/libobs;${BEAUTI_SIMDE}")
  add_library(OBS::obs-frontend-api SHARED IMPORTED)
  set_target_properties(OBS::obs-frontend-api PROPERTIES IMPORTED_LOCATION "${BEAUTI_OBS_APP}/Contents/Frameworks/libobs-frontend-api.1.dylib" INTERFACE_INCLUDE_DIRECTORIES "${BEAUTI_OBS_SOURCE}/frontend/api"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${BEAUTI_OBS_SOURCE}/frontend/api")
elseif(WIN32)
  # Official OBS development SDK exports these targets, including all header paths.
  find_package(libobs CONFIG REQUIRED)
  find_package(obs-frontend-api CONFIG REQUIRED)
endif()
