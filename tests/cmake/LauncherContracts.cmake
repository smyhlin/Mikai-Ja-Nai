# The launcher process-lifetime contract (KB-26).
#
# Runs the real `mikai_test_launcher` binary -- never a source-text check -- and
# asserts the observable behaviour a hung test suite must produce:
#
#   1. Deadline resolution: the CLI flag wins over the environment variable,
#      which wins over the built-in default; bad values are refused, not
#      clamped, and a deadline query runs nothing and stages nothing.
#   2. A hung child that spawned a real descendant: the launcher exits with its
#      deadline code, says so on stderr, kills the direct child AND the
#      descendant, and removes the scratch directory it owns.
#   3. Cleanup safety: a neighbouring file and directory under the same scratch
#      parent survive untouched, byte-identical.
#   4. A normal child still propagates its exact exit status, with and without
#      an explicit deadline, and leaves the scratch tree clean.
#
# The child fixture (`mikai_launcher_child`) writes each process's identity (PID
# plus Win32 creation time) to a file, so tree state is read from the OS rather
# than inferred: a launcher that reaps only the direct child, that wipes its
# whole scratch parent, or that never returns, fails here.

if(NOT DEFINED LAUNCHER_EXE OR NOT EXISTS "${LAUNCHER_EXE}")
  message(FATAL_ERROR "LauncherContracts: LAUNCHER_EXE is not set or missing: '${LAUNCHER_EXE}'")
endif()
if(NOT DEFINED FIXTURE_EXE OR NOT EXISTS "${FIXTURE_EXE}")
  message(FATAL_ERROR "LauncherContracts: FIXTURE_EXE is not set or missing: '${FIXTURE_EXE}'")
endif()
if(NOT DEFINED SHADERS_DIR OR NOT IS_DIRECTORY "${SHADERS_DIR}")
  message(FATAL_ERROR "LauncherContracts: SHADERS_DIR is not set or missing: '${SHADERS_DIR}'")
endif()
if(NOT DEFINED MPV_DLL OR NOT EXISTS "${MPV_DLL}")
  message(FATAL_ERROR "LauncherContracts: MPV_DLL is not set or missing: '${MPV_DLL}'")
endif()
if(NOT DEFINED QT_BIN_DIR OR NOT IS_DIRECTORY "${QT_BIN_DIR}")
  message(FATAL_ERROR "LauncherContracts: QT_BIN_DIR is not set or missing: '${QT_BIN_DIR}'")
endif()
if(NOT DEFINED QT_PLUGIN_DIR OR NOT IS_DIRECTORY "${QT_PLUGIN_DIR}/platforms")
  message(FATAL_ERROR
    "LauncherContracts: QT_PLUGIN_DIR is not set or has no platforms/: '${QT_PLUGIN_DIR}'")
endif()

# ---------------------------------------------------------------------------
# Workspace: fresh, isolated, and never inside the public build output
# ---------------------------------------------------------------------------

set(_workspace "${CMAKE_CURRENT_BINARY_DIR}/LauncherContracts")
file(REMOVE_RECURSE "${_workspace}")
file(MAKE_DIRECTORY "${_workspace}")

set(_failures "")

macro(check)
  # Every comparison is written as `check(<lhs> EQUAL <rhs> "description")`, so
  # the condition is everything but the last argument. Taking a fixed first
  # parameter would make `EQUAL` the description and the bare left-hand value
  # the condition, which silently inverts every comparison in this file.
  set(_all ${ARGN})
  list(POP_BACK _all _description)
  if(${_all})
    message(STATUS "LauncherContracts: PASS ${_description}")
  else()
    message(SEND_ERROR "LauncherContracts: FAIL ${_description}")
    list(APPEND _failures "${_description}")
  endif()
endmacro()

macro(cleanup _paths)
  file(REMOVE_RECURSE ${_paths})
endmacro()

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function(run_launcher result_var stdout_var stderr_var scratch_parent)
  # The full six-positional launcher invocation, identical in shape to the
  # `mikai_tests` registration: the child under test, the shaders source, the
  # pinned mpv module, the Qt bin dir, the Qt plugin dir and the scratch parent.
  # Anything after the scratch parent is forwarded verbatim, which is why the
  # callers pass plain arguments and not brace-wrapped text: CMake keeps the
  # braces as literal characters, so `{--launcher-deadline}` reaches the child
  # as an unknown argument and the option under test never runs.
  set(_args
    "${LAUNCHER_EXE}"
    "${FIXTURE_EXE}"
    "${SHADERS_DIR}"
    "${MPV_DLL}"
    "${QT_BIN_DIR}"
    "${QT_PLUGIN_DIR}"
    "${scratch_parent}")
  foreach(_arg IN LISTS ARGN)
    list(APPEND _args "${_arg}")
  endforeach()
  execute_process(
    COMMAND ${_args}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
  )
  set(${result_var} "${_rc}" PARENT_SCOPE)
  set(${stdout_var} "${_out}" PARENT_SCOPE)
  set(${stderr_var} "${_err}" PARENT_SCOPE)
endfunction()

function(read_identity_file pid_var creation_var path)
  # The fixture writes `pid=<n>` and `creation=<epoch ms>`. A missing or
  # malformed file is reported by the caller, never silently treated as a
  # process that exists.
  set(${pid_var} "" PARENT_SCOPE)
  set(${creation_var} "" PARENT_SCOPE)
  if(NOT EXISTS "${path}")
    return()
  endif()
  file(READ "${path}" _text)
  string(REGEX MATCH "pid=([0-9]+)" _pid_match "${_text}")
  string(REGEX MATCH "creation=([0-9]+)" _creation_match "${_text}")
  if(NOT _pid_match OR NOT _creation_match)
    return()
  endif()
  set(${pid_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${creation_var} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

function(process_identity_or_empty result_var pid expected_name)
  # One PowerShell query, one argument, no nested quoting: the PID, the image
  # name and the creation time of the process that currently holds this PID.
  # Prints `alive` when such a process exists, and nothing when it does not.
  if(NOT pid MATCHES "^[0-9]+$" OR pid EQUAL 0)
    set(${result_var} "" PARENT_SCOPE)
    return()
  endif()
  set(_ps
    "foreach ($p in (Get-Process)) { "
    "if ($p.Id -eq ${pid} -and $p.SafeHandle -ne $null -and $p.Name -eq '${expected_name}') { "
    "$stamp = [int64]([DateTimeOffset]$p.StartTime.ToUniversalTime()).ToUnixTimeMilliseconds(); "
    "Write-Output \"$($p.Id)|$stamp\"; break } }")
  execute_process(
    COMMAND powershell -NoProfile -NonInteractive -Command "${_ps}"
    OUTPUT_VARIABLE _out
    ERROR_QUIET
    RESULT_VARIABLE _rc
  )
  string(STRIP "${_out}" _out)
  set(${result_var} "${_out}" PARENT_SCOPE)
endfunction()

function(process_alive_matching result_var pid creation_ms expected_name)
  # The identity test: a process with this PID, this image name and this
  # creation time. A recycled PID has a different start time and almost always a
  # different image, so it cannot answer yes. This is what makes "the tree died"
  # a fact about the actual processes rather than an assumption about PID
  # numbers.
  set(${result_var} FALSE PARENT_SCOPE)
  if(NOT creation_ms MATCHES "^[0-9]+$")
    return()
  endif()
  process_identity_or_empty(_identity "${pid}" "${expected_name}")
  if(_identity STREQUAL "")
    return()
  endif()
  string(REPLACE "|" ";" _parts "${_identity}")
  list(GET _parts 1 _stamp)
  if(NOT _stamp MATCHES "^[0-9]+$")
    # The PID is held by the expected image but its start time is unreadable
    # (an access error, not an absence). Reporting it as alive is the
    # conservative reading: it cannot turn a real leak into a pass.
    set(${result_var} TRUE PARENT_SCOPE)
    return()
  endif()
  math(EXPR _delta "${_stamp} - ${creation_ms}")
  if(_delta LESS 0)
    math(EXPR _delta "0 - ${_delta}")
  endif()
  # A process seen with the recorded start time is the same process; anything
  # further away than the launch of this run is a recycled PID.
  if(_delta LESS 600000)
    set(${result_var} TRUE PARENT_SCOPE)
  endif()
endfunction()

function(wait_for_processes_gone result_var pids expected_name deadline_ms)
  # TerminateJobObject is complete when it returns, but the OS still needs a
  # moment for the image names to disappear. Bounded so the contract stays fast;
  # this cannot mask a leak, because the fixture's descendant only ever exits
  # after ten minutes on its own.
  set(_remaining "${deadline_ms}")
  set(_all_gone TRUE)
  while(_remaining GREATER 0)
    set(_all_gone TRUE)
    foreach(_pid IN LISTS pids)
      process_identity_or_empty(_identity "${_pid}" "${expected_name}")
      if(NOT _identity STREQUAL "")
        set(_all_gone FALSE)
      endif()
    endforeach()
    if(_all_gone)
      break()
    endif()
    execute_process(COMMAND powershell -NoProfile -NonInteractive -Command "Start-Sleep -Milliseconds 100")
    math(EXPR _remaining "${_remaining} - 100")
  endwhile()
  set(${result_var} "${_all_gone}" PARENT_SCOPE)
endfunction()

function(reap _pids)
  # Deterministic best-effort reaping of anything a fixture left behind, so a
  # broken assertion cannot leak a parked process out of this run.
  foreach(_pid IN LISTS _pids)
    execute_process(
      COMMAND taskkill /PID ${_pid} /T /F
      OUTPUT_QUIET ERROR_QUIET)
  endforeach()
endfunction()

function(collect_leaks result_var parent)
  # Every `mikai-tests-*` staging directory left under a scratch parent.
  file(GLOB _found LIST_DIRECTORIES TRUE "${parent}/mikai-tests-*")
  set(${result_var} "${_found}" PARENT_SCOPE)
endfunction()

# The staged child keeps its own file name, so the processes the fixture starts
# are discoverable by image name as well as by PID.
file(REMOVE_RECURSE "${_workspace}/LauncherContracts.tmp")
get_filename_component(_fixture_name "${FIXTURE_EXE}" NAME_WE)

# ===========================================================================
# Scenario 1: deadline resolution, and the query stages nothing
# ===========================================================================

run_launcher(_rc _out _err "${_workspace}/deadline-default" --launcher-deadline)
check(_rc EQUAL 0 "the deadline query exits 0")
check(_out MATCHES "600000" "the reported default deadline is the built-in 600000 ms")

set(_env_backup "$ENV{MIKAI_TEST_TIMEOUT_MS}")
set(ENV{MIKAI_TEST_TIMEOUT_MS} "4321")
run_launcher(_rc _out _err "${_workspace}/deadline-env" --launcher-deadline)
check(_rc EQUAL 0 "an environment deadline is accepted")
check(_out MATCHES "4321" "the environment deadline is reported")

run_launcher(_rc _out _err "${_workspace}/deadline-both"
  --launcher-timeout-ms 1234 --launcher-deadline)
check(_rc EQUAL 0 "a CLI deadline alongside an environment value is accepted")
check(_out MATCHES "1234" "the CLI deadline wins over the environment value")

set(ENV{MIKAI_TEST_TIMEOUT_MS} "0")
run_launcher(_rc _out _err "${_workspace}/deadline-bad-env" --launcher-deadline)
check(_rc EQUAL 2 "a non-positive environment deadline is refused, not clamped")

set(ENV{MIKAI_TEST_TIMEOUT_MS} "${_env_backup}")
run_launcher(_rc _out _err "${_workspace}/deadline-bad-cli"
  --launcher-timeout-ms abc --launcher-deadline)
check(_rc EQUAL 2 "a non-numeric CLI deadline is refused, not clamped")

run_launcher(_rc _out _err "${_workspace}/deadline-negative"
  --launcher-timeout-ms=-7 --launcher-deadline)
check(_rc EQUAL 2 "a negative CLI deadline is refused, not clamped")

run_launcher(_rc _out _err "${_workspace}/deadline-missing-value"
  --launcher-timeout-ms)
check(_rc EQUAL 2 "a deadline option with no value is refused")

# The protocol is six positional inputs; a shorter call is a launcher-side
# failure, not a run. Spelled out because run_launcher always supplies the full
# protocol by design.
execute_process(
  COMMAND "${LAUNCHER_EXE}" "${FIXTURE_EXE}" "${SHADERS_DIR}"
  RESULT_VARIABLE _rc
  OUTPUT_QUIET ERROR_QUIET)
check(_rc EQUAL 2 "a launcher call without the six positional inputs is refused")

# A deadline query runs nothing, so no staging directory may appear under any of
# the parents above. Empty parents are removed here so the workspace stays small.
set(_any_scratch "")
foreach(_case deadline-default deadline-env deadline-both deadline-bad-env
             deadline-bad-cli deadline-negative deadline-missing-value)
  collect_leaks(_found "${_workspace}/${_case}")
  if(_found)
    list(APPEND _any_scratch "${_found}")
  else()
    cleanup("${_workspace}/${_case}")
  endif()
endforeach()
check(NOT _any_scratch "the deadline query creates no scratch directory")

# ===========================================================================
# Scenario 2: a hung child that spawned a real descendant
# ===========================================================================

# One isolated scratch parent, holding a sentinel the launcher must never touch
# and a sibling directory that must survive the run.
set(_hang_parent "${_workspace}/hang-parent")
file(MAKE_DIRECTORY "${_hang_parent}")
file(WRITE "${_hang_parent}/NEIGHBOUR-SENTINEL.txt" "neighbour data, must survive\n")
file(MAKE_DIRECTORY "${_hang_parent}/neighbour-directory")
file(WRITE "${_hang_parent}/neighbour-directory/neighbour.txt" "sibling payload\n")

set(_child_id "${_workspace}/hang-child.id")
set(_descendant_id "${_workspace}/hang-descendant.id")

# 1500 ms is enough for both processes to record their identities and short
# enough that the scenario finishes far inside the CTest bound of 180 s. The
# descendant parks for ten minutes, far beyond any deadline here, so an
# un-reaped descendant is still there to be found.
run_launcher(_rc _out _err "${_hang_parent}"
  --hang
  --print-id "${_child_id}"
  --descendant-id "${_descendant_id}"
  --descendant-ms 600000
  --launcher-timeout-ms 1500)

check(_rc EQUAL 3 "a hung child yields the launcher's deadline exit code 3")
check(_err MATCHES "deadline" "stderr names the deadline")
check(_err MATCHES "kill" "stderr names the kill")

# Both identities come from processes that actually ran. The child's file is
# written only after the descendant has started, so a missing child file means
# the hang never really began, which is reported separately from the kill.
check(EXISTS "${_child_id}" "the hung child recorded its own identity before parking")
read_identity_file(_child_pid _child_creation "${_child_id}")
check(EXISTS "${_descendant_id}" "the descendant recorded its identity before parking")
read_identity_file(_descendant_pid _descendant_creation "${_descendant_id}")
check(_child_pid MATCHES "^[0-9]+$" "the child's identity is well formed")
check(_descendant_pid MATCHES "^[0-9]+$" "the descendant's identity is well formed")

# The tree kill is what is being measured, so the processes are given a bounded
# moment to disappear and then checked by identity, not by PID number.
set(_hung_pids "${_child_pid}")
if(_descendant_pid MATCHES "^[0-9]+$")
  list(APPEND _hung_pids "${_descendant_pid}")
endif()
wait_for_processes_gone(_tree_settled "${_hung_pids}" "${_fixture_name}" 5000)
check(_tree_settled "the killed process tree is gone by identity (child and descendant)")

process_alive_matching(_child_alive "${_child_pid}" "${_child_creation}" "${_fixture_name}")
process_alive_matching(_descendant_alive "${_descendant_pid}" "${_descendant_creation}"
                       "${_fixture_name}")
check(NOT _child_alive "the hung child process is gone (identity checked, not PID reuse)")
check(NOT _descendant_alive
       "the hung child's descendant is gone too (the kill is a tree kill)")

collect_leaks(_leftover "${_hang_parent}")
check(NOT _leftover "the launcher-owned scratch directory is removed")

check(EXISTS "${_hang_parent}/NEIGHBOUR-SENTINEL.txt" "the neighbouring sentinel file survives")
file(READ "${_hang_parent}/NEIGHBOUR-SENTINEL.txt" _sentinel_text)
check(_sentinel_text STREQUAL "neighbour data, must survive\n"
       "the neighbouring sentinel file is byte-identical")
check(EXISTS "${_hang_parent}/neighbour-directory/neighbour.txt"
       "the neighbouring directory survives")
file(READ "${_hang_parent}/neighbour-directory/neighbour.txt" _sibling_text)
check(_sibling_text STREQUAL "sibling payload\n"
       "the neighbouring directory is byte-identical")

# Reap and remove even though the assertions above may have failed.
reap("${_child_pid}" "${_descendant_pid}")
cleanup("${_hang_parent}")

# ===========================================================================
# Scenario 3: a normal child, with and without an explicit deadline
# ===========================================================================

set(_normal_parent "${_workspace}/normal-parent")
file(MAKE_DIRECTORY "${_normal_parent}")
set(_normal_id "${_workspace}/normal-child.id")

run_launcher(_rc _out _err "${_normal_parent}"
  --exit-code 0 --print-id "${_normal_id}")
check(_rc EQUAL 0 "a child that exits 0 propagates launcher exit 0")
read_identity_file(_normal_pid _normal_creation "${_normal_id}")
process_alive_matching(_normal_alive "${_normal_pid}" "${_normal_creation}" "${_fixture_name}")
check(NOT _normal_alive "the successful child is gone after it returned")
collect_leaks(_normal_leftover "${_normal_parent}")
check(NOT _normal_leftover "a successful child still removes its owned scratch")

set(_fail_parent "${_workspace}/fail-parent")
file(MAKE_DIRECTORY "${_fail_parent}")
run_launcher(_rc _out _err "${_fail_parent}"
  --exit-code 5
  --launcher-timeout-ms 60000)
check(_rc EQUAL 5 "a child that exits 5 propagates exactly 5 under an explicit deadline")
collect_leaks(_fail_leftover "${_fail_parent}")
check(NOT _fail_leftover "a failing child still removes its owned scratch")

reap("${_normal_pid}")
cleanup("${_normal_parent}" "${_fail_parent}")

# ===========================================================================
# Verdict
# ===========================================================================

if(_failures)
  list(LENGTH _failures _count)
  message(FATAL_ERROR
    "LauncherContracts: ${_count} contract check(s) failed:\n  ${_failures}")
endif()
cleanup("${_workspace}")
message(STATUS "LauncherContracts: the launcher process-lifetime contract holds")
