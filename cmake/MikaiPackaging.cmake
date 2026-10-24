# Private `dist` remains compatible with DistContracts; public releases use the
# generated, configuration-bound entry point and never copy a deployed tree.
option(MIKAI_DIST_NATIVE "Copy the installed Native bundle into the private dist folder" OFF)
set(MIKAI_DIST_DIR "${CMAKE_SOURCE_DIR}/dist-mikai" CACHE PATH
  "Fresh staging directory; existing populated directories are never overwritten")
set(MIKAI_DIST_NATIVE_ROOT "" CACHE PATH
  "Private dist native store override (empty: <EXE dir>/native)")

if(NOT WIN32)
  return()
endif()

get_target_property(MIKAI_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
get_filename_component(MIKAI_QT_BIN_DIR "${MIKAI_QMAKE_EXECUTABLE}" DIRECTORY)
find_program(MIKAI_WINDEPLOYQT NAMES windeployqt.exe
  HINTS "${MIKAI_QT_BIN_DIR}" NO_DEFAULT_PATH REQUIRED)

set(_mikai_dist_arguments
  "-DWINDEPLOYQT=${MIKAI_WINDEPLOYQT}"
  "-DSHADERS=${CMAKE_CURRENT_SOURCE_DIR}/shaders"
  "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
  "-DMIKAI_DIST_MPV_ARCHIVE=${MIKAI_PINNED_MPV_ARCHIVE}"
  "-DMIKAI_DIST_CACHE_DIR=${MIKAI_DIST_MPV_CACHE_DIR}"
  "-DMIKAI_MPV_ARCHIVE_NAME=${MIKAI_MPV_ARCHIVE_NAME}"
  "-DMIKAI_MPV_ARCHIVE_URL=${MIKAI_MPV_ARCHIVE_URL}"
  "-DMIKAI_MPV_ARCHIVE_SHA256=${MIKAI_MPV_ARCHIVE_SHA256}")
add_custom_target(dist
  COMMAND "${CMAKE_COMMAND}"
    "-DDIST_DIR=${MIKAI_DIST_DIR}"
    "-DEXE=$<TARGET_FILE:mikai>"
    "-DMIKAI_DIST_NATIVE=${MIKAI_DIST_NATIVE}"
    "-DMIKAI_DIST_NATIVE_ROOT=${MIKAI_DIST_NATIVE_ROOT}"
    ${_mikai_dist_arguments}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/dist.cmake"
  DEPENDS mikai
  COMMENT "Assembling the fresh self-contained private dist folder"
  VERBATIM)

# No CMakeCache parsing or output-directory guessing in package.ps1. This file
# is generated for the actual target/configuration and consumed read-only.
set(_mikai_package_script "")
foreach(_pair IN ITEMS
    "EXE|$<TARGET_FILE:mikai>"
    "WINDEPLOYQT|${MIKAI_WINDEPLOYQT}"
    "SHADERS|${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    "SOURCE_DIR|${CMAKE_CURRENT_SOURCE_DIR}"
    "MIKAI_DIST_MPV_ARCHIVE|${MIKAI_PINNED_MPV_ARCHIVE}"
    "MIKAI_DIST_CACHE_DIR|${MIKAI_DIST_MPV_CACHE_DIR}"
    "MIKAI_MPV_ARCHIVE_NAME|${MIKAI_MPV_ARCHIVE_NAME}"
    "MIKAI_MPV_ARCHIVE_URL|${MIKAI_MPV_ARCHIVE_URL}"
    "MIKAI_MPV_ARCHIVE_SHA256|${MIKAI_MPV_ARCHIVE_SHA256}"
    "MIKAI_RELEASE_VERSION|${PROJECT_VERSION}"
    "MIKAI_RELEASE_QT_VERSION|${Qt6_VERSION}"
    "MIKAI_RELEASE_COMPILER|${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    "MIKAI_RELEASE_COMPILER_BIN|${CMAKE_CXX_COMPILER}"
    "MIKAI_RELEASE_OBJDUMP|${CMAKE_OBJDUMP}")
  string(REPLACE "|" ";" _parts "${_pair}")
  list(GET _parts 0 _name)
  list(GET _parts 1 _value)
  string(APPEND _mikai_package_script "set(${_name} [==[${_value}]==])\n")
endforeach()
string(APPEND _mikai_package_script "
if(NOT [==[$<CONFIG>]==] STREQUAL [==[Release]==])
  message(FATAL_ERROR \"Public packages require the Release configuration\")
endif()
if(NOT [==[${CMAKE_SIZEOF_VOID_P}]==] STREQUAL [==[8]==] OR NOT [==[${MINGW}]==])
  message(FATAL_ERROR \"Public Windows packages require the supported x64 MinGW toolchain\")
endif()
if([==[${MIKAI_WITH_NATIVE}]==] OR [==[${MIKAI_WITH_MODEL_INSPECTION}]==])
  message(FATAL_ERROR \"Public packages must be configured with optional native/model inspection OFF\")
endif()
set(MIKAI_PUBLIC_RELEASE ON)
set(MIKAI_DIST_NATIVE OFF)
include([==[${CMAKE_CURRENT_SOURCE_DIR}/cmake/dist.cmake]==])
")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/mikai-package-$<CONFIG>.cmake"
  CONTENT "${_mikai_package_script}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/mikai-package-$<CONFIG>.json" CONTENT
  "{\n  \"version\": \"${PROJECT_VERSION}\",\n  \"configuration\": \"$<CONFIG>\",\n  \"executable\": \"$<TARGET_FILE:mikai>\",\n  \"sourceDir\": \"${CMAKE_CURRENT_SOURCE_DIR}\"\n}\n")
