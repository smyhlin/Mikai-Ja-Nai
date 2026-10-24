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
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)
endif()

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

# A private extraction directory prevents concurrent staging jobs from removing
# each other's input. Never recursively remove a caller-provided cache path.
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef mpv_extract_id)
set(mpv_extract_dir "${MIKAI_DIST_CACHE_DIR}/extract-${mpv_extract_id}")
if(EXISTS "${mpv_extract_dir}")
  message(FATAL_ERROR "dist: extraction directory collision; retry with fresh staging")
endif()
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

if(MIKAI_PUBLIC_RELEASE)
  if(MIKAI_DIST_NATIVE)
    message(FATAL_ERROR "dist: public packages cannot contain an optional native runtime")
  endif()
  foreach(legal_file LICENSE packaging/third-party.json)
    if(NOT EXISTS "${SOURCE_DIR}/${legal_file}")
      message(FATAL_ERROR "dist: missing release legal material: ${legal_file}")
    endif()
  endforeach()
  if(NOT IS_DIRECTORY "${SOURCE_DIR}/packaging/licenses")
    message(FATAL_ERROR "dist: missing third-party license directory")
  endif()
  if(NOT MIKAI_RELEASE_OBJDUMP OR NOT EXISTS "${MIKAI_RELEASE_OBJDUMP}")
    message(FATAL_ERROR "dist: public dependency closure requires the configured MinGW objdump")
  endif()
endif()

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
if(IS_SYMLINK "${DIST_DIR}")
  message(FATAL_ERROR "dist: refusing a symbolic-link staging directory: ${DIST_DIR}")
endif()
if(EXISTS "${DIST_DIR}")
  if(NOT IS_DIRECTORY "${DIST_DIR}")
    message(FATAL_ERROR "dist: staging path is not a directory: ${DIST_DIR}")
  endif()
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
  file(REAL_PATH "${DIST_DIR}" dist_abs)
  file(REAL_PATH "${dist_native_root}" input_abs)
  set(dist_abs_compare "${dist_abs}")
  set(input_abs_compare "${input_abs}")
  if(CMAKE_HOST_WIN32)
    string(TOLOWER "${dist_abs}" dist_abs_compare)
    string(TOLOWER "${input_abs}" input_abs_compare)
  endif()
  cmake_path(IS_PREFIX input_abs_compare "${dist_abs_compare}" NORMALIZE dist_inside_input)
  cmake_path(IS_PREFIX dist_abs_compare "${input_abs_compare}" NORMALIZE input_inside_dist)
  if(dist_inside_input)
    message(FATAL_ERROR
      "dist: MIKAI_DIST_DIR (${dist_abs}) must not be inside MIKAI_DIST_NATIVE_ROOT "
      "(${input_abs}); the script rewrites its output")
  endif()
  if(input_inside_dist)
    message(FATAL_ERROR
      "dist: MIKAI_DIST_NATIVE_ROOT (${input_abs}) must not be inside MIKAI_DIST_DIR "
      "(${dist_abs}); the script rewrites its output")
  endif()
endif()

file(MAKE_DIRECTORY "${DIST_DIR}")

file(COPY "${EXE}" DESTINATION "${DIST_DIR}")
get_filename_component(exe_name "${EXE}" NAME)
set(dist_exe "${DIST_DIR}/${exe_name}")

set(deploy_arguments --release --translations uk)
if(MIKAI_PUBLIC_RELEASE)
  # Plugin inputs are copied by the explicit allowlist below. This avoids
  # silently adding a new optional codec/TLS dependency after a Qt update.
  list(APPEND deploy_arguments --compiler-runtime --no-plugins --no-opengl-sw
    --no-system-d3d-compiler --no-system-dxc-compiler)
endif()
execute_process(
  COMMAND "${WINDEPLOYQT}" ${deploy_arguments} "${dist_exe}"
  RESULT_VARIABLE deploy_rc
  OUTPUT_VARIABLE deploy_out
  ERROR_VARIABLE deploy_err)
if(NOT deploy_rc EQUAL 0)
  message(FATAL_ERROR "dist: windeployqt failed (${deploy_rc}): ${deploy_err}")
endif()

if(MIKAI_PUBLIC_RELEASE)
  file(MAKE_DIRECTORY "${DIST_DIR}/shaders")
  file(GLOB shader_files LIST_DIRECTORIES FALSE "${SHADERS}/*.glsl")
  foreach(shader IN LISTS shader_files)
    if(IS_SYMLINK "${shader}")
      message(FATAL_ERROR "dist: shader symlinks are not accepted in public packages")
    endif()
    file(COPY "${shader}" DESTINATION "${DIST_DIR}/shaders")
  endforeach()
else()
  file(COPY "${SHADERS}" DESTINATION "${DIST_DIR}")
endif()

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
else()
  get_filename_component(exe_dir "${EXE}" DIRECTORY)
  set(built_catalog "${exe_dir}/mikai_uk.qm")
  if(EXISTS "${built_catalog}")
    file(COPY "${built_catalog}" DESTINATION "${DIST_DIR}/translations")
  else()
    # The catalog is embedded in the executable; a missing side-car only removes
    # the ability to override it with an external file.
    message(WARNING
      "dist: no application catalogue next to ${exe_name}; the package relies on "
      "the embedded catalog at :/i18n/mikai_uk.qm")
  endif()
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
# Keep the private VERSION contract; public artifacts additionally identify the
# product/toolchain and ship a hash inventory written after all payload copies.
if(MIKAI_PUBLIC_RELEASE)
  file(WRITE "${DIST_DIR}/VERSION"
    "Mikai ${MIKAI_RELEASE_VERSION}\n${git_rev}\n${built_at}\nQt ${MIKAI_RELEASE_QT_VERSION}\n${MIKAI_RELEASE_COMPILER}\n")
else()
  file(WRITE "${DIST_DIR}/VERSION" "${git_rev}\n${built_at}\n")
endif()

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

if(MIKAI_PUBLIC_RELEASE)
  file(COPY_FILE "${SOURCE_DIR}/LICENSE" "${DIST_DIR}/LICENSE")
  file(COPY_FILE "${SOURCE_DIR}/packaging/third-party.json" "${DIST_DIR}/third-party.json")
  file(COPY "${SOURCE_DIR}/packaging/licenses" DESTINATION "${DIST_DIR}")
  file(WRITE "${DIST_DIR}/qt.conf" "[Paths]\nPrefix=.\nPlugins=.\nTranslations=translations\n")

  # Schannel uses the OS certificate store; do not ship an unproven OpenSSL
  # DLL or a plugin that would silently depend on a developer's PATH.
  set(required_plugins platforms/qwindows.dll imageformats/qjpeg.dll
    imageformats/qwebp.dll tls/qschannelbackend.dll)
  set(optional_plugins imageformats/qgif.dll imageformats/qico.dll
    imageformats/qsvg.dll iconengines/qsvgicon.dll tls/qcertonlybackend.dll
    styles/qmodernwindowsstyle.dll styles/qwindowsvistastyle.dll
    networkinformation/qnetworklistmanager.dll)
  foreach(plugin IN LISTS required_plugins optional_plugins)
    if(EXISTS "${qt_prefix}/plugins/${plugin}")
      get_filename_component(plugin_type "${plugin}" DIRECTORY)
      file(MAKE_DIRECTORY "${DIST_DIR}/${plugin_type}")
      file(COPY "${qt_prefix}/plugins/${plugin}" DESTINATION "${DIST_DIR}/${plugin_type}")
    elseif(plugin IN_LIST required_plugins)
      message(FATAL_ERROR "dist: required Qt plugin is not installed: ${plugin}")
    endif()
  endforeach()

  # LoadLibrary dependencies (mpv and plugins) are not reachable by scanning
  # mikai.exe alone. Scan every deployed PE and close the non-system imports.
  get_filename_component(compiler_bin "${MIKAI_RELEASE_COMPILER_BIN}" DIRECTORY)
  foreach(runtime libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
    if(NOT EXISTS "${DIST_DIR}/${runtime}")
      if(EXISTS "${qt_bin_dir}/${runtime}")
        file(COPY "${qt_bin_dir}/${runtime}" DESTINATION "${DIST_DIR}")
      elseif(EXISTS "${compiler_bin}/${runtime}")
        file(COPY "${compiler_bin}/${runtime}" DESTINATION "${DIST_DIR}")
      else()
        message(FATAL_ERROR "dist: missing matching MinGW runtime ${runtime}")
      endif()
    endif()
    if(EXISTS "${qt_bin_dir}/${runtime}")
      file(SHA256 "${qt_bin_dir}/${runtime}" qt_runtime_hash)
      file(SHA256 "${DIST_DIR}/${runtime}" deployed_runtime_hash)
      if(NOT qt_runtime_hash STREQUAL deployed_runtime_hash)
        message(FATAL_ERROR "dist: ${runtime} does not match the runtime provided with Qt")
      endif()
    endif()
  endforeach()
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM windows+pe)
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL objdump)
  set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${MIKAI_RELEASE_OBJDUMP}")
  file(GLOB_RECURSE deployed_libraries LIST_DIRECTORIES FALSE "${DIST_DIR}/*.dll")
  get_filename_component(mpv_dll_dir "${mpv_dll}" DIRECTORY)
  file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${dist_exe}"
    LIBRARIES ${deployed_libraries}
    DIRECTORIES "${DIST_DIR}" "${qt_bin_dir}" "${compiler_bin}" "${mpv_dll_dir}"
    PRE_EXCLUDE_REGEXES "^[aA][pP][iI]-" "^[eE][xX][tT]-"
    POST_EXCLUDE_REGEXES ".*[/\\\\]([Ss][Yy][Ss][Tt][Ee][Mm]32|[Ss][Yy][Ss][Ww][Oo][Ww]64)[/\\\\].*"
    RESOLVED_DEPENDENCIES_VAR resolved_dependencies
    UNRESOLVED_DEPENDENCIES_VAR unresolved_dependencies
    CONFLICTING_DEPENDENCIES_PREFIX dependency_conflicts)
  if(unresolved_dependencies OR dependency_conflicts_FILENAMES)
    message(FATAL_ERROR
      "dist: dependency closure failed; unresolved=${unresolved_dependencies}; conflicting=${dependency_conflicts_FILENAMES}")
  endif()
  file(TO_CMAKE_PATH "$ENV{SystemRoot}" system_root)
  string(TOLOWER "${system_root}" system_root)
  if(system_root STREQUAL "")
    message(FATAL_ERROR "dist: SystemRoot is unavailable for dependency classification")
  endif()
  foreach(dependency IN LISTS resolved_dependencies)
    file(TO_CMAKE_PATH "${dependency}" dependency_path)
    string(TOLOWER "${dependency_path}" dependency_lower)
    cmake_path(IS_PREFIX system_root "${dependency_lower}" NORMALIZE system_dependency)
    if(NOT system_dependency)
      get_filename_component(dependency_name "${dependency}" NAME)
      # Only known runtime families are redistributable inputs to this package.
      # An unknown mpv import needs provenance review, not a directory-wide copy.
      if(NOT dependency_name MATCHES "^(Qt6(Core|Gui|Widgets|Network|OpenGL|OpenGLWidgets|Svg)|libgcc_s_seh-1|libstdc\\+\\+-6|libwinpthread-1|libmpv-2)\\.dll$")
        message(FATAL_ERROR "dist: dependency outside the reviewed runtime allowlist: ${dependency_name}")
      endif()
      if(NOT dependency_name STREQUAL "libmpv-2.dll")
        if(EXISTS "${DIST_DIR}/${dependency_name}")
          file(SHA256 "${dependency}" dependency_hash)
          file(SHA256 "${DIST_DIR}/${dependency_name}" staged_hash)
          if(NOT dependency_hash STREQUAL staged_hash)
            message(FATAL_ERROR "dist: conflicting deployed dependency ${dependency_name}")
          endif()
        else()
          file(COPY "${dependency}" DESTINATION "${DIST_DIR}")
        endif()
      endif()
    endif()
  endforeach()

  foreach(required Qt6Core.dll Qt6Gui.dll Qt6Widgets.dll Qt6Network.dll
      Qt6OpenGL.dll Qt6OpenGLWidgets.dll platforms/qwindows.dll
      imageformats/qjpeg.dll imageformats/qwebp.dll tls/qschannelbackend.dll
      libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll mpv/libmpv-2.dll)
    if(NOT EXISTS "${DIST_DIR}/${required}")
      message(FATAL_ERROR "dist: required public runtime file is missing: ${required}")
    endif()
  endforeach()

  # This validates the entire resulting tree, not just names excluded from a
  # broad source copy. User settings, stores, probes and neural graphs cannot
  # enter an artifact via a dirty build/deployment directory.
  file(GLOB_RECURSE public_entries RELATIVE "${DIST_DIR}" LIST_DIRECTORIES TRUE "${DIST_DIR}/*")
  set(public_inventory "")
  foreach(entry IN LISTS public_entries)
    if(IS_SYMLINK "${DIST_DIR}/${entry}")
      message(FATAL_ERROR "dist: public payload contains a symbolic link: ${entry}")
    endif()
    if(IS_DIRECTORY "${DIST_DIR}/${entry}")
      continue()
    endif()
    if(entry MATCHES "^(mikai\\.exe|run\\.bat|VERSION|qt\\.conf|LICENSE|third-party\\.json)$")
      set(owner mikai)
    elseif(entry MATCHES "^Qt6(Core|Gui|Widgets|Network|OpenGL|OpenGLWidgets|Svg)\\.dll$"
        OR entry MATCHES "^(platforms/qwindows|styles/q(modernwindowsstyle|windowsvistastyle)|iconengines/qsvgicon|networkinformation/qnetworklistmanager|tls/q(schannelbackend|certonlybackend)|imageformats/q(gif|ico|jpeg|svg|tga|tiff|wbmp|webp|icns))\\.dll$"
        OR entry MATCHES "^translations/(qt|qtbase)_uk\\.qm$")
      set(owner qt)
    elseif(entry MATCHES "^(libgcc_s_seh-1|libstdc\\+\\+-6|libwinpthread-1)\\.dll$")
      set(owner mingw)
    elseif(entry STREQUAL "mpv/libmpv-2.dll")
      set(owner mpv)
    elseif(entry MATCHES "^shaders/[A-Za-z0-9_-]+\\.glsl$")
      set(owner anime4k)
    elseif(entry STREQUAL "translations/mikai_uk.qm")
      set(owner mikai)
    elseif(entry MATCHES "^licenses/[A-Za-z0-9_./+-]+\\.(txt|json|md|html)$"
        OR entry MATCHES "^licenses/ffmpeg/COPYING\\.(GPLv[23]|LGPLv(2\\.1|3))$")
      set(owner legal)
    else()
      message(FATAL_ERROR "dist: public payload is not allowlisted: ${entry}")
    endif()
    file(SHA256 "${DIST_DIR}/${entry}" payload_hash)
    string(APPEND public_inventory "${payload_hash}  ${entry}\n")
  endforeach()
  file(WRITE "${DIST_DIR}/PACKAGE-CONTENTS.sha256" "${public_inventory}")
  message(STATUS "dist: public payload allowlist and non-system dependency closure complete")
endif()

# Only our random scratch extraction is removed, never the cache or inputs.
file(REMOVE_RECURSE "${mpv_extract_dir}")
message(STATUS "dist: assembled ${DIST_DIR}")
