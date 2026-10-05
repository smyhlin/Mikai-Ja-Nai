# M02 hermetic fixtures: the media matrix, the real HLS renditions and the
# loopback HTTP/HLS fixture server.
#
# This directory is self-contained and is pulled in by the root CMakeLists with
# a single add_subdirectory(tests/fixtures). It defines exactly one target,
# mikai_http_fixture, and it deliberately does NOT add itself to `all`: the other
# QA probes in this repository are EXCLUDE_FROM_ALL and built explicitly, and a
# fixture server is not something a plain `cmake --build` should leave running
# or produce.
#
#     cmake --build build-cmake --target mikai_http_fixture
#
# The server needs Qt6::Core and Qt6::Network only -- no GUI, no Qt Widgets, no
# mpv, no app or backend library. It must stay that way: it is a fixture, and a
# fixture that links the thing under test cannot catch the thing under test.

add_executable(mikai_http_fixture EXCLUDE_FROM_ALL http_fixture.cpp)

target_link_libraries(mikai_http_fixture PRIVATE Qt6::Core Qt6::Network)

mikai_deploy_mingw_runtime_targets()

# A console target: unlike mikai.exe it prints its bound port and its URL list
# on stdout, which is how a parent process learns the port when --port 0 was
# requested.
set_target_properties(mikai_http_fixture PROPERTIES WIN32_EXECUTABLE FALSE)

# The generated assets are addressed by default relative to the source tree, so
# a run from the build directory finds them without extra flags. --media-dir
# and --hls-dir still override this at run time.
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
