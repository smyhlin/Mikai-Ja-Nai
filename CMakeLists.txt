cmake_minimum_required(VERSION 3.21)
project(mikai LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Qt6Widgets pulls in OpenGL; QOpenGLWidget itself lives in Qt6OpenGLWidgets.
# Test is deliberately not in this list: it is only needed by the QA probes and
# mikai_tests, so the app itself configures on a Qt install without it.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Network)

# The QA probes and the C++ test runner. ON by default so the tree configures
# exactly as it did before, but the probe *executables* are EXCLUDE_FROM_ALL: a
# bare `cmake --build build-cmake` no longer pays for eleven extra Qt link
# steps just to produce mikai.exe. Build them by name instead:
#   cmake --build build-cmake --target title_page_probe
#   cmake --build build-cmake --target widget_pixel_probe
#   cmake --build build-cmake --target shell_probe
#   cmake --build build-cmake --target dialog_probe
#   cmake --build build-cmake --target interaction_probe
#   cmake --build build-cmake --target compare_probe
#   cmake --build build-cmake --target compare_shot_probe
#   cmake --build build-cmake --target mpv_surface_smoke
#   cmake --build build-cmake --target headless_pixel_probe
#   cmake --build build-cmake --target crash_probe
#   cmake --build build-cmake --target resolver_probe
option(MIKAI_BUILD_PROBES "Build the QA probes and mikai_tests" ON)
if(MIKAI_BUILD_PROBES)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  enable_testing()
endif()

# libmpv is provided by the runtime the app already downloads. Point
# MPV_RUNTIME_DIR at it; the headers and the import library both live there.
set(MPV_RUNTIME_DIR "" CACHE PATH "Directory containing mpv headers and libmpv-2.dll")
if(NOT MPV_RUNTIME_DIR)
  message(FATAL_ERROR "Set -DMPV_RUNTIME_DIR=<path with include/ and libmpv-2.dll>")
endif()

set(PLAYER_SOURCES
  src/player/MpvHandle.cpp
  src/player/MpvRenderContext.cpp
  src/player/VideoSurface.cpp
  src/player/Anime4K.cpp
  src/player/ShellTheme.cpp
  src/player/ShellWidgets.cpp
  src/player/ShellSliders.cpp
  src/player/PlayerShell.cpp
  src/player/PlayerPopups.cpp
  src/player/PlayerDialog.cpp
  src/player/CompareWindow.cpp
  src/player/CompareCanvas.cpp
  src/player/CompareLayout.cpp
  src/player/ComparePane.cpp
  src/player/CompareGrid.cpp
)

set(PLAYER_HEADERS
  src/player/MpvHandle.hpp
  src/player/MpvRenderContext.hpp
  src/player/VideoSurface.hpp
  src/player/Anime4K.hpp
  src/player/ShellTheme.hpp
  src/player/ShellWidgets.hpp
  src/player/ShellSliders.hpp
  src/player/PlayerShell.hpp
  src/player/PlayerPopups.hpp
  src/player/PlayerDialog.hpp
  src/player/CompareWindow.hpp
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
  ${MPV_RUNTIME_DIR}/include
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

# libmpv ships as a plain DLL; MinGW links against the .dll.a import library.
# MinGW names the import library libmpv.dll.a, not libmpv-2.dll.a, so the
# name has to be listed explicitly or find_library silently misses it.
find_library(MPV_LIBRARY
  NAMES libmpv.dll.a libmpv-2.dll.a libmpv
  PATHS ${MPV_RUNTIME_DIR}
  NO_DEFAULT_PATH
  REQUIRED
)
target_link_libraries(mikai_player PUBLIC ${MPV_LIBRARY})

if(MIKAI_BUILD_PROBES)
# Live smoke: a real Qt window with the VideoSurface, proving mpv actually
# draws through the render bridge rather than through a native HWND.
add_executable(mpv_surface_smoke EXCLUDE_FROM_ALL src/probes/mpv_surface_smoke.cpp)
target_link_libraries(mpv_surface_smoke PRIVATE mikai_player Qt6::Widgets)
add_executable(headless_pixel_probe EXCLUDE_FROM_ALL src/probes/HeadlessPixelProbe.cpp)
target_link_libraries(headless_pixel_probe PRIVATE mikai_player Qt6::Core Qt6::Gui Qt6::OpenGL)
set_target_properties(headless_pixel_probe PROPERTIES WIN32_EXECUTABLE FALSE)


add_executable(widget_pixel_probe EXCLUDE_FROM_ALL src/probes/WidgetPixelProbe.cpp)
target_link_libraries(widget_pixel_probe PRIVATE mikai_player Qt6::Widgets)


add_executable(shell_probe EXCLUDE_FROM_ALL src/probes/ShellProbe.cpp)
target_link_libraries(shell_probe PRIVATE mikai_player Qt6::Widgets)
set_target_properties(shell_probe PROPERTIES WIN32_EXECUTABLE FALSE)

add_executable(dialog_probe EXCLUDE_FROM_ALL src/probes/DialogProbe.cpp)
target_link_libraries(dialog_probe PRIVATE mikai_player Qt6::Widgets)
set_target_properties(dialog_probe PROPERTIES WIN32_EXECUTABLE FALSE)

add_executable(interaction_probe EXCLUDE_FROM_ALL src/probes/InteractionProbe.cpp)
target_link_libraries(interaction_probe PRIVATE mikai_player Qt6::Widgets Qt6::Test)
set_target_properties(interaction_probe PROPERTIES WIN32_EXECUTABLE FALSE)

# Proves the crash flow actually writes a report rather than merely being wired
# up: it installs the same logging the app installs, then faults on purpose.
add_executable(crash_probe EXCLUDE_FROM_ALL src/probes/CrashProbe.cpp)
target_link_libraries(crash_probe PRIVATE mikai_app Qt6::Core)
set_target_properties(crash_probe PROPERTIES WIN32_EXECUTABLE FALSE)

# Live title-page probe: builds the real TitlePage against the real API and
# reports what each provider tab received, then what the player did with a pill.
# It exists because "the Ashdi episode list never appears" survived every round
# of source reading, and the only honest way to settle it was to look.
add_executable(compare_shot_probe EXCLUDE_FROM_ALL src/probes/CompareShotProbe.cpp)
target_link_libraries(compare_shot_probe PRIVATE mikai_player Qt6::Widgets Qt6::Test)
set_target_properties(compare_shot_probe PROPERTIES WIN32_EXECUTABLE FALSE)

add_executable(compare_probe EXCLUDE_FROM_ALL src/probes/CompareProbe.cpp)
target_link_libraries(compare_probe PRIVATE mikai_player Qt6::Widgets Qt6::Test)
set_target_properties(compare_probe PROPERTIES WIN32_EXECUTABLE FALSE)
add_executable(resolver_probe EXCLUDE_FROM_ALL src/probes/ResolverProbe.cpp)
target_link_libraries(resolver_probe PRIVATE mikai_player Qt6::Core)
set_target_properties(resolver_probe PROPERTIES WIN32_EXECUTABLE FALSE)
add_executable(title_page_probe EXCLUDE_FROM_ALL src/probes/TitlePageProbe.cpp)
target_link_libraries(title_page_probe PRIVATE mikai_app mikai_backend Qt6::Widgets Qt6::Test)
set_target_properties(title_page_probe PROPERTIES WIN32_EXECUTABLE FALSE)
endif() # MIKAI_BUILD_PROBES

# mikai_backend: the Qt-free domain models plus the Qt Network client that
# replaced mikai_backend/. It only needs Core and Network so the worker pools
# can use it from threads without dragging widgets in.
add_library(mikai_backend STATIC
  src/backend/Models.cpp
  src/backend/Models.hpp
  src/backend/Errors.hpp
  src/backend/Releases.cpp
  src/backend/Releases.hpp
  src/backend/Streams.cpp
  src/backend/Streams.hpp
  src/backend/MikaiClient.cpp
  src/backend/MikaiClient.hpp
  src/backend/Runtime.cpp
  src/backend/Runtime.hpp
)
# No AUTOMOC: src/backend declares no Q_OBJECT/Q_GADGET, so the autogen step
# would scan all nine headers and moc each one into nothing.
set_target_properties(mikai_backend PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(mikai_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mikai_backend PUBLIC Qt6::Core Qt6::Network)

# mikai_tests: the C++ replacement for tests/*.py. One executable so ctest has a
# single hermetic command; the runner in tests/cpp/main.cpp drives each suite in
# turn and sums the failures. Everything but the live oracle is hermetic.
#
# The suite links mikai_app for the pager, the quality cache and the clipboard,
# and mikai_backend for the domain. Set MIKAI_LIVE=1 to add the live oracle.
#
# Unlike the probes this stays in the default `all` target, so a plain
# `cmake --build build-cmake` still produces mikai_tests.exe and ctest still
# knows about it. It needs Qt6::Test, hence the MIKAI_BUILD_PROBES guard.
if(MIKAI_BUILD_PROBES)
add_executable(mikai_tests
  tests/cpp/main.cpp
  tests/cpp/TestSupport.hpp
  tests/cpp/TestModels.hpp
  tests/cpp/TestModels.cpp
  tests/cpp/TestReleases.hpp
  tests/cpp/TestReleases.cpp
  tests/cpp/TestAnime4K.hpp
  tests/cpp/TestAnime4K.cpp
  tests/cpp/TestStreams.hpp
  tests/cpp/TestStreams.cpp
  tests/cpp/TestPagers.hpp
  tests/cpp/TestPagers.cpp
  tests/cpp/TestQualityCache.hpp
  tests/cpp/TestQualityCache.cpp
  tests/cpp/TestRateLimiter.hpp
  tests/cpp/TestRateLimiter.cpp
  tests/cpp/TestLayering.hpp
  tests/cpp/TestLayering.cpp
  tests/cpp/TestLiveOracle.hpp
  tests/cpp/TestLiveOracle.cpp
)
set_target_properties(mikai_tests PROPERTIES AUTOMOC ON)
target_include_directories(mikai_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# MIKAI_SOURCE_DIR lets the layer guards read the sources they police; without
# the repo root a guard that found no files would pass vacuously.
target_compile_definitions(mikai_tests PRIVATE
  MIKAI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(mikai_tests PRIVATE mikai_app mikai_backend Qt6::Test Qt6::Widgets)

add_test(NAME mikai_tests COMMAND mikai_tests)
endif() # MIKAI_BUILD_PROBES


# ---------------------------------------------------------------------------
# The desktop app: a Qt Widgets frontend over mikai_backend, driving the
# existing C++ player in-process. Appended after the player targets so the
# player project still configures and builds on its own.
# ---------------------------------------------------------------------------

if(TARGET mikai_backend)
  set(APP_SOURCES
    src/app/CrashLog.cpp
    src/app/theme/Theme.cpp
    src/app/theme/Icons.cpp
    src/app/Labels.cpp
    src/app/state/Feeds.cpp
    src/app/state/QualityCache.cpp
    src/app/workers/Runner.cpp
    src/app/widgets/RangePager.cpp
    src/app/widgets/TitleCard.cpp
    src/app/widgets/RateLimitLabel.cpp
    src/app/dialogs/SettingsDialog.cpp
    src/app/dialogs/CoverViewer.cpp
    src/app/pages/SearchPage.cpp
    src/app/pages/TitlePage.cpp
    src/app/pages/MainWindow.cpp
  )

  set(APP_HEADERS
    src/app/Clipboard.hpp
    src/app/CrashLog.hpp
    src/app/Labels.hpp
    src/app/theme/Theme.hpp
    src/app/theme/Colors.hpp
    src/app/theme/Metrics.hpp
    src/app/theme/Icons.hpp
    src/app/state/Feeds.hpp
    src/app/state/QualityCache.hpp
    src/app/workers/Runner.hpp
    src/app/widgets/RangePager.hpp
    src/app/widgets/TitleCard.hpp
    src/app/widgets/RateLimitLabel.hpp
    src/app/dialogs/SettingsDialog.hpp
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

  add_executable(mikai src/app/main.cpp)
  set_target_properties(mikai PROPERTIES WIN32_EXECUTABLE TRUE)
  target_link_libraries(mikai PRIVATE mikai_app)

  # Copy Anime4K shaders next to the executable so libmpv can find them at runtime.
  add_custom_command(TARGET mikai POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_CURRENT_SOURCE_DIR}/shaders
      $<TARGET_FILE_DIR:mikai>/shaders
    COMMENT "Deploying Anime4K shaders to output directory"
  )
else()
  message(STATUS "mikai_backend not present: skipping the mikai app targets")
endif()

# ---------------------------------------------------------------------------
# Release-only tuning for the four targets that make up the shipped app.
# Generator expressions keep this inert for Debug and for any non-GNU/Clang
# toolchain, so the build commands in readme.md/AGENTS.md need no extra flags.
#
# -march=x86-64-v2 (SSE4.2 + POPCNT) is the ceiling that keeps the deploy
# folder handable to any Windows x64 box; -march=native would not be.
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

set(MIKAI_TUNED_TARGETS mikai_player mikai_backend mikai_app mikai)
foreach(t IN LISTS MIKAI_TUNED_TARGETS)
  if(TARGET ${t})
    if(MIKAI_IPO)
      set_property(TARGET ${t} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
    target_compile_options(${t} PRIVATE
      $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>>:-march=x86-64-v2>
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
