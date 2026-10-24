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
# The child fixture (`mikai_launcher_child`) writes each process's PID and exact
# OS start token. Windows checks the process creation time; Linux checks /proc's
# start-time ticks and executable. Tree state is read from the OS rather than
# inferred, so a direct-child-only kill fails here.

if(NOT DEFINED LAUNCHER_EXE OR NOT EXISTS "${LAUNCHER_EXE}")
  message(FATAL_ERROR "LauncherContracts: LAUNCHER_EXE is not set or missing: '${LAUNCHER_EXE}'")
endif()
if(NOT DEFINED FIXTURE_EXE OR NOT EXISTS "${FIXTURE_EXE}")
  message(FATAL_ERROR "LauncherContracts: FIXTURE_EXE is not set or missing: '${FIXTURE_EXE}'")
endif()
if(NOT DEFINED SHADERS_DIR OR NOT IS_DIRECTORY "${SHADERS_DIR}")
  message(FATAL_ERROR "LauncherContracts: SHADERS_DIR is not set or missing: '${SHADERS_DIR}'")
endif()
if(NOT DEFINED MPV_MODULE OR NOT EXISTS "${MPV_MODULE}")
  message(FATAL_ERROR "LauncherContracts: MPV_MODULE is not set or missing: '${MPV_MODULE}'")
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
  # Braces are literal CMake arguments, not grouping delimiters.
  set(_args
    "${LAUNCHER_EXE}"
    "${FIXTURE_EXE}"
    "${SHADERS_DIR}"
    "${MPV_MODULE}"
    "${QT_BIN_DIR}"
    "${QT_PLUGIN_DIR}"
    "${scratch_parent}")
  if(DEFINED MPV_RUNTIME_DIR AND NOT MPV_RUNTIME_DIR STREQUAL "")
    list(APPEND _args --mpv-runtime-dir "${MPV_RUNTIME_DIR}")
  endif()
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
  # The fixture writes `pid=<n>` and `creation=<OS process-start token>`. A
  # missing or malformed file is reported by the caller, never silently treated
  # as a process that exists.
  set(${pid_var} "" PARENT_SCOPE)
  set(${creation_var} "" PARENT_SCOPE)
  if(NOT EXISTS "${path}")
    return()
  endif()
  file(READ "${path}" _text)
  string(REGEX MATCH "pid=([0-9]+)" _pid_match "${_text}")
  set(_pid "${CMAKE_MATCH_1}")
  string(REGEX MATCH "creation=([0-9]+)" _creation_match "${_text}")
  set(_creation "${CMAKE_MATCH_1}")
  if(NOT _pid_match OR NOT _creation_match)
    return()
  endif()
  set(${pid_var} "${_pid}" PARENT_SCOPE)
  set(${creation_var} "${_creation}" PARENT_SCOPE)
endfunction()

function(process_identity_or_empty result_var pid creation_token expected_name)
  # Resolve the PID to this fixture's executable and its OS process-start token.
  # The token is exact on both supported systems, so PID reuse cannot stand in
  # for the process that wrote the identity file.
  if(NOT pid MATCHES "^[0-9]+$" OR pid EQUAL 0 OR
     NOT creation_token MATCHES "^[0-9]+$")
    set(${result_var} "" PARENT_SCOPE)
    return()
  endif()
  set(_identity "")
  if(WIN32)
    set(_ps
      "foreach ($p in (Get-Process)) { "
      "if ($p.Id -eq ${pid} -and $p.SafeHandle -ne $null -and $p.Name -eq '${expected_name}') { "
      "$stamp = [int64]([DateTimeOffset]$p.StartTime.ToUniversalTime()).ToUnixTimeMilliseconds(); "
      "Write-Output \"$($p.Id)|$stamp\"; break } }")
    execute_process(
      COMMAND powershell -NoProfile -NonInteractive -Command "${_ps}"
      OUTPUT_VARIABLE _out ERROR_QUIET RESULT_VARIABLE _rc)
    string(STRIP "${_out}" _out)
    string(REPLACE "|" ";" _parts "${_out}")
    list(LENGTH _parts _part_count)
    if(_part_count EQUAL 2)
      list(GET _parts 0 _actual_pid)
      list(GET _parts 1 _actual_start)
      if(_actual_pid STREQUAL "${pid}" AND _actual_start STREQUAL "${creation_token}")
        set(_identity "alive")
      endif()
    endif()
  elseif(EXISTS "/proc/${pid}/exe" AND EXISTS "/proc/${pid}/stat")
    file(REAL_PATH "/proc/${pid}/exe" _image)
    get_filename_component(_image_name "${_image}" NAME)
    if(_image_name STREQUAL "${expected_name}")
      execute_process(
        COMMAND cat "/proc/${pid}/stat"
        OUTPUT_VARIABLE _stat ERROR_QUIET RESULT_VARIABLE _stat_result)
      if("${_stat_result}" STREQUAL "0")
        string(FIND "${_stat}" ")" _command_end REVERSE)
        if(NOT _command_end EQUAL -1)
          math(EXPR _fields_start "${_command_end} + 1")
          string(SUBSTRING "${_stat}" ${_fields_start} -1 _stat_fields)
          string(REGEX REPLACE "^ +" "" _stat_fields "${_stat_fields}")
          string(REGEX REPLACE " +" ";" _stat_fields "${_stat_fields}")
          list(LENGTH _stat_fields _field_count)
          if(_field_count GREATER 19)
            list(GET _stat_fields 19 _actual_start)
            if(_actual_start STREQUAL "${creation_token}")
              set(_identity "alive")
            endif()
          endif()
        endif()
      endif()
    endif()
  endif()
  set(${result_var} "${_identity}" PARENT_SCOPE)
endfunction()

function(process_alive_matching result_var pid creation_token expected_name)
  # Match PID, image name and the exact OS process-start token on both systems.
  set(${result_var} FALSE PARENT_SCOPE)
  process_identity_or_empty(_identity "${pid}" "${creation_token}" "${expected_name}")
  if(NOT _identity STREQUAL "")
    set(${result_var} TRUE PARENT_SCOPE)
  endif()
endfunction()

function(wait_for_processes_gone result_var pids creation_tokens expected_name deadline_ms)
  # A tree kill may precede removal of the final process image. Bounded so the
  # contract stays fast; the fixture's descendant self-exits only after ten
  # minutes, and each observation is tied to its recorded process-start token.
  list(LENGTH pids _process_count)
  set(_last_index -1)
  if(_process_count GREATER 0)
    math(EXPR _last_index "${_process_count} - 1")
  endif()
  set(_remaining "${deadline_ms}")
  set(_all_gone FALSE)
  while(_remaining GREATER 0)
    set(_all_gone TRUE)
    if(_process_count GREATER 0)
      foreach(_index RANGE 0 ${_last_index})
        list(GET pids ${_index} _pid)
        list(GET creation_tokens ${_index} _creation_token)
        process_identity_or_empty(_identity "${_pid}" "${_creation_token}"
                                  "${expected_name}")
        if(NOT _identity STREQUAL "")
          set(_all_gone FALSE)
        endif()
      endforeach()
    endif()
    if(_all_gone)
      break()
    endif()
    if(WIN32)
      execute_process(COMMAND powershell -NoProfile -NonInteractive -Command "Start-Sleep -Milliseconds 100")
    else()
      execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
    endif()
    math(EXPR _remaining "${_remaining} - 100")
  endwhile()
  set(${result_var} "${_all_gone}" PARENT_SCOPE)
endfunction()

function(reap expected_name)
  # Best-effort cleanup only for the exact fixture identities recorded by this
  # contract; a reused PID or same-named unrelated process is never targeted.
  foreach(_spec IN LISTS ARGN)
    string(REPLACE "|" ";" _parts "${_spec}")
    list(LENGTH _parts _part_count)
    if(NOT _part_count EQUAL 2)
      continue()
    endif()
    list(GET _parts 0 _pid)
    list(GET _parts 1 _creation_token)
    process_identity_or_empty(_identity "${_pid}" "${_creation_token}"
                              "${expected_name}")
    if(_identity STREQUAL "")
      continue()
    endif()
    if(WIN32)
      execute_process(COMMAND taskkill /PID ${_pid} /T /F OUTPUT_QUIET ERROR_QUIET)
    else()
      execute_process(COMMAND kill -KILL ${_pid} OUTPUT_QUIET ERROR_QUIET)
    endif()
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
check(_child_creation MATCHES "^[0-9]+$" "the child's process-start token is well formed")
check(_descendant_pid MATCHES "^[0-9]+$" "the descendant's identity is well formed")
check(_descendant_creation MATCHES "^[0-9]+$"
       "the descendant's process-start token is well formed")

# The tree kill is what is being measured, so the processes are given a bounded
# moment to disappear and then checked by identity, not by PID number.
set(_hung_pids "")
set(_hung_creation_tokens "")
if(_child_pid MATCHES "^[0-9]+$" AND _child_creation MATCHES "^[0-9]+$")
  list(APPEND _hung_pids "${_child_pid}")
  list(APPEND _hung_creation_tokens "${_child_creation}")
endif()
if(_descendant_pid MATCHES "^[0-9]+$" AND _descendant_creation MATCHES "^[0-9]+$")
  list(APPEND _hung_pids "${_descendant_pid}")
  list(APPEND _hung_creation_tokens "${_descendant_creation}")
endif()
wait_for_processes_gone(_tree_settled "${_hung_pids}" "${_hung_creation_tokens}"
                        "${_fixture_name}" 5000)
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
reap("${_fixture_name}" "${_child_pid}|${_child_creation}"
     "${_descendant_pid}|${_descendant_creation}")
cleanup("${_hang_parent}")

# ===========================================================================
# Scenario 3: a normal child, with and without an explicit deadline
# ===========================================================================

set(_normal_parent "${_workspace}/normal-parent")
file(MAKE_DIRECTORY "${_normal_parent}")
set(_normal_id "${_workspace}/normal-child.id")

run_launcher(_rc _out _err "${_normal_parent}"
  --exit-code 0 --print-id "${_normal_id}")
string(REPLACE "\n" " | " _normal_err_diag "${_err}")
string(REPLACE ";" "," _normal_err_diag "${_normal_err_diag}")
string(REPLACE "\\" "/" _normal_err_diag "${_normal_err_diag}")
check(_rc EQUAL 0
      "a child that exits 0 propagates launcher exit 0 (actual rc='${_rc}', stderr='${_normal_err_diag}')")
read_identity_file(_normal_pid _normal_creation "${_normal_id}")
check(_normal_pid MATCHES "^[0-9]+$" "the normal child's identity is well formed")
check(_normal_creation MATCHES "^[0-9]+$"
       "the normal child's process-start token is well formed")
process_alive_matching(_normal_alive "${_normal_pid}" "${_normal_creation}" "${_fixture_name}")
check(NOT _normal_alive "the successful child is gone after it returned")
collect_leaks(_normal_leftover "${_normal_parent}")
check(NOT _normal_leftover "a successful child still removes its owned scratch")

# A direct child can return while its genuinely detached descendant is still
# running. Linux must identify, kill and reap that adopted child; Windows must
# preserve the exit verdict while the kill-on-close job drains the same tree.
set(_exit_descendant_parent "${_workspace}/exit-descendant-parent")
file(MAKE_DIRECTORY "${_exit_descendant_parent}")
set(_exit_child_id "${_workspace}/exit-child.id")
set(_exit_descendant_id "${_workspace}/exit-descendant.id")
run_launcher(_rc _out _err "${_exit_descendant_parent}"
  --exit-code 0
  --print-id "${_exit_child_id}"
  --descendant-id "${_exit_descendant_id}"
  --descendant-ms 600000)
check(_rc EQUAL 2 "the launcher reports a detached descendant after normal child exit")
check(_err MATCHES "cleaned" "the launcher reports normal-exit descendant cleanup")
check(EXISTS "${_exit_child_id}" "the normally exiting child recorded its identity")
read_identity_file(_exit_child_pid _exit_child_creation "${_exit_child_id}")
check(_exit_child_pid MATCHES "^[0-9]+$" "the normal-exit child's identity is well formed")
check(_exit_child_creation MATCHES "^[0-9]+$"
       "the normal-exit child's process-start token is well formed")
check(EXISTS "${_exit_descendant_id}" "the detached normal-exit descendant recorded its identity")
read_identity_file(_exit_descendant_pid _exit_descendant_creation "${_exit_descendant_id}")
check(_exit_descendant_pid MATCHES "^[0-9]+$"
       "the normal-exit descendant's identity is well formed")
check(_exit_descendant_creation MATCHES "^[0-9]+$"
       "the normal-exit descendant's process-start token is well formed")
process_alive_matching(_exit_descendant_alive "${_exit_descendant_pid}"
                       "${_exit_descendant_creation}" "${_fixture_name}")
check(NOT _exit_descendant_alive
       "a detached descendant cannot survive the direct child's normal exit")
collect_leaks(_exit_descendant_leftover "${_exit_descendant_parent}")
check(NOT _exit_descendant_leftover
       "normal-exit descendant cleanup removes only the launcher's owned scratch")

# Nonzero direct-child exits must take the same ownership-safe path before
# propagating/returning a failure, rather than leaving a detached process behind.
set(_abnormal_descendant_parent "${_workspace}/abnormal-descendant-parent")
file(MAKE_DIRECTORY "${_abnormal_descendant_parent}")
set(_abnormal_child_id "${_workspace}/abnormal-child.id")
set(_abnormal_descendant_id "${_workspace}/abnormal-descendant.id")
run_launcher(_rc _out _err "${_abnormal_descendant_parent}"
  --exit-code 5
  --print-id "${_abnormal_child_id}"
  --descendant-id "${_abnormal_descendant_id}"
  --descendant-ms 600000)
check(EXISTS "${_abnormal_child_id}"
       "the nonzero direct child recorded its identity before exiting")
read_identity_file(_abnormal_child_pid _abnormal_child_creation "${_abnormal_child_id}")
check(_abnormal_child_pid MATCHES "^[0-9]+$"
       "the nonzero direct child's identity is well formed")
check(_abnormal_child_creation MATCHES "^[0-9]+$"
       "the nonzero direct child's process-start token is well formed")
check(_rc EQUAL 2 "the launcher reports and cleans descendants after a nonzero child exit")
check(_err MATCHES "cleaned" "the launcher reports nonzero-exit descendant cleanup")
check(EXISTS "${_abnormal_descendant_id}"
       "the detached nonzero-exit descendant recorded its identity")
read_identity_file(_abnormal_descendant_pid _abnormal_descendant_creation
                   "${_abnormal_descendant_id}")
check(_abnormal_descendant_pid MATCHES "^[0-9]+$"
       "the nonzero-exit descendant's identity is well formed")
check(_abnormal_descendant_creation MATCHES "^[0-9]+$"
       "the nonzero-exit descendant's process-start token is well formed")
process_alive_matching(_abnormal_descendant_alive "${_abnormal_descendant_pid}"
                       "${_abnormal_descendant_creation}" "${_fixture_name}")
check(NOT _abnormal_descendant_alive
       "a detached descendant cannot survive a nonzero direct-child exit")
collect_leaks(_abnormal_descendant_leftover "${_abnormal_descendant_parent}")
check(NOT _abnormal_descendant_leftover
       "nonzero-exit descendant cleanup removes the launcher's owned scratch")

set(_fail_parent "${_workspace}/fail-parent")
file(MAKE_DIRECTORY "${_fail_parent}")
run_launcher(_rc _out _err "${_fail_parent}"
  --exit-code 5
  --launcher-timeout-ms 60000)
string(REPLACE "\n" " | " _fail_err_diag "${_err}")
string(REPLACE ";" "," _fail_err_diag "${_fail_err_diag}")
string(REPLACE "\\" "/" _fail_err_diag "${_fail_err_diag}")
check(_rc EQUAL 5
      "a child that exits 5 propagates exactly 5 under an explicit deadline (actual rc='${_rc}', stderr='${_fail_err_diag}')")
collect_leaks(_fail_leftover "${_fail_parent}")
check(NOT _fail_leftover "a failing child still removes its owned scratch")

reap("${_fixture_name}" "${_normal_pid}|${_normal_creation}"
     "${_exit_child_pid}|${_exit_child_creation}"
     "${_exit_descendant_pid}|${_exit_descendant_creation}"
     "${_abnormal_child_pid}|${_abnormal_child_creation}"
     "${_abnormal_descendant_pid}|${_abnormal_descendant_creation}")
cleanup("${_normal_parent}" "${_exit_descendant_parent}"
        "${_abnormal_descendant_parent}" "${_fail_parent}")

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
