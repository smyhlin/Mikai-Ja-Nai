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
    MIKAI_LINUX_MPV_MODULE_RELATIVE="mpv/libmpv.so.2")
  file(GLOB MIKAI_LINUX_MPV_BUILD_LIBRARIES LIST_DIRECTORIES FALSE
    "${MIKAI_LINUX_MPV_BUNDLE_ROOT}/*.so*")
  set(_mikai_linux_mpv_build_copy_commands
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:mikai>/mpv")
  foreach(_library IN LISTS MIKAI_LINUX_MPV_BUILD_LIBRARIES)
    get_filename_component(_library_name "${_library}" NAME)
    list(APPEND _mikai_linux_mpv_build_copy_commands
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${_library}" "$<TARGET_FILE_DIR:mikai>/mpv/${_library_name}")
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
  set(MIKAI_LINUX_MPV_MODULE_RELATIVE "${CMAKE_INSTALL_BINDIR}/mpv/libmpv.so.2")
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
if(MIKAI_LINUX_DEPLOY_QT)
  # Do not suppress platform/deployment errors or omit plugins. The official Qt
  # deploy script writes qt.conf and gathers plugins + linked non-system deps.
  qt6_generate_deploy_app_script(TARGET mikai OUTPUT_SCRIPT mikai_linux_qt_deploy)
  install(SCRIPT "${mikai_linux_qt_deploy}")
endif()

if(NOT DEFINED MIKAI_SYSTEM_MPV_PC_VERSION)
  set(MIKAI_SYSTEM_MPV_PC_VERSION "")
endif()
set(MIKAI_LINUX_MPV_HEADER_PKGCONFIG "${MIKAI_SYSTEM_MPV_PC_VERSION}")
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
