# CMake contract tests, test launcher, and hermetic Qt Test suite (mikai_tests).

# Unconditional launcher & dist contract registrations
add_executable(mikai_test_launcher cmake/MikaiTestLauncher.cpp)
target_link_libraries(mikai_test_launcher PRIVATE Qt6::Core)

add_test(NAME LauncherContracts
  COMMAND ${CMAKE_COMMAND}
    -DFIXTURE_EXE=$<TARGET_FILE:mikai_launcher_child>
    -DLAUNCHER_EXE=$<TARGET_FILE:mikai_test_launcher>
    -DSHADERS_DIR=${CMAKE_SOURCE_DIR}/shaders
    -DMPV_DLL=${MIKAI_MPV_DLL}
    -DQT_BIN_DIR=$<TARGET_FILE_DIR:Qt6::Core>
    -DQT_PLUGIN_DIR=${MIKAI_TEST_QT_PREFIX}/plugins
    -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
    -P ${CMAKE_SOURCE_DIR}/tests/cmake/LauncherContracts.cmake
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(LauncherContracts PROPERTIES
  TIMEOUT 180
  ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)

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

if(MIKAI_BUILD_PROBES)
  add_test(NAME NativeStageContracts
    COMMAND ${CMAKE_COMMAND}
      -DAJI_STAGE_CONTRACT_PROJECT=ON
      -DAJI_STAGE_HELPER=${CMAKE_SOURCE_DIR}/native/cmake/StageInference.cmake
      -P ${CMAKE_SOURCE_DIR}/tests/cmake/NativeStageContracts.cmake
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
  set_tests_properties(NativeStageContracts PROPERTIES TIMEOUT 300)

  if(WIN32)
    add_library(mikai_test_aji_dispatcher SHARED EXCLUDE_FROM_ALL
      ${CMAKE_SOURCE_DIR}/tests/fixtures/aji_dispatcher_fixture.cpp
    )
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
    tests/cpp/backend/TestStreams.hpp
    tests/cpp/backend/TestStreams.cpp
    # Player & shader suites
    tests/cpp/player/TestAnime4K.hpp
    tests/cpp/player/TestAnime4K.cpp
    tests/cpp/player/TestMpvAsync.hpp
    tests/cpp/player/TestMpvAsync.cpp
    tests/cpp/player/TestPlaybackDiagnostics.hpp
    tests/cpp/player/TestPlaybackDiagnostics.cpp
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
    tests/cpp/app/TestI18n.hpp
    tests/cpp/app/TestI18n.cpp
    tests/cpp/app/TestLayering.hpp
    tests/cpp/app/TestLayering.cpp
    tests/cpp/app/TestPagers.hpp
    tests/cpp/app/TestPagers.cpp
    tests/cpp/app/TestQualityCache.hpp
    tests/cpp/app/TestQualityCache.cpp
    tests/cpp/app/TestRunner.hpp
    tests/cpp/app/TestRunner.cpp
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
    target_compile_definitions(mikai_tests PRIVATE
      MIKAI_UK_CATALOG_PATH="${CMAKE_BINARY_DIR}/mikai_uk.qm"
    )
  endif()

  add_custom_command(TARGET mikai_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_SOURCE_DIR}/shaders"
      "$<TARGET_FILE_DIR:mikai_tests>/shaders"
    COMMENT "Deploying Anime4K shader fixtures for mikai_tests"
  )

  add_test(NAME mikai_tests COMMAND mikai_test_launcher
    "$<TARGET_FILE:mikai_tests>"
    "${CMAKE_SOURCE_DIR}/shaders"
    "${MIKAI_MPV_DLL}"
    "$<TARGET_FILE_DIR:Qt6::Core>"
    "${MIKAI_TEST_QT_PREFIX}/plugins"
    "${CMAKE_BINARY_DIR}/_test-runs"
  )
  set_tests_properties(mikai_tests PROPERTIES
    TIMEOUT 900
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
  )
endif()
