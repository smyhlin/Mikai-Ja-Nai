# Staging and patch-policy logic for the optional native AnimeJaNai shim.
#
# This is PRODUCTION logic, not a test helper: native/CMakeLists.txt includes
# it, and tests/cmake/NativeStageContracts.cmake exercises it against synthetic
# trees. It holds no GPU, MSVC or nvcc knowledge.
#
# Contract, all of it behavioural (see the regression script):
#   1. AJI_SOURCE_DIR is read-only input. It is never written to, never patched
#      and never deleted, and no failure path may touch it.
#   2. AJI_STAGE_DIR must overlap AJI_SOURCE_DIR in NO way: not equal, not an
#      ancestor, not nested inside it, and not through a junction or symlink
#      that resolves into it. Every overlap is a configure-time refusal.
#   3. Only a positively owned stage is deleted. A directory is owned when it is
#      absent, or when it carries the .mikai-staged stamp. A foreign directory
#      is never removed.
#   4. Staged identity covers every byte the copy takes, the patch series and
#      the policy. There is no 16-byte shortcut over one file.
#   5. The full-series stamp (.mikai-patched) is written only when every
#      applicable patch applied. A partial or absent application never gets it.
#   6. A failure never leaves a stamp claiming an applied series, and never
#      leaves a claim that the tree is unpatched-but-present after it was
#      deleted.
#
# The mode names and their meaning are fixed here and relied upon by the
# regression script:
#   strict - refuse to configure unless a nonempty applicable series applies in
#            full.
#   warn   - configure anyway, reporting unpatched/partial truthfully and
#            writing no full-series stamp.
#   off    - never apply the series; the stage stays unpatched and unstamped.

# ---------------------------------------------------------------------------
# Path helpers
# ---------------------------------------------------------------------------

# Canonical key: absolute, cmake path separators, lower case, no trailing slash
# (a drive root keeps its single slash so prefix tests stay meaningful).
function(mikai_stage_canonical_path path out)
  if(NOT IS_ABSOLUTE "${path}")
    get_filename_component(path "${path}" ABSOLUTE)
  endif()
  get_filename_component(path "${path}" ABSOLUTE)
  file(TO_CMAKE_PATH "${path}" path)
  if(path MATCHES "^[A-Za-z]:/*$")
    string(SUBSTRING "${path}" 0 1 _drive)
    set(path "${_drive}:/")
  else()
    set(_trimmed "")
    string(REGEX REPLACE "([^/])/+$" "\\1" _trimmed "${path}")
    set(path "${_trimmed}")
  endif()
  string(TOLOWER "${path}" path)
  set(${out} "${path}" PARENT_SCOPE)
endfunction()

# The OS-resolved path, so a junction or symlink is compared as what it really
# points at rather than as the name it was given.
function(mikai_stage_real_path path out)
  set(_real "")
  if(EXISTS "${path}")
    file(REAL_PATH "${path}" _real)
  endif()
  if(_real STREQUAL "")
    set(_real "${path}")
  endif()
  mikai_stage_canonical_path("${_real}" _key)
  set(${out} "${_key}" PARENT_SCOPE)
endfunction()

# TRUE when child is inside parent, or is parent itself.
function(mikai_stage_is_within child parent out)
  set(_result FALSE)
  if(NOT parent STREQUAL "")
    string(LENGTH "${parent}" _plen)
    string(SUBSTRING "${child}" 0 ${_plen} _head)
    if(_head STREQUAL parent)
      string(LENGTH "${child}" _clen)
      if(_clen EQUAL _plen)
        set(_result TRUE)
      else()
        string(SUBSTRING "${child}" ${_plen} -1 _rest)
        if(_rest MATCHES "^/")
          set(_result TRUE)
        endif()
      endif()
    endif()
  endif()
  set(${out} "${_result}" PARENT_SCOPE)
endfunction()

# TRUE when the two paths describe the same directory or contain one another.
function(mikai_stage_overlaps a b out)
  mikai_stage_is_within("${a}" "${b}" _a_in_b)
  mikai_stage_is_within("${b}" "${a}" _b_in_a)
  if(_a_in_b OR _b_in_a)
    set(${out} TRUE PARENT_SCOPE)
  else()
    set(${out} FALSE PARENT_SCOPE)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Content identity
# ---------------------------------------------------------------------------

# Every regular file the staged copy will take, as a deterministic sorted list
# of source-relative paths. The excluded names must agree exactly with the
# file(COPY ...) patterns below, so identity describes what is really copied.
function(mikai_stage_copied_files root out)
  set(_all "")
  file(GLOB_RECURSE _all LIST_DIRECTORIES false RELATIVE "${root}" "${root}/*")
  set(_kept "")
  foreach(_rel IN LISTS _all)
    set(_excluded FALSE)
    if(_rel MATCHES "(^|/)\\.git(/|$)")
      set(_excluded TRUE)
    endif()
    if(_rel MATCHES "(^|/)build(/|$)")
      set(_excluded TRUE)
    endif()
    # Our own markers must never be copied out of a source tree that happens to
    # be a stage of its own: a borrowed .mikai-* file would make a foreign
    # directory look owned.
    if(_rel MATCHES "(^|/)\\.mikai-[^/]*$")
      set(_excluded TRUE)
    endif()
    if(NOT _excluded)
      list(APPEND _kept "${_rel}")
    endif()
  endforeach()
  list(SORT _kept)
  set(${out} "${_kept}" PARENT_SCOPE)
endfunction()

# One "source:<rel>=<sha256>" line per copied file, in sorted order. Every file
# contributes, so an edit anywhere in the tree - header, dispatcher, DML source
# or late TRT code - changes the identity.
function(mikai_stage_source_identity root out)
  mikai_stage_copied_files("${root}" _files)
  set(_lines "")
  foreach(_rel IN LISTS _files)
    file(SHA256 "${root}/${_rel}" _digest)
    string(APPEND _lines "source:${_rel}=${_digest}\n")
  endforeach()
  set(${out} "${_lines}" PARENT_SCOPE)
endfunction()

# One "<name>=<sha256>" line per patch file, in the order the build globs and
# sorts. This is the series identity the manifest records: it is reproducible by
# a recipient from the repository's own files alone.
function(mikai_stage_series_identity patch_dir out_lines out_files)
  set(_series "")
  if(EXISTS "${patch_dir}")
    file(GLOB _series LIST_DIRECTORIES false "${patch_dir}/*.patch")
  endif()
  list(SORT _series)
  set(_lines "")
  foreach(_p IN LISTS _series)
    file(SHA256 "${_p}" _digest)
    get_filename_component(_name "${_p}" NAME)
    string(APPEND _lines "${_name}=${_digest}\n")
  endforeach()
  set(${out_lines} "${_lines}" PARENT_SCOPE)
  set(${out_files} "${_series}" PARENT_SCOPE)
endfunction()

# TRUE for a patch that carries consumable hunks. Files that are high-level
# descriptions with bare or placeholder @@ headers are not applicable, and are
# reported as such rather than counted as applied.
function(mikai_stage_patch_is_applicable patch_file out)
  file(READ "${patch_file}" _text)
  if(_text MATCHES "@@ -[0-9]+(,[0-9]+)? \\+[0-9]+(,[0-9]+)? @@")
    set(${out} TRUE PARENT_SCOPE)
  else()
    set(${out} FALSE PARENT_SCOPE)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Ownership
# ---------------------------------------------------------------------------

function(mikai_stage_stamp_path stage_dir out)
  set(${out} "${stage_dir}/.mikai-staged" PARENT_SCOPE)
endfunction()

function(mikai_stage_patched_stamp_path stage_dir out)
  set(${out} "${stage_dir}/.mikai-patched" PARENT_SCOPE)
endfunction()

# A stage may be deleted only when it is absent, or when it is a directory that
# carries one of our own markers. Anything else - a user directory, a junction
# target, a file - is refused instead of removed.
#
# Both markers count, and they are never copied out of a source tree (the
# file(COPY) above excludes .mikai-* and the fingerprint skips them), so a
# directory carrying either one was written by this build and can only be one
# of its stages. Accepting either is what makes a stage recoverable: someone (or
# a clean) may have removed .mikai-staged alone, and that must trigger a rebuild
# rather than permanently refuse the path.
function(mikai_stage_is_owned stage_dir out)
  mikai_stage_stamp_path("${stage_dir}" _stamp)
  mikai_stage_patched_stamp_path("${stage_dir}" _patched_stamp)
  if(NOT EXISTS "${stage_dir}")
    set(${out} TRUE PARENT_SCOPE)
    return()
  endif()
  if(NOT IS_DIRECTORY "${stage_dir}")
    set(${out} FALSE PARENT_SCOPE)
    return()
  endif()
  if(EXISTS "${_stamp}" OR EXISTS "${_patched_stamp}")
    set(${out} TRUE PARENT_SCOPE)
  else()
    set(${out} FALSE PARENT_SCOPE)
  endif()
endfunction()

# Remove a positively owned stage. A foreign path is a fatal error, never a
# deletion.
function(mikai_stage_discard_stage stage_dir why)
  mikai_stage_is_owned("${stage_dir}" _owned)
  if(NOT _owned)
    message(FATAL_ERROR
      "Refusing to remove ${stage_dir}: ${why} asked for a fresh stage there, but "
      "that path already exists and is not a staged tree owned by this build. "
      "Point AJI_STAGE_DIR somewhere else; this build never deletes a directory "
      "it did not create. Its ownership marker is "
      "${stage_dir}/.mikai-staged.")
  endif()
  file(REMOVE_RECURSE "${stage_dir}")
endfunction()

# ---------------------------------------------------------------------------
# The production entry point
# ---------------------------------------------------------------------------

function(mikai_stage_inference_source)
  cmake_parse_arguments(_s ""
    "AJI_SOURCE_DIR;AJI_STAGE_DIR;AJI_PATCH_DIR;AJI_PATCH_POLICY;AJI_RESTAGE;OUT_STAGE_VALID;OUT_PATCH_STATE"
    "" ${ARGN})

  set(_stage_valid FALSE)
  set(_patch_state "")

  if(_s_AJI_SOURCE_DIR STREQUAL "" OR _s_AJI_STAGE_DIR STREQUAL "")
    message(FATAL_ERROR
      "mikai_stage_inference_source: AJI_SOURCE_DIR and AJI_STAGE_DIR are required.")
  endif()

  set(_policy "${_s_AJI_PATCH_POLICY}")
  if(_policy STREQUAL "")
    set(_policy "strict")
  endif()
  if(NOT _policy MATCHES "^(strict|warn|off)$")
    message(FATAL_ERROR
      "AJI_PATCH_POLICY must be strict, warn or off; got '${_policy}'.")
  endif()

  mikai_stage_canonical_path("${_s_AJI_SOURCE_DIR}" _source)
  mikai_stage_canonical_path("${_s_AJI_STAGE_DIR}" _stage)
  set(_patch_dir "${_s_AJI_PATCH_DIR}")
  if(_patch_dir STREQUAL "")
    set(_patch_dir "${CMAKE_CURRENT_LIST_DIR}/patches")
  endif()
  mikai_stage_canonical_path("${_patch_dir}" _patch_dir_key)

  # --- Guard 1: the stage must not touch the source, by any spelling of the
  # same path. This runs before anything is created or removed.
  mikai_stage_real_path("${_s_AJI_SOURCE_DIR}" _source_real)
  mikai_stage_real_path("${_s_AJI_STAGE_DIR}" _stage_real)
  foreach(_candidate IN LISTS _stage _stage_real)
    foreach(_target IN LISTS _source _source_real)
      mikai_stage_overlaps("${_candidate}" "${_target}" _overlap)
      if(_overlap)
        message(FATAL_ERROR
          "AJI_STAGE_DIR (${_s_AJI_STAGE_DIR}) overlaps AJI_SOURCE_DIR "
          "(${_s_AJI_SOURCE_DIR}). The source tree is read-only input, so a stage "
          "that is equal to it, contains it, or sits inside it can destroy the "
          "external source. Point AJI_STAGE_DIR at a directory outside the source "
          "tree; the default is a build-local path.")
      endif()
    endforeach()
  endforeach()

  # A stage that is a link to something outside the source is fine and is used
  # as its real path; one that merely re-spells the source is already refused.
  set(_stage_path "${_stage}")
  if(NOT _stage_real STREQUAL _stage)
    set(_stage_path "${_stage_real}")
  endif()

  # --- Identity: the whole copied tree, plus the series and the policy.
  mikai_stage_series_identity("${_patch_dir}" _series_lines _series_files)
  mikai_stage_source_identity("${_s_AJI_SOURCE_DIR}" _source_lines)
  set(_identity "${_series_lines}policy=${_policy}\n${_source_lines}")

  set(_applicable "")
  set(_description_only "")
  foreach(_p IN LISTS _series_files)
    mikai_stage_patch_is_applicable("${_p}" _ok)
    if(_ok)
      list(APPEND _applicable "${_p}")
    else()
      list(APPEND _description_only "${_p}")
    endif()
  endforeach()

  mikai_stage_stamp_path("${_stage_path}" _staged_stamp)
  mikai_stage_patched_stamp_path("${_stage_path}" _patched_stamp)

  # --- Decide whether the staged tree is still current.
  set(_restage FALSE)
  if(_s_AJI_RESTAGE)
    set(_restage TRUE)
    message(STATUS "AJI_RESTAGE is on; re-staging ${_s_AJI_SOURCE_DIR}.")
  elseif(NOT EXISTS "${_staged_stamp}")
    set(_restage TRUE)
  else()
    file(READ "${_staged_stamp}" _staged_identity)
    if(NOT _staged_identity STREQUAL _identity)
      message(STATUS
        "Staged source is stale (source bytes, patch series or policy changed); restaging.")
      set(_restage TRUE)
    endif()
  endif()

  # A missing stamp is a rebuild trigger, not an ownership violation, so this
  # guard runs only for a tree we intend to KEEP: keeping means touching a
  # directory we must be able to name as ours, and a foreign directory is never
  # touched at all. A tree that is about to be restaged is discarded by
  # mikai_stage_discard_stage, which makes the same ownership check and removes
  # nothing that is not ours.
  if(NOT _restage)
    mikai_stage_is_owned("${_stage_path}" _owned)
    if(NOT _owned)
      message(FATAL_ERROR
        "AJI_STAGE_DIR (${_s_AJI_STAGE_DIR}) already exists and is not a staged tree "
        "owned by this build: ${_stage_path}/.mikai-staged is missing while its "
        "identity matches the current inputs. Remove it yourself, choose another "
        "AJI_STAGE_DIR, or use a default build-local path. This build never deletes a "
        "directory it did not create.")
    endif()
  endif()

  if(_restage)
    # The whole tree dies with the stage, so any series stamp dies with it: a
    # fresh copy is unpatched until this configure proves otherwise.
    mikai_stage_discard_stage("${_stage_path}" "restaging")
    file(COPY "${_s_AJI_SOURCE_DIR}/" DESTINATION "${_stage_path}"
         PATTERN ".git" EXCLUDE
         PATTERN "build" EXCLUDE
         PATTERN ".mikai-*" EXCLUDE)
    file(WRITE "${_staged_stamp}" "${_identity}")
    message(STATUS "Staged animejanai-inference into ${_stage_path}")
    file(REMOVE "${_patched_stamp}")
  endif()

  set(_stage_valid TRUE)
  set(_patch_state "unpatched")

  # A tree that already carries this exact series is not re-applied to it: the
  # second configure is a no-op, not a failed patch. This is the idempotence
  # rule that lets an unchanged reconfigure succeed.
  set(_already_patched FALSE)
  if(NOT _restage AND EXISTS "${_patched_stamp}")
    file(READ "${_patched_stamp}" _patched_identity)
    if(_patched_identity STREQUAL _identity)
      set(_already_patched TRUE)
      message(STATUS "Staged tree already carries this patch series; not re-applying.")
    endif()
  endif()
  if(_already_patched)
    set(_patch_state "full")
  endif()

  if(NOT _applicable)
    if(_description_only)
      set(_patch_state "none")
      message(STATUS
        "No applicable patch in ${_patch_dir}; ${_description_only} read as descriptions, not diffs.")
    else()
      set(_patch_state "none")
      message(STATUS "No patch series found in ${_patch_dir}.")
    endif()
  endif()

  # --- Apply, per policy. `off` never applies anything, and a tree that already
  # carries this exact series is never re-applied to.
  set(_apply FALSE)
  if(_policy MATCHES "^(strict|warn)$" AND _applicable AND NOT _already_patched)
    set(_apply TRUE)
  endif()

  if(_apply)
    # Already-set variables win: a caller (or the regression script) may pin an
    # exact applier, or an unusable one to observe the no-applier branch.
    if(NOT DEFINED MIKAI_STAGE_GIT_EXECUTABLE)
      find_program(MIKAI_STAGE_GIT_EXECUTABLE NAMES git)
    endif()
    if(NOT DEFINED MIKAI_STAGE_PATCH_EXECUTABLE)
      find_program(MIKAI_STAGE_PATCH_EXECUTABLE NAMES patch)
    endif()
    set(_applier "")
    if(MIKAI_STAGE_GIT_EXECUTABLE)
      set(_applier "git")
    elseif(MIKAI_STAGE_PATCH_EXECUTABLE)
      set(_applier "patch")
    endif()

    set(_total 0)
    set(_applied 0)
    set(_failed "")
    foreach(_p IN LISTS _applicable)
      get_filename_component(_name "${_p}" NAME)
      if(_applier STREQUAL "")
        message(WARNING
          "${_name} not applied: neither git nor patch was found on PATH.")
        continue()
      endif()
      math(EXPR _total "${_total} + 1")
      if(_applier STREQUAL "git")
        execute_process(COMMAND "${MIKAI_STAGE_GIT_EXECUTABLE}" apply -p1 --whitespace=nowarn "${_p}"
          WORKING_DIRECTORY "${_stage_path}"
          RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
      else()
        execute_process(COMMAND "${MIKAI_STAGE_PATCH_EXECUTABLE}" -p1 -i "${_p}"
          WORKING_DIRECTORY "${_stage_path}"
          RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
      endif()
      if(_rc EQUAL 0)
        math(EXPR _applied "${_applied} + 1")
        message(STATUS "Applied ${_name}")
      else()
        list(APPEND _failed "${_name}")
        # A half-applied series is worse than none, because the failure would
        # only surface later as wrong behaviour. Under strict the tree is
        # discarded and the configure fails. Under warn the tree is kept but is
        # reported as partial: warn's whole point is to configure onward, so
        # destroying the stage there would contradict its own contract.
        if(_policy STREQUAL "strict")
          mikai_stage_discard_stage("${_stage_path}" "a failed patch")
          file(REMOVE "${_patched_stamp}")
          set(_stage_valid FALSE)
          set(_patch_state "stale")
          message(WARNING
            "Failed to apply ${_name}; the staged tree has been discarded. It is "
            "gone, not unpatched, and no series stamp was written.")
        else()
          message(WARNING
            "Failed to apply ${_name}: ${_err}\nThe staged tree is kept and is now "
            "PARTIALLY patched (AJI_PATCH_POLICY=${_policy}); no full-series stamp "
            "was written, so the tree is never mistaken for a fully patched one.")
        endif()
        break()
      endif()
    endforeach()

    if(_stage_valid)
      if(_applied EQUAL _total AND _total GREATER 0)
        set(_patch_state "full")
        file(WRITE "${_patched_stamp}" "${_identity}")
        message(STATUS "Applied the complete series (${_applied}/${_total}).")
      elseif(_applied GREATER 0)
        set(_patch_state "partial")
        message(WARNING
          "Only ${_applied} of ${_total} applicable patches applied; no full-series "
          "stamp was written.")
      else()
        set(_patch_state "unpatched")
        message(WARNING
          "No applicable patch could be applied; no full-series stamp was written.")
      endif()
    endif()
  endif()

  # A stale stage must never keep a series stamp, whatever the policy: the tree
  # was deleted, so any later configure cannot read that stamp as proof of an
  # applied series.
  if(NOT _stage_valid AND EXISTS "${_patched_stamp}")
    file(REMOVE "${_patched_stamp}")
  endif()

  # --- Truthfulness for strict and warn, after the state is known.
  if(_patch_state STREQUAL "stale")
    if(_policy STREQUAL "strict")
      message(FATAL_ERROR
        "The staged tree was discarded after a failed patch in ${_patch_dir} and "
        "AJI_PATCH_POLICY=strict. A shim built without the series would not answer "
        "MP_FILTER_COMMAND_GET_META, would have no preparedOnly refusal and would "
        "still keep the weak engine cache, so it would not be the artefact M04/M05 "
        "record. Write the real diffs, or reconfigure with -DAJI_PATCH_POLICY=warn "
        "for a toolchain smoke that proves nothing about behaviour.")
    endif()
  elseif(_patch_state STREQUAL "none")
    if(_policy STREQUAL "strict")
      message(FATAL_ERROR
        "AJI_PATCH_POLICY=strict requires a nonempty applicable patch series in "
        "${_patch_dir}, and none is present. A shim built without it would not "
        "answer MP_FILTER_COMMAND_GET_META, would have no preparedOnly refusal and "
        "would still keep the weak engine cache, so it would not be the artefact "
        "M04/M05 record. Write the real diffs, or reconfigure with "
        "-DAJI_PATCH_POLICY=warn for a toolchain smoke that proves nothing about "
        "behaviour.")
    elseif(_policy STREQUAL "warn")
      message(WARNING
        "Configuring WITHOUT the patch series (AJI_PATCH_POLICY=warn): no applicable "
        "patch was found. The resulting DLLs are a toolchain smoke only and must not "
        "be bundled, hashed or recorded as the M04 artefact.")
    endif()
  elseif(_patch_state STREQUAL "partial" OR _patch_state STREQUAL "unpatched")
    if(_policy STREQUAL "strict")
      message(FATAL_ERROR
        "AJI_PATCH_POLICY=strict requires the applicable patch series in "
        "${_patch_dir} to apply in full, and it did not. A shim built without it "
        "would not answer MP_FILTER_COMMAND_GET_META, would have no preparedOnly "
        "refusal and would still keep the weak engine cache, so it would not be the "
        "artefact M04/M05 record. Write the real diffs, or reconfigure with "
        "-DAJI_PATCH_POLICY=warn for a toolchain smoke that proves nothing about "
        "behaviour.")
    else()
      message(WARNING
        "Configuring with an INCOMPLETE patch series (AJI_PATCH_POLICY=warn). The "
        "resulting DLLs are a toolchain smoke only and must not be bundled, hashed "
        "or recorded as the M04 artefact.")
    endif()
  elseif(_patch_state STREQUAL "full")
    if(_policy STREQUAL "off")
      message(FATAL_ERROR
        "Internal error: AJI_PATCH_POLICY=off must never apply the series.")
    endif()
  endif()

  if(_s_OUT_STAGE_VALID)
    set(${_s_OUT_STAGE_VALID} "${_stage_valid}" PARENT_SCOPE)
  endif()
  if(_s_OUT_PATCH_STATE)
    set(${_s_OUT_PATCH_STATE} "${_patch_state}" PARENT_SCOPE)
  endif()
endfunction()
