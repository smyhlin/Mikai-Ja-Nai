cmake_minimum_required(VERSION 3.21)
project(mikai LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Qt6Widgets pulls in OpenGL; QOpenGLWidget itself lives in Qt6OpenGLWidgets.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Network Test)

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

# Live smoke: a real Qt window with the VideoSurface, proving mpv actually
# draws through the render bridge rather than through a native HWND.
add_executable(mpv_surface_smoke src/probes/mpv_surface_smoke.cpp)
target_link_libraries(mpv_surface_smoke PRIVATE mikai_player Qt6::Widgets)
add_executable(headless_pixel_probe src/probes/HeadlessPixelProbe.cpp)
target_link_libraries(headless_pixel_probe PRIVATE mikai_player Qt6::Core Qt6::Gui Qt6::OpenGL)
set_target_properties(headless_pixel_probe PROPERTIES WIN32_EXECUTABLE FALSE)


add_executable(widget_pixel_probe src/probes/WidgetPixelProbe.cpp)
target_link_libraries(widget_pixel_probe PRIVATE mikai_player Qt6::Widgets)


add_executable(shell_probe src/probes/ShellProbe.cpp)
target_link_libraries(shell_probe PRIVATE mikai_player Qt6::Widgets)
set_target_properties(shell_probe PROPERTIES WIN32_EXECUTABLE FALSE)

add_executable(dialog_probe src/probes/DialogProbe.cpp)
target_link_libraries(dialog_probe PRIVATE mikai_player Qt6::Widgets)
set_target_properties(dialog_probe PROPERTIES WIN32_EXECUTABLE FALSE)

add_executable(interaction_probe src/probes/InteractionProbe.cpp)
target_link_libraries(interaction_probe PRIVATE mikai_player Qt6::Widgets Qt6::Test)
set_target_properties(interaction_probe PROPERTIES WIN32_EXECUTABLE FALSE)

# Proves the crash flow actually writes a report rather than merely being wired
# up: it installs the same logging the app installs, then faults on purpose.
add_executable(crash_probe src/probes/CrashProbe.cpp)
target_link_libraries(crash_probe PRIVATE mikai_app Qt6::Core)
set_target_properties(crash_probe PROPERTIES WIN32_EXECUTABLE FALSE)

# Live title-page probe: builds the real TitlePage against the real API and
# reports what each provider tab received, then what the player did with a pill.
# It exists because "the Ashdi episode list never appears" survived every round
# of source reading, and the only honest way to settle it was to look.
add_executable(title_page_probe src/probes/TitlePageProbe.cpp)
target_link_libraries(title_page_probe PRIVATE mikai_app mikai_backend Qt6::Widgets Qt6::Test)
set_target_properties(title_page_probe PROPERTIES WIN32_EXECUTABLE FALSE)

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
set_target_properties(mikai_backend PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)
target_include_directories(mikai_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mikai_backend PUBLIC Qt6::Core Qt6::Network)

# mikai_tests: the C++ replacement for tests/*.py. One executable so ctest has a
# single hermetic command; the runner in tests/cpp/main.cpp drives each suite in
# turn and sums the failures. Everything but the live oracle is hermetic.
#
# The suite links mikai_app for the pager, the quality cache and the clipboard,
# and mikai_backend for the domain. Set MIKAI_LIVE=1 to add the live oracle.
enable_testing()

add_executable(mikai_tests
  tests/cpp/main.cpp
  tests/cpp/TestSupport.hpp
  tests/cpp/TestModels.hpp
  tests/cpp/TestModels.cpp
  tests/cpp/TestReleases.hpp
  tests/cpp/TestReleases.cpp
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
