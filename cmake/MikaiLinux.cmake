# Linux dependency-based install tree and optional Qt deployment.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()
include(GNUInstallDirs)
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR "Linux release packaging currently targets x86_64 only.")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MikaiLinuxMpv.cmake")
target_compile_definitions(mikai_native_support PRIVATE
  MIKAI_LINUX_BUNDLE_MPV=$<BOOL:${MIKAI_LINUX_MPV_BUNDLE}>)
if(MIKAI_LINUX_MPV_BUNDLE)
  # Runtime lookup is executable-relative in both the build and install trees.
  target_compile_definitions(mikai_native_support PRIVATE
    MIKAI_LINUX_MPV_MODULE_RELATIVE="${mikai_linux_mpv_from_bin}/libmpv.so.2")
  file(GLOB MIKAI_LINUX_MPV_BUILD_LIBRARIES LIST_DIRECTORIES FALSE
    "${MIKAI_LINUX_MPV_BUNDLE_ROOT}/*.so*")
  set(_mikai_linux_mpv_build_copy_commands
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "$<TARGET_FILE_DIR:mikai>/${mikai_linux_mpv_from_bin}")
  foreach(_library IN LISTS MIKAI_LINUX_MPV_BUILD_LIBRARIES)
    get_filename_component(_library_name "${_library}" NAME)
    list(APPEND _mikai_linux_mpv_build_copy_commands
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_library}" "$<TARGET_FILE_DIR:mikai>/${mikai_linux_mpv_from_bin}/${_library_name}")
  endforeach()
  add_custom_command(TARGET mikai POST_BUILD
    ${_mikai_linux_mpv_build_copy_commands}
    COMMENT "Deploying pinned Linux libmpv and private dependencies"
    VERBATIM)
else()
  target_compile_definitions(mikai_native_support PRIVATE
    MIKAI_LINUX_SYSTEM_MPV_MODULE="${mpv_system_real_library}")
endif()
set(MIKAI_LINUX_MPV_MODE "${MIKAI_LINUX_MPV_BUNDLE_MODE}")
# Host libmpv header/pkg-config evidence stays independent of bundled runtime identity.
if(NOT DEFINED MIKAI_SYSTEM_MPV_PC_VERSION)
  set(MIKAI_SYSTEM_MPV_PC_VERSION "")
endif()
set(MIKAI_LINUX_MPV_HEADER_PKGCONFIG "${MIKAI_SYSTEM_MPV_PC_VERSION}")
set(MIKAI_LINUX_MPV_ARCHIVE_SHA256 "${MIKAI_LINUX_MPV_BUNDLE_ARCHIVE_SHA256}")
if(MIKAI_LINUX_MPV_BUNDLE)
  set(MIKAI_LINUX_MPV_MODULE_RELATIVE "${MIKAI_LINUX_MPV_INSTALL_SUBDIR}/libmpv.so.2")
else()
  set(MIKAI_LINUX_MPV_MODULE_RELATIVE "not-bundled")
endif()

option(MIKAI_LINUX_DEPLOY_QT
  "Deploy Qt libraries/plugins into the Linux install tree (requires separate redistribution review)" OFF)
# Qt deployment follows GNUInstallDirs. Keep all locations prefix-relative so
# cmake --install --prefix and archive extraction remain relocatable.
foreach(dir BINDIR LIBDIR DATADIR)
  if(IS_ABSOLUTE "${CMAKE_INSTALL_${dir}}")
    message(FATAL_ERROR "Linux packaging requires relative CMAKE_INSTALL_${dir}.")
  endif()
endforeach()
file(RELATIVE_PATH mikai_linux_lib_from_bin
  "/${CMAKE_INSTALL_BINDIR}" "/${CMAKE_INSTALL_LIBDIR}")
set_target_properties(mikai PROPERTIES
  INSTALL_RPATH "\$ORIGIN/${mikai_linux_lib_from_bin}"
  INSTALL_RPATH_USE_LINK_PATH FALSE
  BUILD_RPATH_USE_ORIGIN TRUE)
install(TARGETS mikai RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/shaders/"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/mikai/shaders"
  FILES_MATCHING PATTERN "*.glsl")
# The shader resolver already supports <executable>/shaders. A relative link
# keeps a single asset copy and needs neither working-directory assumptions nor
# a compiled-in installation prefix.
file(RELATIVE_PATH mikai_linux_shaders_from_bin
  "/${CMAKE_INSTALL_BINDIR}" "/${CMAKE_INSTALL_DATADIR}/mikai/shaders")
configure_file("${CMAKE_SOURCE_DIR}/packaging/linux/InstallShaderLink.cmake.in"
  "${CMAKE_BINARY_DIR}/mikai-linux-shader-link.cmake" @ONLY)
install(SCRIPT "${CMAKE_BINARY_DIR}/mikai-linux-shader-link.cmake")
# The benchmark catalogue resolves its clips relative to the executable. Both
# install directories are configurable and the tree must stay relocatable, so the
# relative path is derived from them and compiled in instead of assuming bin/
# plus ../share. Every Linux route (tar, AppImage, deb, Arch) installs the same
# data-dir subdirectory, and configuration fails before anything is built when a
# clip is absent rather than packaging an app whose benchmark reports
# "clip not found".
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MikaiBenchmarks.cmake")
mikai_benchmark_clips("${CMAKE_SOURCE_DIR}/${MIKAI_BENCHMARK_SOURCE_SUBDIR}"
  mikai_linux_benchmark_clips)
file(RELATIVE_PATH mikai_linux_benchmarks_from_bin
  "/${CMAKE_INSTALL_BINDIR}" "/${CMAKE_INSTALL_DATADIR}/mikai/benchmarks")
# PUBLIC follows the build-identity definitions on this target: the report test
# resolves its expected fixture location from the same compiled path, so it must
# see the value even though only mikai_app implements the lookup.
target_compile_definitions(mikai_app PUBLIC
  MIKAI_BENCHMARK_INSTALL_RELATIVE="${mikai_linux_benchmarks_from_bin}")
install(FILES ${mikai_linux_benchmark_clips}
  DESTINATION "${CMAKE_INSTALL_DATADIR}/mikai/benchmarks")
install(FILES "${CMAKE_SOURCE_DIR}/packaging/linux/mikai.desktop"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES "${CMAKE_SOURCE_DIR}/resources/app/mikai.svg"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
install(FILES "${CMAKE_SOURCE_DIR}/resources/app/mikai.png"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps")
configure_file("${CMAKE_SOURCE_DIR}/packaging/linux/mikai.metainfo.xml.in"
  "${CMAKE_BINARY_DIR}/mikai.metainfo.xml" @ONLY)
install(FILES "${CMAKE_BINARY_DIR}/mikai.metainfo.xml"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
install(FILES "${CMAKE_SOURCE_DIR}/LICENSE"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/mikai")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/packaging/licenses/"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/mikai/third-party")
install(FILES "${CMAKE_SOURCE_DIR}/packaging/third-party.json"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/mikai")
configure_file("${CMAKE_SOURCE_DIR}/packaging/linux/runtime-requirements.txt.in"
  "${CMAKE_BINARY_DIR}/mikai-runtime-requirements.txt" @ONLY)
install(FILES "${CMAKE_BINARY_DIR}/mikai-runtime-requirements.txt"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/mikai" RENAME "runtime-requirements.txt")
# The Qt plugin tree this build links against: Qt's own deploy step gathers
# plugins by module and leaves the QPA platform plugins out when the
# distribution Qt ships no qpa description, so both the completed install tree
# and the AppImage need the real directory.
set(MIKAI_LINUX_QT_PLUGIN_DIR "")
if(MIKAI_LINUX_DEPLOY_QT AND DEFINED QT6_INSTALL_PREFIX AND DEFINED QT6_INSTALL_PLUGINS)
  set(_mikai_qt_plugin_dir "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}")
  if(IS_DIRECTORY "${_mikai_qt_plugin_dir}")
    set(MIKAI_LINUX_QT_PLUGIN_DIR "${_mikai_qt_plugin_dir}")
  endif()
endif()

if(MIKAI_LINUX_DEPLOY_QT)
  # Do not suppress platform/deployment errors or omit plugins. The official Qt
  # deploy script writes qt.conf and gathers plugins + linked non-system deps.
  #
  # CMake >= 3.21 makes Qt adjust plugin RPATHs with file(RPATH_CHANGE), which
  # refuses a file that carries no RPATH at all. A distribution Qt (Arch, for
  # one) strips those entries because its libraries live in the default search
  # path, so every plugin would abort the install. Qt's own patchelf path adds
  # the entry instead of rewriting one.
  find_program(MIKAI_DEPLOY_PATCHELF_EXECUTABLE NAMES patchelf)
  if(NOT MIKAI_DEPLOY_PATCHELF_EXECUTABLE)
    message(FATAL_ERROR
      "MIKAI_LINUX_DEPLOY_QT needs patchelf to give the gathered Qt plugins their "
      "relative RUNPATH; install patchelf and reconfigure.")
  endif()
  set(QT_DEPLOY_USE_PATCHELF ON)
  # The C library, the loader and the graphics-driver libraries must come from
  # the host. A private libc beside the host ld.so is not a portable bundle: it
  # raises the baseline to the build host and breaks as soon as the loader and
  # libc disagree. GL/EGL dispatch belongs to the installed driver for the same
  # reason, and Qt's deployment gathers them because the build links them.
  qt6_generate_deploy_app_script(TARGET mikai OUTPUT_SCRIPT mikai_linux_qt_deploy
    PRE_EXCLUDE_REGEXES
      "ld-linux[^/]*\\.so[^/]*"
      "libc\\.so[^/]*"
      "libm\\.so[^/]*"
      "libmvec\\.so[^/]*"
      "libpthread\\.so[^/]*"
      "libdl\\.so[^/]*"
      "librt\\.so[^/]*"
      "libresolv\\.so[^/]*"
      "libutil\\.so[^/]*"
      "libanl\\.so[^/]*"
      "libnss_[^/]*\\.so[^/]*"
      "libgcc_s\\.so[^/]*"
      "libGL\\.so[^/]*"
      "libGLX\\.so[^/]*"
      "libGLESv2\\.so[^/]*"
      "libOpenGL\\.so[^/]*"
      "libGLdispatch\\.so[^/]*"
      "libEGL\\.so[^/]*"
      "libdrm\\.so[^/]*"
      "libgbm\\.so[^/]*"
  )
  install(SCRIPT "${mikai_linux_qt_deploy}")
  # The gathered set alone is not runnable: complete the tree and give every
  # plugin a relative RUNPATH. Without this the install tree starts only where
  # the one gathered platform plugin happens to apply (xcb on Arch, so a
  # Wayland session cannot start it at all).
  if(NOT MIKAI_LINUX_QT_PLUGIN_DIR STREQUAL "")
    configure_file("${CMAKE_SOURCE_DIR}/packaging/linux/CompleteQtPlugins.cmake.in"
      "${CMAKE_BINARY_DIR}/mikai-linux-qt-plugins.cmake" @ONLY)
    install(SCRIPT "${CMAKE_BINARY_DIR}/mikai-linux-qt-plugins.cmake")
  endif()
endif()

# Preserve the configured feature switches for package-time refusal checks.
set(MIKAI_LINUX_WITH_NATIVE "${MIKAI_WITH_NATIVE}")
set(MIKAI_LINUX_DIST_NATIVE "${MIKAI_DIST_NATIVE}")
set(MIKAI_LINUX_WITH_MODEL_INSPECTION "${MIKAI_WITH_MODEL_INSPECTION}")
configure_file("${CMAKE_SOURCE_DIR}/packaging/linux/release-config.cmake.in"
  "${CMAKE_BINARY_DIR}/mikai-linux-release.cmake" @ONLY)
add_custom_target(dist
  COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DSTAGE_DIR=${MIKAI_DIST_DIR}"
    "-DCONFIG=$<CONFIG>"
    -P "${CMAKE_SOURCE_DIR}/packaging/linux/Stage.cmake"
  DEPENDS mikai
  COMMENT "Installing Linux release into a fresh dependency-based staging tree"
  VERBATIM)

# One-file AppImage for local use. It refuses a build that would rely on the
# host's Qt or libmpv, and it refuses to overwrite an existing artifact.
set(MIKAI_APPIMAGE_TOOL "" CACHE FILEPATH "appimagetool used by the appimage target")
add_custom_target(appimage
  COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DOUTPUT_DIR=${CMAKE_BINARY_DIR}/_appimage"
    "-DAPPIMAGE_TOOL=${MIKAI_APPIMAGE_TOOL}"
    -P "${CMAKE_SOURCE_DIR}/packaging/linux/AppImage.cmake"
  DEPENDS mikai
  COMMENT "Building the AppImage under ${CMAKE_BINARY_DIR}/_appimage"
  VERBATIM)
