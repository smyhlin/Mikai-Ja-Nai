# Standalone GUI, headless, media, and native QA probe executables.
# Included when MIKAI_BUILD_PROBES=ON.

if(NOT MIKAI_BUILD_PROBES)
  return()
endif()

add_library(mikai_probe_shared INTERFACE)
target_include_directories(mikai_probe_shared INTERFACE
  ${CMAKE_SOURCE_DIR}/src/probes/shared
  ${CMAKE_SOURCE_DIR}/tests/fixtures
)

function(mikai_add_probe target_name source_file)
  set(options DEPLOY_SHADERS GUI_EXECUTABLE)
  set(oneValueArgs)
  set(multiValueArgs LINKS EXTRA_SOURCES)
  cmake_parse_arguments(PROBE "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  add_executable(${target_name} EXCLUDE_FROM_ALL ${source_file} ${PROBE_EXTRA_SOURCES})
  target_link_libraries(${target_name} PRIVATE ${PROBE_LINKS} mikai_probe_shared)
  if(NOT PROBE_GUI_EXECUTABLE)
    set_target_properties(${target_name} PROPERTIES WIN32_EXECUTABLE FALSE)
  endif()
  if(PROBE_DEPLOY_SHADERS)
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/shaders"
        "$<TARGET_FILE_DIR:${target_name}>/shaders"
      COMMENT "Deploying Anime4K shader fixtures for ${target_name}"
    )
  endif()
endfunction()

# --- Native & runtime probes ---
mikai_add_probe(mpv_loader_probe src/probes/native/MpvLoaderProbe.cpp
  LINKS mikai_player)
mikai_add_probe(runtime_selection_probe src/probes/native/RuntimeSelectionProbe.cpp
  LINKS mikai_native_support mikai_player)
mikai_add_probe(runtime_recovery_probe src/probes/native/RuntimeRecoveryProbe.cpp
  LINKS mikai_player mikai_backend)
mikai_add_probe(native_status_probe src/probes/native/NativeStatusProbe.cpp
  LINKS mikai_player)
mikai_add_probe(native_component_probe src/probes/native/NativeComponentProbe.cpp
  LINKS mikai_player)
mikai_add_probe(native_prep_probe src/probes/native/NativePrepProbe.cpp
  LINKS Qt6::Core)
mikai_add_probe(native_verification_probe src/probes/native/NativeVerificationProbe.cpp
  LINKS Qt6::Core)
mikai_add_probe(resolver_probe src/probes/native/ResolverProbe.cpp
  LINKS mikai_player Qt6::Core
  DEPLOY_SHADERS)

if(WIN32)
  mikai_add_probe(native_dml_interop_probe src/probes/native/NativeDmlInteropProbe.cpp
    LINKS Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets d3d11 dxgi opengl32 gdi32)
endif()

if(MIKAI_WITH_MODEL_INSPECTION)
  mikai_add_probe(native_builder_probe src/probes/native/NativeBuilderProbe.cpp
    LINKS mikai_model_inspection)
endif()

# --- Player & surface probes ---
mikai_add_probe(engine_popup_probe src/probes/player/EnginePopupProbe.cpp
  LINKS mikai_player Qt6::Widgets)
mikai_add_probe(mpv_surface_smoke src/probes/player/mpv_surface_smoke.cpp
  LINKS mikai_player Qt6::Widgets
  GUI_EXECUTABLE DEPLOY_SHADERS)
mikai_add_probe(animejanai_probe src/probes/player/AnimeJanaiProbe.cpp
  LINKS mikai_player Qt6::Widgets Qt6::Test
  DEPLOY_SHADERS)
mikai_add_probe(headless_pixel_probe src/probes/player/HeadlessPixelProbe.cpp
  LINKS mikai_player Qt6::Core Qt6::Gui Qt6::OpenGL
  DEPLOY_SHADERS)
mikai_add_probe(widget_pixel_probe src/probes/player/WidgetPixelProbe.cpp
  LINKS mikai_player Qt6::Widgets
  GUI_EXECUTABLE DEPLOY_SHADERS)
mikai_add_probe(shell_probe src/probes/player/ShellProbe.cpp
  LINKS mikai_player Qt6::Widgets
  DEPLOY_SHADERS)
mikai_add_probe(dialog_probe src/probes/player/DialogProbe.cpp
  LINKS mikai_player Qt6::Widgets
  DEPLOY_SHADERS)
mikai_add_probe(fullscreen_stability_probe src/probes/player/FullscreenStabilityProbe.cpp
  LINKS mikai_player Qt6::Widgets Qt6::Test
  DEPLOY_SHADERS)
if(WIN32)
  target_link_libraries(fullscreen_stability_probe PRIVATE psapi)
endif()
mikai_add_probe(interaction_probe src/probes/player/InteractionProbe.cpp
  LINKS mikai_player Qt6::Widgets Qt6::Test
  DEPLOY_SHADERS)
mikai_add_probe(compare_shot_probe src/probes/player/CompareShotProbe.cpp
  LINKS mikai_player Qt6::Widgets Qt6::Test
  DEPLOY_SHADERS)
mikai_add_probe(compare_probe src/probes/player/CompareProbe.cpp
  LINKS mikai_player Qt6::Widgets Qt6::Test
  DEPLOY_SHADERS)
mikai_add_probe(compare_export_probe src/probes/player/CompareExportProbe.cpp
  LINKS mikai_player Qt6::Widgets Qt6::Test
  DEPLOY_SHADERS)
mikai_add_probe(anime4k_menu_probe src/probes/player/Anime4KMenuProbe.cpp
  LINKS mikai_player Qt6::Widgets Qt6::Test
  DEPLOY_SHADERS)

# --- App-linked probes ---
if(TARGET mikai_app)
  mikai_add_probe(native_menu_probe src/probes/native/NativeMenuProbe.cpp
    LINKS mikai_app Qt6::Test)
  mikai_add_probe(native_hud_probe src/probes/native/NativeHudProbe.cpp
    LINKS mikai_app Qt6::Widgets)
  mikai_add_probe(native_settings_probe src/probes/native/NativeSettingsProbe.cpp
    EXTRA_SOURCES tests/fixtures/IdentityOnnxFixture.hpp
    LINKS mikai_app Qt6::Test)
  mikai_add_probe(crash_probe src/probes/player/CrashProbe.cpp
    LINKS mikai_app Qt6::Core)
  mikai_add_probe(title_page_probe src/probes/player/TitlePageProbe.cpp
    LINKS mikai_app mikai_backend Qt6::Widgets Qt6::Test
    DEPLOY_SHADERS)
  mikai_add_probe(external_media_probe src/probes/media/ExternalMediaProbe.cpp
    LINKS mikai_app Qt6::Test
    DEPLOY_SHADERS)
  mikai_add_probe(hls_recovery_probe src/probes/media/HlsRecoveryProbe.cpp
    LINKS mikai_app Qt6::Test
    DEPLOY_SHADERS)
  mikai_add_probe(probe_runner src/probes/runner/main.cpp
    EXTRA_SOURCES
      src/probes/runner/ProbeRunner.cpp
      src/probes/runner/ProbeRunner.hpp
      src/probes/shared/ProbeManifest.cpp
      src/probes/shared/ProbeManifest.hpp
    LINKS mikai_app Qt6::Test
    DEPLOY_SHADERS)
  mikai_add_probe(rife_settings_probe src/probes/native/RifeSettingsProbe.cpp
    LINKS mikai_app Qt6::Test)
  mikai_add_probe(native_playback_assembly_probe src/probes/native/NativePlaybackAssemblyProbe.cpp
    LINKS mikai_app Qt6::Test)
  mikai_add_probe(native_framebuffer_probe src/probes/native/NativeFramebufferProbe.cpp
    LINKS mikai_app)
  mikai_add_probe(native_geometry_probe src/probes/native/NativeGeometryProbe.cpp
    LINKS mikai_app Qt6::Test)
  mikai_add_probe(benchmark_probe src/probes/player/BenchmarkProbe.cpp
    LINKS mikai_app Qt6::Test
    DEPLOY_SHADERS)
  mikai_add_probe(main_window_modal_probe src/probes/app/MainWindowModalProbe.cpp
    LINKS mikai_app Qt6::Widgets)

  if(MIKAI_WITH_MODEL_INSPECTION)
    mikai_add_probe(native_assets_probe src/probes/native/NativeAssetsProbe.cpp
      LINKS mikai_app Qt6::Test)
    mikai_add_probe(native_preparation_references_probe src/probes/native/NativePreparationReferencesProbe.cpp
      LINKS mikai_model_inspection)
    mikai_add_probe(spatial_assets_probe src/probes/native/SpatialAssetsProbe.cpp
      LINKS mikai_app Qt6::Test)
  endif()
endif()
