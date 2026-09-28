cmake_minimum_required(VERSION 3.21)
project(mikai_player LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Qt6Widgets pulls in OpenGL; QOpenGLWidget itself lives in Qt6OpenGLWidgets.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)

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
  src/player/ShellTheme.cpp
  src/player/ShellWidgets.cpp
  src/player/ShellSliders.cpp
  src/player/PlayerShell.cpp
  src/player/PlayerPopups.cpp
  src/player/PlayerDialog.cpp
  src/player/IpcChannel.cpp
)

set(PLAYER_HEADERS
  src/player/MpvHandle.hpp
  src/player/MpvRenderContext.hpp
  src/player/VideoSurface.hpp
  src/player/ShellTheme.hpp
  src/player/ShellWidgets.hpp
  src/player/ShellSliders.hpp
  src/player/PlayerShell.hpp
  src/player/PlayerPopups.hpp
  src/player/PlayerDialog.hpp
  src/player/IpcChannel.hpp
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

# The player executable. It is a process, not a DLL: PySide6 and this Qt
# are separate builds whose Qt6Core.dll collides by name, so loading both
# into one process fails with WinError 127.
add_executable(mikai-player src/player/main.cpp)
target_link_libraries(mikai-player PRIVATE mikai_player)

add_executable(dialog_probe src/probes/DialogProbe.cpp)
target_link_libraries(dialog_probe PRIVATE mikai_player Qt6::Widgets)
set_target_properties(dialog_probe PROPERTIES WIN32_EXECUTABLE FALSE)
