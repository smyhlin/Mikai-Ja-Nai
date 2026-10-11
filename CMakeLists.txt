# Schema version 6 in CMakePresets.json was introduced in CMake 3.25, so the
# declared minimum and the preset format must agree: a 3.24 minimum with a
# schema-6 preset is a range the project cannot actually configure.
cmake_minimum_required(VERSION 3.25)
project(mikai LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Warnings on, and kept on. The tree had none configured, and a single sweep with
# them turned up a buffer overrun, an unreachable suite and two functions that
# were never called - all of which compiled silently. The two suppressions are
# deliberate: Qt's signal/slot signatures leave unused parameters everywhere, and
# the GetProcAddress cast is the only way to load a symbol by name.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type)
endif()

# Deprecated Qt APIs are errors, not warnings. 0x060C00 is Qt 6.12.0: anything
# Qt deprecated up to and including the version this tree is built against
# disappears from the headers, so a use fails the build here instead of
# surfacing after the next framework bump. It applies to every target in this
# directory and in native/, which keeps the whole tree on one deprecation level.
add_compile_definitions(QT_DISABLE_DEPRECATED_UP_TO=0x060C00)

# And the next bump's deprecations are visible now, so the tree is not surprised
# by them: 0x060D00 is Qt 6.13, which Qt 6.12's headers already annotate. The
# unversioned QT_DEPRECATED_VERSION_X_* markers warn regardless of this.
add_compile_definitions(QT_WARN_DEPRECATED_UP_TO=0x060D00)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(MIKAI_MINGW_RUNTIME_DIR "" CACHE PATH
  "MinGW runtime DLL directory shared by Qt and the selected mpv module")
if(MINGW AND MIKAI_MINGW_RUNTIME_DIR)
  set(MIKAI_COMPILER_RUNTIME_FILES)
  foreach(runtime IN ITEMS libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
    set(runtime_file "${MIKAI_MINGW_RUNTIME_DIR}/${runtime}")
    if(NOT EXISTS "${runtime_file}")
      message(FATAL_ERROR "The selected MinGW runtime is missing: ${runtime_file}")
    endif()
    list(APPEND MIKAI_COMPILER_RUNTIME_FILES "${runtime_file}")
  endforeach()
endif()

# The three DLLs are only deployable when they *are* the compiler runtime the
# configured Qt links: Qt6Core.dll imports them by name and the executable's
# directory wins that lookup, so another MinGW generation would replace Qt's
# thread runtime rather than share it.
function(mikai_qt_compiler_runtime_dir out_var)
  set(${out_var} "" PARENT_SCOPE)
  if(NOT TARGET Qt6::Core)
    return()
  endif()
  set(qt_core_location "")
  foreach(property IN ITEMS IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION IMPORTED_LOCATION_DEBUG)
    get_target_property(qt_core_location Qt6::Core "${property}")
    if(qt_core_location)
      break()
    endif()
    set(qt_core_location "")
  endforeach()
  if(NOT qt_core_location)
    return()
  endif()
  get_filename_component(runtime_dir "${qt_core_location}" DIRECTORY)
  set(${out_var} "${runtime_dir}" PARENT_SCOPE)
endfunction()

function(mikai_deploy_mingw_runtime_targets)
  if(NOT MINGW OR NOT MIKAI_MINGW_RUNTIME_DIR)
    return()
  endif()

  # A selection that is not Qt's runtime cannot be deployed - it would replace
  # the compiler runtime Qt's own modules resolve - so refuse it by name.
  mikai_qt_compiler_runtime_dir(qt_runtime_dir)
  foreach(runtime_file IN LISTS MIKAI_COMPILER_RUNTIME_FILES)
    get_filename_component(runtime_name "${runtime_file}" NAME)
    set(qt_runtime_file "${qt_runtime_dir}/${runtime_name}")
    set(mismatch "")
    if(NOT qt_runtime_dir)
      set(mismatch "the compiler runtime Qt links could not be located")
    elseif(NOT EXISTS "${qt_runtime_file}")
      set(mismatch "Qt's runtime directory has no ${runtime_name} at ${qt_runtime_file}")
    else()
      file(SHA256 "${runtime_file}" selected_hash)
      file(SHA256 "${qt_runtime_file}" qt_hash)
      if(NOT selected_hash STREQUAL qt_hash)
        set(mismatch "${qt_runtime_file} is a different MinGW generation")
      endif()
    endif()
    if(mismatch)
      message(FATAL_ERROR
        "MIKAI_MINGW_RUNTIME_DIR is ${MIKAI_MINGW_RUNTIME_DIR}, but ${runtime_name} is not "
        "the compiler runtime Qt links: ${mismatch}. Deploying it beside a Qt executable "
        "would replace the thread runtime Qt was built against. Point the option at the Qt "
        "MinGW runtime, or leave it unset.")
    endif()
  endforeach()

  # POST_BUILD must be attached in the target's defining directory.
  get_property(directory_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS directory_targets)
    get_target_property(target_type "${target}" TYPE)
    if(target_type STREQUAL "EXECUTABLE")
      add_custom_command(TARGET "${target}" POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${MIKAI_COMPILER_RUNTIME_FILES}
          "$<TARGET_FILE_DIR:${target}>"
        VERBATIM
      )
    endif()
  endforeach()
endfunction()

# Qt6Widgets pulls in OpenGL; QOpenGLWidget itself lives in Qt6OpenGLWidgets.
# Test is deliberately not in this list: it is only needed by the QA probes and
# mikai_tests, so the app itself configures on a Qt install without it.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Network LinguistTools)

# Derived at top level, before any guard: both the launcher contract test (which
# runs even when the probes are off) and the mikai_tests entry need the same Qt
# install prefix.
get_filename_component(MIKAI_TEST_QT_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)

# The QA probes and the C++ test runner. ON by default so the tree configures
# exactly as it did before, but the probe *executables* are EXCLUDE_FROM_ALL: a
# bare `cmake --build --preset app` no longer pays for eleven extra Qt link
# steps just to produce mikai.exe. Build them by name instead, using one of the
# four *build* presets (`release` is a configure preset and cannot be built):
#   cmake --build --preset app --target <probe>
#   cmake --build --preset tests --target <probe>
# e.g.
#   cmake --build --preset tests --target title_page_probe
#   cmake --build --preset tests --target widget_pixel_probe
#   cmake --build --preset tests --target shell_probe
#   cmake --build --preset tests --target dialog_probe
#   cmake --build --preset tests --target interaction_probe
#   cmake --build --preset tests --target compare_probe
#   cmake --build --preset tests --target compare_shot_probe
#   cmake --build --preset tests --target anime4k_menu_probe
#   cmake --build --preset tests --target mpv_surface_smoke
#   cmake --build --preset tests --target headless_pixel_probe
#   cmake --build --preset tests --target crash_probe
#   cmake --build --preset tests --target resolver_probe
add_executable(mikai_manifest_check EXCLUDE_FROM_ALL native/tools/manifest_check.cpp)
target_link_libraries(mikai_manifest_check PRIVATE Qt6::Core)

option(MIKAI_BUILD_PROBES "Build the QA probes and mikai_tests" ON)
# Top level, before any guard: tests that need neither the probes nor mikai_tests
# still have to be registerable, and add_test() requires this to have run.
enable_testing()
if(MIKAI_BUILD_PROBES)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  # The native staging/patch-policy contract. It is registered *within* this
  # block only because add_test() needs enable_testing() to have run; nothing
  # about the entry depends on the probes or on MIKAI_WITH_NATIVE. The default
  # build never configures native/ at all, so gating the contract on
  # MIKAI_WITH_NATIVE would register a test nobody ever runs.
  #
  # It needs no compiler, no Qt and no nvcc - only script-mode CMake - and it
  # builds its own synthetic sources, so the baseline build's real runtime,
  # components and models are never touched.
  add_test(NAME NativeStageContracts
    COMMAND ${CMAKE_COMMAND}
      -DAJI_STAGE_CONTRACT_PROJECT=ON
      -DAJI_STAGE_HELPER=${CMAKE_CURRENT_SOURCE_DIR}/native/cmake/StageInference.cmake
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/NativeStageContracts.cmake
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  # No external timeout is needed: this runs a handful of configure-scale passes
  # over small synthetic files. A hung pass must still surface, so it is bounded.
  set_tests_properties(NativeStageContracts PROPERTIES TIMEOUT 300)

  # Shared probe helpers (ProbeRuntime/ProbeUi/ProbePicture). One include dir
  # for all probe targets so grouped sources keep bare `#include "ProbeX.hpp"`.
  add_library(mikai_probe_shared INTERFACE)
  target_include_directories(mikai_probe_shared INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/probes/shared)
endif()

set(MIKAI_DIST_MPV_ARCHIVE "" CACHE FILEPATH
  "Optional local copy of the pinned mpv-dev archive (its published SHA-256 is always checked)")
set(MIKAI_DIST_MPV_CACHE_DIR "${CMAKE_BINARY_DIR}/_dist-cache/mpv")
set(MIKAI_MPV_ARCHIVE_NAME "mpv-dev-x86_64-20261007-git-d6d93599d5.7z")
set(MIKAI_MPV_ARCHIVE_URL
  "https://github.com/the-database/mpv-winbuild/releases/download/2026-10-07-d6d93599d5/${MIKAI_MPV_ARCHIVE_NAME}")
set(MIKAI_MPV_ARCHIVE_SHA256
  "077cb75fed47b185428f97224e1d798e2d4c2f4063fd8cda2662b74f2892327c")
file(MAKE_DIRECTORY "${MIKAI_DIST_MPV_CACHE_DIR}")
if(MIKAI_DIST_MPV_ARCHIVE)
  set(MIKAI_PINNED_MPV_ARCHIVE "${MIKAI_DIST_MPV_ARCHIVE}")
  if(NOT EXISTS "${MIKAI_PINNED_MPV_ARCHIVE}")
    message(FATAL_ERROR "The local mpv archive override was not found: ${MIKAI_PINNED_MPV_ARCHIVE}")
  endif()
else()
  set(MIKAI_PINNED_MPV_ARCHIVE "${MIKAI_DIST_MPV_CACHE_DIR}/${MIKAI_MPV_ARCHIVE_NAME}")
  if(NOT EXISTS "${MIKAI_PINNED_MPV_ARCHIVE}")
    message(STATUS "Downloading pinned mpv developer archive for matching headers")
    file(DOWNLOAD "${MIKAI_MPV_ARCHIVE_URL}" "${MIKAI_PINNED_MPV_ARCHIVE}"
      EXPECTED_HASH "SHA256=${MIKAI_MPV_ARCHIVE_SHA256}"
      STATUS mpv_download_status
      SHOW_PROGRESS)
    list(GET mpv_download_status 0 mpv_download_code)
    list(GET mpv_download_status 1 mpv_download_message)
    if(NOT mpv_download_code EQUAL 0)
      file(REMOVE "${MIKAI_PINNED_MPV_ARCHIVE}")
      message(FATAL_ERROR "mpv archive download failed: ${mpv_download_message}")
    endif()
  endif()
endif()
file(SHA256 "${MIKAI_PINNED_MPV_ARCHIVE}" actual_mpv_archive_sha256)
if(NOT actual_mpv_archive_sha256 STREQUAL MIKAI_MPV_ARCHIVE_SHA256)
  message(FATAL_ERROR
    "The mpv archive SHA-256 does not match the pinned release: "
    "expected ${MIKAI_MPV_ARCHIVE_SHA256}, got ${actual_mpv_archive_sha256}")
endif()

set(MIKAI_MPV_HEADERS_DIR "${MIKAI_DIST_MPV_CACHE_DIR}/headers")
if(NOT EXISTS "${MIKAI_MPV_HEADERS_DIR}/include/mpv/client.h"
    OR NOT EXISTS "${MIKAI_MPV_HEADERS_DIR}/include/mpv/render_gl.h"
    OR NOT EXISTS "${MIKAI_MPV_HEADERS_DIR}/libmpv-2.dll")
  file(REMOVE_RECURSE "${MIKAI_MPV_HEADERS_DIR}")
  file(MAKE_DIRECTORY "${MIKAI_MPV_HEADERS_DIR}")
  file(ARCHIVE_EXTRACT INPUT "${MIKAI_PINNED_MPV_ARCHIVE}"
    DESTINATION "${MIKAI_MPV_HEADERS_DIR}")
endif()
set(MIKAI_MPV_HEADERS "${MIKAI_MPV_HEADERS_DIR}/include")
if(NOT EXISTS "${MIKAI_MPV_HEADERS}/mpv/client.h"
    OR NOT EXISTS "${MIKAI_MPV_HEADERS}/mpv/render_gl.h")
  message(FATAL_ERROR
    "The verified mpv archive does not contain include/mpv/client.h and include/mpv/render_gl.h")
endif()
file(GLOB_RECURSE MIKAI_MPV_DLL_CANDIDATES LIST_DIRECTORIES FALSE
  "${MIKAI_MPV_HEADERS_DIR}/libmpv-2.dll")
list(LENGTH MIKAI_MPV_DLL_CANDIDATES MIKAI_MPV_DLL_COUNT)
if(NOT MIKAI_MPV_DLL_COUNT EQUAL 1)
  message(FATAL_ERROR
    "The verified mpv archive must contain exactly one libmpv-2.dll; found ${MIKAI_MPV_DLL_COUNT}")
endif()
list(GET MIKAI_MPV_DLL_CANDIDATES 0 MIKAI_MPV_DLL)

# The launcher's deadline/kill/scratch contract, verified by running the real
# launcher binary against a disposable child. Unconditional on purpose: the
# launcher and its fixture child are QtCore-only console targets, so a
# probes=OFF configure still exercises KB-26 - and a hung child is exactly the
# case where the probes being off must not also mean the bound being off.
#
# The script drives the launcher through its real positional protocol, so it is
# handed the same staging inputs the mikai_tests entry passes; it owns only the
# scratch parent it creates for itself. Registered here (not earlier) because it
# consumes the verified pinned libmpv path, which is only known by now.
add_test(NAME LauncherContracts
  COMMAND ${CMAKE_COMMAND}
    -DFIXTURE_EXE=$<TARGET_FILE:mikai_launcher_child>
    -DLAUNCHER_EXE=$<TARGET_FILE:mikai_test_launcher>
    -DSHADERS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/shaders
    -DMPV_DLL=${MIKAI_MPV_DLL}
    -DQT_BIN_DIR=$<TARGET_FILE_DIR:Qt6::Core>
    -DQT_PLUGIN_DIR=${MIKAI_TEST_QT_PREFIX}/plugins
    -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/LauncherContracts.cmake
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
# The launcher deadline the scenario uses is 1.5 s, so the entry is internally
# bounded; the outer timeout only has to catch a script that wedges. CTest's
# timeout must stay well above the launcher deadline, for the same reason the
# mikai_tests entry's does.
# The launcher itself links QtCore, so its DLL search path has to be set before
# it starts; the contract script then drives it through the real protocol.
set_tests_properties(LauncherContracts PROPERTIES
  TIMEOUT 180
  ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)

# The dist safety-boundary contract: the populated-output refusal, the source/
# output overlap guards in both directions, the explicit MIKAI_DIST_NATIVE=ON
# missing-runtime failure, and the forbidden-payload sweep at any nesting depth
# - all driven against scratch directories this script owns, never the installed
# dist-mikai output. Unconditional: script-mode CMake needs no compiler, no Qt
# and no windeployqt, so the boundaries are verified whatever else is configured.
set(MIKAI_DIST_CONTRACT_PE_SOURCE "${CMAKE_SOURCE_DIR}/dist-mikai/libwinpthread-1.dll"
  CACHE FILEPATH
  "Genuine read-only x64 PE DLL the DistContracts script re-uses as its archive member")
# Optional. The contract resolves a genuine PE on its own when this is absent
# (the in-tree dist-mikai, then a build/* glob) and then fails loudly with a
# named missing prerequisite rather than substituting a fake module, so passing
# the -D only when the file exists is a preference, not a requirement.
set(dist_contract_pe_args)
if(MIKAI_DIST_CONTRACT_PE_SOURCE AND EXISTS "${MIKAI_DIST_CONTRACT_PE_SOURCE}")
  list(APPEND dist_contract_pe_args
    -DDIST_CONTRACT_PE_SOURCE=${MIKAI_DIST_CONTRACT_PE_SOURCE})
endif()
add_test(NAME DistContracts
  COMMAND ${CMAKE_COMMAND}
    -DDIST_CONTRACT_PROJECT=ON
    -DDIST_CONTRACT_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/dist.cmake
    ${dist_contract_pe_args}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/DistContracts.cmake
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(DistContracts PROPERTIES TIMEOUT 300)

# mikai_backend first: mikai_native_support links it (bundle runtime resolution
# calls mikai::backend helpers), so it must be declared before use.
add_library(mikai_backend STATIC
  src/backend/Models.cpp
  src/backend/Models.hpp
  src/backend/Errors.hpp
  src/backend/Errors.cpp
  src/backend/Cancellation.hpp
  src/backend/Releases.cpp
  src/backend/Releases.hpp
  src/backend/Streams.cpp
  src/backend/Streams.hpp
  src/backend/MikaiClient.cpp
  src/backend/MikaiClient.hpp
  src/backend/Runtime.cpp
  src/backend/Runtime.hpp
  src/backend/Downloads.cpp
  src/backend/Downloads.hpp
  src/backend/Diagnostics.hpp
)
# No AUTOMOC: src/backend declares no Q_OBJECT/Q_GADGET (Cancellation.hpp and
# Diagnostics.hpp are header-only, thread_local state and QtCore-only helpers
# respectively), so the autogen step would scan every header and moc each one
# into nothing.
target_include_directories(mikai_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mikai_backend PUBLIC Qt6::Core Qt6::Network)

add_library(mikai_native_support STATIC
  src/player/native/bundle/NativeBundle.cpp
  src/player/native/bundle/NativeBundle.hpp
  src/player/native/bundle/NativePaths.cpp
  src/player/native/bundle/NativePaths.hpp
  src/player/native/bundle/NativePeImage.cpp
  src/player/native/bundle/NativePeImage.hpp
  src/player/native/bundle/NativeManifest.cpp
  src/player/native/bundle/NativeManifest.hpp
  src/player/native/bundle/NativeRuntimeSelection.cpp
  src/player/native/bundle/NativeRuntimeSelection.hpp
  src/player/native/bundle/NativeCapabilities.cpp
  src/player/native/bundle/NativeCapabilities.hpp
  src/player/native/bundle/NativePrefs.cpp
  src/player/native/bundle/NativePrefs.hpp
  src/player/native/bundle/NativeDiagnostics.cpp
  src/player/native/bundle/NativeDiagnostics.hpp
  src/player/native/models/NativeCatalog.hpp
  src/player/native/models/NativeRecipe.cpp
  src/player/native/models/NativeRecipe.hpp
  src/player/native/models/NativeProfiles.cpp
  src/player/native/models/NativeProfiles.hpp
  src/player/native/models/NativeRife.cpp
  src/player/native/models/NativeRife.hpp
  src/player/native/models/NativeUserRecipes.cpp
  src/player/native/models/NativeUserRecipes.hpp
  src/player/native/models/NativeModelInventory.cpp
  src/player/native/models/NativeModelInventory.hpp
  src/player/native/models/NativeSpatialAssets.cpp
  src/player/native/models/NativeSpatialAssets.hpp
  src/player/native/prepare/NativeBuilder.cpp
  src/player/native/prepare/NativeBuilder.hpp
  src/player/native/prepare/NativePreparationPlan.cpp
  src/player/native/prepare/NativePreparationPlan.hpp
  src/player/native/prepare/NativePreparationReferences.cpp
  src/player/native/prepare/NativePreparationReferences.hpp
  src/player/native/prepare/NativePreparedPlan.cpp
  src/player/native/prepare/NativePreparedPlan.hpp
  src/player/native/prepare/NativePreparationContext.hpp
  src/player/native/prepare/NativeArtifactStore.cpp
  src/player/native/prepare/NativeArtifactStore.hpp
  src/player/native/prepare/NativeWorkLease.cpp
  src/player/native/prepare/NativeWorkLease.hpp
  src/player/native/prepare/NativeFrameFixture.cpp
  src/player/native/prepare/NativeFrameFixture.hpp
  src/player/native/status/NativeStatus.cpp
  src/player/native/status/NativeStatus.hpp
  src/player/native/status/NativePlaybackController.cpp
  src/player/native/status/NativePlaybackController.hpp
  src/player/native/status/NativeMenu.cpp
  src/player/native/status/NativeMenu.hpp
  src/player/native/status/NativeFilterCommand.cpp
  src/player/native/status/NativeFilterCommand.hpp
  src/player/native/status/NativeTypes.hpp
  src/player/native/components/NativeComponentCatalog.cpp
  src/player/native/components/NativeComponentCatalog.hpp
  src/player/native/components/NativeGpuProbe.cpp
  src/player/native/components/NativeGpuProbe.hpp
  src/player/native/components/NativeComponentStore.cpp
  src/player/native/components/NativeComponentStore.hpp
  src/player/native/components/NativeComponentToolchain.cpp
  src/player/native/components/NativeComponentToolchain.hpp
  src/player/native/components/AnimeJanaiRuntime.cpp
  src/player/native/components/AnimeJanaiRuntime.hpp
  src/player/native/components/NativeComponentProvisioning.cpp
  src/player/native/components/NativeComponentProvisioning.hpp
  src/player/native/components/AnimeJanaiInstallation.cpp
)
# Grouped as native/{bundle,models,prepare,status}. Consumers include the
# domain: #include "native/bundle/NativeBundle.hpp".
target_include_directories(mikai_native_support PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/player
  ${MIKAI_MPV_HEADERS}
)
target_link_libraries(mikai_native_support PUBLIC Qt6::Core mikai_backend)
set_target_properties(mikai_native_support PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(mikai_bundle EXCLUDE_FROM_ALL native/tools/bundle_tool.cpp)
target_link_libraries(mikai_bundle PRIVATE mikai_native_support)

option(MIKAI_WITH_MODEL_INSPECTION "Build the owned ONNX model inspector" OFF)
# The native AnimeJaNai shim (M04) is a second toolchain: MSVC + nvcc, configured
# standalone per native/build-msvc.md. It is wired in here only so an MSVC host
# can build it in the same tree; nvcc rejects a MinGW host, so native/ errors
# out clearly if this is ON under the default MinGW/Qt generator. Default OFF:
# the default build touches nothing in native/.
option(MIKAI_WITH_NATIVE "Build optional AnimeJaNai native inference support (requires external source tree + patches)" OFF)
if(MIKAI_WITH_NATIVE)
  add_subdirectory(native)
endif()
set(ORT_INCLUDE_DIR "" CACHE PATH "Directory containing onnxruntime_c_api.h")
if(MIKAI_WITH_MODEL_INSPECTION)
  find_path(MIKAI_ORT_HEADERS onnxruntime_c_api.h HINTS ${ORT_INCLUDE_DIR} REQUIRED)
  add_library(mikai_model_inspection STATIC
    src/player/native/models/NativeModelInspect.cpp
    src/player/native/models/NativeModelInspect.hpp
    src/player/native/prepare/NativeFrameReference.cpp
    src/player/native/prepare/NativeFrameReference.hpp
    src/player/native/models/NativeModelImport.cpp
    src/player/native/models/NativeInspectedPlan.cpp
    src/player/native/models/NativeInspectedPlan.hpp
  )
  target_include_directories(mikai_model_inspection PRIVATE ${MIKAI_ORT_HEADERS})
  target_link_libraries(mikai_model_inspection PUBLIC mikai_native_support)
  add_executable(native_inspect EXCLUDE_FROM_ALL native/tools/native_inspect.cpp)
  target_link_libraries(native_inspect PRIVATE mikai_model_inspection)
endif()

# Player-only translation units. The native-backed units below (runtime selection,
# status/HUD, playback controller, prefs, capabilities, menu) are compiled exactly
# once, inside mikai_native_support, which this target links; listing them here too
# gave them two compilation owners with different release tuning.
set(PLAYER_SOURCES
  src/player/MpvApi.cpp
  src/player/PresetShortcuts.cpp
  src/player/MpvHandle.cpp
  src/player/MpvRenderContext.cpp
  src/player/VideoSurface.cpp
  src/player/PlaybackDiagnostics.cpp
  src/player/Anime4K.cpp
  src/player/ShellTheme.cpp
  src/player/ShellWidgets.cpp
  src/player/ShellSliders.cpp
  src/player/PlayerShell.cpp
  src/player/PlayerPopups.cpp
  src/player/PlayerDialog.cpp
  src/player/CompareWindow.cpp
  src/player/CompareRenderer.cpp
  src/player/CompareCanvas.cpp
  src/player/CompareLayout.cpp
  src/player/ComparePane.cpp
  src/player/CompareGrid.cpp
)

set(PLAYER_HEADERS
  src/player/MpvApi.hpp
  src/player/native/models/NativeCatalog.hpp
  src/player/native/bundle/NativeBundle.hpp
  src/player/native/bundle/NativePaths.hpp
  src/player/native/bundle/NativePeImage.hpp
  src/player/native/bundle/NativeRuntimeSelection.hpp
  src/player/native/status/NativeStatus.hpp
  src/player/native/bundle/NativePrefs.hpp
  src/player/native/bundle/NativeCapabilities.hpp
  src/player/native/status/NativeMenu.hpp
  src/player/native/models/NativeRecipe.hpp
  src/player/native/prepare/NativePreparationPlan.hpp
  src/player/native/bundle/NativeManifest.hpp
  src/player/native/prepare/NativeArtifactStore.hpp
  src/player/MpvHandle.hpp
  src/player/MpvRenderContext.hpp
  src/player/VideoSurface.hpp
  src/player/PlaybackDiagnostics.hpp
  src/player/Anime4K.hpp
  src/player/PresetShortcuts.hpp
  src/player/ShellTheme.hpp
  src/player/ShellWidgets.hpp
  src/player/ShellSliders.hpp
  src/player/PlayerShell.hpp
  src/player/PlayerPopups.hpp
  src/player/PlayerDialog.hpp
  src/player/ProcessingSettingsIntent.hpp
  src/player/CompareWindow.hpp
  src/player/CompareRenderer.hpp
  src/player/CompareCanvas.hpp
  src/player/CompareLayout.hpp
  src/player/ComparePane.hpp
  src/player/CompareGrid.hpp
)

# STATIC so mikai can link it into a shared module without re-exporting it.
add_library(mikai_player STATIC ${PLAYER_SOURCES} ${PLAYER_HEADERS})

set_target_properties(mikai_player PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)

target_include_directories(mikai_player PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/player
  ${MIKAI_MPV_HEADERS}
)

target_link_libraries(mikai_player PUBLIC
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  Qt6::OpenGL
  Qt6::OpenGLWidgets
  opengl32
  gdi32
)

target_link_libraries(mikai_player PUBLIC mikai_backend mikai_native_support)

if(MIKAI_BUILD_PROBES)
# Live smoke: a real Qt window with the VideoSurface, proving mpv actually
# draws through the render bridge rather than through a native HWND.
add_executable(mpv_loader_probe EXCLUDE_FROM_ALL src/probes/native/MpvLoaderProbe.cpp)
target_link_libraries(mpv_loader_probe PRIVATE mikai_player mikai_probe_shared)
set_target_properties(mpv_loader_probe PROPERTIES WIN32_EXECUTABLE FALSE)
add_executable(runtime_selection_probe EXCLUDE_FROM_ALL src/probes/native/RuntimeSelectionProbe.cpp)
# native_support comes first: its selection TU drives the module loader that
# lives in the player, and a static archive is only searched for symbols a later
# archive asked for. Listing the player first would leave those references
# unresolved, which is exactly what happened before this order was fixed.
target_link_libraries(runtime_selection_probe PRIVATE
  mikai_native_support mikai_player mikai_probe_shared)
set_target_properties(runtime_selection_probe PROPERTIES WIN32_EXECUTABLE FALSE)
add_executable(runtime_recovery_probe EXCLUDE_FROM_ALL src/probes/native/RuntimeRecoveryProbe.cpp)
target_link_libraries(runtime_recovery_probe PRIVATE mikai_player mikai_backend mikai_probe_shared)
set_target_properties(runtime_recovery_probe PROPERTIES WIN32_EXECUTABLE FALSE)
add_executable(native_status_probe EXCLUDE_FROM_ALL src/probes/native/NativeStatusProbe.cpp)
target_link_libraries(native_status_probe PRIVATE mikai_player mikai_probe_shared)
set_target_properties(native_status_probe PROPERTIES WIN32_EXECUTABLE FALSE)
add_executable(engine_popup_probe EXCLUDE_FROM_ALL src/probes/player/EnginePopupProbe.cpp)
target_link_libraries(engine_popup_probe PRIVATE mikai_player Qt6::Widgets mikai_probe_shared)
set_target_properties(engine_popup_probe PROPERTIES WIN32_EXECUTABLE FALSE)
add_executable(native_component_probe EXCLUDE_FROM_ALL src/probes/native/NativeComponentProbe.cpp)
target_link_libraries(native_component_probe PRIVATE mikai_player mikai_probe_shared)
set_target_properties(native_component_probe PROPERTIES WIN32_EXECUTABLE FALSE)
if(MIKAI_WITH_MODEL_INSPECTION)
  add_executable(native_builder_probe EXCLUDE_FROM_ALL src/probes/native/NativeBuilderProbe.cpp)
  target_link_libraries(native_builder_probe PRIVATE mikai_model_inspection mikai_probe_shared)
  set_target_properties(native_builder_probe PROPERTIES WIN32_EXECUTABLE FALSE)
endif()
add_executable(native_prep_probe EXCLUDE_FROM_ALL src/probes/native/NativePrepProbe.cpp)
target_link_libraries(native_prep_probe PRIVATE Qt6::Core mikai_probe_shared)
set_target_properties(native_prep_probe PROPERTIES WIN32_EXECUTABLE FALSE)
add_executable(native_verification_probe EXCLUDE_FROM_ALL src/probes/native/NativeVerificationProbe.cpp)
target_link_libraries(native_verification_probe PRIVATE Qt6::Core mikai_probe_shared)
set_target_properties(native_verification_probe PROPERTIES WIN32_EXECUTABLE FALSE)
if(WIN32)
  add_executable(native_dml_interop_probe EXCLUDE_FROM_ALL src/probes/native/NativeDmlInteropProbe.cpp)
  target_link_libraries(native_dml_interop_probe PRIVATE
    Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets d3d11 dxgi opengl32 gdi32 mikai_probe_shared)
  set_target_properties(native_dml_interop_probe PROPERTIES WIN32_EXECUTABLE FALSE)
endif()
add_executable(native_menu_probe EXCLUDE_FROM_ALL src/probes/native/NativeMenuProbe.cpp)
target_link_libraries(native_menu_probe PRIVATE mikai_app Qt6::Test mikai_probe_shared)
set_target_properties(native_menu_probe PROPERTIES WIN32_EXECUTABLE FALSE)
add_executable(native_hud_probe EXCLUDE_FROM_ALL src/probes/native/NativeHudProbe.cpp)
target_link_libraries(native_hud_probe PRIVATE mikai_app Qt6::Widgets mikai_probe_shared)
set_target_properties(native_hud_probe PROPERTIES WIN32_EXECUTABLE FALSE)
add_executable(native_settings_probe EXCLUDE_FROM_ALL src/probes/native/NativeSettingsProbe.cpp)
target_link_libraries(native_settings_probe PRIVATE mikai_app Qt6::Test mikai_probe_shared)
set_target_properties(native_settings_probe PROPERTIES WIN32_EXECUTABLE FALSE)
add_executable(mpv_surface_smoke EXCLUDE_FROM_ALL src/probes/player/mpv_surface_smoke.cpp)
target_link_libraries(mpv_surface_smoke PRIVATE mikai_player Qt6::Widgets mikai_probe_shared)
add_executable(animejanai_probe EXCLUDE_FROM_ALL src/probes/player/AnimeJanaiProbe.cpp)
target_link_libraries(animejanai_probe PRIVATE mikai_player Qt6::Widgets Qt6::Test mikai_probe_shared)
set_target_properties(animejanai_probe PROPERTIES WIN32_EXECUTABLE FALSE)
add_executable(headless_pixel_probe EXCLUDE_FROM_ALL src/probes/player/HeadlessPixelProbe.cpp)
target_link_libraries(headless_pixel_probe PRIVATE mikai_player Qt6::Core Qt6::Gui Qt6::OpenGL mikai_probe_shared)
set_target_properties(headless_pixel_probe PROPERTIES WIN32_EXECUTABLE FALSE)


add_executable(widget_pixel_probe EXCLUDE_FROM_ALL src/probes/player/WidgetPixelProbe.cpp)
target_link_libraries(widget_pixel_probe PRIVATE mikai_player Qt6::Widgets mikai_probe_shared)


add_executable(shell_probe EXCLUDE_FROM_ALL src/probes/player/ShellProbe.cpp)
target_link_libraries(shell_probe PRIVATE mikai_player Qt6::Widgets mikai_probe_shared)
set_target_properties(shell_probe PROPERTIES WIN32_EXECUTABLE FALSE)

add_executable(dialog_probe EXCLUDE_FROM_ALL src/probes/player/DialogProbe.cpp)
target_link_libraries(dialog_probe PRIVATE mikai_player Qt6::Widgets mikai_probe_shared)
set_target_properties(dialog_probe PROPERTIES WIN32_EXECUTABLE FALSE)

add_executable(interaction_probe EXCLUDE_FROM_ALL src/probes/player/InteractionProbe.cpp)
target_link_libraries(interaction_probe PRIVATE mikai_player Qt6::Widgets Qt6::Test mikai_probe_shared)
set_target_properties(interaction_probe PROPERTIES WIN32_EXECUTABLE FALSE)

# Proves the crash flow actually writes a report rather than merely being wired
# up: it installs the same logging the app installs, then faults on purpose.
add_executable(crash_probe EXCLUDE_FROM_ALL src/probes/player/CrashProbe.cpp)
target_link_libraries(crash_probe PRIVATE mikai_app Qt6::Core mikai_probe_shared)
set_target_properties(crash_probe PROPERTIES WIN32_EXECUTABLE FALSE)

# Live title-page probe: builds the real TitlePage against the real API and
# reports what each provider tab received, then what the player did with a pill.
# It exists because "the Ashdi episode list never appears" survived every round
# of source reading, and the only honest way to settle it was to look.
add_executable(compare_shot_probe EXCLUDE_FROM_ALL src/probes/player/CompareShotProbe.cpp)
target_link_libraries(compare_shot_probe PRIVATE mikai_player Qt6::Widgets Qt6::Test mikai_probe_shared)
set_target_properties(compare_shot_probe PROPERTIES WIN32_EXECUTABLE FALSE)

add_executable(compare_probe EXCLUDE_FROM_ALL src/probes/player/CompareProbe.cpp)
target_link_libraries(compare_probe PRIVATE mikai_player Qt6::Widgets Qt6::Test mikai_probe_shared)
set_target_properties(compare_probe PROPERTIES WIN32_EXECUTABLE FALSE)

add_executable(anime4k_menu_probe EXCLUDE_FROM_ALL src/probes/player/Anime4KMenuProbe.cpp)
target_link_libraries(anime4k_menu_probe PRIVATE mikai_player Qt6::Widgets Qt6::Test mikai_probe_shared)
set_target_properties(anime4k_menu_probe PROPERTIES WIN32_EXECUTABLE FALSE)

add_executable(resolver_probe EXCLUDE_FROM_ALL src/probes/native/ResolverProbe.cpp)
target_link_libraries(resolver_probe PRIVATE mikai_player Qt6::Core mikai_probe_shared)
set_target_properties(resolver_probe PROPERTIES WIN32_EXECUTABLE FALSE)
add_executable(title_page_probe EXCLUDE_FROM_ALL src/probes/player/TitlePageProbe.cpp)
target_link_libraries(title_page_probe PRIVATE mikai_app mikai_backend Qt6::Widgets Qt6::Test mikai_probe_shared)
set_target_properties(title_page_probe PROPERTIES WIN32_EXECUTABLE FALSE)
endif() # MIKAI_BUILD_PROBES

# mikai_tests: the C++ replacement for tests/*.py. One executable so ctest has a
# single hermetic command; the runner in tests/cpp/main.cpp drives each suite in
# turn and sums the failures. Everything but the live oracle is hermetic.
#
# The suite links mikai_app for the pager, the quality cache and the clipboard,
# and mikai_backend for the domain. Set MIKAI_LIVE=1 to add the live oracle.
#
# Unlike the probes this stays in the default `all` target, so a plain
# `cmake --build --preset tests` still produces mikai_tests.exe and ctest still
# knows about it. It needs Qt6::Test, hence the MIKAI_BUILD_PROBES guard.
if(MIKAI_BUILD_PROBES)
add_executable(mikai_tests
  tests/cpp/main.cpp
  tests/cpp/TestSupport.hpp
  tests/cpp/TestMpvAsync.hpp
  tests/cpp/TestMpvAsync.cpp
  tests/cpp/TestNativeTypes.hpp
  tests/cpp/TestNativeTypes.cpp
  tests/cpp/TestNativeRecipe.hpp
  tests/cpp/TestNativeRecipe.cpp
  tests/cpp/TestNativeProfiles.hpp
  tests/cpp/TestNativeProfiles.cpp
  tests/cpp/TestNativeRife.hpp
  tests/cpp/TestNativeRife.cpp
  tests/cpp/TestNativeBundle.hpp
  tests/cpp/TestNativeBundle.cpp
  tests/cpp/TestNativePrepPlan.hpp
  tests/cpp/TestNativePrepPlan.cpp
  tests/cpp/TestNativeManifest.hpp
  tests/cpp/TestNativeBuilder.hpp
  tests/cpp/TestNativeBuilder.cpp
  tests/cpp/TestNativePreparationReferences.hpp
  tests/cpp/TestNativePreparationReferences.cpp
  tests/cpp/TestNativeStoredModels.hpp
  tests/cpp/TestNativeStoredModels.cpp
  tests/cpp/TestNativePreparedPlan.hpp
  tests/cpp/TestNativePreparedPlan.cpp
  tests/cpp/TestNativePlaybackController.hpp
  tests/cpp/TestNativePlaybackController.cpp
  tests/cpp/TestNativeManifest.cpp
  tests/cpp/TestRuntimeSelection.hpp
  tests/cpp/TestRuntimeSelection.cpp
  tests/cpp/TestNativeStatus.hpp
  tests/cpp/TestNativeStatus.cpp
  tests/cpp/TestPlaybackDiagnostics.hpp
  tests/cpp/TestPlaybackDiagnostics.cpp
  tests/cpp/TestNativePrefs.hpp
  tests/cpp/TestNativePrefs.cpp
  tests/cpp/TestNativeMenu.hpp
  tests/cpp/TestNativeMenu.cpp
  tests/cpp/TestNativeUserRecipes.hpp
  tests/cpp/TestNativeUserRecipes.cpp
  tests/cpp/TestNativeAssets.hpp
  tests/cpp/TestNativeAssets.cpp
  tests/cpp/TestNativeComponents.hpp
  tests/cpp/TestNativeComponents.cpp
  tests/cpp/TestAnimeJanaiRuntime.hpp
  tests/cpp/TestAnimeJanaiRuntime.cpp
  tests/cpp/TestModels.hpp
  tests/cpp/TestModels.cpp
  tests/cpp/TestReleases.hpp
  tests/cpp/TestReleases.cpp
  tests/cpp/TestAnime4K.hpp
  tests/cpp/TestAnime4K.cpp
  tests/cpp/TestI18n.hpp
  tests/cpp/TestI18n.cpp
  tests/cpp/TestStreams.hpp
  tests/cpp/TestStreams.cpp
  tests/cpp/TestPagers.hpp
  tests/cpp/TestPagers.cpp
  tests/cpp/TestQualityCache.hpp
  tests/cpp/TestQualityCache.cpp
  tests/cpp/TestRateLimiter.hpp
  tests/cpp/TestRateLimiter.cpp
  # Function-backed regression suites (own report capture and verdict), still
  # driven from tests/cpp/main.cpp so ctest keeps one hermetic command.
  tests/cpp/TestDownloads.hpp
  tests/cpp/TestDownloads.cpp
  tests/cpp/TestDiagnostics.hpp
  tests/cpp/TestDiagnostics.cpp
  tests/cpp/TestApiClient.hpp
  tests/cpp/TestApiClient.cpp
  tests/cpp/TestRunner.hpp
  tests/cpp/TestRunner.cpp
  tests/cpp/TestLayering.hpp
  tests/cpp/TestLayering.cpp
  tests/cpp/TestLiveOracle.hpp
  tests/cpp/TestLiveOracle.cpp
)
if(MIKAI_WITH_MODEL_INSPECTION)
  target_sources(mikai_tests PRIVATE
    tests/cpp/TestNativeCatalog.hpp
    tests/cpp/TestNativeCatalog.cpp
  )
  target_link_libraries(mikai_tests PRIVATE mikai_model_inspection)
  target_compile_definitions(mikai_tests PRIVATE MIKAI_WITH_MODEL_INSPECTION=1)
endif()
set_target_properties(mikai_tests PROPERTIES AUTOMOC ON)
target_include_directories(mikai_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# MIKAI_SOURCE_DIR lets the layer guards read the sources they police; without
# the repo root a guard that found no files would pass vacuously.
target_compile_definitions(mikai_tests PRIVATE
  MIKAI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(mikai_tests PRIVATE mikai_player mikai_app mikai_backend Qt6::Test Qt6::Widgets)

# Declared outside the probes guard: it is a QtCore-only console target, and the
# launcher's own deadline/kill/cleanup contract is verified by the unconditional
# LauncherContracts test even when neither the probes nor mikai_tests are built.
add_executable(mikai_test_launcher cmake/MikaiTestLauncher.cpp)
target_link_libraries(mikai_test_launcher PRIVATE Qt6::Core)
if(TARGET mikai_tests)
  # `all` builds the runner before the suite that ctest invokes through it.
  add_dependencies(mikai_tests mikai_test_launcher)
endif()

if(MIKAI_BUILD_PROBES)
  # nativeRoot() is intentionally executable-local in production and in tests.
  # Run a physical copy, never a cwd/environment illusion or the app's output
  # directory: runtime fixtures are allowed to mutate only this owned scratch.
  add_test(NAME mikai_tests COMMAND mikai_test_launcher
    "$<TARGET_FILE:mikai_tests>"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    "${MIKAI_MPV_DLL}"
    "$<TARGET_FILE_DIR:Qt6::Core>"
    "${MIKAI_TEST_QT_PREFIX}/plugins"
    "${CMAKE_CURRENT_BINARY_DIR}/_test-runs"
  )
  # The launcher itself links QtCore, so its DLL search path must be set before
  # it starts; it supplies the same exact Qt runtime/plugin paths to the child.
  #
  # CTest's own deadline must be strictly greater than the launcher's internal
  # one (cmake/MikaiTestLauncher.cpp, default 600000 ms). Ordered this way, a
  # hung suite is attributed to the launcher, which names the deadline in its
  # message and exits nonzero after killing the child and removing its owned
  # scratch. A CTest-side timeout would instead kill the launcher itself,
  # leaving the child and the scratch directory behind.
  set_tests_properties(mikai_tests PROPERTIES
    TIMEOUT 900
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
  )
endif() # the mikai_tests ctest entry needs the probes guard
endif() # MIKAI_BUILD_PROBES


# ---------------------------------------------------------------------------
# The desktop app: a Qt Widgets frontend over mikai_backend, driving the
# existing C++ player in-process. Appended after the player targets so the
# player project still configures and builds on its own.
# ---------------------------------------------------------------------------

if(TARGET mikai_backend)
  set(APP_SOURCES
    src/app/CrashLog.cpp
    src/app/I18n.cpp
    src/app/theme/Theme.cpp
    src/app/theme/Icons.cpp
    src/app/Labels.cpp
    src/app/state/Feeds.cpp
    src/app/state/NativeComponentProvisioner.cpp
    src/app/state/NativePlaybackAssembly.cpp
    src/app/state/QualityCache.cpp
    src/app/workers/Runner.cpp
    src/app/widgets/RangePager.cpp
    src/app/widgets/TitleCard.cpp
    src/app/widgets/RateLimitLabel.cpp
    src/app/dialogs/SettingsDialog.cpp
    src/app/dialogs/ComponentStoreTab.cpp
    src/app/dialogs/NativeAssetsPanel.cpp
    src/app/dialogs/NativeRifeControls.cpp
    src/app/dialogs/CoverViewer.cpp
    src/app/pages/SearchPage.cpp
    src/app/pages/TitlePage.cpp
    src/app/pages/MainWindow.cpp
  )

  set(APP_HEADERS
    src/gui/Clipboard.hpp
    src/app/CrashLog.hpp
    src/app/I18n.hpp
    src/app/Labels.hpp
    src/app/theme/Theme.hpp
    src/app/theme/Colors.hpp
    src/app/theme/Metrics.hpp
    src/app/theme/Icons.hpp
    src/app/state/Feeds.hpp
    src/app/state/NativeComponentProvisioner.hpp
    src/app/state/NativePlaybackAssembly.hpp
    src/app/state/QualityCache.hpp
    src/app/workers/Runner.hpp
    src/app/widgets/RangePager.hpp
    src/app/widgets/TitleCard.hpp
    src/app/widgets/RateLimitLabel.hpp
    src/app/dialogs/SettingsDialog.hpp
    src/app/dialogs/ComponentStoreTab.hpp
    src/app/dialogs/NativeAssetsPanel.hpp
    src/app/dialogs/NativeRifeControls.hpp
    src/app/dialogs/CoverViewer.hpp
    src/app/pages/SearchPage.hpp
    src/app/pages/TitlePage.hpp
    src/app/pages/MainWindow.hpp
  )

  # STATIC so tests can link the pages without re-exporting them.
  add_library(mikai_app STATIC ${APP_SOURCES} ${APP_HEADERS})
  set_target_properties(mikai_app PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)
  target_include_directories(mikai_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
  target_link_libraries(mikai_app PUBLIC
    mikai_backend
    mikai_player
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
  )

  if(MIKAI_WITH_MODEL_INSPECTION)
    target_link_libraries(mikai_app PUBLIC mikai_model_inspection)
    target_compile_definitions(mikai_app PRIVATE MIKAI_WITH_MODEL_INSPECTION=1)
    if(MIKAI_BUILD_PROBES)
      add_executable(native_assets_probe EXCLUDE_FROM_ALL src/probes/native/NativeAssetsProbe.cpp)
      target_link_libraries(native_assets_probe PRIVATE mikai_app Qt6::Test mikai_probe_shared)
      set_target_properties(native_assets_probe PROPERTIES WIN32_EXECUTABLE FALSE)
    endif()
  endif()

  add_executable(mikai src/app/main.cpp)
  set_target_properties(mikai PROPERTIES WIN32_EXECUTABLE TRUE)
  target_link_libraries(mikai PRIVATE mikai_app)

  set(MIKAI_TRANSLATION_TARGETS mikai mikai_app mikai_player mikai_native_support)
  if(TARGET mikai_model_inspection)
    list(APPEND MIKAI_TRANSLATION_TARGETS mikai_model_inspection)
  endif()
  qt_add_translations(mikai
    SOURCE_TARGETS ${MIKAI_TRANSLATION_TARGETS}
    TS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/translations/mikai_uk.ts
    RESOURCE_PREFIX "/i18n"
    LUPDATE_TARGET update_translations
    LUPDATE_OPTIONS -no-obsolete
    LRELEASE_TARGET release_translations
    LRELEASE_OPTIONS -nounfinished
  )
  if(TARGET mikai_tests)
    add_dependencies(mikai_tests release_translations)
    target_compile_definitions(mikai_tests PRIVATE
      MIKAI_UK_CATALOG_PATH="${CMAKE_CURRENT_BINARY_DIR}/mikai_uk.qm"
    )
  endif()

  get_filename_component(MIKAI_QT_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)
  set(MIKAI_QTBASE_UK_CATALOG "${MIKAI_QT_PREFIX}/translations/qtbase_uk.qm")
  if(EXISTS "${MIKAI_QTBASE_UK_CATALOG}")
    add_custom_command(TARGET mikai POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:mikai>/translations"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MIKAI_QTBASE_UK_CATALOG}"
        "$<TARGET_FILE_DIR:mikai>/translations/qtbase_uk.qm"
      COMMENT "Deploying Qt Ukrainian base translations"
    )
  else()
    message(WARNING "Qt Ukrainian base translations not found at ${MIKAI_QTBASE_UK_CATALOG}")
  endif()

  if(MIKAI_BUILD_PROBES)
    add_executable(external_media_probe EXCLUDE_FROM_ALL src/probes/media/ExternalMediaProbe.cpp)
    target_link_libraries(external_media_probe PRIVATE mikai_app Qt6::Test mikai_probe_shared)
    set_target_properties(external_media_probe PROPERTIES WIN32_EXECUTABLE FALSE)

    # The expired-HLS recovery probe. It links the app for the real
    # PlayerDialog/PlayerHost bridge, Runner and QualityCache; it does not need
    # the fixture server target itself, only a loopback base URL at run time.
    add_executable(hls_recovery_probe EXCLUDE_FROM_ALL src/probes/media/HlsRecoveryProbe.cpp)
    target_link_libraries(hls_recovery_probe PRIVATE mikai_app Qt6::Test mikai_probe_shared)
    set_target_properties(hls_recovery_probe PROPERTIES WIN32_EXECUTABLE FALSE)
    add_custom_command(TARGET hls_recovery_probe POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "$<TARGET_FILE_DIR:hls_recovery_probe>/shaders"
      COMMENT "Deploying Anime4K shader fixtures for hls_recovery_probe")

    # The manifest-driven runner: ONE executable that executes declarative JSON
    # probes. New coverage is a manifest, not a new binary.
    add_executable(probe_runner EXCLUDE_FROM_ALL
      src/probes/runner/main.cpp
      src/probes/runner/ProbeRunner.cpp
      src/probes/shared/ProbeManifest.cpp
    )
    target_link_libraries(probe_runner PRIVATE mikai_app Qt6::Test mikai_probe_shared)
    set_target_properties(probe_runner PROPERTIES WIN32_EXECUTABLE FALSE)
    add_custom_command(TARGET probe_runner POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "$<TARGET_FILE_DIR:probe_runner>/shaders"
      COMMENT "Deploying Anime4K shader fixtures for probe_runner")

    add_executable(rife_settings_probe EXCLUDE_FROM_ALL src/probes/native/RifeSettingsProbe.cpp)
    target_link_libraries(rife_settings_probe PRIVATE mikai_app Qt6::Test mikai_probe_shared)
    set_target_properties(rife_settings_probe PROPERTIES WIN32_EXECUTABLE FALSE)

    add_executable(native_playback_assembly_probe EXCLUDE_FROM_ALL src/probes/native/NativePlaybackAssemblyProbe.cpp)
    target_link_libraries(native_playback_assembly_probe PRIVATE mikai_app Qt6::Test mikai_probe_shared)
    set_target_properties(native_playback_assembly_probe PROPERTIES WIN32_EXECUTABLE FALSE)

    add_executable(native_framebuffer_probe EXCLUDE_FROM_ALL src/probes/native/NativeFramebufferProbe.cpp)
    target_link_libraries(native_framebuffer_probe PRIVATE mikai_app mikai_probe_shared)
    set_target_properties(native_framebuffer_probe PROPERTIES WIN32_EXECUTABLE FALSE)

    add_executable(native_geometry_probe EXCLUDE_FROM_ALL src/probes/native/NativeGeometryProbe.cpp)
    target_link_libraries(native_geometry_probe PRIVATE mikai_app Qt6::Test mikai_probe_shared)
    set_target_properties(native_geometry_probe PROPERTIES WIN32_EXECUTABLE FALSE)

    if(MIKAI_WITH_MODEL_INSPECTION)
      add_executable(native_preparation_references_probe EXCLUDE_FROM_ALL src/probes/native/NativePreparationReferencesProbe.cpp)
      target_link_libraries(native_preparation_references_probe PRIVATE mikai_model_inspection mikai_probe_shared)
      set_target_properties(native_preparation_references_probe PROPERTIES WIN32_EXECUTABLE FALSE)

      add_executable(spatial_assets_probe EXCLUDE_FROM_ALL src/probes/native/SpatialAssetsProbe.cpp)
      target_link_libraries(spatial_assets_probe PRIVATE mikai_app Qt6::Test mikai_probe_shared)
      set_target_properties(spatial_assets_probe PROPERTIES WIN32_EXECUTABLE FALSE)
    endif()
  endif()

  # Copy the verified libmpv API DLL beside the app; no process or IPC runtime is used.
  add_custom_command(TARGET mikai POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:mikai>/mpv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${MIKAI_MPV_DLL}" "$<TARGET_FILE_DIR:mikai>/mpv/libmpv-2.dll"
    COMMENT "Deploying the pinned libmpv API DLL"
    VERBATIM
  )

  # Copy Anime4K shaders next to the executable so libmpv can find them at runtime.
  add_custom_command(TARGET mikai POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_CURRENT_SOURCE_DIR}/shaders
      $<TARGET_FILE_DIR:mikai>/shaders
    COMMENT "Deploying Anime4K shaders to output directory"
  )

  # The self-contained dist folder: the app, its Qt runtime, the shaders, the
  # translations, and the pinned AnimeJaNai-compatible libmpv API DLL.
  option(MIKAI_DIST_NATIVE "Copy the installed Native bundle into the dist folder" OFF)
  # The assembly is refused if this directory already contains anything: the
  # installer updates an installed app in place, so a populate-and-wipe default
  # would destroy the user's installed state. Point it at scratch space for a
  # verification run.
  set(MIKAI_DIST_DIR "${CMAKE_SOURCE_DIR}/dist-mikai" CACHE PATH
    "Where the deploy target assembles the app (must not already exist)")
  # Production nativeRoot() is executable-local, so the dist script derives the
  # same root from the staged executable. This override exists for the case
  # where the store genuinely lives elsewhere; an empty value means "use the
  # executable-local store", which is what the packaged app resolves.
  set(MIKAI_DIST_NATIVE_ROOT "" CACHE PATH
    "Override the executable-local native store that MIKAI_DIST_NATIVE copies "
    "(empty: <EXE dir>/native, matching production nativeRoot())")
  get_target_property(MIKAI_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
  if(MIKAI_QMAKE_EXECUTABLE)
    get_filename_component(MIKAI_QT_BIN_DIR "${MIKAI_QMAKE_EXECUTABLE}" DIRECTORY)
    set(MIKAI_WINDEPLOYQT "${MIKAI_QT_BIN_DIR}/windeployqt.exe")
  endif()
  add_custom_target(dist
    COMMAND ${CMAKE_COMMAND}
      -DDIST_DIR=${MIKAI_DIST_DIR}
      -DEXE=$<TARGET_FILE:mikai>
      -DWINDEPLOYQT=${MIKAI_WINDEPLOYQT}
      -DSHADERS=${CMAKE_CURRENT_SOURCE_DIR}/shaders
      -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -DMIKAI_DIST_NATIVE=${MIKAI_DIST_NATIVE}
      "-DMIKAI_DIST_NATIVE_ROOT=${MIKAI_DIST_NATIVE_ROOT}"
      "-DMIKAI_DIST_MPV_ARCHIVE=${MIKAI_PINNED_MPV_ARCHIVE}"
      "-DMIKAI_DIST_CACHE_DIR=${MIKAI_DIST_MPV_CACHE_DIR}"
      "-DMIKAI_MPV_ARCHIVE_NAME=${MIKAI_MPV_ARCHIVE_NAME}"
      "-DMIKAI_MPV_ARCHIVE_URL=${MIKAI_MPV_ARCHIVE_URL}"
      "-DMIKAI_MPV_ARCHIVE_SHA256=${MIKAI_MPV_ARCHIVE_SHA256}"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dist.cmake
    DEPENDS mikai
    COMMENT "Assembling the self-contained dist folder")
else()
  message(STATUS "mikai_backend not present: skipping the mikai app targets")
endif()

# ---------------------------------------------------------------------------
# Release-only tuning for the four targets that make up the shipped app.
# Generator expressions keep this inert for Debug and for any non-GNU/Clang
# toolchain, so the build commands in readme.md/AGENTS.md need no extra flags.
#
# The baseline is x86-64 + SSE2, which is what "any Windows x64 box" means: the
# Microsoft x64 ABI defines SSE2, and it is the wholesale floor for the
# architecture. x86-64-v2 (SSE4.2/POPCNT) is NOT implied by x64 and would
# exclude older-but-x64 CPUs, so it is deliberately not requested. There is no
# native/v2-only tuning claim here at all: -march=native and -march=x86-64-v2
# would both be stronger than the advertised baseline.
# -ffunction-sections/-fdata-sections plus -Wl,--gc-sections drop the player and
# compare code that is unreachable from the home path. GCC 16 MinGW accepts all
# four. The link option belongs only on the executable, not on the archives.
# IPO lets the three archives inline across each other; they are header-coupled
# and all PIC, which is what LTO needs.
# ---------------------------------------------------------------------------
# LTO is only requested where the toolchain actually has it. Some MinGW GCC
# builds ship a cc1plus with LTO compiled out, and CMake's IPO flags are
# unconditional, so setting the property blind turns every build into a wall of
# "LTO support has not been enabled in this configuration". Probed once per
# configure, cached afterwards.
set(MIKAI_IPO OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT MIKAI_IPO_OUTPUT LANGUAGES CXX)
  if(MIKAI_IPO_OUTPUT)
    set(MIKAI_IPO ON)
  else()
    message(STATUS "LTO unsupported by this toolchain: building Release without IPO")
  endif()
endif()

# Unconditional: tests/fixtures defines the disposable child the launcher
# contract test drives (mikai_launcher_child) and the hermetic media/HLS fixture
# server. Neither belongs to the probe set only, so a probes=OFF configure still
# gets them.
add_subdirectory(tests/fixtures)

if(MIKAI_BUILD_PROBES)
  foreach(target IN ITEMS mikai_tests mpv_surface_smoke animejanai_probe headless_pixel_probe
      widget_pixel_probe shell_probe dialog_probe interaction_probe compare_probe
      compare_shot_probe anime4k_menu_probe resolver_probe title_page_probe external_media_probe)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "$<TARGET_FILE_DIR:${target}>/shaders"
      COMMENT "Deploying Anime4K shader fixtures for ${target}"
    )
  endforeach()
endif()

mikai_deploy_mingw_runtime_targets()

set(MIKAI_TUNED_TARGETS mikai_player mikai_backend mikai_app mikai)
foreach(t IN LISTS MIKAI_TUNED_TARGETS)
  if(TARGET ${t})
    if(MIKAI_IPO)
      set_property(TARGET ${t} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
    target_compile_options(${t} PRIVATE
      $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>>:-march=x86-64>
      $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>>:-ffunction-sections>
      $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>>:-fdata-sections>
    )
  endif()
endforeach()
if(TARGET mikai)
  target_link_options(mikai PRIVATE
    $<$<AND:$<CONFIG:Release>,$<LINK_LANG_AND_ID:CXX,GNU,Clang>>:-Wl,--gc-sections>
  )
endif()
