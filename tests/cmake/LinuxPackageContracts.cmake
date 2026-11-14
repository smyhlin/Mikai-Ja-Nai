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
# The Qt Quick marker requirement must be able to refuse a payload, not only
# pass when the modules are present. The package just validated is the fixture:
# remove one required QML module marker from the staged tree and run the same
# contract again over it. It has to fail, and its refusal has to name that
# module. Nothing here inspects the contract's source, and the staged tree is
# removed afterwards either way.
include("${BUILD_DIR}/mikai-linux-release.cmake")
if(MIKAI_LINUX_DEPLOY_QT)
  set(_stage "${_output}/mikai-${MIKAI_LINUX_VERSION}-linux-x86_64")
  set(_archive_name "mikai-${MIKAI_LINUX_VERSION}-linux-x86_64.tar.gz")
  if(NOT IS_DIRECTORY "${_stage}")
    file(REMOVE_RECURSE "${_output}")
    message(FATAL_ERROR "LinuxPackageContracts: the staged tree is missing: ${_stage}")
  endif()
  file(READ "${_output}/SHA256SUMS" _outer_checksum)
  if(NOT _outer_checksum MATCHES "^([0-9a-f]+)  ${_archive_name}")
    file(REMOVE_RECURSE "${_output}")
    message(FATAL_ERROR
      "LinuxPackageContracts: the outer checksum does not cover ${_archive_name}")
  endif()
  set(_archive_digest "${CMAKE_MATCH_1}")

  # Resolve the deployed QML import directory through qt.conf, exactly as the
  # runtime does, so a distribution Qt and the official SDK are both handled.
  file(STRINGS "${_stage}/${MIKAI_LINUX_BINDIR}/qt.conf" _qt_conf_paths
    REGEX "^[ \t]*(Prefix|Libraries|Plugins|QmlImports|Qml2Imports)[ \t]*=")
  set(_qt_prefix ".")
  set(_qt_libraries "lib")
  set(_qt_plugins "plugins")
  set(_qt_qml_imports "qml")
  set(_qt_qml_imports_seen FALSE)
  set(_qt_qml2_imports "")
  foreach(_qt_conf_path IN LISTS _qt_conf_paths)
    if(_qt_conf_path MATCHES "^[ \t]*Prefix[ \t]*=[ \t]*(.+)$")
      string(STRIP "${CMAKE_MATCH_1}" _qt_prefix)
    elseif(_qt_conf_path MATCHES "^[ \t]*Libraries[ \t]*=[ \t]*(.+)$")
      string(STRIP "${CMAKE_MATCH_1}" _qt_libraries)
    elseif(_qt_conf_path MATCHES "^[ \t]*Plugins[ \t]*=[ \t]*(.+)$")
      string(STRIP "${CMAKE_MATCH_1}" _qt_plugins)
    elseif(_qt_conf_path MATCHES "^[ \t]*QmlImports[ \t]*=[ \t]*(.+)$")
      string(STRIP "${CMAKE_MATCH_1}" _qt_qml_imports)
      set(_qt_qml_imports_seen TRUE)
    elseif(_qt_conf_path MATCHES "^[ \t]*Qml2Imports[ \t]*=[ \t]*(.+)$")
      string(STRIP "${CMAKE_MATCH_1}" _qt_qml2_imports)
    endif()
  endforeach()
  if(NOT _qt_qml_imports_seen AND NOT _qt_qml2_imports STREQUAL "")
    set(_qt_qml_imports "${_qt_qml2_imports}")
  endif()
  set(_qt_prefix_dir "${_stage}/${MIKAI_LINUX_BINDIR}/${_qt_prefix}")
  cmake_path(NORMAL_PATH _qt_prefix_dir)
  set(_qml_dir "${_qt_prefix_dir}/${_qt_qml_imports}")
  cmake_path(NORMAL_PATH _qml_dir)
  set(_qt_plugins_dir "${_qt_prefix_dir}/${_qt_plugins}")
  set(_qt_lib_dir "${_qt_prefix_dir}/${_qt_libraries}")
  cmake_path(NORMAL_PATH _qt_plugins_dir)
  cmake_path(NORMAL_PATH _qt_lib_dir)
  set(_qt_conf "${_stage}/${MIKAI_LINUX_BINDIR}/qt.conf")
  file(READ "${_qt_conf}" _original_qt_conf)
  file(SHA256 "${_qt_conf}" _original_qt_conf_hash)
  file(READ "${_stage}/SHA256SUMS" _original_checksums)
  set(_path_runner "${_output}/qt-path-contract.cmake")
  file(WRITE "${_path_runner}" "
set(BUILD_DIR [==[${BUILD_DIR}]==])
set(STAGE_DIR [==[${_stage}]==])
set(OUTPUT_DIR [==[${_output}]==])
set(archive_name [==[${_archive_name}]==])
set(archive_digest [==[${_archive_digest}]==])
include([==[${BUILD_DIR}/mikai-linux-release.cmake]==])
include([==[${MIKAI_LINUX_SOURCE_DIR}/packaging/linux/Contract.cmake]==])
")
  function(_check_qt_paths label contents)
    file(WRITE "${_qt_conf}" "${contents}")
    file(SHA256 "${_qt_conf}" _qt_conf_hash)
    string(REPLACE "${_original_qt_conf_hash}  ${MIKAI_LINUX_BINDIR}/qt.conf"
      "${_qt_conf_hash}  ${MIKAI_LINUX_BINDIR}/qt.conf"
      _checksums "${_original_checksums}")
    file(WRITE "${_stage}/SHA256SUMS" "${_checksums}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -P "${_path_runner}"
      RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr
      TIMEOUT 300
    )
    file(WRITE "${_qt_conf}" "${_original_qt_conf}")
    file(WRITE "${_stage}/SHA256SUMS" "${_original_checksums}")
    if(NOT _result EQUAL 0)
      file(REMOVE_RECURSE "${_output}")
      message(FATAL_ERROR
        "LinuxPackageContracts: ${label} paths were rejected.\n${_stdout}\n${_stderr}")
    endif()
    message(STATUS "LinuxPackageContracts: ${label} paths accepted")
  endfunction()

  foreach(_layout IN ITEMS default distribution)
    if(_layout STREQUAL "default")
      set(_prefix_dir "${_stage}/${MIKAI_LINUX_BINDIR}")
      set(_libraries "lib")
      set(_plugins "plugins")
      set(_imports "qml")
      set(_contents "[Paths]\n")
    else()
      set(_prefix_dir "${_stage}/qt-layout")
      set(_libraries "lib/x86_64-linux-gnu")
      set(_plugins "lib/qt6/plugins")
      set(_imports "lib/qt6/qml")
      file(RELATIVE_PATH _prefix "/${MIKAI_LINUX_BINDIR}" "/qt-layout")
      set(_contents
        "[Paths]\nPrefix=${_prefix}\nLibraries=${_libraries}\nPlugins=${_plugins}\nQmlImports=${_imports}\n")
    endif()
    set(_fixture_links)
    foreach(_pair IN ITEMS
        "${_qt_lib_dir}|${_prefix_dir}/${_libraries}"
        "${_qt_plugins_dir}|${_prefix_dir}/${_plugins}"
        "${_qml_dir}|${_prefix_dir}/${_imports}")
      string(REPLACE "|" ";" _pair_parts "${_pair}")
      list(GET _pair_parts 0 _source_path)
      list(GET _pair_parts 1 _target_path)
      cmake_path(NORMAL_PATH _source_path)
      cmake_path(NORMAL_PATH _target_path)
      if(_source_path STREQUAL _target_path)
        continue()
      endif()
      if(EXISTS "${_target_path}" OR IS_SYMLINK "${_target_path}")
        file(REMOVE_RECURSE "${_output}")
        message(FATAL_ERROR "LinuxPackageContracts: Qt fixture path already exists: ${_target_path}")
      endif()
      get_filename_component(_target_parent "${_target_path}" DIRECTORY)
      file(MAKE_DIRECTORY "${_target_parent}")
      file(RELATIVE_PATH _link_target "${_target_parent}" "${_source_path}")
      file(CREATE_LINK "${_link_target}" "${_target_path}" SYMBOLIC RESULT _link_result)
      if(NOT _link_result STREQUAL "0")
        file(REMOVE_RECURSE "${_output}")
        message(FATAL_ERROR "LinuxPackageContracts: Qt fixture link failed: ${_link_result}")
      endif()
      list(APPEND _fixture_links "${_target_path}")
    endforeach()
    _check_qt_paths("${_layout}" "${_contents}")
    if(_fixture_links)
      file(REMOVE ${_fixture_links})
    endif()
  endforeach()
  file(REMOVE_RECURSE "${_stage}/qt-layout")
  set(_removed_marker "${_qml_dir}/QtQuick/Controls/Basic/qmldir")
  if(NOT EXISTS "${_removed_marker}")
    file(REMOVE_RECURSE "${_output}")
    message(FATAL_ERROR
      "LinuxPackageContracts: the validated package has no ${_removed_marker}, so "
      "the refusal cannot be exercised")
  endif()
  file(REMOVE "${_removed_marker}")

  set(_runner "${_output}/refusal-contract.cmake")
  file(WRITE "${_runner}" "
set(BUILD_DIR [==[${BUILD_DIR}]==])
set(STAGE_DIR [==[${_stage}]==])
set(OUTPUT_DIR [==[${_output}]==])
set(archive_name [==[${_archive_name}]==])
set(archive_digest [==[${_archive_digest}]==])
include([==[${BUILD_DIR}/mikai-linux-release.cmake]==])
include([==[${MIKAI_LINUX_SOURCE_DIR}/packaging/linux/Contract.cmake]==])
")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -P "${_runner}"
    RESULT_VARIABLE _refusal_result
    OUTPUT_VARIABLE _refusal_stdout
    ERROR_VARIABLE _refusal_stderr
    TIMEOUT 300
  )
  if(_refusal_result EQUAL 0)
    file(REMOVE_RECURSE "${_output}")
    message(FATAL_ERROR
      "LinuxPackageContracts: the contract accepted a package whose "
      "QtQuick/Controls/Basic QML module was removed")
  endif()
  if(NOT _refusal_stderr MATCHES "QtQuick/Controls/Basic")
    file(REMOVE_RECURSE "${_output}")
    message(FATAL_ERROR
      "LinuxPackageContracts: the refusal did not name the removed QML module.\n"
      "${_refusal_stdout}\n${_refusal_stderr}")
  endif()
  message(STATUS
    "LinuxPackageContracts: a package without the QtQuick.Controls.Basic QML "
    "module is refused by name")
endif()

file(REMOVE_RECURSE "${_output}")
message(STATUS "LinuxPackageContracts: the real install/archive contract passed")
