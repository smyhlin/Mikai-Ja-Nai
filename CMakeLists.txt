# Schema version 6 in CMakePresets.json requires CMake >= 3.25.
cmake_minimum_required(VERSION 3.25)
project(mikai LANGUAGES CXX)

if(WIN32)
  enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type)
endif()

add_compile_definitions(
  QT_DISABLE_DEPRECATED_UP_TO=0x060C00
  QT_WARN_DEPRECATED_UP_TO=0x060D00
)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/MinGWRuntime.cmake)

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Network LinguistTools)
get_filename_component(MIKAI_TEST_QT_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)

option(MIKAI_BUILD_PROBES "Build the QA probes and mikai_tests" ON)
option(MIKAI_WITH_MODEL_INSPECTION "Build the owned ONNX model inspector" OFF)
option(MIKAI_WITH_NATIVE "Build optional AnimeJaNai native inference support (requires external source tree + patches)" OFF)

enable_testing()
if(MIKAI_BUILD_PROBES)
  find_package(Qt6 REQUIRED COMPONENTS Test)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindOrFetchMpv.cmake)

# Out-of-band manifest verifier CLI
add_executable(mikai_manifest_check EXCLUDE_FROM_ALL native/tools/manifest_check.cpp)
target_link_libraries(mikai_manifest_check PRIVATE Qt6::Core)

# ---------------------------------------------------------------------------
# 1. mikai_backend — Pure Domain & Network Layer (Qt6::Core + Qt6::Network)
# ---------------------------------------------------------------------------
add_library(mikai_backend STATIC
  src/backend/Cancellation.hpp
  src/backend/Diagnostics.hpp
  src/backend/Downloads.cpp
  src/backend/Downloads.hpp
  src/backend/Errors.cpp
  src/backend/Errors.hpp
  src/backend/MikaiClient.cpp
  src/backend/MikaiClient.hpp
  src/backend/Models.cpp
  src/backend/Models.hpp
  src/backend/Releases.cpp
  src/backend/Releases.hpp
  src/backend/UpdateFeed.cpp
  src/backend/UpdateFeed.hpp
  src/backend/Runtime.cpp
  src/backend/Runtime.hpp
  src/backend/Streams.cpp
  src/backend/Streams.hpp
)
target_include_directories(mikai_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mikai_backend PUBLIC Qt6::Core Qt6::Network)

# ---------------------------------------------------------------------------
# 2. mikai_native_support — Core libmpv Transport & Native Support (GUI-free)
#    Owns MpvApi, MpvHandle, and PlaybackDiagnostics alongside src/player/native
#    so mikai_native_support is 100% self-contained with no circular link back
#    into mikai_player.
# ---------------------------------------------------------------------------
add_library(mikai_native_support STATIC
  # Core GUI-free libmpv loader, async handle & diagnostics
  src/player/core/MpvApi.cpp
  src/player/core/MpvApi.hpp
  src/player/core/MpvHandle.cpp
  src/player/core/MpvHandle.hpp
  src/player/core/MediaTracks.cpp
  src/player/core/MediaTracks.hpp
  src/player/core/PlaybackDiagnostics.cpp
  src/player/core/PlaybackDiagnostics.hpp
  # Native bundle discovery & runtime selection
  src/player/native/bundle/NativeBundle.cpp
  src/player/native/bundle/NativeBundle.hpp
  src/player/native/bundle/NativeCapabilities.cpp
  src/player/native/bundle/NativeCapabilities.hpp
  src/player/native/bundle/NativeDiagnostics.cpp
  src/player/native/bundle/NativeDiagnostics.hpp
  src/player/native/bundle/NativeManifest.cpp
  src/player/native/bundle/NativeManifest.hpp
  src/player/native/bundle/NativePaths.cpp
  src/player/native/bundle/NativePaths.hpp
  src/player/native/bundle/NativePeImage.cpp
  src/player/native/bundle/NativePeImage.hpp
  src/player/native/bundle/NativePrefs.cpp
  src/player/native/bundle/NativePrefs.hpp
  src/player/native/bundle/NativeRuntimeSelection.cpp
  src/player/native/bundle/NativeRuntimeSelection.hpp
  # Native component store, GPU NVML probe & AnimeJaNai runtime
  src/player/native/components/AnimeJanaiBuildLog.hpp
  src/player/native/components/AnimeJanaiCatalogWatcher.cpp
  src/player/native/components/AnimeJanaiCatalogWatcher.hpp
  src/player/native/components/AnimeJanaiInstallation.cpp
  src/player/native/components/AnimeJanaiRuntime.cpp
  src/player/native/components/AnimeJanaiRuntime.hpp
  src/player/native/components/AnimeJanaiConfig.cpp
  src/player/native/components/AnimeJanaiConfig.hpp
  src/player/native/components/NativeComponentCatalog.cpp
  src/player/native/components/NativeComponentCatalog.hpp
  src/player/native/components/NativeComponentProvisioning.cpp
  src/player/native/components/NativeComponentProvisioning.hpp
  src/player/native/components/NativeComponentStore.cpp
  src/player/native/components/NativeComponentStore.hpp
  src/player/native/components/NativeComponentToolchain.cpp
  src/player/native/components/NativeComponentToolchain.hpp
  src/player/native/components/NativeGpuProbe.cpp
  src/player/native/components/NativeGpuProbe.hpp
  # Native model inventory, profiles & recipes
  src/player/native/models/NativeCatalog.hpp
  src/player/native/models/NativeModelInventory.cpp
  src/player/native/models/NativeModelInventory.hpp
  src/player/native/models/NativeProfiles.cpp
  src/player/native/models/NativeProfiles.hpp
  src/player/native/models/NativeRecipe.cpp
  src/player/native/models/NativeRecipe.hpp
  src/player/native/models/NativeRife.cpp
  src/player/native/models/NativeRife.hpp
  src/player/native/models/NativeSpatialAssets.cpp
  src/player/native/models/NativeSpatialAssets.hpp
  src/player/native/models/NativeUserRecipes.cpp
  src/player/native/models/NativeUserRecipes.hpp
  # Native preparation & artifact builder
  src/player/native/prepare/NativeArtifactStore.cpp
  src/player/native/prepare/NativeArtifactStore.hpp
  src/player/native/prepare/NativeBuilder.cpp
  src/player/native/prepare/NativeBuilder.hpp
  src/player/native/prepare/NativeBuilderInternal.hpp
  src/player/native/prepare/NativeBuilderTools.cpp
  src/player/native/prepare/NativeFrameFixture.cpp
  src/player/native/prepare/NativeFrameFixture.hpp
  src/player/native/prepare/NativePreparationContext.hpp
  src/player/native/prepare/NativePreparationPlan.cpp
  src/player/native/prepare/NativePreparationPlan.hpp
  src/player/native/prepare/NativePreparationReferences.cpp
  src/player/native/prepare/NativePreparationReferences.hpp
  src/player/native/prepare/NativePreparedPlan.cpp
  src/player/native/prepare/NativePreparedPlan.hpp
  src/player/native/prepare/NativeWorkLease.cpp
  src/player/native/prepare/NativeWorkLease.hpp
  # Native status wire codec, consumer & playback controller
  src/player/native/status/NativeFilterCommand.cpp
  src/player/native/status/NativeFilterCommand.hpp
  src/player/native/status/NativeMenu.cpp
  src/player/native/status/NativeMenu.hpp
  src/player/native/status/NativePlaybackController.cpp
  src/player/native/status/NativePlaybackController.hpp
  src/player/native/status/NativeStatus.cpp
  src/player/native/status/NativeStatus.hpp
  src/player/native/status/NativeStatusConsumer.cpp
  src/player/native/status/NativeStatusInternal.hpp
  src/player/native/status/NativeTypes.hpp
)
target_include_directories(mikai_native_support PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/player
  ${CMAKE_CURRENT_SOURCE_DIR}/src/player/core
  ${MIKAI_MPV_HEADERS}
)
target_link_libraries(mikai_native_support PUBLIC Qt6::Core mikai_backend ${CMAKE_DL_LIBS})
set_target_properties(mikai_native_support PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC ON)

add_executable(mikai_bundle EXCLUDE_FROM_ALL native/tools/bundle_tool.cpp)
target_link_libraries(mikai_bundle PRIVATE mikai_native_support)

if(MIKAI_WITH_NATIVE)
  add_subdirectory(native)
endif()

set(ORT_INCLUDE_DIR "" CACHE PATH "Directory containing onnxruntime_c_api.h")
if(MIKAI_WITH_MODEL_INSPECTION)
  find_path(MIKAI_ORT_HEADERS onnxruntime_c_api.h HINTS ${ORT_INCLUDE_DIR} REQUIRED)
  add_library(mikai_model_inspection STATIC
    src/player/native/models/NativeInspectedPlan.cpp
    src/player/native/models/NativeInspectedPlan.hpp
    src/player/native/models/NativeModelImport.cpp
    src/player/native/models/NativeModelInspect.cpp
    src/player/native/models/NativeModelInspect.hpp
    src/player/native/prepare/NativeFrameReference.cpp
    src/player/native/prepare/NativeFrameReference.hpp
  )
  target_include_directories(mikai_model_inspection PRIVATE ${MIKAI_ORT_HEADERS})
  target_link_libraries(mikai_model_inspection PUBLIC mikai_native_support)
  add_executable(native_inspect EXCLUDE_FROM_ALL native/tools/native_inspect.cpp)
  target_link_libraries(native_inspect PRIVATE mikai_model_inspection)
endif()

# ---------------------------------------------------------------------------
# 3. mikai_player — OpenGL VideoSurface, Shaders, Shell Chrome & Compare UI
# ---------------------------------------------------------------------------
set(PLAYER_SOURCES
  # OpenGL render bridge & video surface
  src/player/core/MpvRenderContext.cpp
  src/player/core/VideoSurface.cpp
  src/player/core/VideoSurfaceAnimeJanai.cpp
  src/player/core/VideoSurfaceNative.cpp
  # Shader presets & shortcuts
  src/player/shaders/Anime4K.cpp
  src/player/shaders/PresetShortcuts.cpp
  # Player shell, popups, PiP float & dialog
  src/player/shell/PipFloat.cpp
  src/player/shell/PlayerDialog.cpp
  src/player/shell/PlayerDialogProcessing.cpp
  src/player/shell/PlayerPopups.cpp
  src/player/shell/PlayerShell.cpp
  src/player/shell/ShellSliders.cpp
  src/player/shell/ShellTheme.cpp
  src/player/shell/ShellWidgets.cpp
  # Visual Compare Workstation
  src/player/compare/CompareCanvas.cpp
  src/player/compare/CompareGrid.cpp
  src/player/compare/CompareGif.cpp
  src/player/compare/CompareLayout.cpp
  src/player/compare/ComparePane.cpp
  src/player/compare/CompareRenderer.cpp
  src/player/compare/CompareWindow.cpp
)

set(PLAYER_HEADERS
  src/player/core/MpvRenderContext.hpp
  src/player/core/VideoSurface.hpp
  src/player/shaders/Anime4K.hpp
  src/player/shaders/PresetShortcuts.hpp
  src/player/shell/PipFloat.hpp
  src/player/shell/PlayerDialog.hpp
  src/player/shell/PlayerPopups.hpp
  src/player/shell/PlayerShell.hpp
  src/player/shell/ProcessingSettingsIntent.hpp
  src/player/shell/ShellSliders.hpp
  src/player/shell/ShellTheme.hpp
  src/player/shell/ShellWidgets.hpp
  src/player/compare/CompareCanvas.hpp
  src/player/compare/CompareGrid.hpp
  src/player/compare/CompareGif.hpp
  src/player/compare/CompareLayout.hpp
  src/player/compare/ComparePane.hpp
  src/player/compare/CompareRenderer.hpp
  src/player/compare/CompareWindow.hpp
)

add_library(mikai_player STATIC ${PLAYER_SOURCES} ${PLAYER_HEADERS})
set_target_properties(mikai_player PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)

target_include_directories(mikai_player PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/player
  ${CMAKE_CURRENT_SOURCE_DIR}/src/player/core
  ${CMAKE_CURRENT_SOURCE_DIR}/src/player/shaders
  ${CMAKE_CURRENT_SOURCE_DIR}/src/player/shell
  ${CMAKE_CURRENT_SOURCE_DIR}/src/player/compare
  ${MIKAI_MPV_HEADERS}
)

if(WIN32)
  set(MIKAI_PLATFORM_GL_LIBS opengl32 gdi32)
else()
  find_package(OpenGL REQUIRED)
  set(MIKAI_PLATFORM_GL_LIBS OpenGL::GL ${CMAKE_DL_LIBS})
endif()

target_link_libraries(mikai_player PUBLIC
  mikai_backend
  mikai_native_support
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  Qt6::OpenGL
  Qt6::OpenGLWidgets
  ${MIKAI_PLATFORM_GL_LIBS}
)

# ---------------------------------------------------------------------------
# 4. mikai_app & mikai — Desktop Client Pages, Dialogs, State & Workers
# ---------------------------------------------------------------------------
set(APP_SOURCES
  src/gui/ImageDecode.cpp
  src/app/support/CrashLog.cpp
  src/app/support/I18n.cpp
  src/app/support/Labels.cpp
  src/app/support/WindowRaisePolicy.cpp
  src/app/theme/Appearance.cpp
  src/app/theme/Icons.cpp
  src/app/theme/Palette.cpp
  src/app/theme/Theme.cpp
  src/app/state/AppearanceStore.cpp
  src/app/state/Feeds.cpp
  src/app/state/NativeComponentProvisioner.cpp
  src/app/state/NativePlaybackAssembly.cpp
  src/app/state/QualityCache.cpp
  src/app/state/WatchHistory.cpp
  src/app/workers/Runner.cpp
  src/app/widgets/FailureView.cpp
  src/app/widgets/RangePager.cpp
  src/app/widgets/RateLimitLabel.cpp
  src/app/widgets/PageGlow.cpp
  src/app/widgets/SwatchButton.cpp
  src/app/widgets/TitleCard.cpp
  src/app/widgets/ToggleSwitch.cpp
  src/app/dialogs/ComponentStoreTab.cpp
  src/app/dialogs/ComponentStoreTabAcquisition.cpp
  src/app/dialogs/CoverViewer.cpp
  src/app/dialogs/NativeAssetsPanel.cpp
  src/app/dialogs/NativeAssetsPanelWorkflow.cpp
  src/app/dialogs/NativeRifeControls.cpp
  src/app/dialogs/SettingsDialog.cpp
  src/app/dialogs/AnimeJanaiManagerPanel.cpp
  src/app/dialogs/settings/BenchmarkPlayback.cpp
  src/app/dialogs/settings/BenchmarkReport.cpp
  src/app/dialogs/settings/SettingsBenchmarkTab.cpp
  src/app/dialogs/settings/SettingsComponentsTab.cpp
  src/app/dialogs/settings/SettingsCustomizationTab.cpp
  src/app/dialogs/settings/SettingsGeneralTab.cpp
  src/app/dialogs/settings/SettingsHelpers.cpp
  src/app/dialogs/settings/SettingsPresetsTab.cpp
  src/app/dialogs/settings/SettingsProcessingTab.cpp
  src/app/pages/MainWindow.cpp
  src/app/pages/SearchPage.cpp
  src/app/pages/TitlePage.cpp
  src/app/pages/TitlePagePlayerSession.cpp
)

set(APP_HEADERS
  src/gui/Clipboard.hpp
  src/gui/ImageDecode.hpp
  src/app/support/CrashLog.hpp
  src/app/support/I18n.hpp
  src/app/support/Labels.hpp
  src/app/support/WindowRaisePolicy.hpp
  src/app/theme/Appearance.hpp
  src/app/theme/Colors.hpp
  src/app/theme/Icons.hpp
  src/app/theme/Palette.hpp
  src/app/theme/Metrics.hpp
  src/app/theme/Theme.hpp
  src/app/state/AppearanceStore.hpp
  src/app/state/Feeds.hpp
  src/app/state/NativeComponentProvisioner.hpp
  src/app/state/NativePlaybackAssembly.hpp
  src/app/state/QualityCache.hpp
  src/app/state/WatchHistory.hpp
  src/app/workers/Runner.hpp
  src/app/widgets/FailureView.hpp
  src/app/widgets/FlowLayout.hpp
  src/app/widgets/RangePager.hpp
  src/app/widgets/RateLimitLabel.hpp
  src/app/widgets/PageGlow.hpp
  src/app/widgets/SwatchButton.hpp
  src/app/widgets/TitleCard.hpp
  src/app/widgets/ToggleSwitch.hpp
  src/app/dialogs/ComponentStoreTab.hpp
  src/app/dialogs/ComponentStoreTabInternal.hpp
  src/app/dialogs/CoverViewer.hpp
  src/app/dialogs/NativeAssetsPanel.hpp
  src/app/dialogs/NativeRifeControls.hpp
  src/app/dialogs/SettingsDialog.hpp
  src/app/dialogs/AnimeJanaiManagerPanel.hpp
  src/app/dialogs/settings/BenchmarkPlayback.hpp
  src/app/dialogs/settings/BenchmarkReport.hpp
  src/app/dialogs/settings/SettingsHelpers.hpp
  src/app/dialogs/settings/SettingsUi.hpp
  src/app/pages/EpisodeGrid.hpp
  src/app/pages/MainWindow.hpp
  src/app/pages/SearchPage.hpp
  src/app/pages/TitlePage.hpp
)

add_library(mikai_app STATIC ${APP_SOURCES} ${APP_HEADERS})
qt_add_resources(mikai_app "mikai_assets"
  PREFIX "/icons"
  BASE resources
  FILES
    resources/chevron-down.svg
)
set_target_properties(mikai_app PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)
target_include_directories(mikai_app PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/app
  ${CMAKE_CURRENT_SOURCE_DIR}/src/app/support
)
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
endif()

qt_add_executable(mikai src/app/main.cpp)
target_link_libraries(mikai PRIVATE mikai_app)
set_target_properties(mikai PROPERTIES WIN32_EXECUTABLE TRUE)

if(WIN32)
  set(mikai_manifest "${CMAKE_CURRENT_BINARY_DIR}/mikai.exe.manifest")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/mikai.exe.manifest.in"
                 "${mikai_manifest}" @ONLY)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/mikai_manifest.rc.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/mikai_manifest.rc" @ONLY)
  target_sources(mikai PRIVATE "${mikai_manifest}"
                               "${CMAKE_CURRENT_BINARY_DIR}/mikai_manifest.rc")
  set_source_files_properties("${mikai_manifest}"
                              "${CMAKE_CURRENT_BINARY_DIR}/mikai_manifest.rc"
                              PROPERTIES GENERATED TRUE)
endif()

set(MIKAI_TRANSLATION_TARGETS mikai mikai_app mikai_backend mikai_player mikai_native_support)
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

if(MIKAI_MPV_DLL AND EXISTS "${MIKAI_MPV_DLL}")
  get_filename_component(MIKAI_MPV_DEPLOY_NAME "${MIKAI_MPV_DLL}" NAME)
  if(WIN32)
    set(MIKAI_MPV_DEPLOY_NAME "libmpv-2.dll")
  endif()
  add_custom_command(TARGET mikai POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:mikai>/mpv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${MIKAI_MPV_DLL}" "$<TARGET_FILE_DIR:mikai>/mpv/${MIKAI_MPV_DEPLOY_NAME}"
    COMMENT "Deploying the libmpv runtime library"
    VERBATIM
  )
endif()

add_custom_command(TARGET mikai POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders
    $<TARGET_FILE_DIR:mikai>/shaders
  COMMENT "Deploying Anime4K shaders to output directory"
)

# ---------------------------------------------------------------------------
# 5. Distribution, Probes, Tests & Release Tuning
# ---------------------------------------------------------------------------
option(MIKAI_DIST_NATIVE "Copy the installed Native bundle into the dist folder" OFF)
set(MIKAI_DIST_DIR "${CMAKE_SOURCE_DIR}/dist-mikai" CACHE PATH
  "Where the deploy target assembles the app (must not already exist)")
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
  COMMENT "Assembling the self-contained dist folder"
)

add_subdirectory(tests/fixtures)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/MikaiProbes.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/MikaiTests.cmake)

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

mikai_deploy_mingw_runtime_targets()

set(MIKAI_TUNED_TARGETS mikai_native_support mikai_player mikai_backend mikai_app mikai)
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
