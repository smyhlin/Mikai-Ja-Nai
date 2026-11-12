# Release package contract: validate the installed payload and its archive metadata.
foreach(required BUILD_DIR OUTPUT_DIR STAGE_DIR archive_name archive_digest)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "Linux package contract: ${required} is required.")
  endif()
endforeach()
foreach(required MIKAI_LINUX_BINDIR MIKAI_LINUX_DATADIR MIKAI_LINUX_VERSION)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "Linux package contract: ${required} is missing from release configuration.")
  endif()
endforeach()

set(package_exe "${STAGE_DIR}/${MIKAI_LINUX_BINDIR}/mikai")
set(package_desktop "${STAGE_DIR}/${MIKAI_LINUX_DATADIR}/applications/mikai.desktop")
set(package_metainfo "${STAGE_DIR}/${MIKAI_LINUX_DATADIR}/metainfo/mikai.metainfo.xml")
set(package_icon "${STAGE_DIR}/${MIKAI_LINUX_DATADIR}/icons/hicolor/512x512/apps/mikai.png")
set(package_svg "${STAGE_DIR}/${MIKAI_LINUX_DATADIR}/icons/hicolor/scalable/apps/mikai.svg")
set(package_license "${STAGE_DIR}/${MIKAI_LINUX_DATADIR}/licenses/mikai/LICENSE")
set(package_notice "${STAGE_DIR}/${MIKAI_LINUX_DATADIR}/licenses/mikai/third-party/NOTICE.txt")
set(package_legal "${STAGE_DIR}/${MIKAI_LINUX_DATADIR}/licenses/mikai/third-party.json")
set(package_shader_link "${STAGE_DIR}/${MIKAI_LINUX_BINDIR}/shaders")
set(package_shader_dir "${STAGE_DIR}/${MIKAI_LINUX_DATADIR}/mikai/shaders")
foreach(required_file IN ITEMS
    "${package_exe}" "${package_desktop}" "${package_metainfo}"
    "${package_icon}" "${package_svg}" "${package_license}" "${package_notice}"
    "${package_legal}" "${STAGE_DIR}/VERSION"
    "${STAGE_DIR}/${MIKAI_LINUX_DATADIR}/mikai/runtime-requirements.txt")
  if(NOT EXISTS "${required_file}" OR IS_DIRECTORY "${required_file}")
    message(FATAL_ERROR "Linux package contract: required payload missing: ${required_file}")
  endif()
endforeach()
if(NOT IS_SYMLINK "${package_shader_link}" OR NOT IS_DIRECTORY "${package_shader_dir}")
  message(FATAL_ERROR "Linux package contract: executable-relative shader link or data directory is missing.")
endif()
file(READ_SYMLINK "${package_shader_link}" shader_link_target)
file(RELATIVE_PATH expected_shader_link "/${MIKAI_LINUX_BINDIR}"
  "/${MIKAI_LINUX_DATADIR}/mikai/shaders")
if(IS_ABSOLUTE "${shader_link_target}" OR NOT shader_link_target STREQUAL expected_shader_link)
  message(FATAL_ERROR "Linux package contract: shader link must be relative for relocation.")
endif()
file(REAL_PATH "${package_shader_link}" shader_link_real)
file(REAL_PATH "${package_shader_dir}" shader_dir_real)
if(NOT shader_link_real STREQUAL shader_dir_real)
  message(FATAL_ERROR "Linux package contract: shader link does not resolve to the installed shader directory.")
endif()
file(GLOB shader_files LIST_DIRECTORIES FALSE "${package_shader_dir}/*.glsl")
if(NOT shader_files)
  message(FATAL_ERROR "Linux package contract: no GLSL shaders were installed.")
endif()
# The benchmark catalogue reads <executable dir>/../share/mikai/benchmarks, so a
# package without those clips installs an app that reports "clip not found" for
# every resolution. Compare bytes rather than names: the installed asset must be
# the packaged source asset, not a re-encode or a substitute.
include("${MIKAI_LINUX_SOURCE_DIR}/cmake/MikaiBenchmarks.cmake")
mikai_benchmark_clips("${MIKAI_LINUX_SOURCE_DIR}/${MIKAI_BENCHMARK_SOURCE_SUBDIR}"
  contract_benchmark_clips)
foreach(clip IN LISTS contract_benchmark_clips)
  get_filename_component(clip_name "${clip}" NAME)
  set(installed_clip "${STAGE_DIR}/${MIKAI_LINUX_DATADIR}/mikai/benchmarks/${clip_name}")
  if(NOT EXISTS "${installed_clip}" OR IS_DIRECTORY "${installed_clip}")
    message(FATAL_ERROR
      "Linux package contract: benchmark clip was not installed: ${installed_clip}")
  endif()
  file(SHA256 "${clip}" clip_digest)
  file(SHA256 "${installed_clip}" installed_clip_digest)
  if(NOT clip_digest STREQUAL installed_clip_digest)
    message(FATAL_ERROR
      "Linux package contract: installed benchmark clip differs from the packaged "
      "source asset: ${clip_name}")
  endif()
endforeach()
file(STRINGS "${package_desktop}" desktop_exec REGEX "^Exec=mikai --open-video %f$")
if(NOT desktop_exec)
  message(FATAL_ERROR "Linux package contract: desktop entry is missing the safe video-open command.")
endif()
file(STRINGS "${package_legal}" legal_status REGEX "linux-x86_64")
if(NOT legal_status)
  message(FATAL_ERROR "Linux package contract: third-party manifest has no Linux x86_64 release status.")
endif()

if(MIKAI_LINUX_MPV_BUNDLE_MODE STREQUAL "bundled")
  set(package_mpv_module "${STAGE_DIR}/${MIKAI_LINUX_MPV_MODULE_RELATIVE}")
  if(NOT EXISTS "${package_mpv_module}" OR IS_SYMLINK "${package_mpv_module}" OR IS_DIRECTORY "${package_mpv_module}")
    message(FATAL_ERROR "Linux package contract: bundled mpv module missing or not a regular file: ${package_mpv_module}")
  endif()
  file(GLOB package_mpv_private LIST_DIRECTORIES FALSE
    "${STAGE_DIR}/${MIKAI_LINUX_MPV_INSTALL_SUBDIR}/*.so*")
  list(LENGTH package_mpv_private package_mpv_private_count)
  if(package_mpv_private_count LESS 2)
    message(FATAL_ERROR "Linux package contract: no private mpv dependencies were staged")
  endif()
  foreach(private_library IN LISTS package_mpv_private)
    if(IS_SYMLINK "${private_library}" OR IS_DIRECTORY "${private_library}")
      message(FATAL_ERROR "Linux package contract: private mpv library is not regular: ${private_library}")
    endif()
  endforeach()
endif()
# Qt deployment: the player is a native Quick window, so a package that carries
# the Qt libraries but no QML import tree cannot create its scene at all.
# Resolve the import directory the way the runtime does - through the deployed
# qt.conf, whose Prefix and QmlImports keep the configured Qt's prefix-relative
# layout (lib/qt6/qml for a distribution Qt, qml for the official SDK) - and
# require the modules the player imports plus the libraries behind them. A
# host-Qt package (MIKAI_LINUX_DEPLOY_QT=OFF) takes these from the host Qt by
# design and is not checked here.
if(MIKAI_LINUX_DEPLOY_QT)
  set(package_qt_conf "${STAGE_DIR}/${MIKAI_LINUX_BINDIR}/qt.conf")
  if(NOT EXISTS "${package_qt_conf}" OR IS_DIRECTORY "${package_qt_conf}")
    message(FATAL_ERROR
      "Linux package contract: Qt deployment did not write a relocatable qt.conf: ${package_qt_conf}")
  endif()
  file(STRINGS "${package_qt_conf}" package_qt_conf_paths
    REGEX "^[ \t]*(Prefix|Libraries|Plugins|QmlImports)[ \t]*=")
  set(package_qt_prefix ".")
  set(package_qt_prefix_seen FALSE)
  set(package_qt_libraries "")
  set(package_qt_plugins "")
  set(package_qt_qml_imports "")
  foreach(package_qt_conf_path IN LISTS package_qt_conf_paths)
    if(package_qt_conf_path MATCHES "^[ \t]*Prefix[ \t]*=[ \t]*(.+)$")
      string(STRIP "${CMAKE_MATCH_1}" package_qt_prefix)
      set(package_qt_prefix_seen TRUE)
    elseif(package_qt_conf_path MATCHES "^[ \t]*Libraries[ \t]*=[ \t]*(.+)$")
      string(STRIP "${CMAKE_MATCH_1}" package_qt_libraries)
    elseif(package_qt_conf_path MATCHES "^[ \t]*Plugins[ \t]*=[ \t]*(.+)$")
      string(STRIP "${CMAKE_MATCH_1}" package_qt_plugins)
    elseif(package_qt_conf_path MATCHES "^[ \t]*QmlImports[ \t]*=[ \t]*(.+)$")
      string(STRIP "${CMAKE_MATCH_1}" package_qt_qml_imports)
    endif()
  endforeach()
  if(NOT package_qt_prefix_seen OR package_qt_libraries STREQUAL ""
      OR package_qt_plugins STREQUAL "" OR package_qt_qml_imports STREQUAL ""
      OR IS_ABSOLUTE "${package_qt_prefix}"
      OR IS_ABSOLUTE "${package_qt_libraries}"
      OR IS_ABSOLUTE "${package_qt_plugins}"
      OR IS_ABSOLUTE "${package_qt_qml_imports}")
    message(FATAL_ERROR
      "Linux package contract: qt.conf must define prefix-relative Prefix, Libraries, Plugins and QmlImports paths.")
  endif()
  set(package_qt_prefix_dir "${STAGE_DIR}/${MIKAI_LINUX_BINDIR}/${package_qt_prefix}")
  cmake_path(NORMAL_PATH package_qt_prefix_dir)
  set(package_qt_qml_dir "${package_qt_prefix_dir}/${package_qt_qml_imports}")
  cmake_path(NORMAL_PATH package_qt_qml_dir)
  if(NOT IS_DIRECTORY "${package_qt_qml_dir}")
    message(FATAL_ERROR
      "Linux package contract: the deployed QML import tree is missing: ${package_qt_qml_dir}")
  endif()
  set(package_qt_plugins_dir "${package_qt_prefix_dir}/${package_qt_plugins}")
  cmake_path(NORMAL_PATH package_qt_plugins_dir)
  set(package_stage_dir "${STAGE_DIR}")
  cmake_path(NORMAL_PATH package_stage_dir)
  file(RELATIVE_PATH package_qt_plugins_relative "${package_stage_dir}"
    "${package_qt_plugins_dir}")
  if(IS_ABSOLUTE "${package_qt_plugins_relative}"
      OR package_qt_plugins_relative MATCHES "^\\.\\."
      OR NOT IS_DIRECTORY "${package_qt_plugins_dir}")
    message(FATAL_ERROR
      "Linux package contract: qt.conf plugin path is missing or escapes the package: ${package_qt_plugins_dir}")
  endif()
  set(package_qt_lib_dir "${package_qt_prefix_dir}/${package_qt_libraries}")
  cmake_path(NORMAL_PATH package_qt_lib_dir)
  file(RELATIVE_PATH package_qt_lib_relative "${package_stage_dir}"
    "${package_qt_lib_dir}")
  if(IS_ABSOLUTE "${package_qt_lib_relative}"
      OR package_qt_lib_relative MATCHES "^\\.\\."
      OR NOT IS_DIRECTORY "${package_qt_lib_dir}")
    message(FATAL_ERROR
      "Linux package contract: qt.conf library path is missing or escapes the package: ${package_qt_lib_dir}")
  endif()
  file(RELATIVE_PATH package_qt_qml_relative "${package_stage_dir}"
    "${package_qt_qml_dir}")
  if(IS_ABSOLUTE "${package_qt_qml_relative}"
      OR package_qt_qml_relative MATCHES "^\\.\\.")
    message(FATAL_ERROR
      "Linux package contract: qt.conf points the QML import path outside the package: ${package_qt_qml_dir}")
  endif()
  # Names are the modules the player scene imports by name, so a package that
  # lost one of them fails here instead of at the first window construction.
  file(GLOB package_qt_platform_plugins LIST_DIRECTORIES FALSE
    "${package_qt_plugins_dir}/platforms/*.so" "${package_qt_plugins_dir}/platforms/*.so.*")
  if(NOT package_qt_platform_plugins)
    message(FATAL_ERROR
      "Linux package contract: no Qt platform plugins were deployed under ${package_qt_plugins_dir}/platforms")
  endif()
  foreach(package_qml_module IN ITEMS
      QtQuick QtQuick/Controls QtQuick/Controls/Basic QtQuick/Dialogs)
    set(package_qml_module_dir "${package_qt_qml_dir}/${package_qml_module}")
    if(NOT EXISTS "${package_qml_module_dir}/qmldir"
        OR IS_DIRECTORY "${package_qml_module_dir}/qmldir")
      message(FATAL_ERROR
        "Linux package contract: deployed QML module ${package_qml_module} has no qmldir: ${package_qml_module_dir}")
    endif()
    file(GLOB package_qml_plugins LIST_DIRECTORIES FALSE
      "${package_qml_module_dir}/*.so*")
    if(NOT package_qml_plugins)
      message(FATAL_ERROR
        "Linux package contract: deployed QML module ${package_qml_module} has no plugin library beside its qmldir: ${package_qml_module_dir}")
    endif()
  endforeach()
  # The QML stack is loaded at run time, so the shared libraries it needs are
  # not implied by the executable's own dependency list being satisfiable.
  foreach(package_qt_library IN ITEMS
      Qt6Core Qt6Gui Qt6Qml Qt6Quick Qt6QuickControls2)
    file(GLOB package_qt_library_files LIST_DIRECTORIES FALSE
      "${package_qt_lib_dir}/${package_qt_library}.so*")
    if(NOT package_qt_library_files)
      message(FATAL_ERROR
        "Linux package contract: ${package_qt_library} was not deployed into ${package_qt_lib_dir}; the Quick player needs it at run time.")
    endif()
  endforeach()
endif()

file(GLOB_RECURSE staged_entries LIST_DIRECTORIES FALSE RELATIVE "${STAGE_DIR}" "${STAGE_DIR}/*")
foreach(entry IN LISTS staged_entries)
  if(entry MATCHES "(^|/)(native|models|records|cache|logs|crashes|probes|tests)(/|$)"
      OR entry MATCHES "(^|/)\\.env($|/)")
    message(FATAL_ERROR "Linux package contract: forbidden user/runtime payload included: ${entry}")
  endif()
endforeach()

set(stage_checksums "${STAGE_DIR}/SHA256SUMS")
if(NOT EXISTS "${stage_checksums}")
  message(FATAL_ERROR "Linux package contract: staged SHA256SUMS is missing.")
endif()
file(STRINGS "${stage_checksums}" checksum_lines)
if(NOT checksum_lines)
  message(FATAL_ERROR "Linux package contract: staged SHA256SUMS is empty.")
endif()
foreach(checksum_line IN LISTS checksum_lines)
  if(NOT checksum_line MATCHES "^([0-9a-f]+)  (.+)$")
    message(FATAL_ERROR "Linux package contract: malformed staged checksum: ${checksum_line}")
  endif()
  set(expected_hash "${CMAKE_MATCH_1}")
  string(LENGTH "${expected_hash}" hash_length)
  if(NOT hash_length EQUAL 64)
    message(FATAL_ERROR "Linux package contract: checksum is not SHA-256: ${checksum_line}")
  endif()
  set(relative_file "${CMAKE_MATCH_2}")
  if(NOT EXISTS "${STAGE_DIR}/${relative_file}" OR IS_SYMLINK "${STAGE_DIR}/${relative_file}")
    message(FATAL_ERROR "Linux package contract: checksum entry is not a regular staged file: ${relative_file}")
  endif()
  file(SHA256 "${STAGE_DIR}/${relative_file}" actual_hash)
  if(NOT actual_hash STREQUAL expected_hash)
    message(FATAL_ERROR "Linux package contract: staged checksum mismatch: ${relative_file}")
  endif()
endforeach()

set(archive_path "${OUTPUT_DIR}/${archive_name}")
if(NOT EXISTS "${archive_path}" OR IS_DIRECTORY "${archive_path}")
  message(FATAL_ERROR "Linux package contract: archive is missing: ${archive_path}")
endif()
file(SHA256 "${archive_path}" actual_archive_hash)
if(NOT actual_archive_hash STREQUAL archive_digest)
  message(FATAL_ERROR "Linux package contract: archive SHA-256 does not match its manifest.")
endif()
file(READ "${OUTPUT_DIR}/SHA256SUMS" outer_checksum)
string(STRIP "${outer_checksum}" outer_checksum)
if(NOT outer_checksum STREQUAL "${archive_digest}  ${archive_name}")
  message(FATAL_ERROR "Linux package contract: outer SHA256SUMS does not cover the archive.")
endif()
message(STATUS "Linux package contract passed: install payload, relocation link, legal metadata and SHA-256 manifests are consistent.")
