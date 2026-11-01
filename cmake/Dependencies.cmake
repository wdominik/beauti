# SPDX-License-Identifier: GPL-3.0-or-later
# Build-time dependencies pinned in buildspec.json.
#
# macOS: OBS headers, SIMDe, the ONNX Runtime release archive, and (when no OBS
# installation exists) the OBS application bundle are downloaded into .cache/
# and verified by SHA-256. No Python or extra tooling is involved, so
# `cmake --preset macos` configures on a clean checkout.
#
# Windows: `uv run beauti-prepare-windows` generates the import libraries with
# Microsoft's lib.exe and writes the SDK into .cache/windows/. This file only
# supplies the defaults that point at that output.
include_guard(GLOBAL)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" BEAUTI_BUILDSPEC)
set(BEAUTI_CACHE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.cache")
set(BEAUTI_DOWNLOAD_DIR "${BEAUTI_CACHE_DIR}/downloads")

# beauti_spec(<out> <json path...>): read one value from buildspec.json/dependencies.
function(beauti_spec out)
  string(JSON value GET "${BEAUTI_BUILDSPEC}" dependencies ${ARGN})
  set(${out} "${value}" PARENT_SCOPE)
endfunction()

# beauti_download(<out> <name> <url> <sha256>): download once into .cache/downloads.
# A cached archive with a different checksum is replaced; a failed download leaves
# no file behind.
function(beauti_download out name url sha256)
  get_filename_component(filename "${url}" NAME)
  set(archive "${BEAUTI_DOWNLOAD_DIR}/${name}-${filename}")
  if(EXISTS "${archive}")
    file(SHA256 "${archive}" actual)
    if(NOT actual STREQUAL sha256)
      message(STATUS "Discarding ${archive}: checksum mismatch")
      file(REMOVE "${archive}")
    endif()
  endif()
  if(NOT EXISTS "${archive}")
    message(STATUS "Downloading ${name} from ${url}")
    file(MAKE_DIRECTORY "${BEAUTI_DOWNLOAD_DIR}")
    file(REMOVE "${archive}.partial")
    file(DOWNLOAD "${url}" "${archive}.partial" EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
      list(GET status 1 text)
      file(REMOVE "${archive}.partial")
      message(FATAL_ERROR "Download of ${name} failed: ${text}")
    endif()
    file(RENAME "${archive}.partial" "${archive}")
  endif()
  set(${out} "${archive}" PARENT_SCOPE)
endfunction()

# beauti_managed_path(<variable> <default> <docstring> [type]): declare a cache path whose
# default lives under .cache/. A value that still points into .cache/ but at a
# previous pin (for example .cache/sdk/obs-32.2.2 after bumping buildspec.json) is
# replaced by the new default; a path outside .cache/ is the user's and stays.
function(beauti_managed_path variable default doc)
  set(type PATH)
  if(ARGC GREATER 3)
    set(type "${ARGV3}")
  endif()
  set(${variable} "${default}" CACHE ${type} "${doc}")
  if(NOT "${${variable}}" STREQUAL "${default}")
    file(TO_CMAKE_PATH "${${variable}}" current)
    string(FIND "${current}" "${BEAUTI_CACHE_DIR}/" inside)
    if(inside EQUAL 0)
      message(STATUS "${variable}: following the pin in buildspec.json (${default})")
      set(${variable} "${default}" CACHE ${type} "${doc}" FORCE)
    endif()
  endif()
endfunction()

# beauti_fetch_archive(<name> <url> <sha256> <archive_root> <destination> [patterns...]):
# download and extract an archive whose entries live under <archive_root>/, placing
# that directory's content at <destination>. Extraction is transactional: an
# interrupted run leaves no completion marker and is redone next time.
function(beauti_fetch_archive name url sha256 archive_root destination)
  set(marker "${destination}/.beauti-sha256")
  if(EXISTS "${marker}")
    file(READ "${marker}" recorded)
    string(STRIP "${recorded}" recorded)
    if(recorded STREQUAL sha256)
      return()
    endif()
  endif()
  beauti_download(archive "${name}" "${url}" "${sha256}")
  set(scratch "${destination}.extracting")
  file(REMOVE_RECURSE "${scratch}" "${destination}")
  file(MAKE_DIRECTORY "${scratch}")
  message(STATUS "Extracting ${name} to ${destination}")
  set(patterns)
  foreach(pattern IN LISTS ARGN)
    list(APPEND patterns "${archive_root}/${pattern}")
  endforeach()
  if(patterns)
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${scratch}" PATTERNS ${patterns})
  else()
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${scratch}")
  endif()
  if(NOT IS_DIRECTORY "${scratch}/${archive_root}")
    message(FATAL_ERROR "${name}: ${archive} does not contain ${archive_root}/")
  endif()
  get_filename_component(parent "${destination}" DIRECTORY)
  file(MAKE_DIRECTORY "${parent}")
  file(RENAME "${scratch}/${archive_root}" "${destination}")
  file(REMOVE_RECURSE "${scratch}")
  file(WRITE "${marker}" "${sha256}\n")
endfunction()

# Catch2 (native tests only): the amalgamated header and source of the pinned
# release, fetched on every platform and compiled once into beauti-catch2.
if(BUILD_TESTING)
  beauti_spec(catch2_version catch2 version)
  set(BEAUTI_CATCH2_DIR "${BEAUTI_CACHE_DIR}/sdk/catch2-${catch2_version}")
  file(MAKE_DIRECTORY "${BEAUTI_CATCH2_DIR}")
  foreach(part IN ITEMS header source)
    beauti_spec(url catch2 ${part} url)
    beauti_spec(sha catch2 ${part} sha256)
    beauti_download(file catch2 "${url}" "${sha}")
    get_filename_component(name "${url}" NAME)
    file(COPY_FILE "${file}" "${BEAUTI_CATCH2_DIR}/${name}" ONLY_IF_DIFFERENT)
  endforeach()
endif()

if(APPLE)
  beauti_spec(obs_version obs-studio version)

  # OBS headers: the release source archive, reduced to libobs/, frontend/api/, COPYING.
  beauti_managed_path(BEAUTI_OBS_SOURCE "${BEAUTI_CACHE_DIR}/sdk/obs-${obs_version}" "OBS header source root")
  if(BEAUTI_OBS_SOURCE STREQUAL "${BEAUTI_CACHE_DIR}/sdk/obs-${obs_version}")
    beauti_spec(url obs-studio sources url)
    beauti_spec(sha obs-studio sources sha256)
    beauti_spec(root obs-studio sources archive_root)
    beauti_fetch_archive(obs-studio "${url}" "${sha}" "${root}" "${BEAUTI_OBS_SOURCE}" "libobs/*" "frontend/api/*" "COPYING")
  endif()

  # SIMDe: libobs headers include it for SSE intrinsics on arm64.
  beauti_managed_path(BEAUTI_SIMDE "${BEAUTI_CACHE_DIR}/sdk/simde" "SIMDe header root")
  if(BEAUTI_SIMDE STREQUAL "${BEAUTI_CACHE_DIR}/sdk/simde")
    beauti_spec(url simde url)
    beauti_spec(sha simde sha256)
    beauti_spec(root simde archive_root)
    beauti_fetch_archive(simde "${url}" "${sha}" "${root}" "${BEAUTI_SIMDE}" "simde/*" "COPYING")
  endif()

  # ONNX Runtime: the official arm64 release archive (library, headers, notices).
  beauti_spec(ort_version onnxruntime macos-arm64 version)
  beauti_spec(ort_root onnxruntime macos-arm64 archive_root)
  set(ort_dir "${BEAUTI_CACHE_DIR}/runtime/${ort_root}")
  beauti_managed_path(BEAUTI_ORT_LIBRARY "${ort_dir}/lib/libonnxruntime.${ort_version}.dylib"
                      "ONNX Runtime library (dylib on Mac, import lib on Windows)")
  if(BEAUTI_ORT_LIBRARY STREQUAL "${ort_dir}/lib/libonnxruntime.${ort_version}.dylib")
    beauti_spec(url onnxruntime macos-arm64 url)
    beauti_spec(sha onnxruntime macos-arm64 sha256)
    beauti_fetch_archive(onnxruntime "${url}" "${sha}" "${ort_root}" "${ort_dir}" "lib/*" "include/*" "LICENSE" "ThirdPartyNotices.txt" "VERSION_NUMBER" "GIT_COMMIT_ID")
  endif()
  if(NOT EXISTS "${BEAUTI_ORT_LIBRARY}")
    message(FATAL_ERROR "BEAUTI_ORT_LIBRARY does not exist: ${BEAUTI_ORT_LIBRARY}")
  endif()

  # The bundled copy carries its own install name so the plugin and beauti-bench
  # load it through @rpath. Re-created only when the source library changes.
  set(BEAUTI_ORT_IMPORT "${CMAKE_BINARY_DIR}/deps/libbeauti_onnxruntime.dylib")
  file(SHA256 "${BEAUTI_ORT_LIBRARY}" ort_sha)
  set(ort_marker "${BEAUTI_ORT_IMPORT}.sha256")
  set(ort_recorded "")
  if(EXISTS "${ort_marker}")
    file(READ "${ort_marker}" ort_recorded)
    string(STRIP "${ort_recorded}" ort_recorded)
  endif()
  if(NOT EXISTS "${BEAUTI_ORT_IMPORT}" OR NOT ort_recorded STREQUAL ort_sha)
    message(STATUS "Staging ${BEAUTI_ORT_IMPORT}")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/deps")
    file(COPY_FILE "${BEAUTI_ORT_LIBRARY}" "${BEAUTI_ORT_IMPORT}")
    execute_process(COMMAND install_name_tool -id @rpath/libbeauti_onnxruntime.dylib "${BEAUTI_ORT_IMPORT}"
                    COMMAND_ERROR_IS_FATAL ANY ERROR_QUIET)
    # Changing the install name invalidates the release signature; dyld refuses
    # unsigned arm64 code, so re-sign ad hoc. Packaging signs the bundle again.
    execute_process(COMMAND codesign --force --sign - "${BEAUTI_ORT_IMPORT}" COMMAND_ERROR_IS_FATAL ANY)
    file(WRITE "${ort_marker}" "${ort_sha}\n")
  endif()

  # OBS application: link against the installed app; without one (CI), use the
  # pinned release image copied into .cache/obs-app/.
  set(BEAUTI_OBS_APP "/Applications/OBS.app" CACHE PATH "OBS application bundle")
  if(NOT EXISTS "${BEAUTI_OBS_APP}/Contents/Frameworks/libobs.framework/libobs")
    set(cached_app "${BEAUTI_CACHE_DIR}/obs-app/OBS.app")
    if(NOT EXISTS "${cached_app}/Contents/Frameworks/libobs.framework/libobs")
      beauti_spec(url obs-studio macos-app url)
      beauti_spec(sha obs-studio macos-app sha256)
      beauti_download(image obs-studio-app "${url}" "${sha}")
      message(STATUS "Copying OBS.app from ${image}")
      execute_process(COMMAND hdiutil attach -nobrowse -readonly -plist "${image}"
                      OUTPUT_VARIABLE attach_plist COMMAND_ERROR_IS_FATAL ANY)
      string(REGEX MATCH "<key>mount-point</key>[ \t\r\n]*<string>([^<]+)</string>" _ "${attach_plist}")
      set(mount "${CMAKE_MATCH_1}")
      if(NOT mount)
        message(FATAL_ERROR "Could not determine the mount point of ${image}")
      endif()
      file(REMOVE_RECURSE "${cached_app}")
      file(MAKE_DIRECTORY "${BEAUTI_CACHE_DIR}/obs-app")
      execute_process(COMMAND ditto "${mount}/OBS.app" "${cached_app}" RESULT_VARIABLE copy_result)
      execute_process(COMMAND hdiutil detach "${mount}" -quiet)
      if(copy_result)
        file(REMOVE_RECURSE "${cached_app}")
        message(FATAL_ERROR "Could not copy OBS.app from ${image}")
      endif()
    endif()
    set(BEAUTI_OBS_APP "${cached_app}" CACHE PATH "OBS application bundle" FORCE)
    message(STATUS "Using OBS from ${BEAUTI_OBS_APP}")
  endif()
  execute_process(COMMAND plutil -extract CFBundleShortVersionString raw "${BEAUTI_OBS_APP}/Contents/Info.plist"
                  OUTPUT_VARIABLE app_version OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT app_version STREQUAL obs_version)
    message(WARNING "OBS ${app_version} at ${BEAUTI_OBS_APP} differs from the pinned headers (${obs_version}). "
                    "Headers and the linked libobs must match; update buildspec.json or BEAUTI_OBS_APP.")
  endif()
elseif(WIN32)
  beauti_spec(ort_root onnxruntime windows-x64 archive_root)
  set(ort_dir "${BEAUTI_CACHE_DIR}/windows/ort_cpu/${ort_root}")
  beauti_managed_path(BEAUTI_ORT_LIBRARY "${ort_dir}/lib/onnxruntime.lib"
      "ONNX Runtime library (dylib on Mac, import lib on Windows)" FILEPATH)
  beauti_managed_path(BEAUTI_ORT_RUNTIME "${ort_dir}/lib/onnxruntime.dll"
      "ONNX Runtime DLL on Windows" FILEPATH)
  if(EXISTS "${BEAUTI_CACHE_DIR}/windows/sdk/lib/cmake")
    list(APPEND CMAKE_PREFIX_PATH "${BEAUTI_CACHE_DIR}/windows/sdk")
  endif()
  if(BEAUTI_ENABLE_TENSORRT)
    beauti_spec(BEAUTI_TENSORRT_DRIVER_MINIMUM tensorrt-rtx driver_api_minimum)
    beauti_spec(runtime_dlls tensorrt-rtx files tensorrt-rtx dlls)
    beauti_spec(runtime_bytes tensorrt-rtx files tensorrt-rtx dll_bytes)
    string(JSON dll_count LENGTH "${runtime_dlls}")
    math(EXPR dll_last "${dll_count} - 1")
    set(BEAUTI_TENSORRT_LIBRARIES "")
    set(BEAUTI_TENSORRT_LIBRARY_BYTES "")
    foreach(index RANGE 0 ${dll_last})
      string(JSON dll GET "${runtime_dlls}" ${index})
      string(APPEND BEAUTI_TENSORRT_LIBRARIES "\"${dll}\", ")
      string(JSON bytes GET "${runtime_bytes}" "${dll}")
      string(APPEND BEAUTI_TENSORRT_LIBRARY_BYTES "${bytes}ull, ")
    endforeach()
  endif()
  if(NOT EXISTS "${BEAUTI_ORT_LIBRARY}" OR NOT EXISTS "${BEAUTI_ORT_RUNTIME}")
    if(BEAUTI_ENABLE_TENSORRT)
      set(device tensorrt)
    else()
      set(device cpu)
    endif()
    message(FATAL_ERROR "ONNX Runtime for Windows is not prepared (${ort_dir}). "
                        "Run: uv run beauti-prepare-windows --device ${device}")
  endif()
endif()
