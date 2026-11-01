# SPDX-License-Identifier: GPL-3.0-or-later
# CPU reference effects library: portable Skin Tone, Micro Dodge & Burn, and
# Texture implementations for the numerical tests. Builds standalone
# (beauti-build-reference) or as a subdirectory with BEAUTI_BUILD_REFERENCE=ON.
cmake_minimum_required(VERSION 3.25...4.4)
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "27.0" CACHE STRING "Minimum macOS version" FORCE)
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(BeautiReference LANGUAGES CXX)
  include(CTest)
endif()
add_library(beauti-effects SHARED api.cpp skin_tone.cpp texture.cpp dodge_burn.cpp)
# tuning.h holds the constants shared with the GPU path; it has no other dependencies.
target_include_directories(beauti-effects PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../core")
target_compile_features(beauti-effects PRIVATE cxx_std_20)
set_target_properties(beauti-effects PROPERTIES CXX_EXTENSIONS OFF)
set_target_properties(beauti-effects PROPERTIES CXX_VISIBILITY_PRESET hidden
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/reference"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/reference")
if(APPLE)
  set_target_properties(beauti-effects PROPERTIES BUILD_WITH_INSTALL_NAME_DIR TRUE INSTALL_NAME_DIR "@rpath")
endif()
foreach(configuration DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(beauti-effects PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${configuration} "${CMAKE_BINARY_DIR}/reference")
endforeach()
if(MSVC)
  target_compile_options(beauti-effects PRIVATE /W4 /EHsc /fp:precise /guard:cf /sdl)
  target_link_options(beauti-effects PRIVATE /guard:cf)
else()
  target_compile_options(beauti-effects PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Wold-style-cast -Werror=return-type -fstack-protector-strong -ffp-contract=off "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=." "-ffile-prefix-map=${CMAKE_BINARY_DIR}=build")
  target_compile_definitions(beauti-effects PRIVATE $<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>)
endif()
