# Offline Linux libmpv bundle: pinned acquisition, bounded owned extraction,
# private dependency resolution, and $ORIGIN RUNPATH patching.
#
# This is an explicit opt-in. System-only Linux builds remain supported by
# leaving MIKAI_LINUX_BUNDLE_MPV=OFF; there is no silent fallback from a
# requested bundle to system libraries.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()

option(MIKAI_LINUX_BUNDLE_MPV
  "Bundle the pinned Linux libmpv runtime and its private dependencies into the install tree"
  OFF)

# Where the private runtime lives inside the prefix. A distribution package may
# not put a directory named `mpv` in /usr/bin: the distribution's mpv package
# owns /usr/bin/mpv as a file, and pacman refuses the collision.
set(MIKAI_LINUX_MPV_INSTALL_SUBDIR "${CMAKE_INSTALL_BINDIR}/mpv" CACHE STRING
  "Prefix-relative directory holding the bundled libmpv runtime")
file(RELATIVE_PATH mikai_linux_mpv_from_bin
  "/${CMAKE_INSTALL_BINDIR}" "/${MIKAI_LINUX_MPV_INSTALL_SUBDIR}")

# The published archive at this commit was configured without an FFmpeg TLS
# backend, so it cannot open any https URL. This pin is the same runtime with a
# same-tag FFmpeg rebuilt with --enable-openssl; packaging/mpv/build-tls-archive.sh
# produces it and release-artifacts/ carries the result.
set(MIKAI_LINUX_MPV_ARCHIVE_NAME "mpv-linux-x64-2026-10-24-c493f1c-tls.tar.zst")
set(MIKAI_LINUX_MPV_ARCHIVE_SHA256
  "6796d922cfd3ffb9bcd8948f77c609a4055ac343a9898f2433d2aa4b0eb6a17d")
set(MIKAI_LINUX_MPV_ARCHIVE_SIZE "41533948")

set(MIKAI_LINUX_MPV_ARCHIVE "" CACHE FILEPATH
  "Optional local copy of the pinned Linux mpv archive (its published SHA-256 is always checked)")
set(MIKAI_LINUX_MPV_CACHE_DIR "${CMAKE_BINARY_DIR}/_dist-cache/mpv-linux")

set(MIKAI_LINUX_MPV_BUNDLE OFF)
set(MIKAI_LINUX_MPV_BUNDLE_ROOT "")
set(MIKAI_LINUX_MPV_BUNDLE_MODE "system")
set(MIKAI_LINUX_MPV_RUNTIME_IDENTITY "host libmpv.so.2 (resolved at runtime)")
set(MIKAI_LINUX_MPV_PATCHED_RUNPATH "not-patched")
set(MIKAI_LINUX_MPV_BUNDLE_ARCHIVE_SHA256 "${MIKAI_LINUX_MPV_ARCHIVE_SHA256}")

if(NOT MIKAI_LINUX_BUNDLE_MPV)
  set(MIKAI_LINUX_MPV_BUNDLE_ARCHIVE_SHA256 "not-bundled")
  message(STATUS "Linux libmpv bundle disabled; using system libmpv development/runtime.")
  return()
endif()

find_program(MIKAI_PATCHELF_EXECUTABLE NAMES patchelf)
if(NOT MIKAI_PATCHELF_EXECUTABLE)
  message(FATAL_ERROR
    "MIKAI_LINUX_BUNDLE_MPV requires patchelf (build-only dependency) to set "
    "$ORIGIN RUNPATH on freshly owned private .so copies; install patchelf or "
    "disable MIKAI_LINUX_BUNDLE_MPV.")
endif()

if(IS_SYMLINK "${MIKAI_LINUX_MPV_CACHE_DIR}")
  message(FATAL_ERROR "Refusing a symlink Linux mpv cache directory: ${MIKAI_LINUX_MPV_CACHE_DIR}")
endif()
file(MAKE_DIRECTORY "${MIKAI_LINUX_MPV_CACHE_DIR}")

if(MIKAI_LINUX_MPV_ARCHIVE)
  get_filename_component(MIKAI_PINNED_LINUX_MPV_ARCHIVE "${MIKAI_LINUX_MPV_ARCHIVE}"
    ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  if(NOT EXISTS "${MIKAI_PINNED_LINUX_MPV_ARCHIVE}")
    message(FATAL_ERROR "The local Linux mpv archive override was not found: ${MIKAI_PINNED_LINUX_MPV_ARCHIVE}")
  endif()
else()
  set(_mpv_archive_candidate "${CMAKE_CURRENT_SOURCE_DIR}/release-artifacts/${MIKAI_LINUX_MPV_ARCHIVE_NAME}")
  if(EXISTS "${_mpv_archive_candidate}" AND NOT IS_DIRECTORY "${_mpv_archive_candidate}")
    set(MIKAI_PINNED_LINUX_MPV_ARCHIVE "${_mpv_archive_candidate}")
  else()
    set(MIKAI_PINNED_LINUX_MPV_ARCHIVE "${MIKAI_LINUX_MPV_CACHE_DIR}/${MIKAI_LINUX_MPV_ARCHIVE_NAME}")
  endif()
  if(NOT EXISTS "${MIKAI_PINNED_LINUX_MPV_ARCHIVE}")
    message(FATAL_ERROR
      "The TLS-enabled Linux mpv runtime is not available: ${MIKAI_LINUX_MPV_ARCHIVE_NAME} "
      "was not found in release-artifacts/ or the build cache. Build it with "
      "'bash packaging/mpv/build-tls-archive.sh' or pass -DMIKAI_LINUX_MPV_ARCHIVE=<path>. "
      "The archive published at the pinned commit has no FFmpeg TLS backend, so "
      "bundling it would ship a player that cannot open https streams.")
  endif()
endif()

file(SHA256 "${MIKAI_PINNED_LINUX_MPV_ARCHIVE}" actual_linux_mpv_archive_sha256)
if(NOT actual_linux_mpv_archive_sha256 STREQUAL MIKAI_LINUX_MPV_ARCHIVE_SHA256)
  message(FATAL_ERROR
    "The Linux mpv archive SHA-256 does not match the pinned release: "
    "expected ${MIKAI_LINUX_MPV_ARCHIVE_SHA256}, got ${actual_linux_mpv_archive_sha256}")
endif()

set(MIKAI_LINUX_MPV_EXTRACTED_DIR "${MIKAI_LINUX_MPV_CACHE_DIR}/extracted")
if(IS_SYMLINK "${MIKAI_LINUX_MPV_EXTRACTED_DIR}")
  message(FATAL_ERROR "Refusing a symlink Linux mpv extraction directory: ${MIKAI_LINUX_MPV_EXTRACTED_DIR}")
endif()
set(MIKAI_LINUX_MPV_STAMP "${MIKAI_LINUX_MPV_EXTRACTED_DIR}/.mikai-linux-mpv.sha256")
set(_mikai_linux_mpv_stamp_identity "mikai-linux-mpv-v3 ${MIKAI_LINUX_MPV_ARCHIVE_SHA256}")

function(mikai_linux_mpv_cache_valid result)
  set(${result} FALSE PARENT_SCOPE)
  if(NOT EXISTS "${MIKAI_LINUX_MPV_STAMP}" OR IS_SYMLINK "${MIKAI_LINUX_MPV_STAMP}"
      OR IS_DIRECTORY "${MIKAI_LINUX_MPV_STAMP}")
    return()
  endif()
  file(STRINGS "${MIKAI_LINUX_MPV_STAMP}" stamp_lines)
  list(LENGTH stamp_lines stamp_line_count)
  if(stamp_line_count LESS 3)
    return()
  endif()
  list(POP_FRONT stamp_lines stamp_identity)
  if(NOT stamp_identity STREQUAL _mikai_linux_mpv_stamp_identity)
    return()
  endif()
  set(stamped_libraries "")
  foreach(stamp_line IN LISTS stamp_lines)
    if(NOT stamp_line MATCHES "^([0-9a-f]+)  ([A-Za-z0-9_.+-]+\\.so[.0-9]*)$")
      return()
    endif()
    set(expected_hash "${CMAKE_MATCH_1}")
    set(library_name "${CMAKE_MATCH_2}")
    string(LENGTH "${expected_hash}" hash_length)
    set(library "${MIKAI_LINUX_MPV_EXTRACTED_DIR}/${library_name}")
    if(NOT hash_length EQUAL 64 OR NOT EXISTS "${library}"
        OR IS_DIRECTORY "${library}" OR IS_SYMLINK "${library}")
      return()
    endif()
    file(SHA256 "${library}" actual_hash)
    if(NOT actual_hash STREQUAL expected_hash)
      return()
    endif()
    list(APPEND stamped_libraries "${library_name}")
  endforeach()
  file(GLOB cached_libraries LIST_DIRECTORIES FALSE
    RELATIVE "${MIKAI_LINUX_MPV_EXTRACTED_DIR}" "${MIKAI_LINUX_MPV_EXTRACTED_DIR}/*.so*")
  list(SORT stamped_libraries)
  list(SORT cached_libraries)
  if(NOT stamped_libraries STREQUAL cached_libraries
      OR NOT "libmpv.so.2" IN_LIST stamped_libraries)
    return()
  endif()
  set(${result} TRUE PARENT_SCOPE)
endfunction()

mikai_linux_mpv_cache_valid(_mikai_linux_mpv_cache_valid)
if(NOT _mikai_linux_mpv_cache_valid)
  file(REMOVE_RECURSE "${MIKAI_LINUX_MPV_EXTRACTED_DIR}")
  file(MAKE_DIRECTORY "${MIKAI_LINUX_MPV_EXTRACTED_DIR}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xf "${MIKAI_PINNED_LINUX_MPV_ARCHIVE}"
    WORKING_DIRECTORY "${MIKAI_LINUX_MPV_EXTRACTED_DIR}"
    RESULT_VARIABLE linux_mpv_extract_result)
  if(NOT linux_mpv_extract_result EQUAL 0)
    file(REMOVE_RECURSE "${MIKAI_LINUX_MPV_EXTRACTED_DIR}")
    message(FATAL_ERROR "Linux mpv archive extraction failed (${linux_mpv_extract_result})")
  endif()

  set(_mikai_linux_mpv_module "${MIKAI_LINUX_MPV_EXTRACTED_DIR}/libmpv.so.2")
  if(NOT EXISTS "${_mikai_linux_mpv_module}" OR IS_SYMLINK "${_mikai_linux_mpv_module}"
      OR IS_DIRECTORY "${_mikai_linux_mpv_module}")
    message(FATAL_ERROR "The pinned Linux mpv archive must contain a regular libmpv.so.2")
  endif()
  file(REMOVE "${MIKAI_LINUX_MPV_EXTRACTED_DIR}/libmpv.so")
  # The archive ships libmpv.so.2 and one full-version real file. Drop the real
  # file by pattern so a release that renumbers it needs no pin-side edit; the
  # SONAME module the loader needs is libmpv.so.2.
  file(GLOB _mikai_linux_mpv_version_aliases
    LIST_DIRECTORIES FALSE "${MIKAI_LINUX_MPV_EXTRACTED_DIR}/libmpv.so.2.*")
  file(REMOVE ${_mikai_linux_mpv_version_aliases})

  file(GLOB _mikai_linux_mpv_so_files
    LIST_DIRECTORIES FALSE "${MIKAI_LINUX_MPV_EXTRACTED_DIR}/*.so*")
  set(_mikai_linux_mpv_stamp "${_mikai_linux_mpv_stamp_identity}\n")
  set(_mikai_linux_mpv_library_count 0)
  foreach(_so IN LISTS _mikai_linux_mpv_so_files)
    get_filename_component(_so_name "${_so}" NAME)
    # Driver- and display-coupled libraries come from the host. The Vulkan
    # loader does not: the runtime links it, and an unmodified host without
    # vulkan-icd-loader would otherwise fail to load the module at all.
    if(_so_name MATCHES "^(ld-linux[^/]*|lib(c|m|mvec|pthread|dl|rt|resolv|util|anl|BrokenLocale|thread_db|nss_[^.]+|SegFault|memusage|pcprofile|EGL|GL|GLX|GLdispatch|OpenGL|GLESv[0-9]+|cuda|nvidia[^.]*|Qt[0-9][^.]*))\\.so")
      file(REMOVE "${_so}")
      continue()
    endif()
    if(IS_SYMLINK "${_so}")
      file(REAL_PATH "${_so}" _so_real)
      cmake_path(IS_PREFIX MIKAI_LINUX_MPV_EXTRACTED_DIR "${_so_real}" NORMALIZE _so_owned)
      if(NOT _so_owned OR NOT EXISTS "${_so_real}" OR IS_DIRECTORY "${_so_real}")
        message(FATAL_ERROR "Linux mpv dependency link escapes the owned extraction or is invalid: ${_so}")
      endif()
      file(COPY_FILE "${_so_real}" "${_so}.mikai-copy")
      file(REMOVE "${_so}")
      file(RENAME "${_so}.mikai-copy" "${_so}")
    endif()
    file(READ "${_so}" _so_magic LIMIT 4 HEX)
    if(NOT _so_magic STREQUAL "7f454c46")
      message(FATAL_ERROR "Linux mpv private library is not an ELF file: ${_so}")
    endif()
    execute_process(
      COMMAND "${MIKAI_PATCHELF_EXECUTABLE}" --set-rpath "$ORIGIN" "${_so}"
      RESULT_VARIABLE _patch_result
      ERROR_VARIABLE _patch_stderr)
    if(NOT _patch_result EQUAL 0)
      message(FATAL_ERROR "patchelf failed for ${_so}: ${_patch_stderr}")
    endif()
    file(SHA256 "${_so}" _so_hash)
    string(APPEND _mikai_linux_mpv_stamp "${_so_hash}  ${_so_name}\n")
    math(EXPR _mikai_linux_mpv_library_count "${_mikai_linux_mpv_library_count} + 1")
  endforeach()
  if(_mikai_linux_mpv_library_count LESS 2)
    message(FATAL_ERROR "The pinned Linux mpv archive has no private dependency payload")
  endif()
  file(WRITE "${MIKAI_LINUX_MPV_STAMP}" "${_mikai_linux_mpv_stamp}")
endif()

set(MIKAI_LINUX_MPV_BUNDLE ON)
set(MIKAI_LINUX_MPV_BUNDLE_ROOT "${MIKAI_LINUX_MPV_EXTRACTED_DIR}")
set(MIKAI_LINUX_MPV_BUNDLE_MODE "bundled")
set(MIKAI_LINUX_MPV_RUNTIME_IDENTITY "mpv-linux-x64-2026-10-24-c493f1c7e10a6b2301653596e7deeb91295d3148+tls")
set(MIKAI_LINUX_MPV_PATCHED_RUNPATH "$ORIGIN")

# HTTPS needs a TLS backend inside the bundled FFmpeg; without one every
# provider stream fails with "No protocol handler found", and no host library
# can supply it after the fact. Read the backend from libavformat's own dynamic
# requirements: FFmpeg embeds the "recompile FFmpeg with openssl" advice text in
# every build, so its presence proves nothing.
set(MIKAI_LINUX_MPV_TLS OFF)
file(GLOB _mikai_mpv_avformat "${MIKAI_LINUX_MPV_BUNDLE_ROOT}/libavformat.so.*")
if(_mikai_mpv_avformat)
  list(GET _mikai_mpv_avformat 0 _mikai_mpv_avformat_first)
  if(CMAKE_READELF)
    execute_process(
      COMMAND "${CMAKE_READELF}" -d "${_mikai_mpv_avformat_first}"
      OUTPUT_VARIABLE _mikai_mpv_avformat_deps ERROR_QUIET RESULT_VARIABLE _mikai_mpv_readelf)
    if(_mikai_mpv_readelf EQUAL 0
        AND _mikai_mpv_avformat_deps MATCHES "lib(ssl|crypto|gnutls|mbedtls)\\.so")
      set(MIKAI_LINUX_MPV_TLS ON)
    endif()
  else()
    file(GLOB _mikai_mpv_tls_libs
      "${MIKAI_LINUX_MPV_BUNDLE_ROOT}/libssl.so.*"
      "${MIKAI_LINUX_MPV_BUNDLE_ROOT}/libgnutls.so.*"
      "${MIKAI_LINUX_MPV_BUNDLE_ROOT}/libmbedtls.so.*")
    if(_mikai_mpv_tls_libs)
      set(MIKAI_LINUX_MPV_TLS ON)
    endif()
  endif()
endif()
if(NOT MIKAI_LINUX_MPV_TLS)
  message(WARNING
    "The pinned Linux libmpv was built without an FFmpeg TLS backend: https "
    "streams (every provider stream and HLS URL over https) cannot be opened by "
    "this bundle. Playback of local files still works. Use a runtime with TLS "
    "(for example the distribution libmpv) if https playback is required.")
endif()

message(STATUS "Linux libmpv bundle enabled: ${MIKAI_LINUX_MPV_BUNDLE_ROOT} (https/TLS: ${MIKAI_LINUX_MPV_TLS})")

file(GLOB MIKAI_LINUX_MPV_INSTALL_LIBRARIES LIST_DIRECTORIES FALSE
  "${MIKAI_LINUX_MPV_BUNDLE_ROOT}/*.so*")
install(FILES ${MIKAI_LINUX_MPV_INSTALL_LIBRARIES}
  DESTINATION "${MIKAI_LINUX_MPV_INSTALL_SUBDIR}")
