# Builds and validates a real Linux release archive from the configured build.
# Package.cmake includes the Linux-owned artifact contract after producing the
# install tree/archive; this wrapper gives each CTest run a fresh, private output
# path and removes only that path afterwards.

if(NOT DEFINED BUILD_DIR OR NOT IS_DIRECTORY "${BUILD_DIR}")
  message(FATAL_ERROR "LinuxPackageContracts: BUILD_DIR is missing: '${BUILD_DIR}'")
endif()
if(NOT DEFINED PACKAGE_SCRIPT OR NOT EXISTS "${PACKAGE_SCRIPT}")
  message(FATAL_ERROR "LinuxPackageContracts: PACKAGE_SCRIPT is missing: '${PACKAGE_SCRIPT}'")
endif()

set(_output "")
foreach(_attempt RANGE 1 8)
  string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef _suffix)
  set(_candidate "${CMAKE_CURRENT_BINARY_DIR}/LinuxPackageContracts-${_suffix}")
  if(NOT EXISTS "${_candidate}" AND NOT IS_SYMLINK "${_candidate}")
    set(_output "${_candidate}")
    break()
  endif()
endforeach()
if(_output STREQUAL "")
  message(FATAL_ERROR "LinuxPackageContracts: cannot allocate a fresh private output path")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${BUILD_DIR}" "-DOUTPUT_DIR=${_output}"
          "-DCONFIG=Release" -P "${PACKAGE_SCRIPT}"
  RESULT_VARIABLE _result
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr
  TIMEOUT 300
)
if(NOT _result EQUAL 0)
  file(REMOVE_RECURSE "${_output}")
  message(FATAL_ERROR
    "LinuxPackageContracts: package command failed (${_result}).\n${_stdout}\n${_stderr}")
endif()
file(REMOVE_RECURSE "${_output}")
message(STATUS "LinuxPackageContracts: the real install/archive contract passed")
