# Cross-check for the test-suite registry.
#
# Two lists name the same suites and drift silently: `kSuites[]` in
# tests/cpp/main.cpp (what the test binary can run) and MIKAI_TEST_SUITES in
# cmake/MikaiTests.cmake (what ctest generates as mikai_tests_<Suite>).
# A suite added to one and not the other either never runs under ctest or
# fails at launch with an unknown-suite error. This script parses main.cpp
# and asserts set equality with the list CMake generated, so the next
# drift fails here with the names attached instead of silently.
#
# It inspects source text only — it never builds or runs the test binary —
# and it does not restructure either registry; order is cosmetic in both.
#
# Run (as ctest does):
#   cmake -DMAIN_CPP=<abs path to tests/cpp/main.cpp>
#         -DEXPECTED_SUITES="TestA,TestB,..."
#         -DWITH_MODEL_INSPECTION=ON|OFF
#         -DWITH_WIN32=ON|OFF
#         -P tests/cmake/SuiteRegistryContracts.cmake
# The expected list is comma-joined on the way in: a ;-list cannot survive
# add_test as one -D value (CMake splits "${list}" into one argument per
# item at generate time), so the caller joins with commas and this script
# splits them back. Suite names never contain a comma.
# Exit 0 = the registries agree; non-zero = a suite is missing from one side.

if(NOT MAIN_CPP OR NOT DEFINED EXPECTED_SUITES)
  message(FATAL_ERROR "SuiteRegistryContracts: MAIN_CPP and EXPECTED_SUITES are required")
endif()

string(REPLACE "," ";" EXPECTED_SUITES "${EXPECTED_SUITES}")

file(READ "${MAIN_CPP}" _main_text)

# The registry is the kSuites[] initializer; everything else in main.cpp
# (declarations, the runner) is not a suite name.
string(FIND "${_main_text}" "kSuites[] = {" _start)
if(_start EQUAL -1)
  message(FATAL_ERROR "SuiteRegistryContracts: kSuites[] not found in ${MAIN_CPP}")
endif()
string(SUBSTRING "${_main_text}" ${_start} -1 _tail)
string(FIND "${_tail}" "};" _end)
if(_end EQUAL -1)
  message(FATAL_ERROR "SuiteRegistryContracts: kSuites[] initializer is unterminated")
endif()
string(SUBSTRING "${_tail}" 0 ${_end} _registry)

string(REGEX MATCHALL "\"Test[^\"]+\"" _quoted "${_registry}")
set(_parsed "")
foreach(_entry IN LISTS _quoted)
  string(REPLACE "\"" "" _name "${_entry}")
  list(APPEND _parsed "${_name}")
endforeach()

# TestNativeCatalog is inside `#ifdef MIKAI_WITH_MODEL_INSPECTION` in
# main.cpp and appended to MIKAI_TEST_SUITES under the same condition, so
# it only counts when the build has inspection enabled.
if(NOT WITH_MODEL_INSPECTION)
  list(REMOVE_ITEM _parsed "TestNativeCatalog")
endif()

# TestWin32Support is inside `#ifdef Q_OS_WIN` in main.cpp and appended to
# MIKAI_TEST_SUITES under `if(WIN32)`, so it only counts on Windows. The
# parser above reads source text, not preprocessed code, so the name is
# present in the text on every platform.
if(NOT WITH_WIN32)
  list(REMOVE_ITEM _parsed "TestWin32Support")
endif()

list(SORT _parsed)
list(SORT EXPECTED_SUITES)

set(_missing "")
foreach(_suite IN LISTS _parsed)
  list(FIND EXPECTED_SUITES "${_suite}" _at)
  if(_at EQUAL -1)
    list(APPEND _missing "${_suite}")
  endif()
endforeach()
set(_extra "")
foreach(_suite IN LISTS EXPECTED_SUITES)
  list(FIND _parsed "${_suite}" _at)
  if(_at EQUAL -1)
    list(APPEND _extra "${_suite}")
  endif()
endforeach()

if(_missing)
  message(SEND_ERROR "SuiteRegistryContracts: suites in tests/cpp/main.cpp "
    "kSuites[] but with no mikai_tests_<Suite> ctest entry: ${_missing}")
endif()
if(_extra)
  message(SEND_ERROR "SuiteRegistryContracts: suites with a "
    "mikai_tests_<Suite> ctest entry but not in tests/cpp/main.cpp "
    "kSuites[]: ${_extra}")
endif()
if(_missing OR _extra)
  message(FATAL_ERROR "SuiteRegistryContracts: suite registries disagree")
endif()

list(LENGTH _parsed _count)
message(STATUS "SuiteRegistryContracts: PASS ${_count} suites agree")
