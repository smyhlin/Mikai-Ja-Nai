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
# libmpv may come from the host instead. The pinned Linux archive is built
# without an FFmpeg TLS backend, so a bundled image cannot open any https
# stream - which is every provider stream - and carrying it would ship a player
# that fails on real catalogue playback. A host libmpv is then a dependency of
# the image rather than a bundled copy, and that is stated instead of hidden.
if(MIKAI_LINUX_MPV_BUNDLE)
  if(DEFINED MIKAI_LINUX_MPV_TLS AND NOT MIKAI_LINUX_MPV_TLS)
    message(FATAL_ERROR
      "The pinned Linux libmpv has no FFmpeg TLS backend, so the image could not "
      "play https streams. Rebuild without -DMIKAI_LINUX_BUNDLE_MPV=ON to use the "
      "host's libmpv.so.2, or use a runtime archive that was built with TLS.")
  endif()
else()
  message(WARNING
    "This image carries Qt but not libmpv: it requires libmpv.so.2 (client API 2, "
    "built with a TLS backend) on the host. That dependency is what makes https "
    "playback possible at all.")
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

set(required_paths
    "${appdir}/usr/bin/mikai"
    "${appdir}/usr/share/applications/mikai.desktop"
    "${appdir}/usr/share/icons/hicolor/512x512/apps/mikai.png"
    "${appdir}/usr/share/metainfo/mikai.metainfo.xml")
# The image runs from usr/bin inside the AppDir, so the benchmark catalogue needs
# the clips under usr/share/mikai/benchmarks in the same image.
include("${MIKAI_LINUX_SOURCE_DIR}/cmake/MikaiBenchmarks.cmake")
mikai_benchmark_clips("${MIKAI_LINUX_SOURCE_DIR}/${MIKAI_BENCHMARK_SOURCE_SUBDIR}"
  appimage_benchmark_clips)
foreach(clip IN LISTS appimage_benchmark_clips)
  get_filename_component(clip_name "${clip}" NAME)
  list(APPEND required_paths
    "${appdir}/usr/${MIKAI_LINUX_DATADIR}/mikai/benchmarks/${clip_name}")
endforeach()
if(MIKAI_LINUX_MPV_BUNDLE)
  list(APPEND required_paths "${appdir}/usr/bin/mpv/libmpv.so.2")
endif()
foreach(required_path IN LISTS required_paths)
  if(NOT EXISTS "${required_path}")
    message(FATAL_ERROR "The install tree is incomplete, missing ${required_path}")
  endif()
endforeach()

# Qt's deployment gathers plugins by module, but a distribution Qt without a
# qpa description makes it skip the platform plugins, which leaves the image
# unable to start. cmake/MikaiLinux.cmake completes the tree during install with
# packaging/linux/CompleteQtPlugins.cmake; this only proves it happened.
set(_qt_platform_plugins "${appdir}/usr/lib/qt6/plugins/platforms")
if(NOT IS_DIRECTORY "${_qt_platform_plugins}")
  message(FATAL_ERROR
    "The install tree carries no Qt platform plugins (${_qt_platform_plugins}); the "
    "image could not start on any display.")
endif()
file(GLOB _qt_platform_files "${_qt_platform_plugins}/*.so")
if(NOT _qt_platform_files)
  message(FATAL_ERROR "No Qt platform plugin was installed under ${_qt_platform_plugins}")
endif()
message(STATUS "Qt platform plugins present: ${_qt_platform_files}")

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
