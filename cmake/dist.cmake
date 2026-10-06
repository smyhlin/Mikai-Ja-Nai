# Assembles the self-contained dist folder. Run through the `dist` target:
#   cmake --build <build-dir> --target dist
# Required inputs: DIST_DIR, EXE, WINDEPLOYQT, SHADERS, SOURCE_DIR.
# Optional: MIKAI_DIST_NATIVE=ON copies the installed native runtime pointer and
# every bundle it names; MIKAI_DIST_MPV_RUNTIME names an mpv tree to copy.

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

set(mpv_runtime "${MIKAI_DIST_MPV_RUNTIME}")
if(mpv_runtime STREQUAL "")
  set(mpv_runtime "$ENV{LOCALAPPDATA}/MPVBackend/runtime")
endif()
set(mpv_library "")
foreach(candidate mpv-2.dll libmpv-2.dll)
  if(EXISTS "${mpv_runtime}/${candidate}")
    set(mpv_library "${candidate}")
    break()
  endif()
endforeach()
if(NOT mpv_library STREQUAL "")
  file(COPY "${mpv_runtime}" DESTINATION "${DIST_DIR}")
  get_filename_component(mpv_dir_name "${mpv_runtime}" NAME)
  if(NOT mpv_dir_name STREQUAL "mpv")
    file(RENAME "${DIST_DIR}/${mpv_dir_name}" "${DIST_DIR}/mpv")
  endif()
  message(STATUS "dist: copied the mpv runtime from ${mpv_runtime}")
else()
  message(STATUS "dist: no mpv runtime at ${mpv_runtime}; the first playback will download one")
endif()

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
