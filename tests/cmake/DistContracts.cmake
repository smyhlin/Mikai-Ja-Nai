# Behavioural regression for the dist assembly contract (cmake/dist.cmake).
#
# Deterministic standalone CMake: it runs the PRODUCTION script
# cmake/dist.cmake as a child interpreter over synthetic inputs it writes
# itself, and asserts only observable outcomes - what exists on disk after the
# run, what bytes survived, whether the run was accepted or refused, and which
# message a failure names. No source text of the script is inspected, so this
# file cannot pass by pattern instead of by behaviour.
#
# Scenarios:
#   1. A populated MIKAI_DIST_DIR is refused, and every byte in it survives.
#   2. An existing but empty MIKAI_DIST_DIR is accepted and assembled.
#   3. MIKAI_DIST_DIR == MIKAI_DIST_NATIVE_ROOT is refused before any write.
#   4. MIKAI_DIST_DIR nested inside MIKAI_DIST_NATIVE_ROOT is refused.
#   5. MIKAI_DIST_NATIVE_ROOT nested inside MIKAI_DIST_DIR is refused.
#   6. MIKAI_DIST_NATIVE=ON with no <root>/runtime fails by name, and the
#      standard output is still produced by that run (the failure is the
#      native one, not a silent success).
#   7. A valid assembly copies the runtime and strips the downloadable /
#      licence-restricted payload directories - at the direct child level AND
#      nested - while ordinary runtime files and directories survive, and the
#      source tree is untouched.
#
# Two findings this contract caught while it was being built, both of which the
# production script has since been corrected for. They are recorded because each
# is a regression this file is now the guard against, and because a contract
# that only ever passes cannot tell you which of them has come back:
#
#   A. THE STRIP DELETED THE RUNTIME. The sweep used file(GLOB_RECURSE) with a
#      trailing literal, e.g. "${DIST_DIR}/native/components". A recursive glob
#      recurses from the glob's BASE rather than from the literal, so a hit at
#      native/runtime/b1/components also returned native/runtime,
#      native/runtime/b1 and every sibling across the runtime tree. The
#      file(REMOVE_RECURSE) that followed then deleted native/runtime itself -
#      the entire bundled runtime, ordinary files and all, leaving dist/native
#      empty. The post-sweep "stowaway" verification re-globbed the same way and
#      over-matched identically, so it could never catch the damage it had just
#      done. The script now enumerates "${DIST_DIR}/native/*" once, collects the
#      directories whose own NAME is forbidden, and removes only those, so a
#      nested occurrence is found because the enumeration recurses, not because
#      the name is repeated in the pattern. Scenario 7 asserts the ordinary
#      plugins, translations and sibling directories survive beside the payload
#      that did not.
#
#   B. THE OVERLAP REFUSAL CAME AFTER THE DAMAGE. The output/native-root overlap
#      guards ran at the very END of the script, inside if(MIKAI_DIST_NATIVE)
#      and after the output had been fully assembled (MAKE_DIRECTORY, EXE copy,
#      windeployqt, shaders, VERSION, run.bat, mpv). An overlapping run was
#      therefore refused only after it had written a complete folder inside the
#      input tree it exists to protect. The script now runs both guards before
#      the first write, and scenario 6 additionally asserts that a
#      missing-runtime refusal leaves the output directory with nothing in it.
#
# Both fixes were verified through this file by observation, not by reading the
# script: the failure signatures above were produced, then disappeared.
#
# Ownership: every path this file creates lives under one scratch directory
# that this file and nothing else owns. It never touches the repository build
# tree, dist-mikai, build/release/native, any existing native root, the pinned
# mpv archive or the real shaders directory - the shaders, executable and mpv
# inputs are synthetic copies written into scratch. Every child run is launched
# from scratch with its own working directory, so a future test can never
# inherit a real DIST_DIR or native asset root from the environment.
#
# Variables the parent's add_test must pass:
#   DIST_CONTRACT_PROJECT=ON                             gate
#   DIST_CONTRACT_SCRIPT=<abs path to cmake/dist.cmake>  the script under test
#   DIST_CONTRACT_PE_SOURCE=<abs path to a genuine x64 PE DLL>
#       The production script verifies that the mpv DLL it packages is a real
#       x64 PE image (MZ signature, PE signature, machine 0x8664), so a text
#       file cannot stand in for one. The member inside the synthetic mpv
#       archive is named libmpv-2.dll and its bytes are a read-only copy of this
#       file, and MIKAI_MPV_ARCHIVE_SHA256 is the SHA-256 of that synthetic 7z
#       itself - so the local-archive override path is exercised end to end
#       without a network fetch and without the pinned release archive.
#       OPTIONAL. The test resolves a genuine PE on its own in this order:
#       DIST_CONTRACT_PE_SOURCE, the repo's dist-mikai/libwinpthread-1.dll, the
#       configured Qt bin dir, then any x64 runtime DLL in build/. If none is
#       present it reports a missing prerequisite rather than passing on a fake
#       module.
#   DIST_CONTRACT_SCRATCH_ROOT (optional)                override the scratch root
#
# Cross-platform: the windeployqt stub is a .cmd on Windows (execute_process
# runs a .cmd directly) and a .sh elsewhere, and the app binary gets the
# platform's own extension, so this contract can be registered on any host.
#
# Run:
#   cmake -DDIST_CONTRACT_PROJECT=ON \
#         -DDIST_CONTRACT_SCRIPT=<abs path to cmake/dist.cmake> \
#         -DDIST_CONTRACT_PE_SOURCE=<abs path to a genuine x64 PE DLL> \
#         -P tests/cmake/DistContracts.cmake
# Exit 0 = every scenario passed; nonzero = a scenario failed or the harness broke.


if(NOT DIST_CONTRACT_PROJECT)
  return()
endif()

set(_prefix "DistContracts")

# ------------------------------------------------------------------ reporting
# `_check <condition...> <message>`: every token but the last is the condition,
# so a multi-token expression such as "NOT _rc EQUAL 0" is evaluated exactly as
# written. A macro keeps the tokens distinguishable while ARGN is unrolled.
macro(_check)
  set(_check_args "${ARGN}")
  list(POP_BACK _check_args _check_message)
  if(NOT _check_args)
    set(_check_args "0")
  endif()
  if(${_check_args})
    message(STATUS "${_prefix}: PASS ${_check_message}")
  else()
    message(SEND_ERROR "${_prefix}: FAIL ${_check_message}")
  endif()
endmacro()

# ------------------------------------------------------------------ scratch
# Own exactly one directory; never operate on anything that is not ours.
if(DIST_CONTRACT_SCRATCH_ROOT)
  set(_scratch "${DIST_CONTRACT_SCRATCH_ROOT}")
elseif(CMAKE_CURRENT_BINARY_DIR)
  set(_scratch "${CMAKE_CURRENT_BINARY_DIR}/_dist-contract")
else()
  file(TO_CMAKE_PATH "$ENV{TEMP}" _scratch)
  if(_scratch STREQUAL "")
    set(_scratch "/tmp")
  endif()
  set(_scratch "${_scratch}/dist-contract")
endif()

if(EXISTS "${_scratch}")
  if(NOT (IS_DIRECTORY "${_scratch}" AND EXISTS "${_scratch}/.scratch-owned"))
    message(SEND_ERROR
      "${_prefix}: FAIL scratch root ${_scratch} exists and is not owned by this "
      "contract; refusing to use it")
    return()
  endif()
  file(REMOVE_RECURSE "${_scratch}")
endif()
file(MAKE_DIRECTORY "${_scratch}")
file(WRITE "${_scratch}/.scratch-owned" "dist-contract scratch\n")

# Everything destructive happens inside a per-scenario subdirectory, so a run
# can never reach a sibling tree even if it walks out of its own output.
set(_cases "${_scratch}/cases")

set(_script "${DIST_CONTRACT_SCRIPT}")
if(NOT EXISTS "${_script}")
  message(SEND_ERROR "${_prefix}: FAIL DIST_CONTRACT_SCRIPT not found: ${_script}")
  return()
endif()
get_filename_component(_script "${_script}" ABSOLUTE)

# The child: set the inherited -D variables, then run the production script
# exactly the way the `dist` target does. A FATAL_ERROR in the script is an
# observable nonzero exit here rather than the end of this file.
set(_child "${_scratch}/dist-child.cmake")
file(WRITE "${_child}" "include(\"${_script}\")\n")

# --------------------------------------------------------------- run helper
# Runs the production script in a fresh interpreter for one scenario.
# Args: <out_rc> <out_out> <out_err> <name> <DEFINE=VALUE>...
#
# Each scenario gets its own working directory inside the case root, so a child
# that changes directory or writes a stray file cannot affect another scenario.
# TIMEOUT bounds a hung child: the script is configure-scale, so 180 s is
# generous and still surfaces a hang.
function(_run_dist out_rc out_out out_err name)
  set(_workdir "${_cases}/${name}")
  file(MAKE_DIRECTORY "${_workdir}")
  set(_cmd_args "-DDIST_CONTRACT_NAME=${name}")
  foreach(_def ${ARGN})
    list(APPEND _cmd_args "-D${_def}")
  endforeach()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_cmd_args} -P "${_child}"
    WORKING_DIRECTORY "${_workdir}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    TIMEOUT 180)
  set(${out_rc} "${_rc}" PARENT_SCOPE)
  set(${out_out} "${_out}" PARENT_SCOPE)
  set(${out_err} "${_err}" PARENT_SCOPE)
endfunction()

# ===========================================================================
# Synthetic inputs
#
# The genuine x64 PE image is referenced read-only and never hand-copied: an
# archive round-trip produces the byte-identical synthetic DLL that the script
# validates. Text fixtures are cheap and written directly.
# ===========================================================================
# A genuine x64 PE image, read only. The production script validates that the
# member inside the mpv archive is a real x64 PE (MZ, PE signature, machine
# 0x8664, size >= 64), so nothing synthetic may stand in for it: the archive
# route below is also the only way to give the script a "local mpv archive"
# whose SHA-256 it can verify against, which keeps this test off the network
# and off the pinned release.
#
# Resolution order, so the test survives a fresh clone where dist-mikai/ is not
# built (it is gitignored):
#   1. DIST_CONTRACT_PE_SOURCE, if the parent supplies one.
#   2. The repository's own deployed MinGW runtime DLL, the natural read-only
#      source - it IS the shipped compiler runtime, so nothing new is invented.
#   3. The configured Qt bin directory, where a genuine x64 runtime DLL lives
#      beside Qt6Core.dll.
#   4. Any x64 DLL in the configured build tree.
# A text fixture is never used: if none of these exist the scenario reports a
# missing prerequisite instead of passing on a fake module.
set(_pe_candidates)
if(DIST_CONTRACT_PE_SOURCE)
  list(APPEND _pe_candidates "${DIST_CONTRACT_PE_SOURCE}")
endif()
list(APPEND _pe_candidates
  "${CMAKE_CURRENT_SOURCE_DIR}/dist-mikai/libwinpthread-1.dll"
  "${CMAKE_CURRENT_SOURCE_DIR}/dist-mikai/libgcc_s_seh-1.dll")
if(WINDEPLOYQT_BIN_DIR)
  list(APPEND _pe_candidates
    "${WINDEPLOYQT_BIN_DIR}/libwinpthread-1.dll"
    "${WINDEPLOYQT_BIN_DIR}/libgcc_s_seh-1.dll"
    "${WINDEPLOYQT_BIN_DIR}/Qt6Core.dll")
endif()
if(MIKAI_DIST_CONTRACT_PE_SOURCE)
  list(APPEND _pe_candidates "${MIKAI_DIST_CONTRACT_PE_SOURCE}")
endif()
file(GLOB_RECURSE _pe_glob LIST_DIRECTORIES FALSE
  "${CMAKE_CURRENT_SOURCE_DIR}/build/*/libwinpthread-1.dll"
  "${CMAKE_CURRENT_SOURCE_DIR}/build/*/libgcc_s_seh-1.dll")
list(APPEND _pe_candidates ${_pe_glob})

set(_pe_source "")
foreach(_candidate IN LISTS _pe_candidates)
  if(EXISTS "${_candidate}")
    set(_pe_source "${_candidate}")
    break()
  endif()
endforeach()
if(_pe_source STREQUAL "")
  message(SEND_ERROR
    "${_prefix}: FAIL no genuine x64 PE DLL was available to use as the archive "
    "member. The production script validates that the packaged mpv DLL is a "
    "real x64 PE image, so a text file cannot stand in for one. Provide one via "
    "-DDIST_CONTRACT_PE_SOURCE=<path to a genuine x64 PE DLL> "
    "(dist-mikai/libwinpthread-1.dll is the default source).")
  return()
endif()
message(STATUS
  "${_prefix}: using the genuine x64 PE image at ${_pe_source} as the "
  "read-only archive member")

# Shaders: real names, synthetic bytes.
set(_shaders_src "${_scratch}/input/shaders")
file(MAKE_DIRECTORY "${_shaders_src}")
foreach(shader Anime4K_Clamp_Highlights.glsl Anime4K_Upscale_x2.glsl README.txt)
  file(WRITE "${_shaders_src}/${shader}" "synthetic shader fixture\n")
endforeach()

# The executable: a genuine PE image under the app's name. windeployqt is a
# stub, so its content is irrelevant to the script - but using a real PE keeps
# the EXE input from being a fabricated "module" standing in for the app.
set(_bin_dir "${_scratch}/input/bin")
file(MAKE_DIRECTORY "${_bin_dir}")
get_filename_component(_pe_name "${_pe_source}" NAME)
# The app binary name, and the executable extension that names it, are the
# platform's own: the production script derives the staged executable from EXE's
# name, so a wrong extension here would silently change what the strip runs on.
if(WIN32)
  set(_exe_name "mikai.exe")
else()
  set(_exe_name "mikai")
endif()
file(COPY "${_pe_source}" DESTINATION "${_bin_dir}")
file(RENAME "${_bin_dir}/${_pe_name}" "${_bin_dir}/${_exe_name}")
set(_exe_path "${_bin_dir}/${_exe_name}")

# A stub windeployqt: the real one is a 250 KB Qt tool that rewrites a Qt
# binary dependency tree - a platform prerequisite this contract deliberately
# does not own. It must exist and be runnable, and it must be recognisable as a
# stub so a run's provenance is never mistaken for a real deployment. It writes
# the ordinary Qt plugin/platform/style trees the real tool creates, so the
# strip boundary is exercised against a realistic layout.
set(_stub_dir "${_scratch}/input/windeployqt")
file(MAKE_DIRECTORY "${_stub_dir}")
# The production invocation is
#   windeployqt --release --translations uk <staged exe>
# so the executable is the LAST argument, not the first. The stub therefore scans
# the whole argument list for the first non-option token that exists, takes its
# directory (the dist directory), and writes the ordinary Qt plugin trees the real
# tool creates - so the strip boundary is exercised against a realistic layout
# rather than a bare directory.
#
# The stub is a .cmd on Windows (execute_process runs a .cmd directly, and a
# Windows-only contract registered unconditionally would otherwise fail to launch
# on any other platform) and a .sh elsewhere, so the contract can be registered
# unconditionally without silently losing coverage.
if(WIN32)
  set(_stub_bin "${_stub_dir}/windeployqt.cmd")
  file(WRITE "${_stub_bin}"
"@echo off\n"
"set \"_D=\"\n"
"set \"_ARGS=%*\"\n"
"if \"%_ARGS%\"==\"\" exit /b 0\n"
"for %%A in (%_ARGS%) do (\n"
"  echo %%~A | findstr /r \"^--\" >nul\n"
"  if errorlevel 1 (\n"
"    if exist \"%%~A\" set \"_D=%%~dpA\"\n"
"  )\n"
")\n"
"if \"%_D%\"==\"\" exit /b 0\n"
"mkdir \"%_D%platforms\" 2>nul\n"
"echo stub > \"%_D%platforms\\qwindows.dll\"\n"
"mkdir \"%_D%styles\" 2>nul\n"
"echo stub > \"%_D%styles\\qwindowsvistastyle.dll\"\n"
"mkdir \"%_D%generic\" 2>nul\n"
"echo stub > \"%_D%generic\\qtuiotouchplugin.dll\"\n"
"echo DistContracts windeployqt stub ran\n"
"exit /b 0\n")
else()
  set(_stub_bin "${_stub_dir}/windeployqt.sh")
  file(WRITE "${_stub_bin}"
"#!/bin/sh\n"
"# DistContracts stub for windeployqt: the executable is the last argument.\n"
"out=\"\"\n"
"for arg in \"$@\"; do\n"
"  case \"$arg\" in\n"
"    --*) ;;\n"
"    *) if [ -e \"$arg\" ]; then out=\"$(dirname \"$arg\")\"; fi ;;\n"
"  esac\n"
"done\n"
"if [ -z \"$out\" ]; then exit 0; fi\n"
"mkdir -p \"$out/platforms\" \"$out/styles\" \"$out/generic\"\n"
"echo stub > \"$out/platforms/qwindows.dll\"\n"
"echo stub > \"$out/styles/qwindowsvistastyle.dll\"\n"
"echo stub > \"$out/generic/qtuiotouchplugin.dll\"\n"
"echo DistContracts windeployqt stub ran\n"
"exit 0\n")
  execute_process(COMMAND chmod +x "${_stub_bin}")
endif()

# A source root that is not a git repository: the script falls back to
# "unknown", which keeps VERSION deterministic and independent of this
# checkout's HEAD.
set(_source_dir "${_scratch}/input/source")
file(MAKE_DIRECTORY "${_source_dir}")
file(WRITE "${_source_dir}/NOTES" "synthetic source root\n")

# One locally built mpv archive, reused by every scenario. The script verifies
# its SHA-256 against the value passed in, extracts it, and checks that the DLL
# inside is a genuine x64 PE - all of which the round-trip below satisfies
# without a network fetch.
set(_mpv_stage "${_scratch}/input/mpv-stage")
file(MAKE_DIRECTORY "${_mpv_stage}")
file(COPY "${_pe_source}" DESTINATION "${_mpv_stage}")
file(RENAME "${_mpv_stage}/${_pe_name}" "${_mpv_stage}/libmpv-2.dll")
set(_mpv_archive "${_scratch}/input/mpv/verified-mpv.7z")
set(_mpv_cache "${_scratch}/cache/mpv")
file(MAKE_DIRECTORY "${_scratch}/input/mpv")
file(ARCHIVE_CREATE OUTPUT "${_mpv_archive}" PATHS "${_mpv_stage}" FORMAT 7zip)
if(NOT EXISTS "${_mpv_archive}")
  message(SEND_ERROR
    "${_prefix}: FAIL the synthetic mpv archive was not produced at ${_mpv_archive}")
  return()
endif()
file(SHA256 "${_mpv_archive}" _mpv_archive_sha)

set(_common_args
  "EXE=${_exe_path}"
  "WINDEPLOYQT=${_stub_bin}"
  "SHADERS=${_shaders_src}"
  "SOURCE_DIR=${_source_dir}"
  "MIKAI_MPV_ARCHIVE_NAME=verified-mpv.7z"
  "MIKAI_MPV_ARCHIVE_URL=https://invalid.invalid/unverified"
  "MIKAI_MPV_ARCHIVE_SHA256=${_mpv_archive_sha}"
  "MIKAI_DIST_CACHE_DIR=${_mpv_cache}"
  "MIKAI_DIST_MPV_ARCHIVE=${_mpv_archive}"
)

# The protected census: a populated installed tree whose every byte is recorded
# before a run and re-checked after it.
set(_canary_sentinel "INSTALLED-APP-STATE-PROTECTED-BY-DIST-CONTRACT")
set(_sentinel_name "installed-state-sentinel.txt")

# A census macro: records hash and size of one path, appending to a named list.
macro(_census entry out_list)
  if(EXISTS "${entry}")
    file(SHA256 "${entry}" _census_hash)
    file(SIZE "${entry}" _census_size)
  else()
    set(_census_hash "MISSING")
    set(_census_size "-1")
  endif()
  list(APPEND ${out_list} "${entry}|${_census_hash}|${_census_size}")
endmacro()

# ===========================================================================
# Scenario 1 - a populated output directory is refused, bytes intact
# ===========================================================================
set(_workdir "${_cases}/populated-output")
file(MAKE_DIRECTORY "${_workdir}")
set(_protected "${_workdir}/dist")
file(MAKE_DIRECTORY "${_protected}/installed/sub")
file(MAKE_DIRECTORY "${_protected}/mpv")
file(MAKE_DIRECTORY "${_protected}/records")
file(MAKE_DIRECTORY "${_protected}/platforms")
file(WRITE "${_protected}/${_sentinel_name}" "${_canary_sentinel}\n")
file(WRITE "${_protected}/installed/mikai.exe" "installed executable bytes\n")
file(WRITE "${_protected}/installed/mikai.log" "installed log bytes\n")
file(WRITE "${_protected}/mpv/libmpv-2.dll" "installed mpv bytes\n")
file(WRITE "${_protected}/records/keep.json" "installed records bytes\n")
file(WRITE "${_protected}/platforms/qwindows.dll" "installed platform bytes\n")

set(_census_paths
  "${_protected}/${_sentinel_name}"
  "${_protected}/installed/mikai.exe"
  "${_protected}/installed/mikai.log"
  "${_protected}/mpv/libmpv-2.dll")
set(_before)
foreach(_path ${_census_paths})
  _census("${_path}" _before)
endforeach()

set(_rc "")
_run_dist(_rc _out _err "populated-output"
  ${_common_args}
  "DIST_DIR=${_protected}"
  "MIKAI_DIST_NATIVE=OFF")
_check(NOT _rc EQUAL 0 "a populated MIKAI_DIST_DIR is refused (nonzero exit)")

set(_combined "${_out}\n${_err}")
string(FIND "${_combined}" "refusing to recreate" _at)
_check(_at GREATER -1 "the refusal names the reason (\"refusing to recreate\")")
string(FIND "${_combined}" "${_sentinel_name}" _at)
_check(_at GREATER -1 "the refusal enumerates what it found, so the tree is identifiable")

set(_after)
foreach(_path ${_census_paths})
  _census("${_path}" _after)
endforeach()
_check(_after STREQUAL _before
  "every protected byte (hash and size) is unchanged by the refused run")

file(READ "${_protected}/${_sentinel_name}" _readback)
string(STRIP "${_readback}" _readback)
_check(_readback STREQUAL "${_canary_sentinel}"
  "the protected sentinel content is byte-for-byte intact")
_check(EXISTS "${_protected}/mpv/libmpv-2.dll" "the pre-existing mpv payload survives")
_check(EXISTS "${_protected}/records/keep.json" "the pre-existing records payload survives")
_check(EXISTS "${_protected}/platforms/qwindows.dll"
  "the pre-existing platform plugin survives")

# None of the files the successful path writes may exist after a refusal: the
# write happens after the guard, so a residual artifact proves the guard moved.
_check(NOT EXISTS "${_protected}/run.bat" "no run.bat is written on a refusal")
_check(NOT EXISTS "${_protected}/VERSION" "no VERSION is written on a refusal")
_check(NOT EXISTS "${_protected}/translations"
  "no translations directory is created on a refusal")
_check(NOT EXISTS "${_protected}/native"
  "no native directory is created on a refusal")

# ===========================================================================
# Scenario 2 - an existing but empty output directory is accepted
# ===========================================================================
set(_workdir "${_cases}/empty-output")
file(MAKE_DIRECTORY "${_workdir}")
set(_empty_out "${_workdir}/dist")
file(MAKE_DIRECTORY "${_empty_out}")

set(_rc "")
_run_dist(_rc _out _err "empty-output"
  ${_common_args}
  "DIST_DIR=${_empty_out}"
  "MIKAI_DIST_NATIVE=OFF")
_check(_rc EQUAL 0 "an existing but empty MIKAI_DIST_DIR is accepted")
_check(EXISTS "${_empty_out}/mikai.exe" "the executable is copied into the output")
_check(EXISTS "${_empty_out}/run.bat" "run.bat is written into the output")
_check(EXISTS "${_empty_out}/VERSION" "VERSION is written into the output")
_check(EXISTS "${_empty_out}/mpv/libmpv-2.dll"
  "the verified mpv API DLL is packaged into the output")
_check(EXISTS "${_empty_out}/shaders/Anime4K_Clamp_Highlights.glsl"
  "the shaders are copied into the output")
_check(EXISTS "${_empty_out}/platforms/qwindows.dll"
  "the Qt runtime deploy step ran against the staging root")
_check(NOT EXISTS "${_empty_out}/native"
  "MIKAI_DIST_NATIVE=OFF packages no native tree at all")

# ===========================================================================
# Scenario 3 - output and native root are the same directory
# ===========================================================================
set(_workdir "${_cases}/same-path")
file(MAKE_DIRECTORY "${_workdir}")
set(_same_root "${_workdir}/shared")
file(MAKE_DIRECTORY "${_same_root}/runtime/bundle/plugins")
file(WRITE "${_same_root}/runtime/bundle/plugins/qwindows.dll" "input bytes\n")
file(WRITE "${_same_root}/INSTALLED" "shared input bytes\n")

set(_rc "")
_run_dist(_rc _out _err "same-path"
  ${_common_args}
  "DIST_DIR=${_same_root}"
  "MIKAI_DIST_NATIVE=ON"
  "MIKAI_DIST_NATIVE_ROOT=${_same_root}")
_check(NOT _rc EQUAL 0 "MIKAI_DIST_DIR equal to MIKAI_DIST_NATIVE_ROOT is refused")
set(_combined "${_out}\n${_err}")
# Either guard reaching this state first is a refusal, and both must refuse
# before any write: the populated-output guard, because the shared directory is
# populated, or the overlap guard directly. Whichever fires, the shared input
# must survive with nothing written beside it.
string(FIND "${_combined}" "must not be inside" _at_overlap)
string(FIND "${_combined}" "refusing to recreate" _at_refuse)
_check(_at_overlap GREATER -1 OR _at_refuse GREATER -1
  "the equality refusal names a reason")
_check(EXISTS "${_same_root}/runtime/bundle/plugins/qwindows.dll"
  "the shared input tree is undisturbed when the paths are equal")
_check(EXISTS "${_same_root}/INSTALLED"
  "the shared input root file survives an equality refusal")
_check(NOT EXISTS "${_same_root}/run.bat"
  "nothing is written into a shared path on a refusal")

# ===========================================================================
# Scenario 4 - output nested inside the native root
# ===========================================================================
set(_workdir "${_cases}/output-inside-root")
file(MAKE_DIRECTORY "${_workdir}")
set(_outer_root "${_workdir}/native-store")
set(_nested_out "${_workdir}/native-store/output")
file(MAKE_DIRECTORY "${_outer_root}/runtime/bundle/plugins")
file(WRITE "${_outer_root}/runtime/bundle/plugins/qwindows.dll" "input bytes\n")

set(_rc "")
_run_dist(_rc _out _err "output-inside-root"
  ${_common_args}
  "DIST_DIR=${_nested_out}"
  "MIKAI_DIST_NATIVE=ON"
  "MIKAI_DIST_NATIVE_ROOT=${_outer_root}")
_check(NOT _rc EQUAL 0 "MIKAI_DIST_DIR nested inside the native root is refused")
set(_combined "${_out}\n${_err}")
string(FIND "${_combined}" "must not be inside" _at_overlap)
string(FIND "${_combined}" "refusing to recreate" _at_refuse)
_check(_at_overlap GREATER -1 OR _at_refuse GREATER -1
  "the nested-output refusal names a reason")
_check(EXISTS "${_outer_root}/runtime/bundle/plugins/qwindows.dll"
  "the native input tree survives a nested-output refusal")
# The overlap guard lives at the very end of the script, after the whole output
# has been assembled, so it can only refuse after the damage is done. Recording
# the observable state keeps the ordering visible: what actually landed inside
# the native root is contradicted by the refusal that follows it.
set(_landed)
foreach(_entry "run.bat" "VERSION" "mikai.exe" "mpv" "platforms" "styles"
                "generic" "translations" "shaders")
  if(EXISTS "${_nested_out}/${_entry}")
    list(APPEND _landed "${_entry}")
  endif()
endforeach()
message(STATUS
  "${_prefix}: NOTE the nested output received these entries before the refusal "
  "fired: ${_landed}")

# ===========================================================================
# Scenario 5 - native root nested inside the output
# ===========================================================================
set(_workdir "${_cases}/root-inside-output")
file(MAKE_DIRECTORY "${_workdir}")
# The output is fresh, so the populated-output guard cannot fire first and the
# overlap guard is what we are actually exercising.
set(_holder_out "${_workdir}/output-holder")
set(_inner_root "${_workdir}/output-holder/native-store")
file(MAKE_DIRECTORY "${_inner_root}/runtime/bundle/plugins")
file(WRITE "${_inner_root}/runtime/bundle/plugins/qwindows.dll" "input bytes\n")

set(_rc "")
_run_dist(_rc _out _err "root-inside-output"
  ${_common_args}
  "DIST_DIR=${_holder_out}"
  "MIKAI_DIST_NATIVE=ON"
  "MIKAI_DIST_NATIVE_ROOT=${_inner_root}")
set(_combined "${_out}\n${_err}")
string(FIND "${_combined}" "must not be inside" _at_overlap)
string(FIND "${_combined}" "refusing to recreate" _at_refuse)
_check(NOT _rc EQUAL 0
  "MIKAI_DIST_NATIVE_ROOT nested inside MIKAI_DIST_DIR is refused")
_check(_at_overlap GREATER -1 OR _at_refuse GREATER -1
  "the reverse-overlap refusal names a reason")
_check(EXISTS "${_inner_root}/runtime/bundle/plugins/qwindows.dll"
  "the inner native input survives a reverse-overlap refusal")

# ===========================================================================
# Scenario 6 - MIKAI_DIST_NATIVE=ON with no runtime fails by name
# ===========================================================================
set(_workdir "${_cases}/missing-runtime")
file(MAKE_DIRECTORY "${_workdir}")
set(_no_runtime_root "${_workdir}/store-without-runtime")
set(_no_runtime_out "${_workdir}/dist")
file(MAKE_DIRECTORY "${_no_runtime_root}")
file(WRITE "${_no_runtime_root}/received-model.txt" "a download arrived here once\n")
file(MAKE_DIRECTORY "${_no_runtime_out}")

set(_rc "")
_run_dist(_rc _out _err "missing-runtime"
  ${_common_args}
  "DIST_DIR=${_no_runtime_out}"
  "MIKAI_DIST_NATIVE=ON"
  "MIKAI_DIST_NATIVE_ROOT=${_no_runtime_root}")
_check(NOT _rc EQUAL 0 "MIKAI_DIST_NATIVE=ON with no runtime fails")
set(_combined "${_out}\n${_err}")
string(FIND "${_combined}" "${_no_runtime_root}/runtime" _at)
_check(_at GREATER -1 "the failure names the resolved root and its runtime path")
string(FIND "${_combined}" "Nothing was" _at2)
_check(_at2 GREATER -1 "the failure states that nothing was assembled")
_check(NOT EXISTS "${_no_runtime_out}/native"
  "no native tree is produced when the runtime is missing")
# The refusal fires BEFORE the first write, so nothing at all may exist in the
# output directory: an assembled folder for a native option that packaged no
# native runtime is exactly the silent-skip outcome this contract forbids, and a
# leftover run.bat/VERSION/mpv would be a folder claimed as complete.
foreach(_artifact IN ITEMS "run.bat" "VERSION" "mikai.exe" "mpv" "shaders"
                          "translations" "platforms")
  _check(NOT EXISTS "${_no_runtime_out}/${_artifact}"
    "the missing-runtime refusal writes nothing: no ${_artifact}")
endforeach()

# ===========================================================================
# Scenario 7 - a valid assembly strips direct AND nested payload directories
# ===========================================================================
set(_workdir "${_cases}/valid-assembly")
file(MAKE_DIRECTORY "${_workdir}")
set(_native_root "${_workdir}/native-store")
set(_dist_out "${_workdir}/dist")

# A realistic installed store: several bundles, each carrying ordinary runtime
# content, direct payload, and payload nested two and three levels deep.
set(_bundles "a-real-bundle" "second-bundle" "third-bundle")
set(_nests "deep/nest" "deeper/far/nest")
foreach(_bundle IN LISTS _bundles)
  file(MAKE_DIRECTORY "${_native_root}/runtime/${_bundle}/plugins")
  file(WRITE "${_native_root}/runtime/${_bundle}/plugins/qwindows.dll"
    "ordinary runtime bytes ${_bundle}\n")
  file(MAKE_DIRECTORY "${_native_root}/runtime/${_bundle}/translations")
  file(WRITE "${_native_root}/runtime/${_bundle}/translations/qwindows.qm"
    "ordinary translation ${_bundle}\n")
  file(MAKE_DIRECTORY "${_native_root}/runtime/${_bundle}/assets/textures")
  file(WRITE "${_native_root}/runtime/${_bundle}/assets/textures/atlas.png"
    "direct asset payload ${_bundle}\n")
  file(MAKE_DIRECTORY
    "${_native_root}/runtime/${_bundle}/components/tensorrt")
  file(WRITE
    "${_native_root}/runtime/${_bundle}/components/tensorrt/blas.dll"
    "direct component payload ${_bundle}\n")
  foreach(_nest IN LISTS _nests)
    file(MAKE_DIRECTORY
      "${_native_root}/runtime/${_bundle}/${_nest}/components")
    file(WRITE
      "${_native_root}/runtime/${_bundle}/${_nest}/components/ba.dll"
      "nested component ${_bundle}/${_nest}\n")
    file(MAKE_DIRECTORY
      "${_native_root}/runtime/${_bundle}/${_nest}/models")
    file(WRITE "${_native_root}/runtime/${_bundle}/${_nest}/models/graph.pb"
      "nested model ${_bundle}/${_nest}\n")
    file(MAKE_DIRECTORY
      "${_native_root}/runtime/${_bundle}/${_nest}/records")
    file(WRITE
      "${_native_root}/runtime/${_bundle}/${_nest}/records/ledger.json"
      "nested record ${_bundle}/${_nest}\n")
  endforeach()
endforeach()
file(MAKE_DIRECTORY "${_native_root}/runtime/a-real-bundle/keep")
file(WRITE "${_native_root}/runtime/a-real-bundle/keep/runtime-state.json"
  "ordinary runtime state\n")

set(_rc "")
_run_dist(_rc _out _err "valid-assembly"
  ${_common_args}
  "DIST_DIR=${_dist_out}"
  "MIKAI_DIST_NATIVE=ON"
  "MIKAI_DIST_NATIVE_ROOT=${_native_root}")
_check(_rc EQUAL 0
  "a valid assembly containing forbidden payload succeeds with no stowaway refusal")

# THE STRIP BOUNDARY. Ordinary runtime content must survive: the sweep exists to
# remove per-machine downloads, not to empty the bundle it copied. A strip that
# takes the whole tree with it deletes the very runtime the option asked to
# ship, and a bundle-wide delete also loses every ordinary sibling.
#
# This is asserted as an explicit separate check, so a strip that "passes"
# because it removed everything is still a failure rather than a success.
foreach(_bundle IN LISTS _bundles)
  _check(EXISTS "${_dist_out}/native/runtime/${_bundle}/plugins/qwindows.dll"
    "ordinary runtime plugin survives the strip for ${_bundle}")
  _check(EXISTS "${_dist_out}/native/runtime/${_bundle}/translations/qwindows.qm"
    "ordinary translation survives the strip for ${_bundle}")
endforeach()
_check(EXISTS "${_dist_out}/native/runtime/a-real-bundle/keep/runtime-state.json"
  "an ordinary runtime sibling of the payload roots survives the strip")
_check(EXISTS "${_dist_out}/native/runtime/a-real-bundle/keep"
  "the ordinary sibling directory is not emptied by the strip")
_check(EXISTS "${_dist_out}/native/runtime"
  "the copied native runtime itself survives the strip")

# Direct-level forbidden payload must be gone.
foreach(_bundle IN LISTS _bundles)
  _check(NOT EXISTS "${_dist_out}/native/runtime/${_bundle}/assets"
    "direct assets payload removed for ${_bundle}")
  _check(NOT EXISTS "${_dist_out}/native/runtime/${_bundle}/components"
    "direct components payload removed for ${_bundle}")
endforeach()

# Nested forbidden payload must be gone too - this is the depth a shallow or
# directory-blind sweep never reaches.
foreach(_bundle IN LISTS _bundles)
  foreach(_nest IN LISTS _nests)
    foreach(_forbidden IN ITEMS "components" "models" "records")
      _check(NOT EXISTS
        "${_dist_out}/native/runtime/${_bundle}/${_nest}/${_forbidden}"
        "nested ${_forbidden} payload removed for ${_bundle}/${_nest}")
    endforeach()
  endforeach()
endforeach()

# A residual forbidden directory anywhere in the assembled output means the
# boundary was not enforced. The script's own verification is fatal, so this is
# a second, independent check on observable state.
set(_residual)
foreach(_forbidden IN ITEMS "components" "assets" "models" "records")
  foreach(_glob
      "${_dist_out}/native/${_forbidden}"
      "${_dist_out}/native/*/${_forbidden}"
      "${_dist_out}/native/*/*/${_forbidden}"
      "${_dist_out}/native/*/*/*/${_forbidden}"
      "${_dist_out}/native/*/*/*/*/${_forbidden}")
    if(EXISTS "${_glob}")
      list(APPEND _residual "${_glob}")
    endif()
  endforeach()
endforeach()
# Empty-list comparison: STREQUAL against "" never evaluates as a valid
# two-argument if(), so the emptiness is resolved into a boolean first, and the
# verdict is passed to _check as a single token.
# An empty CMake list loses its arguments when expanded, and list STREQUAL ""
# then evaluates false, so emptiness is decided on the count instead.
list(LENGTH _residual _residual_count)
if(_residual_count EQUAL 0)
  message(STATUS
    "${_prefix}: PASS no forbidden payload directory survives anywhere in the "
    "assembled output")
else()
  message(SEND_ERROR
    "${_prefix}: FAIL residual forbidden payload still in the assembled "
    "output: ${_residual}")
endif()

# The source tree must be untouched: a strip that reaches into the input is the
# data loss this boundary exists to prevent.
foreach(_entry
    "${_native_root}/runtime/a-real-bundle/assets/textures/atlas.png"
    "${_native_root}/runtime/a-real-bundle/components/tensorrt/blas.dll"
    "${_native_root}/runtime/a-real-bundle/deep/nest/components/ba.dll"
    "${_native_root}/runtime/a-real-bundle/deep/nest/models/graph.pb"
    "${_native_root}/runtime/a-real-bundle/deeper/far/nest/records/ledger.json"
    "${_native_root}/runtime/a-real-bundle/keep/runtime-state.json")
  _check(EXISTS "${_entry}" "the source input survives intact: ${_entry}")
endforeach()

message(STATUS "${_prefix}: sweep complete")
file(REMOVE_RECURSE "${_scratch}")
