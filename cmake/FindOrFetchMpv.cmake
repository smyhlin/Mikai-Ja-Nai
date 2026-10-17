# System libmpv discovery (Linux) and pinned mpv-dev archive download,
# SHA-256 verification, and header/DLL extraction (Windows/fallback).

set(MIKAI_DIST_MPV_ARCHIVE "" CACHE FILEPATH
  "Optional local copy of the pinned mpv-dev archive (its published SHA-256 is always checked)")
set(MIKAI_DIST_MPV_CACHE_DIR "${CMAKE_BINARY_DIR}/_dist-cache/mpv")
set(MIKAI_MPV_ARCHIVE_NAME "mpv-dev-x86_64-20261007-git-d6d93599d5.7z")
set(MIKAI_MPV_ARCHIVE_URL
  "https://github.com/the-database/mpv-winbuild/releases/download/2026-10-07-d6d93599d5/${MIKAI_MPV_ARCHIVE_NAME}")
set(MIKAI_MPV_ARCHIVE_SHA256
  "077cb75fed47b185428f97224e1d798e2d4c2f4063fd8cda2662b74f2892327c")
file(MAKE_DIRECTORY "${MIKAI_DIST_MPV_CACHE_DIR}")
set(MIKAI_USE_SYSTEM_MPV FALSE)
if(NOT WIN32 AND NOT MIKAI_DIST_MPV_ARCHIVE
   AND NOT EXISTS "${MIKAI_DIST_MPV_CACHE_DIR}/${MIKAI_MPV_ARCHIVE_NAME}")
  find_path(MIKAI_SYSTEM_MPV_HEADERS NAMES mpv/client.h)
  find_library(MIKAI_SYSTEM_MPV_LIB NAMES mpv libmpv.so.2 libmpv.so)
  if(MIKAI_SYSTEM_MPV_HEADERS AND EXISTS "${MIKAI_SYSTEM_MPV_HEADERS}/mpv/render_gl.h")
    set(MIKAI_USE_SYSTEM_MPV TRUE)
    set(MIKAI_MPV_HEADERS "${MIKAI_SYSTEM_MPV_HEADERS}")
    if(MIKAI_SYSTEM_MPV_LIB)
      set(MIKAI_MPV_DLL "${MIKAI_SYSTEM_MPV_LIB}")
    else()
      set(MIKAI_MPV_DLL "")
    endif()
    message(STATUS "Using system libmpv headers at ${MIKAI_MPV_HEADERS}")
  endif()
endif()

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
  if(NOT EXISTS "${MIKAI_MPV_HEADERS_DIR}/include/mpv/client.h"
      OR NOT EXISTS "${MIKAI_MPV_HEADERS_DIR}/include/mpv/render_gl.h"
      OR NOT EXISTS "${MIKAI_MPV_HEADERS_DIR}/libmpv-2.dll")
    file(REMOVE_RECURSE "${MIKAI_MPV_HEADERS_DIR}")
    file(MAKE_DIRECTORY "${MIKAI_MPV_HEADERS_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${MIKAI_PINNED_MPV_ARCHIVE}"
      DESTINATION "${MIKAI_MPV_HEADERS_DIR}")
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
