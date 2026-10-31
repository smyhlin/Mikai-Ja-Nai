# CMake contract tests, test launcher, and hermetic Qt Test suite (mikai_tests).

# Launcher process-lifetime contracts are meaningful on both Windows and Linux.
set(MIKAI_TEST_QT_PLUGIN_DIR "${MIKAI_TEST_QT_PREFIX}/plugins")
if(NOT IS_DIRECTORY "${MIKAI_TEST_QT_PLUGIN_DIR}/platforms" AND NOT WIN32)
  get_filename_component(_mikai_distro_qt_plugins
    "${Qt6_DIR}/../../qt6/plugins" ABSOLUTE)
  if(IS_DIRECTORY "${_mikai_distro_qt_plugins}/platforms")
    set(MIKAI_TEST_QT_PLUGIN_DIR "${_mikai_distro_qt_plugins}")
  endif()
endif()

add_executable(mikai_test_launcher cmake/MikaiTestLauncher.cpp)
target_link_libraries(mikai_test_launcher PRIVATE Qt6::Core)
add_dependencies(mikai_test_launcher mikai_launcher_child)

add_test(NAME LauncherContracts
  COMMAND ${CMAKE_COMMAND}
    -DFIXTURE_EXE=$<TARGET_FILE:mikai_launcher_child>
    -DLAUNCHER_EXE=$<TARGET_FILE:mikai_test_launcher>
    -DSHADERS_DIR=${CMAKE_SOURCE_DIR}/shaders
    -DMPV_MODULE=${MIKAI_SELECTED_MPV_MODULE}
    -DMPV_RUNTIME_DIR=${MIKAI_SELECTED_MPV_DEPENDENCY_ROOT}
    -DQT_BIN_DIR=$<TARGET_FILE_DIR:Qt6::Core>
    -DQT_PLUGIN_DIR=${MIKAI_TEST_QT_PLUGIN_DIR}
    -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
    -P ${CMAKE_SOURCE_DIR}/tests/cmake/LauncherContracts.cmake
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(LauncherContracts PROPERTIES
  TIMEOUT 180
  ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)

if(WIN32)
  set(MIKAI_DIST_CONTRACT_PE_SOURCE "${CMAKE_SOURCE_DIR}/dist-mikai/libwinpthread-1.dll"
    CACHE FILEPATH
    "Genuine read-only x64 PE DLL the DistContracts script re-uses as its archive member"
  )
  set(dist_contract_pe_args)
  if(MIKAI_DIST_CONTRACT_PE_SOURCE AND EXISTS "${MIKAI_DIST_CONTRACT_PE_SOURCE}")
    list(APPEND dist_contract_pe_args
      -DDIST_CONTRACT_PE_SOURCE=${MIKAI_DIST_CONTRACT_PE_SOURCE})
  endif()
  add_test(NAME DistContracts
    COMMAND ${CMAKE_COMMAND}
      -DDIST_CONTRACT_PROJECT=ON
      -DDIST_CONTRACT_SCRIPT=${CMAKE_SOURCE_DIR}/cmake/dist.cmake
      ${dist_contract_pe_args}
      -P ${CMAKE_SOURCE_DIR}/tests/cmake/DistContracts.cmake
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
  set_tests_properties(DistContracts PROPERTIES TIMEOUT 300)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME LinuxPackageContracts
    COMMAND ${CMAKE_COMMAND}
      -DBUILD_DIR=${CMAKE_BINARY_DIR}
      -DPACKAGE_SCRIPT=${CMAKE_SOURCE_DIR}/packaging/linux/Package.cmake
      -P ${CMAKE_SOURCE_DIR}/tests/cmake/LinuxPackageContracts.cmake
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
  set_tests_properties(LinuxPackageContracts PROPERTIES TIMEOUT 360)
endif()

# T-419: provenance/pin lockstep. The mpv archive pin in FindOrFetchMpv.cmake
# and the hand-curated provenance JSON in packaging/provenance/ must move
# together; nothing else in the tree cross-checks them, and a pin bump that
# misses the JSON (or the reverse) ships a provenance record describing a
# runtime the build never used. The check is registered unconditionally: it
# parses two source files, needs neither probes nor Qt, and runs on any
# platform's ctest. CI reaches it through the plain ctest invocation in
# packaging/ci/build.py (which runs every registered entry), so no separate
# wiring exists or is needed.
find_program(MIKAI_PYTHON3_EXECUTABLE NAMES python3 python)
if(MIKAI_PYTHON3_EXECUTABLE)
  add_custom_target(check_translations
    COMMAND "${MIKAI_PYTHON3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/packaging/ci/check_translations.py"
      --catalog "${CMAKE_SOURCE_DIR}/translations/mikai_uk.ts"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    VERBATIM
  )
  add_test(NAME mikai_translation_completeness
    COMMAND "${MIKAI_PYTHON3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/packaging/ci/check_translations.py"
      --catalog "${CMAKE_SOURCE_DIR}/translations/mikai_uk.ts"
  )
  set_tests_properties(mikai_translation_completeness PROPERTIES
    TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
  add_test(NAME mikai_provenance_check
    COMMAND "${MIKAI_PYTHON3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/packaging/ci/check_provenance.py"
      --source-root "${CMAKE_SOURCE_DIR}"
  )
  set_tests_properties(mikai_provenance_check PROPERTIES
    TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
else()
  message(STATUS "mikai_provenance_check and translation completeness check are not registered: "
    "no Python 3 interpreter found")
endif()

if(MIKAI_BUILD_PROBES)
  add_test(NAME NativeStageContracts
    COMMAND ${CMAKE_COMMAND}
      -DAJI_STAGE_CONTRACT_PROJECT=ON
      -DAJI_STAGE_HELPER=${CMAKE_SOURCE_DIR}/native/cmake/StageInference.cmake
      -P ${CMAKE_SOURCE_DIR}/tests/cmake/NativeStageContracts.cmake
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
  set_tests_properties(NativeStageContracts PROPERTIES TIMEOUT 300)

  add_library(mikai_test_aji_dispatcher SHARED EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/fixtures/aji_dispatcher_fixture.cpp
  )
  if(WIN32)
    set_target_properties(mikai_test_aji_dispatcher PROPERTIES
      PREFIX ""
      OUTPUT_NAME "aji"
      SUFFIX ".dll"
    )
  endif()

  add_executable(mikai_tests

    tests/cpp/main.cpp
    tests/cpp/TestSupport.hpp
    tests/fixtures/IdentityOnnxFixture.hpp
    # Backend domain & transport suites
    tests/cpp/backend/TestApiClient.hpp
    tests/cpp/backend/TestApiClient.cpp
    tests/cpp/backend/TestDiagnostics.hpp
    tests/cpp/backend/TestDiagnostics.cpp
    tests/cpp/backend/TestDownloads.hpp
    tests/cpp/backend/TestDownloads.cpp
    tests/cpp/backend/TestLiveOracle.hpp
    tests/cpp/backend/TestLiveOracle.cpp
    tests/cpp/backend/TestModels.hpp
    tests/cpp/backend/TestModels.cpp
    tests/cpp/backend/TestRateLimiter.hpp
    tests/cpp/backend/TestRateLimiter.cpp
    tests/cpp/backend/TestReleases.hpp
    tests/cpp/backend/TestReleases.cpp
    tests/cpp/backend/TestUpdateFeed.hpp
    tests/cpp/backend/TestUpdateFeed.cpp
    tests/cpp/backend/TestStreams.hpp
    tests/cpp/backend/TestStreams.cpp
    # Player & shader suites
    tests/cpp/player/TestAnime4K.hpp
    tests/cpp/player/TestAnime4K.cpp
    tests/cpp/player/TestMpvAsync.hpp
    tests/cpp/player/TestMpvAsync.cpp
    tests/cpp/player/TestCompareGif.hpp
    tests/cpp/player/TestCompareGif.cpp
    tests/cpp/player/TestCompareClip.hpp
    tests/cpp/player/TestCompareClip.cpp
    tests/cpp/player/TestCompareLoading.hpp
    tests/cpp/player/TestCompareLoading.cpp
    tests/cpp/player/TestPlaybackDiagnostics.hpp
    tests/cpp/player/TestPlaybackDiagnostics.cpp
    tests/cpp/player/TestMediaInformation.hpp
    tests/cpp/player/TestMediaInformation.cpp
    tests/cpp/player/TestHdrRenderContracts.hpp
    tests/cpp/player/TestHdrRenderContracts.cpp
    # Native bundle, component, model, preparation & status suites
    tests/cpp/native/TestAnimeJanaiRuntime.hpp
    tests/cpp/native/TestAnimeJanaiRuntime.cpp
    tests/cpp/native/TestNativeAssets.hpp
    tests/cpp/native/TestNativeAssets.cpp
    tests/cpp/native/TestNativeBuilder.hpp
    tests/cpp/native/TestNativeBuilder.cpp
    tests/cpp/native/TestNativeBundle.hpp
    tests/cpp/native/TestNativeBundle.cpp
    tests/cpp/native/TestNativeComponents.hpp
    tests/cpp/native/TestNativeComponents.cpp
    tests/cpp/native/TestNativeManifest.hpp
    tests/cpp/native/TestNativeManifest.cpp
    tests/cpp/native/TestNativeMenu.hpp
    tests/cpp/native/TestNativeMenu.cpp
    tests/cpp/native/TestNativePlaybackController.hpp
    tests/cpp/native/TestNativePlaybackController.cpp
    tests/cpp/native/TestNativePrefs.hpp
    tests/cpp/native/TestNativePrefs.cpp
    tests/cpp/native/TestNativePreparationReferences.hpp
    tests/cpp/native/TestNativePreparationReferences.cpp
    tests/cpp/native/TestNativePreparedPlan.hpp
    tests/cpp/native/TestNativePreparedPlan.cpp
    tests/cpp/native/TestNativePrepPlan.hpp
    tests/cpp/native/TestNativePrepPlan.cpp
    tests/cpp/native/TestNativeProfiles.hpp
    tests/cpp/native/TestNativeProfiles.cpp
    tests/cpp/native/TestNativeRecipe.hpp
    tests/cpp/native/TestNativeRecipe.cpp
    tests/cpp/native/TestNativeRife.hpp
    tests/cpp/native/TestNativeRife.cpp
    tests/cpp/native/TestNativeStatus.hpp
    tests/cpp/native/TestNativeStatus.cpp
    tests/cpp/native/TestNativeStoredModels.hpp
    tests/cpp/native/TestNativeStoredModels.cpp
    tests/cpp/native/TestNativeTypes.hpp
    tests/cpp/native/TestNativeTypes.cpp
    tests/cpp/native/TestNativeUserRecipes.hpp
    tests/cpp/native/TestNativeUserRecipes.cpp
    tests/cpp/native/TestRuntimeSelection.hpp
    tests/cpp/native/TestRuntimeSelection.cpp
    # Application, UI, threading & architecture layering suites
    tests/cpp/app/TestBenchmarkReport.hpp
    tests/cpp/app/TestBenchmarkReport.cpp
    tests/cpp/app/TestFeedRender.hpp
    tests/cpp/app/TestFeedRender.cpp
    tests/cpp/app/TestPublicApiUi.hpp
    tests/cpp/app/TestPublicApiUi.cpp
    tests/cpp/app/TestI18n.hpp
    tests/cpp/app/TestI18n.cpp
    tests/cpp/app/TestThemeIcons.hpp
    tests/cpp/app/TestThemeIcons.cpp
    tests/cpp/app/TestLayering.hpp
    tests/cpp/app/TestLayering.cpp
    tests/cpp/app/TestPagers.hpp
    tests/cpp/app/TestPagers.cpp
    tests/cpp/app/TestQualityCache.hpp
    tests/cpp/app/TestQualityCache.cpp
    tests/cpp/app/TestRunner.hpp
    tests/cpp/app/TestRunner.cpp
    tests/cpp/app/TestWatchHistory.hpp
    tests/cpp/app/TestWatchHistory.cpp
    tests/cpp/app/TestSystemIntegration.hpp
    tests/cpp/app/TestSystemIntegration.cpp
    tests/cpp/app/TestWindowRaise.hpp
    tests/cpp/app/TestWindowRaise.cpp
  )
  if(MIKAI_WITH_MODEL_INSPECTION)
    target_sources(mikai_tests PRIVATE
      tests/cpp/native/TestNativeCatalog.hpp
      tests/cpp/native/TestNativeCatalog.cpp
    )
    target_link_libraries(mikai_tests PRIVATE mikai_model_inspection)
    target_compile_definitions(mikai_tests PRIVATE MIKAI_WITH_MODEL_INSPECTION=1)
  endif()
  set_target_properties(mikai_tests PROPERTIES AUTOMOC ON)
  target_include_directories(mikai_tests PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests/cpp
    ${CMAKE_SOURCE_DIR}/tests/cpp/backend
    ${CMAKE_SOURCE_DIR}/tests/cpp/native
    ${CMAKE_SOURCE_DIR}/tests/cpp/player
    ${CMAKE_SOURCE_DIR}/tests/cpp/app
  )
  target_compile_definitions(mikai_tests PRIVATE
    MIKAI_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
  target_link_libraries(mikai_tests PRIVATE mikai_player mikai_app mikai_backend Qt6::Test Qt6::Widgets)
  if(TARGET mikai_test_aji_dispatcher)
    add_dependencies(mikai_tests mikai_test_aji_dispatcher)
    target_compile_definitions(mikai_tests PRIVATE
      MIKAI_TEST_AJI_DISPATCHER="$<TARGET_FILE:mikai_test_aji_dispatcher>"
    )
  endif()
  add_dependencies(mikai_tests mikai_test_launcher)

  if(TARGET release_translations)
    add_dependencies(mikai_tests release_translations)
    qt_add_resources(mikai_tests "test_translations"
      PREFIX "/i18n"
      BASE "${CMAKE_BINARY_DIR}"
      FILES "${CMAKE_BINARY_DIR}/mikai_uk.qm"
    )
    target_compile_definitions(mikai_tests PRIVATE
      MIKAI_UK_CATALOG_PATH="${CMAKE_BINARY_DIR}/mikai_uk.qm"
    )
  endif()

  add_dependencies(mikai_tests mikai_shader_deployment)

  set(_mikai_tests_launcher_args
    "$<TARGET_FILE:mikai_tests>"
    "${CMAKE_SOURCE_DIR}/shaders"
    "${MIKAI_SELECTED_MPV_MODULE}"
    "$<TARGET_FILE_DIR:Qt6::Core>"
    "${MIKAI_TEST_QT_PLUGIN_DIR}"
    "${CMAKE_BINARY_DIR}/_test-runs"
  )
  if(MIKAI_SELECTED_MPV_BUNDLED)
    list(APPEND _mikai_tests_launcher_args
      --mpv-runtime-dir "${MIKAI_SELECTED_MPV_DEPENDENCY_ROOT}")
  endif()
  add_test(NAME mikai_tests COMMAND mikai_test_launcher
    ${_mikai_tests_launcher_args}
  )
  # The plain entry stays the "run everything" contract for local use
  # (`ctest -R ^mikai_tests$`, the docs' plain `ctest`). CI excludes it through
  # the mikai_tests_blob label (see packaging/ci/build.py) so the per-suite
  # entries below are what a hermetic CI run actually exercises; a hard crash
  # in one suite can then no longer hide the rest.
  set_tests_properties(mikai_tests PROPERTIES
    TIMEOUT 900
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    LABELS "mikai_tests_blob"
  )

  # Separate entries ensure a failure in one suite does not hide later suites.
  set(MIKAI_TEST_SUITES
    TestAnimeJanaiRuntime
    TestRuntimeSelection
    TestNativeTypes
    TestNativeRecipe
    TestNativeProfiles
    TestNativeRife
    TestNativeBundle
    TestNativePrepPlan
    TestNativeManifest
    TestNativeBuilder
    TestNativePreparationReferences
    TestNativeComponents
    TestNativeStoredModels
    TestNativePreparedPlan
    TestNativePlaybackController
    TestNativeStatus
    TestPlaybackDiagnostics
    TestMediaInformation
    TestHdrRenderContracts
    TestNativePrefs
    TestNativeMenu
    TestNativeUserRecipes
    TestNativeAssets
    TestMpvAsync
    TestCompareGif
    TestCompareClip
    TestCompareLoading
    TestModels
    TestReleases
    TestAnime4K
    TestBenchmarkReport
    TestI18n
    TestThemeIcons
    TestStreams
    TestPagers
    TestQualityCache
    TestRateLimiter
    TestWatchHistory
    TestDownloads
    TestDiagnostics
    TestApiClient
    TestFeedRender
    TestUpdateFeed
    TestPublicApiUi
    TestRunner
    TestLayering
    TestWindowRaise
    TestSystemIntegration
    TestClipboard
    TestLiveOracle
  )
  # Order in the list is cosmetic (every entry runs exactly one suite); the
  # conditional native suite is appended rather than spliced into position.
  if(MIKAI_WITH_MODEL_INSPECTION)
    list(APPEND MIKAI_TEST_SUITES TestNativeCatalog)
  endif()
  foreach(_mikai_suite IN LISTS MIKAI_TEST_SUITES)
    add_test(NAME mikai_tests_${_mikai_suite}
      COMMAND mikai_test_launcher ${_mikai_tests_launcher_args} ${_mikai_suite})
    set_tests_properties(mikai_tests_${_mikai_suite} PROPERTIES
      TIMEOUT 900
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
      LABELS "mikai_tests_suite"
    )
  endforeach()
  add_dependencies(mikai_tests mikai_bundle)
  set(_mikai_bundle_cli_args
    -DBUNDLE_TOOL=$<TARGET_FILE:mikai_bundle>
    -DMPV_MODULE=${MIKAI_SELECTED_MPV_MODULE}
  )
  if(MIKAI_SELECTED_MPV_BUNDLED)
    list(APPEND _mikai_bundle_cli_args
      -DMPV_RUNTIME_DIR=${MIKAI_SELECTED_MPV_DEPENDENCY_ROOT}
    )
  endif()
  add_test(NAME BundleCliContracts
    COMMAND ${CMAKE_COMMAND}
      ${_mikai_bundle_cli_args}
      -P ${CMAKE_SOURCE_DIR}/tests/cmake/BundleCliContracts.cmake
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
  set_tests_properties(BundleCliContracts PROPERTIES
    TIMEOUT 180
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
  )
endif()
