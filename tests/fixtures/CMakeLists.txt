# Hermetic fixtures: the media matrix, the real HLS renditions, the loopback
# HTTP/HLS fixture server, and the test launcher's disposable child process.
#
# This directory is self-contained and is pulled in by the root CMakeLists with
# a single add_subdirectory(tests/fixtures). The HTTP/HLS server stays
# EXCLUDE_FROM_ALL and is built explicitly: a server is not something a plain
# `cmake --build` should produce. The launcher child is the opposite case - the
# LauncherContracts CTest entry drives it through the real launcher and a CTest
# run cannot build it - so it is a normal target and `all` produces it.
#
#     cmake --build build/release --target mikai_http_fixture
#     cmake --build build/release --target mikai_launcher_child
#
# A fixture links Qt and nothing else -- no GUI, no Qt Widgets, no mpv, no app
# or backend library, no test framework. It must stay that way: a fixture that
# links the thing under test cannot catch the thing under test.

add_executable(mikai_http_fixture EXCLUDE_FROM_ALL http_fixture.cpp)

target_link_libraries(mikai_http_fixture PRIVATE Qt6::Core Qt6::Network)

add_executable(mikai_launcher_child LauncherFixture.cpp)

target_link_libraries(mikai_launcher_child PRIVATE Qt6::Core)

# Both are console targets: unlike mikai.exe they print their state on stdout,
# which is how the contract script reads the bound port and the process identity
# the launcher child records before it parks.
set_target_properties(mikai_http_fixture mikai_launcher_child PROPERTIES
  WIN32_EXECUTABLE FALSE)

mikai_deploy_mingw_runtime_targets()

# The generated HLS/media assets are addressed by default relative to the source
# tree, so a run from the build directory finds them without extra flags.
# --media-dir and --hls-dir still override this at run time.
target_compile_definitions(mikai_http_fixture PRIVATE
  MIKAI_FIXTURE_MEDIA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/media"
  MIKAI_FIXTURE_HLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/hls"
)

# Tell the user how to produce the assets if they are missing, rather than
# failing at configure time on a clean clone where the binaries are not yet
# generated.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/media/motion_320x180_24fps_8bit_hevc.mp4")
  message(STATUS
    "mikai_http_fixture: fixture media not generated yet -- run "
    "tests/fixtures/generate_media.cmd (set FFMPEG=<path to ffmpeg.exe>) before "
    "serving. The target still builds; the server reports a missing directory "
    "at startup.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/hls/v180/playlist.m3u8")
  message(STATUS
    "mikai_http_fixture: HLS renditions not generated yet -- run "
    "tests/fixtures/generate_media.cmd before serving.")
endif()
