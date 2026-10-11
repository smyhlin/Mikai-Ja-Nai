# Behavioural regression for the native staging/patch-policy contract.
#
# Deterministic standalone CMake: it calls the PRODUCTION helper
# native/cmake/StageInference.cmake over synthetic source trees and synthetic
# patches that it writes itself, and asserts only observable outcomes - what
# exists on disk afterwards, what the helper reports, and whether a configure
# run is accepted or refused. No source text of native/CMakeLists.txt is
# inspected, so this file cannot pass by pattern instead of by behaviour.
#
# It never touches the repository build tree, native/patches, the real
# AJI_SOURCE_DIR, the installed dist-mikai tree or any existing native root. It
# creates and removes only paths under its own scratch root.
#
# Run:
#   cmake -DAJI_STAGE_CONTRACT_PROJECT=ON \
#         -DAJI_STAGE_HELPER=<abs path to native/cmake/StageInference.cmake> \
#         -P tests/cmake/NativeStageContracts.cmake
# Exit 0 = every scenario pass; 1 = a scenario failed or the harness broke.

if(NOT AJI_STAGE_CONTRACT_PROJECT)
  return()
endif()

set(_prefix "NativeStageContracts")

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
if(AJI_STAGE_SCRATCH_ROOT)
  set(_scratch "${AJI_STAGE_SCRATCH_ROOT}")
elseif(CMAKE_CURRENT_BINARY_DIR)
  set(_scratch "${CMAKE_CURRENT_BINARY_DIR}/_native-stage-contract")
else()
  file(TO_CMAKE_PATH "$ENV{TEMP}" _scratch)
  if(_scratch STREQUAL "")
    set(_scratch "/tmp")
  endif()
  set(_scratch "${_scratch}/native-stage-contract")
endif()

# Own exactly one directory; never operate on anything that is not ours.
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
file(WRITE "${_scratch}/.scratch-owned" "stage-contract scratch\n")

set(_helper "${AJI_STAGE_HELPER}")
if(NOT EXISTS "${_helper}")
  message(SEND_ERROR "${_prefix}: FAIL AJI_STAGE_HELPER not found: ${_helper}")
  return()
endif()
get_filename_component(_helper "${_helper}" ABSOLUTE)

set(_result_file "${_scratch}/last-result.txt")
set(_child "${_scratch}/stage-child.cmake")

# The child carries the configure in a separate interpreter, so a refusal is an
# observable nonzero exit rather than the end of this script.
file(WRITE "${_child}"
"if(DEFINED AJI_CONTRACT_GIT)
  set(MIKAI_STAGE_GIT_EXECUTABLE \"\${AJI_CONTRACT_GIT}\")
endif()
if(DEFINED AJI_CONTRACT_PATCH)
  set(MIKAI_STAGE_PATCH_EXECUTABLE \"\${AJI_CONTRACT_PATCH}\")
endif()
include(\"\${AJI_CONTRACT_HELPER}\")
mikai_stage_inference_source(
  AJI_SOURCE_DIR   \"\${AJI_CONTRACT_SOURCE}\"
  AJI_STAGE_DIR    \"\${AJI_CONTRACT_STAGE}\"
  AJI_PATCH_POLICY \"\${AJI_CONTRACT_POLICY}\"
  AJI_RESTAGE     \"\${AJI_CONTRACT_RESTAGE}\"
  AJI_PATCH_DIR    \"\${AJI_CONTRACT_PATCH_DIR}\"
  OUT_STAGE_VALID  _valid
  OUT_PATCH_STATE _state)
file(WRITE \"\${AJI_CONTRACT_OUT}\" \"\${_valid}\n\${_state}\n\")
")

# ------------------------------------------------- synthetic source material
# A file whose content far below any 16-byte window carries a version marker: an
# edit there must invalidate the stage. This is the canary that kills a
# prefix-only fingerprint.
function(_make_source root)
  file(MAKE_DIRECTORY "${root}/src" "${root}/include" "${root}/.git" "${root}/build")
  # aji_trt.cpp's second line is the context the synthetic patches hang their
  # hunks on, so it must exist in the source for the "good" patches to apply.
  file(WRITE "${root}/src/aji_trt.cpp"
"int trt_v1() { return 1; }
int extra(void) { return 2; }
")
  file(WRITE "${root}/src/aji_dispatch.cpp"  "int dispatch_v1() { return 1; }\n")
  file(WRITE "${root}/src/aji_dml.cpp"       "int dml_v1() { return 1; }\n")
  file(WRITE "${root}/src/kernels.h"         "int kernels_v1(void);\n")
  file(WRITE "${root}/CMakeLists.txt"        "message(STATUS upstream)\n")
  file(WRITE "${root}/include/aji.h"         "int aji(void);\n")
  file(WRITE "${root}/.git/HEAD"             "ref: refs/heads/main\n")
  file(WRITE "${root}/build/native.log"      "stale build output\n")
  file(WRITE "${root}/src/deep_canary.h"
"// stage-contract deep canary
// VERSION=initial
// padding
//
//
//
//
//
//
//
//
//
//
int deep_canary(void);
")
endfunction()

function(_read_canary path out)
  set(_file "${path}/src/deep_canary.h")
  if(NOT EXISTS "${_file}")
    set(${out} "MISSING" PARENT_SCOPE)
    return()
  endif()
  file(READ "${_file}" _text)
  string(FIND "${_text}" "VERSION=" _at)
  if(_at LESS 0)
    set(${out} "MISSING" PARENT_SCOPE)
    return()
  endif()
  string(SUBSTRING "${_text}" ${_at} -1 _rest)
  string(REGEX REPLACE "\n.*" "" _rest "${_rest}")
  set(${out} "${_rest}" PARENT_SCOPE)
endfunction()

macro(_write_canary_edit path)
  file(WRITE "${path}/src/deep_canary.h"
"// stage-contract deep canary
// VERSION=edited
// padding
//
//
//
//
//
//
//
//
//
//
int deep_canary(void);
")
endmacro()

# --------------------------------------------------- synthetic patch series
# A "good" patch is one an applier actually accepts. Two facts of unified diff
# drive the shapes below: the hunk's `-N,M` counts and context describe the file
# as it is BEFORE the patch, and `git apply` matches that text exactly, so a
# second patch in the same series must expect the first patch's inserted lines.
function(_reset_patch_dir)
  file(REMOVE_RECURSE "${_scratch}/patches")
  file(MAKE_DIRECTORY "${_scratch}/patches")
endfunction()

# The first patch of a series, applying to the untouched synthetic source.
function(_write_first_patch name marker)
  file(WRITE "${_scratch}/patches/${name}"
"--- a/src/aji_trt.cpp
+++ b/src/aji_trt.cpp
@@ -1,2 +1,3 @@
 int trt_v1() { return 1; }
+int ${marker}(void) { return 1; }
 int extra(void) { return 2; }
")
endfunction()

# A later patch in the same series: its context includes the lines the earlier
# patches inserted, and its start line accounts for them.
function(_write_next_patch name marker prior_marker)
  file(WRITE "${_scratch}/patches/${name}"
"--- a/src/aji_trt.cpp
+++ b/src/aji_trt.cpp
@@ -1,3 +1,4 @@
 int trt_v1() { return 1; }
 int ${prior_marker}(void) { return 1; }
+int ${marker}(void) { return 1; }
 int extra(void) { return 2; }
")
endfunction()

function(_write_description_patch name)
  file(WRITE "${_scratch}/patches/${name}"
"High-level description: the series carries prose with a bare @@ header that no
applier can consume, instead of a unified diff.
@@ @@
nothing applicable here.
")
endfunction()

function(_write_bad_patch name)
  file(WRITE "${_scratch}/patches/${name}"
"--- a/src/aji_trt.cpp
+++ b/src/aji_trt.cpp
@@ -900,1 +900,2 @@
 int nowhere_at_all(void) { return 0; }
+int nope(void) { return 0; }
")
endfunction()

# ----------------------------------------------------- the child configure
function(_stage source stage policy restage out_rc)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DAJI_CONTRACT_SOURCE=${source}"
      "-DAJI_CONTRACT_STAGE=${stage}"
      "-DAJI_CONTRACT_POLICY=${policy}"
      "-DAJI_CONTRACT_RESTAGE=${restage}"
      "-DAJI_CONTRACT_PATCH_DIR=${_scratch}/patches"
      "-DAJI_CONTRACT_HELPER=${_helper}"
      "-DAJI_CONTRACT_OUT=${_result_file}"
      -P "${_child}"
    WORKING_DIRECTORY "${_scratch}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  file(REMOVE "${_result_file}")
  set(${out_rc} "${_rc}" PARENT_SCOPE)
endfunction()

# With a specific applier, or an unusable one to observe that branch.
macro(_stage_with_applier applier source stage policy restage out_rc)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DAJI_CONTRACT_SOURCE=${source}"
      "-DAJI_CONTRACT_STAGE=${stage}"
      "-DAJI_CONTRACT_POLICY=${policy}"
      "-DAJI_CONTRACT_RESTAGE=${restage}"
      "-DAJI_CONTRACT_PATCH_DIR=${_scratch}/patches"
      "-DAJI_CONTRACT_HELPER=${_helper}"
      "-DAJI_CONTRACT_OUT=${_result_file}"
      "-DAJI_CONTRACT_GIT=${applier}"
      -P "${_child}"
    WORKING_DIRECTORY "${_scratch}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  file(REMOVE "${_result_file}")
  set(${out_rc} "${_rc}")
endmacro()

macro(_mark stage name)
  file(WRITE "${stage}/${name}" "marker\n")
endmacro()

macro(_has stage name out)
  if(EXISTS "${stage}/${name}")
    set(${out} TRUE)
  else()
    set(${out} FALSE)
  endif()
endmacro()


# ===========================================================================
# Scenarios
# ===========================================================================
set(_src "${_scratch}/source")
set(_work "${_scratch}/work")
_make_source("${_src}")
file(MAKE_DIRECTORY "${_work}")

# --- 1. A foreign stage directory is never deleted.
set(_stage "${_work}/stage-foreign")
_make_source("${_stage}")
_mark("${_stage}" "KEEP-ME.txt")
_reset_patch_dir()
_write_first_patch("0001-good.patch" "p1")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "OFF" _rc)
_check(NOT _rc EQUAL 0 "foreign stage directory is refused")
_check(EXISTS "${_stage}/KEEP-ME.txt" "foreign stage contents survive the refusal")
_check(NOT EXISTS "${_stage}/.mikai-staged" "a refused stage receives no ownership stamp")

# --- 2. A stage path that is a plain file is refused and survives.
set(_stage_file "${_work}/stage-file")
file(WRITE "${_stage_file}" "not a directory\n")
_reset_patch_dir()
_write_first_patch("0001-good.patch" "p1")
set(_rc "")
_stage("${_src}" "${_stage_file}" "strict" "OFF" _rc)
_check(NOT _rc EQUAL 0 "a file at the stage path is refused")
_check(EXISTS "${_stage_file}" "the file at the stage path is not removed")

# --- 3. A stage equal to the source is refused and the source is untouched.
_reset_patch_dir()
_write_first_patch("0001-good.patch" "p1")
set(_rc "")
_stage("${_src}" "${_src}" "strict" "OFF" _rc)
_check(NOT _rc EQUAL 0 "a stage equal to the source is refused")
_read_canary("${_src}" _canary)
_check(_canary STREQUAL "VERSION=initial" "sentinel source survives an equal-path stage")
_check(EXISTS "${_src}/src/aji_trt.cpp" "the equal-path refusal leaves the source files")

# --- 4. A stage that contains the source is refused.
set(_wrap "${_work}/wrap")
_make_source("${_wrap}/inner")
_reset_patch_dir()
_write_first_patch("0001-good.patch" "p1")
set(_rc "")
_stage("${_wrap}/inner" "${_wrap}" "strict" "OFF" _rc)
_check(NOT _rc EQUAL 0 "a stage containing the source is refused")
_read_canary("${_wrap}/inner" _canary)
_check(_canary STREQUAL "VERSION=initial" "wrapped source survives")

# --- 5. A stage nested inside the source is refused.
_reset_patch_dir()
_write_first_patch("0001-good.patch" "p1")
set(_rc "")
_stage("${_src}" "${_src}/stage" "strict" "OFF" _rc)
_check(NOT _rc EQUAL 0 "a stage inside the source is refused")
_read_canary("${_src}" _canary)
_check(_canary STREQUAL "VERSION=initial" "source survives a nested stage")
_check(NOT EXISTS "${_src}/stage" "no stage directory is created inside the source")

# --- 6. A happy configure copies the tree, applies the series, stamps both
#       markers and leaves the source untouched.
set(_stage "${_work}/stage-happy")
_reset_patch_dir()
_write_first_patch("0001-good.patch" "p1")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "OFF" _rc)
_check(_rc EQUAL 0 "a complete applicable series configures under strict")
_check(EXISTS "${_stage}/src/aji_trt.cpp" "the staged copy exists")
_check(NOT EXISTS "${_stage}/.git" ".git is excluded from the copy")
_check(NOT EXISTS "${_stage}/build/native.log" "a nested build directory is excluded")
_check(EXISTS "${_stage}/.mikai-staged" "the staging stamp is written")
_check(EXISTS "${_stage}/.mikai-patched" "the full-series stamp is written")
_read_canary("${_src}" _canary)
_check(_canary STREQUAL "VERSION=initial" "the source is never edited by staging")
file(READ "${_stage}/src/aji_trt.cpp" _trt)
_check(_trt MATCHES "p1" "the applied patch is present in the staged copy")
_check(NOT EXISTS "${_src}/src/aji_trt.cpp.orig" "no patch backups leak into the source")

# --- 7. A second unchanged configure reuses the stage instead of restaging.
_mark("${_stage}" "STAGE-KEEP.txt")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "OFF" _rc)
_check(_rc EQUAL 0 "a reconfigure with unchanged inputs succeeds")
_has("${_stage}" "STAGE-KEEP.txt" _kept)
_check(_kept STREQUAL "TRUE" "an unchanged configure does not restage")
_check(EXISTS "${_stage}/.mikai-patched" "the full-series stamp survives a reconfigure")

# --- 8. An edit deep in the tree, below any 16-byte window, restages.
_write_canary_edit("${_src}")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "OFF" _rc)
_check(_rc EQUAL 0 "an edited source reconfigures")
_has("${_stage}" "STAGE-KEEP.txt" _kept)
_check(_kept STREQUAL "FALSE" "a deep source edit restages the tree")
_read_canary("${_stage}" _canary)
_check(_canary STREQUAL "VERSION=edited" "the restaged copy carries the edited bytes")
_read_canary("${_src}" _canary)
_check(_canary STREQUAL "VERSION=edited" "the source edit is never applied to")

# --- 9. A header-only edit also restages.
file(WRITE "${_src}/src/kernels.h" "int kernels_v2(void);\n")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "OFF" _rc)
_check(_rc EQUAL 0 "a header edit reconfigures")
file(READ "${_stage}/src/kernels.h" _h)
_check(_h MATCHES "v2" "the restaged copy carries the header edit")

# --- 10. Adding a patch file restages and reapplies the series.
_reset_patch_dir()
_write_first_patch("0001-good.patch" "p1")
set(_stage "${_work}/stage-series")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "OFF" _rc)
_check(_rc EQUAL 0 "a first series configures")
_write_next_patch("0002-second.patch" "p2" "p1")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "OFF" _rc)
_check(_rc EQUAL 0 "an extended series reconfigures")
_check(EXISTS "${_stage}/.mikai-patched" "the extended series is stamped")
file(READ "${_stage}/src/aji_trt.cpp" _trt)
_check(_trt MATCHES "p2" "the second patch is applied after restaging")
_check(_trt MATCHES "p1" "the first patch survives the restage")

# --- 11. Editing a patch file's digest restages the series.
_reset_patch_dir()
_write_first_patch("0001-good.patch" "p1")
set(_stage "${_work}/stage-editpatch")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "OFF" _rc)
_check(_rc EQUAL 0 "a first series configures")
_write_first_patch("0001-good.patch" "p1x")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "OFF" _rc)
_check(_rc EQUAL 0 "an edited patch series reconfigures")
file(READ "${_stage}/src/aji_trt.cpp" _trt)
_check(_trt MATCHES "p1x" "the edited patch is applied")

# --- 12. Removing a stamp forces a rebuild from the current source.
set(_stage "${_work}/stage-stamps")
_reset_patch_dir()
_write_first_patch("0001-good.patch" "p1")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "OFF" _rc)
_check(_rc EQUAL 0 "a first series configures")
file(REMOVE "${_stage}/.mikai-staged")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "OFF" _rc)
_check(_rc EQUAL 0 "a missing staging stamp reconfigures")
_check(EXISTS "${_stage}/.mikai-staged" "a missing staging stamp is rewritten")
_read_canary("${_src}" _canary)
_check(_canary STREQUAL "VERSION=edited" "the source survives the rebuild")

# --- 13. strict refuses an empty patch directory and stamps nothing.
_reset_patch_dir()
set(_stage "${_work}/stage-strict-empty")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "OFF" _rc)
_check(NOT _rc EQUAL 0 "strict refuses an empty patch directory")
_check(NOT EXISTS "${_stage}/.mikai-patched" "no full-series stamp under an empty series")

# --- 14. strict refuses a description-only series.
_reset_patch_dir()
_write_description_patch("0001-desc.patch")
set(_stage "${_work}/stage-strict-desc")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "OFF" _rc)
_check(NOT _rc EQUAL 0 "strict refuses a description-only series")
_check(NOT EXISTS "${_stage}/.mikai-patched" "no stamp marks a description as applied")

# --- 15. strict refuses an inapplicable patch and discards the stage.
_reset_patch_dir()
_write_bad_patch("0001-bad.patch")
set(_stage "${_work}/stage-strict-bad")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "OFF" _rc)
_check(NOT _rc EQUAL 0 "strict refuses an inapplicable patch")
_check(NOT EXISTS "${_stage}/.mikai-patched" "no stamp claims an unapplied series")
_check(NOT EXISTS "${_stage}/.mikai-staged" "the discarded stage leaves no stamp")

# --- 16. strict accepts the complete series and stamps it.
_reset_patch_dir()
_write_first_patch("0001-good.patch" "p1")
set(_stage "${_work}/stage-strict-ok")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "OFF" _rc)
_check(_rc EQUAL 0 "strict accepts a complete series")
_check(EXISTS "${_stage}/.mikai-patched" "strict stamps the complete series")

# --- 17. warn configures with no series at all, with no full stamp.
_reset_patch_dir()
set(_stage "${_work}/stage-warn-none")
set(_rc "")
_stage("${_src}" "${_stage}" "warn" "OFF" _rc)
_check(_rc EQUAL 0 "warn configures with no series")
_check(NOT EXISTS "${_stage}/.mikai-patched" "warn writes no full-series stamp")
_check(EXISTS "${_stage}/src/aji_trt.cpp" "warn keeps the staged tree")
file(READ "${_stage}/src/aji_trt.cpp" _trt)
_check(_trt MATCHES "trt_v1" "warn leaves the tree unpatched")

# --- 18. warn configures with a partial series and stamps nothing.
_reset_patch_dir()
_write_first_patch("0001-good.patch" "p1")
_write_bad_patch("0002-bad.patch")
set(_stage "${_work}/stage-warn-partial")
set(_rc "")
_stage("${_src}" "${_stage}" "warn" "OFF" _rc)
_check(_rc EQUAL 0 "warn configures with a partial series")
_check(NOT EXISTS "${_stage}/.mikai-patched" "a partial series gets no stamp")
_check(EXISTS "${_stage}/.mikai-staged" "warn keeps the staged tree")

# --- 19. warn configures with a description-only series, with no stamp.
_reset_patch_dir()
_write_description_patch("0001-desc.patch")
set(_stage "${_work}/stage-warn-desc")
set(_rc "")
_stage("${_src}" "${_stage}" "warn" "OFF" _rc)
_check(_rc EQUAL 0 "warn configures with a description-only series")
_check(NOT EXISTS "${_stage}/.mikai-patched" "descriptions never get a full stamp")

# --- 20. off never applies the series, whatever it contains.
_reset_patch_dir()
_write_first_patch("0001-good.patch" "p1")
set(_stage "${_work}/stage-off")
set(_rc "")
_stage("${_src}" "${_stage}" "off" "OFF" _rc)
_check(_rc EQUAL 0 "off configures with the series present")
_check(NOT EXISTS "${_stage}/.mikai-patched" "off never stamps the series")
file(READ "${_stage}/src/aji_trt.cpp" _trt)
_check(_trt MATCHES "trt_v1" "off never applies the patch")
# The series' patch would insert this line; its absence is the positive proof
# that `off` did not run the applier. "p1(" is not a valid regex, so the marker
# is matched as a literal with the parenthesis escaped.
_check(NOT _trt MATCHES "p1[(]" "off leaves no marker of application")
set(_rc "")
_stage("${_src}" "${_stage}" "off" "OFF" _rc)
_check(_rc EQUAL 0 "off reconfigures unchanged")

# --- 21. A failure promise: strict refuses a mixed series, the stage is gone,
#        the source is intact, and nothing claims an unpatched-but-present tree.
_reset_patch_dir()
_write_first_patch("0001-good.patch" "p1")
_write_bad_patch("0002-bad.patch")
set(_stage "${_work}/stage-strict-mixed")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "OFF" _rc)
_check(NOT _rc EQUAL 0 "strict refuses a mixed series")
_check(NOT EXISTS "${_stage}/.mikai-staged" "the discarded stage leaves no ownership stamp")
_check(NOT EXISTS "${_stage}/.mikai-patched" "the discarded stage leaves no applied stamp")
_read_canary("${_src}" _canary)
_check(_canary STREQUAL "VERSION=edited" "the source survives a failed strict build")

# --- 22. The same mixed series under warn keeps the tree and stamps nothing.
set(_rc "")
_stage("${_src}" "${_stage}" "warn" "OFF" _rc)
_check(_rc EQUAL 0 "warn configures with a mixed series")
_check(EXISTS "${_stage}/.mikai-staged" "warn keeps an owned tree")
_check(NOT EXISTS "${_stage}/.mikai-patched" "warn never stamps a mixed series")

# --- 23. No applier available: the tree stays unpatched and unstamped, and the
#        source is intact. Exercised without touching PATH.
file(REMOVE "${_scratch}/noapplier-dir")
_reset_patch_dir()
_write_first_patch("0001-good.patch" "p1")
set(_stage "${_work}/stage-noapplier")
_stage_with_applier("${_scratch}/noapplier-dir" "${_src}" "${_stage}" "warn" "OFF" _rc)
_check(_rc EQUAL 0 "warn configures when no applier exists")
_check(EXISTS "${_stage}/src/aji_trt.cpp" "the tree stays present when no applier exists")
_check(NOT EXISTS "${_stage}/.mikai-patched" "no stamp without an applier")
file(READ "${_stage}/src/aji_trt.cpp" _trt)
_check(_trt MATCHES "trt_v1" "the tree stays unpatched without an applier")
_reset_patch_dir()
_write_first_patch("0001-good.patch" "p1")
set(_stage "${_work}/stage-noapplier-strict")
_stage_with_applier("${_scratch}/noapplier-dir" "${_src}" "${_stage}" "strict" "OFF" _rc)
_check(NOT _rc EQUAL 0 "strict refuses when no applier exists")
_check(NOT EXISTS "${_stage}/.mikai-patched" "no stamp in the strict no-applier refusal")

# --- 24. AJI_RESTAGE forces a fresh copy of the current source.
_reset_patch_dir()
_write_first_patch("0001-good.patch" "p1")
set(_stage "${_work}/stage-restage")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "OFF" _rc)
_check(_rc EQUAL 0 "an initial series configures")
_mark("${_stage}" "STAGE-DROP.txt")
set(_rc "")
_stage("${_src}" "${_stage}" "strict" "ON" _rc)
_check(_rc EQUAL 0 "AJI_RESTAGE reconfigures")
_has("${_stage}" "STAGE-DROP.txt" _kept)
_check(_kept STREQUAL "FALSE" "AJI_RESTAGE replaces the staged tree")
_check(EXISTS "${_stage}/.mikai-staged" "AJI_RESTAGE rewrites the stamp")
_check(EXISTS "${_stage}/.mikai-patched" "AJI_RESTAGE reapplies the series")

# --- 25. An unknown policy value is refused rather than guessed.
_reset_patch_dir()
_write_first_patch("0001-good.patch" "p1")
set(_stage "${_work}/stage-badpolicy")
set(_rc "")
_stage("${_src}" "${_stage}" "nonsense" "OFF" _rc)
_check(NOT _rc EQUAL 0 "an unknown policy is refused")
_check(NOT EXISTS "${_stage}" "an unknown policy stages nothing")

message(STATUS "${_prefix}: sweep complete")
file(REMOVE_RECURSE "${_scratch}")
