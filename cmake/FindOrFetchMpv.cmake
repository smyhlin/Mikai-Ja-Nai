# Linux uses the host's development headers and shared libmpv ABI 2.
# Only Windows may download the pinned Windows developer archive.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(MIKAI_DIST_MPV_ARCHIVE)
    message(FATAL_ERROR
      "MIKAI_DIST_MPV_ARCHIVE is a Windows-only override. Linux requires system "
      "libmpv development headers and libmpv.so.2; no Windows archive fallback is permitted.")
  endif()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(MIKAI_SYSTEM_MPV_PC QUIET mpv>=0.35)
  endif()
  find_path(MIKAI_SYSTEM_MPV_HEADERS NAMES mpv/client.h
    HINTS ${MIKAI_SYSTEM_MPV_PC_INCLUDE_DIRS}
    DOC "Directory containing the Linux mpv/client.h and mpv/render_gl.h headers")
  find_library(MIKAI_SYSTEM_MPV_LIB NAMES libmpv.so.2 mpv
    HINTS ${MIKAI_SYSTEM_MPV_PC_LIBRARY_DIRS}
    DOC "Linux libmpv ABI 2 shared runtime (not a static archive)")
  if(NOT MIKAI_SYSTEM_MPV_HEADERS
      OR NOT EXISTS "${MIKAI_SYSTEM_MPV_HEADERS}/mpv/render.h"
      OR NOT EXISTS "${MIKAI_SYSTEM_MPV_HEADERS}/mpv/render_gl.h"
      OR NOT MIKAI_SYSTEM_MPV_LIB)
    message(FATAL_ERROR
      "Linux needs libmpv >= 0.35 (client API 2), including client.h, render.h, "
      "render_gl.h and libmpv.so.2. Install the distribution's libmpv-dev or "
      "mpv-devel package, or set MIKAI_SYSTEM_MPV_HEADERS and MIKAI_SYSTEM_MPV_LIB "
      "for a matching Linux build. No Windows binary will be downloaded.")
  endif()
  file(STRINGS "${MIKAI_SYSTEM_MPV_HEADERS}/mpv/client.h" mpv_api_version_line
    REGEX "^#define[ \t]+MPV_CLIENT_API_VERSION[ \t]+MPV_MAKE_VERSION")
  if(NOT mpv_api_version_line MATCHES "MPV_MAKE_VERSION\\([ \t]*2[ \t]*,")
    message(FATAL_ERROR "Linux requires libmpv client API major 2 headers (mpv >= 0.35).")
  endif()
  get_filename_component(mpv_system_real_library "${MIKAI_SYSTEM_MPV_LIB}" REALPATH)
  if(NOT mpv_system_real_library MATCHES "\\.so\\.2(\\.[0-9]+)*$")
    message(FATAL_ERROR
      "Linux requires the shared libmpv.so.2 runtime, not '${MIKAI_SYSTEM_MPV_LIB}'.")
  endif()
  file(READ "${mpv_system_real_library}" mpv_system_magic LIMIT 4 HEX)
  if(NOT mpv_system_magic STREQUAL "7f454c46")
    message(FATAL_ERROR "The selected Linux libmpv is not an ELF shared library.")
  endif()
  set(MIKAI_USE_SYSTEM_MPV TRUE)
  set(MIKAI_MPV_HEADERS "${MIKAI_SYSTEM_MPV_HEADERS}")
  # The existing development-tree deployment copies this into <build>/mpv.
  # Linux install/package rules deliberately keep libmpv a system dependency.
  set(MIKAI_MPV_DLL "${MIKAI_SYSTEM_MPV_LIB}")
  message(STATUS "Using Linux libmpv API 2: ${MIKAI_MPV_HEADERS}; ${MIKAI_MPV_DLL}")
  return()
elseif(NOT WIN32)
  message(FATAL_ERROR "Mikai release builds currently support Windows and Linux only.")
endif()

set(MIKAI_DIST_MPV_ARCHIVE "" CACHE FILEPATH
  "Optional local copy of the pinned mpv-dev archive (its published SHA-256 is always checked)")
set(MIKAI_DIST_MPV_CACHE_DIR "${CMAKE_BINARY_DIR}/_dist-cache/mpv")
set(MIKAI_MPV_ARCHIVE_NAME "mpv-dev-x86_64-20261024-git-b4f9f5bfed.7z")
set(MIKAI_MPV_ARCHIVE_URL
  "https://github.com/the-database/mpv-winbuild/releases/download/2026-10-24-b4f9f5bfed/${MIKAI_MPV_ARCHIVE_NAME}")
set(MIKAI_MPV_ARCHIVE_SHA256
  "2efe77b9e3028c32cd74c9d871c5d2cd8315758d0da3f59bf64c867c8e7e6d0c")
file(MAKE_DIRECTORY "${MIKAI_DIST_MPV_CACHE_DIR}")
set(MIKAI_USE_SYSTEM_MPV FALSE)

if(NOT MIKAI_USE_SYSTEM_MPV)
  if(MIKAI_DIST_MPV_ARCHIVE)
    set(MIKAI_PINNED_MPV_ARCHIVE "${MIKAI_DIST_MPV_ARCHIVE}")
    if(NOT EXISTS "${MIKAI_PINNED_MPV_ARCHIVE}")
      message(FATAL_ERROR "The local mpv archive override was not found: ${MIKAI_PINNED_MPV_ARCHIVE}")
    endif()
  else()
    set(MIKAI_PINNED_MPV_ARCHIVE "${MIKAI_DIST_MPV_CACHE_DIR}/${MIKAI_MPV_ARCHIVE_NAME}")
    if(NOT EXISTS "${MIKAI_PINNED_MPV_ARCHIVE}")
      message(STATUS "Downloading pinned mpv developer archive for matching headers")
      file(DOWNLOAD "${MIKAI_MPV_ARCHIVE_URL}" "${MIKAI_PINNED_MPV_ARCHIVE}"
        EXPECTED_HASH "SHA256=${MIKAI_MPV_ARCHIVE_SHA256}"
        STATUS mpv_download_status
        SHOW_PROGRESS)
      list(GET mpv_download_status 0 mpv_download_code)
      list(GET mpv_download_status 1 mpv_download_message)
      if(NOT mpv_download_code EQUAL 0)
        file(REMOVE "${MIKAI_PINNED_MPV_ARCHIVE}")
        message(FATAL_ERROR "mpv archive download failed: ${mpv_download_message}")
      endif()
    endif()
  endif()
  file(SHA256 "${MIKAI_PINNED_MPV_ARCHIVE}" actual_mpv_archive_sha256)
  if(NOT actual_mpv_archive_sha256 STREQUAL MIKAI_MPV_ARCHIVE_SHA256)
    message(FATAL_ERROR
      "The mpv archive SHA-256 does not match the pinned release: "
      "expected ${MIKAI_MPV_ARCHIVE_SHA256}, got ${actual_mpv_archive_sha256}")
  endif()

  set(MIKAI_MPV_HEADERS_DIR "${MIKAI_DIST_MPV_CACHE_DIR}/headers")
  set(MIKAI_MPV_HEADERS_STAMP "${MIKAI_MPV_HEADERS_DIR}/.mikai-mpv-archive.sha256")
  set(_mikai_mpv_headers_need_refresh FALSE)
  if(NOT EXISTS "${MIKAI_MPV_HEADERS_DIR}/include/mpv/client.h"
      OR NOT EXISTS "${MIKAI_MPV_HEADERS_DIR}/include/mpv/render_gl.h"
      OR NOT EXISTS "${MIKAI_MPV_HEADERS_DIR}/libmpv-2.dll"
      OR NOT EXISTS "${MIKAI_MPV_HEADERS_STAMP}")
    set(_mikai_mpv_headers_need_refresh TRUE)
  else()
    file(READ "${MIKAI_MPV_HEADERS_STAMP}" _mikai_mpv_headers_stamp_value)
    string(STRIP "${_mikai_mpv_headers_stamp_value}" _mikai_mpv_headers_stamp_value)
    if(NOT _mikai_mpv_headers_stamp_value STREQUAL MIKAI_MPV_ARCHIVE_SHA256)
      set(_mikai_mpv_headers_need_refresh TRUE)
    endif()
  endif()
  if(_mikai_mpv_headers_need_refresh)
    # This is an owned extraction directory beneath the build cache, not the
    # caller-provided cache root itself. Refreshing it is what makes a pin
    # change discard stale headers/DLLs from an older archive.
    file(REMOVE_RECURSE "${MIKAI_MPV_HEADERS_DIR}")
    file(MAKE_DIRECTORY "${MIKAI_MPV_HEADERS_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${MIKAI_PINNED_MPV_ARCHIVE}"
      DESTINATION "${MIKAI_MPV_HEADERS_DIR}")
    file(WRITE "${MIKAI_MPV_HEADERS_STAMP}" "${MIKAI_MPV_ARCHIVE_SHA256}\n")
  endif()
  set(MIKAI_MPV_HEADERS "${MIKAI_MPV_HEADERS_DIR}/include")
  if(NOT EXISTS "${MIKAI_MPV_HEADERS}/mpv/client.h"
      OR NOT EXISTS "${MIKAI_MPV_HEADERS}/mpv/render_gl.h")
    message(FATAL_ERROR
      "The verified mpv archive does not contain include/mpv/client.h and include/mpv/render_gl.h")
  endif()
  file(GLOB_RECURSE MIKAI_MPV_DLL_CANDIDATES LIST_DIRECTORIES FALSE
    "${MIKAI_MPV_HEADERS_DIR}/libmpv-2.dll")
  list(LENGTH MIKAI_MPV_DLL_CANDIDATES MIKAI_MPV_DLL_COUNT)
  if(NOT MIKAI_MPV_DLL_COUNT EQUAL 1)
    message(FATAL_ERROR
      "The verified mpv archive must contain exactly one libmpv-2.dll; found ${MIKAI_MPV_DLL_COUNT}")
  endif()
  list(GET MIKAI_MPV_DLL_CANDIDATES 0 MIKAI_MPV_DLL)
endif()
