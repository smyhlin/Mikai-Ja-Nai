# AppImage packaging for an already configured Linux build.
#
# This is a local packaging path, not a distribution decision. The AppImage
# carries the deployed Qt runtime and the pinned private libmpv closure, so the
# host only has to provide glibc/the loader, graphics drivers, Vulkan/EGL and an
# audio subsystem. It is not a universal binary: a newer build host raises the
# glibc baseline, and redistribution still needs the third-party review in
# docs/redistribution.md.
#
# Usage (build first, then):
#   cmake -DBUILD_DIR=<build> -DOUTPUT_DIR=<fresh dir> \
#         [-DAPPIMAGE_TOOL=<appimagetool>] -P packaging/linux/AppImage.cmake

cmake_minimum_required(VERSION 3.25)

foreach(required BUILD_DIR OUTPUT_DIR)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required.")
  endif()
endforeach()
get_filename_component(BUILD_DIR "${BUILD_DIR}" ABSOLUTE)
get_filename_component(OUTPUT_DIR "${OUTPUT_DIR}" ABSOLUTE)

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "AppImage packaging requires a Linux host.")
endif()
if(NOT EXISTS "${BUILD_DIR}/mikai-linux-release.cmake")
  message(FATAL_ERROR "Not a configured Mikai Linux build: ${BUILD_DIR}")
endif()
include("${BUILD_DIR}/mikai-linux-release.cmake")

if(MIKAI_LINUX_WITH_NATIVE OR MIKAI_LINUX_DIST_NATIVE OR MIKAI_LINUX_WITH_MODEL_INSPECTION)
  message(FATAL_ERROR
    "This AppImage contains Original playback and Anime4K only. Optional native "
    "inference and model inspection are not established; disable those options "
    "before packaging.")
endif()
if(NOT MIKAI_LINUX_BUILD_TYPE STREQUAL "" AND NOT MIKAI_LINUX_BUILD_TYPE STREQUAL "Release")
  message(FATAL_ERROR "AppImage packaging requires -DCMAKE_BUILD_TYPE=Release.")
endif()

# A portable image is the whole point, so both halves have to be inside it. A
# host Qt or a host libmpv would make the result depend on the distribution's
# package versions, which is exactly what an AppImage is asked not to do.
if(NOT MIKAI_LINUX_DEPLOY_QT)
  message(FATAL_ERROR
    "Configure the build with -DMIKAI_LINUX_DEPLOY_QT=ON so the image carries Qt "
    "instead of requiring the host's Qt 6.12.")
endif()
if(NOT MIKAI_LINUX_MPV_BUNDLE)
  message(FATAL_ERROR
    "Configure the build with -DMIKAI_LINUX_BUNDLE_MPV=ON so the image carries the "
    "pinned libmpv closure instead of requiring the host's libmpv.")
endif()

if(EXISTS "${OUTPUT_DIR}" OR IS_SYMLINK "${OUTPUT_DIR}")
  message(FATAL_ERROR "Output already exists; choose a fresh directory: ${OUTPUT_DIR}")
endif()

find_program(MIKAI_PATCHELF_EXECUTABLE NAMES patchelf)
if(NOT MIKAI_PATCHELF_EXECUTABLE)
  message(FATAL_ERROR
    "patchelf is required to give the copied Qt plugins a RUNPATH; install it and "
    "retry.")
endif()
if(DEFINED APPIMAGE_TOOL AND NOT APPIMAGE_TOOL STREQUAL "")
  set(appimage_tool "${APPIMAGE_TOOL}")
else()
  find_program(appimage_tool NAMES appimagetool)
endif()
if(NOT appimage_tool OR NOT EXISTS "${appimage_tool}")
  message(FATAL_ERROR
    "appimagetool was not found. Pass -DAPPIMAGE_TOOL=<path> or put it on PATH "
    "(https://github.com/AppImage/appimagetool/releases).")
endif()

set(appdir "${OUTPUT_DIR}/Mikai.AppDir")
set(appimage_name "mikai-${MIKAI_LINUX_VERSION}-x86_64.AppImage")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

# The install tree stays a normal GNUInstallDirs tree under usr/, which is what
# an AppImage expects; nothing is flattened or rewritten by hand.
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix /usr
  RESULT_VARIABLE install_result OUTPUT_VARIABLE install_output ERROR_VARIABLE install_error
  ENVIRONMENT "DESTDIR=${appdir}")
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "Install into ${appdir} failed (${install_result}):\n${install_error}")
endif()

foreach(required_path
    "${appdir}/usr/bin/mikai"
    "${appdir}/usr/bin/mpv/libmpv.so.2"
    "${appdir}/usr/share/applications/mikai.desktop"
    "${appdir}/usr/share/icons/hicolor/512x512/apps/mikai.png"
    "${appdir}/usr/share/metainfo/mikai.metainfo.xml")
  if(NOT EXISTS "${required_path}")
    message(FATAL_ERROR "The install tree is incomplete, missing ${required_path}")
  endif()
endforeach()

# Qt's deployment gathers plugins by module, but a distribution Qt without a
# qpa description makes it skip the platform plugins, which leaves the image
# unable to start. Copy the plugin tree the build links against and give every
# copied plugin a RUNPATH back to usr/lib.
set(_qt_plugin_source "${MIKAI_LINUX_QT_PLUGIN_DIR}")
if(_qt_plugin_source STREQUAL "" OR NOT IS_DIRECTORY "${_qt_plugin_source}")
  message(FATAL_ERROR
    "This build published no Qt plugin directory (${_qt_plugin_source}); reconfigure "
    "with -DMIKAI_LINUX_DEPLOY_QT=ON against the Qt that will be bundled.")
endif()

set(_qt_plugin_destination "${appdir}/usr/lib/qt6/plugins")
# Shared objects only: a plugin directory also carries QML and metadata files,
# and CMAKE_SHARED_LIBRARY_SUFFIX is not defined in script mode.
file(GLOB_RECURSE _qt_plugins RELATIVE "${_qt_plugin_source}"
  "${_qt_plugin_source}/*.so" "${_qt_plugin_source}/*.so.*")
set(_copied 0)
foreach(_relative IN LISTS _qt_plugins)
  set(_source "${_qt_plugin_source}/${_relative}")
  set(_target "${_qt_plugin_destination}/${_relative}")
  if(NOT EXISTS "${_target}")
    get_filename_component(_target_dir "${_target}" DIRECTORY)
    file(MAKE_DIRECTORY "${_target_dir}")
    file(COPY_FILE "${_source}" "${_target}")
  endif()
  file(READ "${_target}" _elf_magic LIMIT 4 HEX)
  string(TOLOWER "${_elf_magic}" _elf_magic)
  if(NOT _elf_magic STREQUAL "7f454c46")
    message(FATAL_ERROR "Refusing to patch a non-ELF plugin file: ${_target}")
  endif()
  # Depth-relative, so nested plugin types resolve the same usr/lib.
  get_filename_component(_relative_dir "${_relative}" DIRECTORY)
  set(_rpath "$ORIGIN")
  set(_steps 2)
  if(NOT _relative_dir STREQUAL "")
    string(REPLACE "/" ";" _parts "${_relative_dir}")
    list(LENGTH _parts _depth)
    math(EXPR _steps "${_depth} + 2")
  endif()
  foreach(_ignored RANGE 1 ${_steps})
    string(APPEND _rpath "/..")
  endforeach()
  execute_process(COMMAND "${MIKAI_PATCHELF_EXECUTABLE}" --set-rpath "${_rpath}" "${_target}"
    RESULT_VARIABLE patch_result ERROR_VARIABLE patch_error OUTPUT_QUIET)
  if(NOT patch_result EQUAL 0)
    message(FATAL_ERROR "Could not set the RUNPATH of ${_target}: ${patch_error}")
  endif()
  math(EXPR _copied "${_copied} + 1")
endforeach()
message(STATUS "Qt plugins prepared: ${_copied} under ${_qt_plugin_destination}")

# AppImage's own layout: AppRun, the desktop entry, the icon and the AppStream
# file at the AppDir root.
file(COPY_FILE "${CMAKE_CURRENT_LIST_DIR}/AppRun" "${appdir}/AppRun")
execute_process(COMMAND chmod 0755 "${appdir}/AppRun" RESULT_VARIABLE chmod_result)
if(NOT chmod_result EQUAL 0)
  message(FATAL_ERROR "Could not make AppRun executable (${chmod_result})")
endif()
file(COPY_FILE "${appdir}/usr/share/applications/mikai.desktop" "${appdir}/mikai.desktop")
file(COPY_FILE "${appdir}/usr/share/icons/hicolor/512x512/apps/mikai.png"
  "${appdir}/mikai.png")
file(CREATE_LINK "mikai.png" "${appdir}/.DirIcon" SYMBOLIC RESULT diricon_result)
if(NOT diricon_result STREQUAL "0")
  message(FATAL_ERROR "Could not create .DirIcon (${diricon_result})")
endif()
file(COPY_FILE "${appdir}/usr/share/metainfo/mikai.metainfo.xml"
  "${appdir}/mikai.metainfo.xml")
file(COPY_FILE "${CMAKE_CURRENT_LIST_DIR}/../../LICENSE" "${appdir}/LICENSE")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "ARCH=x86_64" "VERSION=${MIKAI_LINUX_VERSION}"
    "${appimage_tool}" "${appdir}" "${OUTPUT_DIR}/${appimage_name}"
  RESULT_VARIABLE image_result OUTPUT_VARIABLE image_output ERROR_VARIABLE image_error)
if(NOT image_result EQUAL 0)
  message(FATAL_ERROR "appimagetool failed (${image_result}):\n${image_error}")
endif()

file(SHA256 "${OUTPUT_DIR}/${appimage_name}" image_digest)
file(WRITE "${OUTPUT_DIR}/SHA256SUMS" "${image_digest}  ${appimage_name}\n")
message(STATUS "Created ${OUTPUT_DIR}/${appimage_name}")
message(STATUS
  "Local artifact only: host glibc/loader, graphics drivers and audio remain "
  "requirements, and third-party redistribution review is still open.")
