# The Anime4K GLSL set is third-party content, not source of this project.
#
# The repository carries the pin and the digests — packaging/shaders/
# anime4k-manifest.json names one upstream commit and the SHA-256 of every file —
# and this module materialises the shaders from that pin into the build tree.
# One tarball of the pinned commit is downloaded into _dist-cache/anime4k and
# every file is checked against the manifest before it is written, so:
#
#   - the source checkout ships no upstream blob and no unverified copy,
#   - an offline build works from a warm cache or from MIKAI_ANIME4K_ARCHIVE,
#   - a moved pin is one `cmake -DMODE=pin` run and a rebuild.
#
# The work happens in cmake/FetchAnime4K.cmake through `cmake -P`, so the build,
# the CTest check and a maintainer all run the same code and no interpreter
# beyond CMake is needed.

set(MIKAI_ANIME4K_MANIFEST
  "${CMAKE_CURRENT_LIST_DIR}/../packaging/shaders/anime4k-manifest.json" CACHE FILEPATH
  "Pinned Anime4K manifest: upstream commit and per-file SHA-256")
set(MIKAI_ANIME4K_ARCHIVE "" CACHE FILEPATH
  "Optional local tar.gz of the pinned Anime4K commit (every file is still digest-checked)")
set(MIKAI_ANIME4K_CACHE_DIR "${CMAKE_BINARY_DIR}/_dist-cache/anime4k" CACHE PATH
  "Download cache for the pinned Anime4K archive")
set(MIKAI_ANIME4K_SHADERS_DIR "${CMAKE_BINARY_DIR}/shaders" CACHE PATH
  "Directory the Anime4K shaders are materialised in")
set(MIKAI_ANIME4K_FETCH_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/FetchAnime4K.cmake")

if(NOT EXISTS "${MIKAI_ANIME4K_MANIFEST}")
  message(FATAL_ERROR "The Anime4K manifest is missing: ${MIKAI_ANIME4K_MANIFEST}")
endif()
file(READ "${MIKAI_ANIME4K_MANIFEST}" _mikai_anime4k_manifest)
string(JSON MIKAI_ANIME4K_COMMIT GET "${_mikai_anime4k_manifest}" commit)
string(LENGTH "${MIKAI_ANIME4K_COMMIT}" _mikai_anime4k_commit_length)
if(NOT _mikai_anime4k_commit_length EQUAL 40)
  message(FATAL_ERROR "The Anime4K manifest names no full commit: ${MIKAI_ANIME4K_MANIFEST}")
endif()

set(_mikai_anime4k_pin_file "${MIKAI_ANIME4K_SHADERS_DIR}/anime4k-pin.txt")
set(_mikai_anime4k_staged_pin "")
if(EXISTS "${_mikai_anime4k_pin_file}")
  file(READ "${_mikai_anime4k_pin_file}" _mikai_anime4k_staged_pin)
  string(REGEX REPLACE "[ \t\r\n].*$" "" _mikai_anime4k_staged_pin "${_mikai_anime4k_staged_pin}")
endif()

if(EXISTS "${MIKAI_ANIME4K_ARCHIVE}" AND NOT IS_DIRECTORY "${MIKAI_ANIME4K_ARCHIVE}")
  set(MIKAI_ANIME4K_ARCHIVE "${MIKAI_ANIME4K_ARCHIVE}")
elseif(NOT MIKAI_ANIME4K_ARCHIVE STREQUAL "")
  message(FATAL_ERROR "MIKAI_ANIME4K_ARCHIVE is not a file: ${MIKAI_ANIME4K_ARCHIVE}")
endif()

if(NOT _mikai_anime4k_staged_pin STREQUAL MIKAI_ANIME4K_COMMIT)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DMODE=fetch"
      "-DMANIFEST=${MIKAI_ANIME4K_MANIFEST}"
      "-DSHADERS_DIR=${MIKAI_ANIME4K_SHADERS_DIR}"
      "-DCACHE_DIR=${MIKAI_ANIME4K_CACHE_DIR}"
      "-DARCHIVE=${MIKAI_ANIME4K_ARCHIVE}"
      -P "${MIKAI_ANIME4K_FETCH_SCRIPT}"
    RESULT_VARIABLE _mikai_anime4k_result
    OUTPUT_VARIABLE _mikai_anime4k_output
    ERROR_VARIABLE _mikai_anime4k_error)
  if(NOT _mikai_anime4k_result EQUAL 0)
    string(STRIP "${_mikai_anime4k_error}" _mikai_anime4k_error)
    message(FATAL_ERROR "${_mikai_anime4k_error}")
  endif()
  string(STRIP "${_mikai_anime4k_output}" _mikai_anime4k_output)
  # The nested `cmake -P` already formats its line; a second STATUS prefix would
  # render it as "-- -- shaders: ...".
  string(REGEX REPLACE "^(--[ 	]*)+" "" _mikai_anime4k_output "${_mikai_anime4k_output}")
  if(_mikai_anime4k_output)
    message(STATUS "${_mikai_anime4k_output}")
  endif()
else()
  message(STATUS
    "Anime4K shaders: pinned ${MIKAI_ANIME4K_COMMIT} already staged in "
    "${MIKAI_ANIME4K_SHADERS_DIR}")
endif()

set(MIKAI_ANIME4K_SHADERS_FILES "${MIKAI_ANIME4K_SHADERS_DIR}/*.glsl")
