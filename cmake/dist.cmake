# Assembles the self-contained dist folder. Run through the `dist` target:
#   cmake --build <build-dir> --target dist
# Required inputs: DIST_DIR, EXE, WINDEPLOYQT, SHADERS, SOURCE_DIR.
# Optional: MIKAI_DIST_NATIVE=ON copies the installed native runtime, defaulting
# to the executable-local store that production nativeRoot() uses
# (<EXE dir>/native); MIKAI_DIST_NATIVE_ROOT overrides that root.
# The mpv runtime is always the SHA-pinned API DLL below.
#
# Two safety boundaries this script enforces rather than assumes:
#   * MIKAI_DIST_DIR must be fresh. An existing populated directory is a refusal,
#     never a wipe: the in-place update path owns installed state, so a
#     verification run points DIST_DIR at scratch space.
#   * MIKAI_DIST_DIR must not be inside MIKAI_DIST_NATIVE_ROOT, nor the reverse.
# 
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

# Production nativeRoot() is executable-local (NativePaths.cpp), so the optional
# copy follows the same rule and derives its default from the staged executable
# rather than an obsolete %LOCALAPPDATA% profile path. An explicit
# MIKAI_DIST_NATIVE_ROOT overrides it; an absent store is decided below, only
# when the copy was actually requested.
set(dist_native_root "${MIKAI_DIST_NATIVE_ROOT}")
if(dist_native_root STREQUAL "")
  get_filename_component(dist_exe_dir "${EXE}" DIRECTORY)
  set(dist_native_root "${dist_exe_dir}/native")
endif()
# The executable and its store are the inputs; the assembly must never delete
# its own input. Creating ${DIST_DIR} by first removing anything already there
# is the behaviour that destroyed adjacent installed state, so the script now
# refuses to recreate any directory that already carries an installed app.
if(EXISTS "${DIST_DIR}")
  file(GLOB dist_existing LIST_DIRECTORIES TRUE "${DIST_DIR}/*")
  if(dist_existing)
    list(JOIN dist_existing "\n  " dist_existing_text)
    message(FATAL_ERROR
      "dist: refusing to recreate ${DIST_DIR} - it already contains:\n  ${dist_existing_text}\n"
      "The installer updates the app in place, so this script must never wipe an "
      "existing installation. Choose a fresh MIKAI_DIST_DIR (for example a "
      "scratch directory) for verification runs, or move the old folder aside "
      "deliberately.")
  endif()
endif()

# Both inputs are validated before the first write. A refusal that fired after
# the output had been assembled would already have written a folder into the
# input tree it exists to protect.
if(MIKAI_DIST_NATIVE)
  # Executable-local is the production rule (see above); a missing runtime under
  # an explicitly requested root is a hard failure, not a silent skip, because
  # "MIKAI_DIST_NATIVE=ON packaged nothing" is exactly the failure the option
  # exists to prevent.
  if(NOT IS_DIRECTORY "${dist_native_root}/runtime")
    message(FATAL_ERROR
      "dist: the native runtime was not found at ${dist_native_root}/runtime. Nothing was "
      "assembled. Set MIKAI_DIST_NATIVE_ROOT to the executable-local native store that "
      "actually holds it, or turn MIKAI_DIST_NATIVE off.")
  endif()
  # The output must not be inside the input: copying a store into itself would
  # let a later strip walk the real installed tree.
  get_filename_component(dist_abs "${DIST_DIR}" ABSOLUTE)
  get_filename_component(input_abs "${dist_native_root}" ABSOLUTE)
  if(dist_abs STREQUAL input_abs OR dist_abs MATCHES "^${input_abs}/")
    message(FATAL_ERROR
      "dist: MIKAI_DIST_DIR (${dist_abs}) must not be inside MIKAI_DIST_NATIVE_ROOT "
      "(${input_abs}); the script rewrites its output")
  endif()
  if(input_abs MATCHES "^${dist_abs}/")
    message(FATAL_ERROR
      "dist: MIKAI_DIST_NATIVE_ROOT (${input_abs}) must not be inside MIKAI_DIST_DIR "
      "(${dist_abs}); the script rewrites its output")
  endif()
endif()

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
  file(MAKE_DIRECTORY "${DIST_DIR}/native")
  file(COPY "${dist_native_root}/runtime" DESTINATION "${DIST_DIR}/native")
  message(STATUS "dist: copied the native runtime from ${dist_native_root}/runtime")
endif()

# The GPU-specific TensorRT components and the licence-restricted model graphs
# are downloaded per machine and proved by digest there. They are never shipped:
# a portable folder or an installer must not carry them. The app-owned stores
# live under %LOCALAPPDATA% and are not inside the tree copied above, but a
# native bundle may carry its own copies, so any that arrived are removed here.
#
# The sweep is name-based over the whole copied tree, never a glob whose
# expression *ends* in the forbidden name: file(GLOB_RECURSE) recurses from the
# glob's base and reports the literal's parents and siblings too, so removing
# what such a glob returns deletes the runtime the payload arrived in. Each
# subtree is enumerated once, the directories whose own name is forbidden are
# collected, and only those are removed - a nested occurrence is found because
# the enumeration recurses, not because the name is repeated in the pattern.
function(dist_collect_forbidden out_var)
  file(GLOB_RECURSE native_entries LIST_DIRECTORIES TRUE "${DIST_DIR}/native/*")
  set(found)
  foreach(entry ${native_entries})
    if(IS_DIRECTORY "${entry}")
      get_filename_component(entry_name "${entry}" NAME)
      if(entry_name IN_LIST dist_forbidden)
        list(APPEND found "${entry}")
      endif()
    endif()
  endforeach()
  list(REMOVE_DUPLICATES found)
  set(${out_var} "${found}" PARENT_SCOPE)
endfunction()

set(dist_forbidden components assets models records)
dist_collect_forbidden(dist_forbidden_entries)
foreach(path ${dist_forbidden_entries})
  file(REMOVE_RECURSE "${path}")
  message(STATUS "dist: removed ${path}; downloadable elements are never shipped")
endforeach()

# Nothing above is allowed to be skipped silently. Verify rather than assume.
dist_collect_forbidden(dist_stowaway)
if(dist_stowaway)
  list(JOIN dist_stowaway "\n  " dist_stowaway_text)
  message(FATAL_ERROR
    "dist: the assembled folder still contains forbidden payload:\n  ${dist_stowaway_text}\n"
    "These are per-machine downloads, never shipped bytes.")
endif()

message(STATUS "dist: assembled ${DIST_DIR}")
