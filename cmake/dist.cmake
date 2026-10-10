# Assembles the self-contained dist folder. Run through the `dist` target:
#   cmake --build <build-dir> --target dist
# Required inputs: DIST_DIR, EXE, WINDEPLOYQT, SHADERS, SOURCE_DIR.
# Optional: MIKAI_DIST_NATIVE=ON copies the installed native runtime pointer and
# every bundle it names. The mpv runtime is always the SHA-pinned API DLL below.

if(NOT DEFINED DIST_DIR OR DIST_DIR STREQUAL "")
  message(FATAL_ERROR "dist: DIST_DIR is not set")
endif()
if(NOT DEFINED EXE OR NOT EXISTS "${EXE}")
  message(FATAL_ERROR "dist: the executable was not found at '${EXE}'")
endif()
if(NOT DEFINED SHADERS OR NOT IS_DIRECTORY "${SHADERS}")
  message(FATAL_ERROR "dist: the shaders directory was not found at '${SHADERS}'")
endif()

if(NOT DEFINED WINDEPLOYQT OR WINDEPLOYQT STREQUAL "" OR NOT EXISTS "${WINDEPLOYQT}")
  message(FATAL_ERROR "dist: windeployqt was not found at '${WINDEPLOYQT}'")
endif()

if(NOT DEFINED MIKAI_MPV_ARCHIVE_NAME OR NOT DEFINED MIKAI_MPV_ARCHIVE_URL
    OR NOT DEFINED MIKAI_MPV_ARCHIVE_SHA256)
  message(FATAL_ERROR "dist: pinned mpv release metadata was not provided by the configured build")
endif()
set(mpv_archive_name "${MIKAI_MPV_ARCHIVE_NAME}")
set(mpv_archive_url "${MIKAI_MPV_ARCHIVE_URL}")
set(mpv_archive_sha256 "${MIKAI_MPV_ARCHIVE_SHA256}")

if(NOT DEFINED MIKAI_DIST_CACHE_DIR OR MIKAI_DIST_CACHE_DIR STREQUAL "")
  message(FATAL_ERROR "dist: MIKAI_DIST_CACHE_DIR was not provided by the configured build")
endif()
file(MAKE_DIRECTORY "${MIKAI_DIST_CACHE_DIR}")
if(MIKAI_DIST_MPV_ARCHIVE)
  set(mpv_archive "${MIKAI_DIST_MPV_ARCHIVE}")
  if(NOT EXISTS "${mpv_archive}")
    message(FATAL_ERROR "dist: the local mpv archive override was not found: ${mpv_archive}")
  endif()
else()
  set(mpv_archive "${MIKAI_DIST_CACHE_DIR}/${mpv_archive_name}")
  if(NOT EXISTS "${mpv_archive}")
    message(STATUS "dist: downloading pinned mpv developer archive")
    file(DOWNLOAD "${mpv_archive_url}" "${mpv_archive}"
      EXPECTED_HASH "SHA256=${mpv_archive_sha256}"
      STATUS mpv_download_status
      SHOW_PROGRESS)
    list(GET mpv_download_status 0 mpv_download_code)
    list(GET mpv_download_status 1 mpv_download_message)
    if(NOT mpv_download_code EQUAL 0)
      file(REMOVE "${mpv_archive}")
      message(FATAL_ERROR "dist: mpv archive download failed: ${mpv_download_message}")
    endif()
  endif()
endif()
file(SHA256 "${mpv_archive}" actual_mpv_archive_sha256)
if(NOT actual_mpv_archive_sha256 STREQUAL mpv_archive_sha256)
  message(FATAL_ERROR
    "dist: mpv archive SHA-256 mismatch for '${mpv_archive}': "
    "expected ${mpv_archive_sha256}, got ${actual_mpv_archive_sha256}")
endif()

set(mpv_extract_dir "${MIKAI_DIST_CACHE_DIR}/extract")
file(REMOVE_RECURSE "${mpv_extract_dir}")
file(MAKE_DIRECTORY "${mpv_extract_dir}")
file(ARCHIVE_EXTRACT INPUT "${mpv_archive}" DESTINATION "${mpv_extract_dir}")
file(GLOB_RECURSE mpv_dll_candidates LIST_DIRECTORIES FALSE "${mpv_extract_dir}/libmpv-2.dll")
list(LENGTH mpv_dll_candidates mpv_dll_count)
if(NOT mpv_dll_count EQUAL 1)
  message(FATAL_ERROR
    "dist: expected exactly one libmpv-2.dll in the verified mpv archive, found ${mpv_dll_count}")
endif()
list(GET mpv_dll_candidates 0 mpv_dll)
file(SIZE "${mpv_dll}" mpv_dll_size)
if(mpv_dll_size LESS 64)
  message(FATAL_ERROR "dist: extracted libmpv-2.dll is too small to be a valid x64 PE image")
endif()
file(READ "${mpv_dll}" mpv_dos_header LIMIT 64 HEX)
string(SUBSTRING "${mpv_dos_header}" 0 4 mpv_dos_signature)
if(NOT mpv_dos_signature STREQUAL "4d5a")
  message(FATAL_ERROR "dist: extracted libmpv-2.dll has no valid DOS/PE signature")
endif()
string(SUBSTRING "${mpv_dos_header}" 120 8 mpv_pe_offset_le)
string(SUBSTRING "${mpv_pe_offset_le}" 0 2 mpv_pe_offset_b0)
string(SUBSTRING "${mpv_pe_offset_le}" 2 2 mpv_pe_offset_b1)
string(SUBSTRING "${mpv_pe_offset_le}" 4 2 mpv_pe_offset_b2)
string(SUBSTRING "${mpv_pe_offset_le}" 6 2 mpv_pe_offset_b3)
math(EXPR mpv_pe_offset "0x${mpv_pe_offset_b3}${mpv_pe_offset_b2}${mpv_pe_offset_b1}${mpv_pe_offset_b0}")
file(READ "${mpv_dll}" mpv_pe_header OFFSET ${mpv_pe_offset} LIMIT 6 HEX)
string(SUBSTRING "${mpv_pe_header}" 0 8 mpv_pe_signature)
string(SUBSTRING "${mpv_pe_header}" 8 4 mpv_pe_machine)
if(NOT mpv_pe_signature STREQUAL "50450000" OR NOT mpv_pe_machine STREQUAL "6486")
  message(FATAL_ERROR "dist: extracted libmpv-2.dll is not a valid x64 PE image")
endif()

get_filename_component(qt_bin_dir "${WINDEPLOYQT}" DIRECTORY)
get_filename_component(qt_prefix "${qt_bin_dir}" DIRECTORY)

file(REMOVE_RECURSE "${DIST_DIR}")
file(MAKE_DIRECTORY "${DIST_DIR}")

file(COPY "${EXE}" DESTINATION "${DIST_DIR}")
get_filename_component(exe_name "${EXE}" NAME)
set(dist_exe "${DIST_DIR}/${exe_name}")

execute_process(
  COMMAND "${WINDEPLOYQT}" --release --translations uk "${dist_exe}"
  RESULT_VARIABLE deploy_rc
  OUTPUT_VARIABLE deploy_out
  ERROR_VARIABLE deploy_err)
if(NOT deploy_rc EQUAL 0)
  message(FATAL_ERROR "dist: windeployqt failed (${deploy_rc}): ${deploy_err}")
endif()

file(COPY "${SHADERS}" DESTINATION "${DIST_DIR}")

file(MAKE_DIRECTORY "${DIST_DIR}/translations")
foreach(catalog qt_uk.qm qtbase_uk.qm)
  set(qt_catalog "${qt_prefix}/translations/${catalog}")
  if(EXISTS "${qt_catalog}")
    file(COPY "${qt_catalog}" DESTINATION "${DIST_DIR}/translations")
  endif()
endforeach()
set(app_catalog "${DIST_DIR}/mikai_uk.qm")
if(EXISTS "${app_catalog}")
  file(RENAME "${app_catalog}" "${DIST_DIR}/translations/mikai_uk.qm")
endif()

file(WRITE "${DIST_DIR}/run.bat" "@echo off\r\nstart \"\" \"%~dp0${exe_name}\" %*\r\n")

execute_process(
  COMMAND git -C "${SOURCE_DIR}" rev-parse HEAD
  RESULT_VARIABLE git_rc
  OUTPUT_VARIABLE git_rev
  ERROR_QUIET
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT git_rc EQUAL 0 OR git_rev STREQUAL "")
  set(git_rev "unknown")
endif()
string(TIMESTAMP built_at "%Y-%m-%dT%H:%M:%SZ" UTC)
file(WRITE "${DIST_DIR}/VERSION" "${git_rev}\n${built_at}\n")

file(MAKE_DIRECTORY "${DIST_DIR}/mpv")
file(COPY_FILE "${mpv_dll}" "${DIST_DIR}/mpv/libmpv-2.dll")
message(STATUS
  "dist: packaged verified x64 libmpv-2.dll from ${mpv_archive_name} "
  "(archive SHA-256 ${mpv_archive_sha256})")

if(MIKAI_DIST_NATIVE)
  set(native_root "${MIKAI_DIST_NATIVE_ROOT}")
  if(native_root STREQUAL "")
    set(native_root "$ENV{LOCALAPPDATA}/Mikai/Mikai Wrapper/native")
  endif()
  if(IS_DIRECTORY "${native_root}/runtime")
    file(MAKE_DIRECTORY "${DIST_DIR}/native")
    file(COPY "${native_root}/runtime" DESTINATION "${DIST_DIR}/native")
    message(STATUS "dist: copied the native runtime from ${native_root}/runtime")
  else()
    message(STATUS "dist: no native runtime at ${native_root}/runtime; skipped")
  endif()
endif()

# The GPU-specific TensorRT components and the licence-restricted model graphs
# are downloaded per machine and proved by digest there. They are never shipped:
# a portable folder or an installer must not carry them. The app-owned stores
# live under %LOCALAPPDATA% and are not inside the tree copied above, but a
# native bundle may carry its own copies, so any that arrived are removed here.
foreach(forbidden components assets)
  file(GLOB_RECURSE stray "${DIST_DIR}/native/*/${forbidden}")
  foreach(path ${stray})
    file(REMOVE_RECURSE "${path}")
    message(STATUS "dist: removed ${path}; downloadable elements are never shipped")
  endforeach()
endforeach()

message(STATUS "dist: assembled ${DIST_DIR}")
