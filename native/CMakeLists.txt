# Optional MSVC + nvcc build for the AnimeJaNai C ABI shim and out-of-band
# TensorRT preparation/verification helpers (M04 / M15).
#
# Default root builds keep MIKAI_WITH_NATIVE=OFF and never enter this directory.
# See native/build-msvc.md and native/cmake/StageInference.cmake for the
# staging, patch-policy, and reproducibility contracts.

cmake_minimum_required(VERSION 3.25)

# CMAKE_CUDA_ARCHITECTURES must be pinned BEFORE project() so CMake does not
# freeze a single-architecture default while probing nvcc.
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES OR CMAKE_CUDA_ARCHITECTURES STREQUAL "")
  set(CMAKE_CUDA_ARCHITECTURES
      "75-real;80-real;86-real;89-real;90-real;100-real;120-real;120-virtual"
      CACHE STRING "Pinned CUDA architectures for TensorRT/CUDA targets" FORCE)
endif()

project(mikai_native LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR
    "The native shim toolchain (native/CMakeLists.txt) requires Windows and an "
    "MSVC generator (Visual Studio 2022 or Ninja under the MSVC x64 environment). "
    "nvcc on Windows rejects a MinGW host; keep MIKAI_WITH_NATIVE=OFF for the "
    "default MinGW/Qt application build.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(
  QT_DISABLE_DEPRECATED_UP_TO=0x060C00
  QT_WARN_DEPRECATED_UP_TO=0x060D00
)

# ---------------------------------------------------------------------------
# Options and SDK roots (native/build-msvc.md)
# ---------------------------------------------------------------------------
set(AJI_SOURCE_DIR "" CACHE PATH
    "Read-only pinned animejanai-inference source tree (required)")
set(AJI_TRT_ROOT "" CACHE PATH
    "TensorRT 11.3 root containing include/ and lib/")
set(AJI_CUDA_ROOT "$ENV{CUDAToolkit_ROOT}" CACHE PATH
    "CUDA Toolkit root")
set(AJI_ORT_ROOT "" CACHE PATH
    "Optional Microsoft.ML.OnnxRuntime.DirectML package root")
set(AJI_DML_ROOT "" CACHE PATH
    "Optional Microsoft.AI.DirectML package root")

option(AJI_BUILD_TRT "Build the TensorRT backend (aji_trt) and TRT helpers" ON)
option(AJI_BUILD_DML "Build the DirectML backend (aji_dml) when ORT+DML roots are set" ON)
option(AJI_BUILD_HARNESS "Build the upstream research harnesses (EXCLUDE_FROM_ALL)" ON)
option(AJI_RESTAGE "Force a fresh copy of AJI_SOURCE_DIR on this configure" OFF)
option(AJI_REPRODUCIBLE "Add /Brepro and /utf-8 to native MSVC compilations and links" ON)

# TensorRT 11 is the target; never fall back to nvinfer_10.
set(AJI_NVINFER "nvinfer_11" CACHE STRING
    "TensorRT import library name in AJI_TRT_ROOT/lib (must be nvinfer_11)")

set(AJI_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/aji-src" CACHE PATH
    "Where the read-only AJI_SOURCE_DIR tree is staged and patched")
set(AJI_PATCH_POLICY "strict" CACHE STRING
    "Patch application policy for the staged tree: strict, warn, or off")
set_property(CACHE AJI_PATCH_POLICY PROPERTY STRINGS strict warn off)

if(AJI_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR
    "AJI_SOURCE_DIR is required when configuring native/. Pass "
    "-DAJI_SOURCE_DIR=<path-to-animejanai-inference>.")
endif()

if(NOT EXISTS "${AJI_SOURCE_DIR}/src/aji_dispatch.cpp")
  message(FATAL_ERROR
    "AJI_SOURCE_DIR (${AJI_SOURCE_DIR}) does not contain src/aji_dispatch.cpp.")
endif()

# ---------------------------------------------------------------------------
# Stage read-only source and apply native/patches/*.patch
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_LIST_DIR}/cmake/StageInference.cmake")

mikai_stage_inference_source(
  AJI_SOURCE_DIR   "${AJI_SOURCE_DIR}"
  AJI_STAGE_DIR    "${AJI_STAGE_DIR}"
  AJI_PATCH_DIR    "${CMAKE_CURRENT_LIST_DIR}/patches"
  AJI_PATCH_POLICY "${AJI_PATCH_POLICY}"
  AJI_RESTAGE      "${AJI_RESTAGE}"
  OUT_STAGE_VALID  AJI_STAGE_VALID
  OUT_PATCH_STATE  AJI_PATCH_STATE
)

if(AJI_REPRODUCIBLE)
  add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>)
  add_link_options(/Brepro)
endif()

# T-402: MSVC defaults to /W3, which under-reports in a shim whose every
# signature crosses a C ABI boundary. /W4 raises the bar for the whole native
# directory (targets configured below this point) without /WX: warnings are
# surfaced for review, not promoted to build breakers, because the staged
# upstream sources this tree patches are not warning-clean at /W4 and
# breaking on them would couple the build to foreign code's hygiene.
if(MSVC)
  add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/W4>)
endif()

# ---------------------------------------------------------------------------
# CUDA and TensorRT discovery
# ---------------------------------------------------------------------------
if(AJI_CUDA_ROOT AND NOT CUDAToolkit_ROOT)
  set(CUDAToolkit_ROOT "${AJI_CUDA_ROOT}")
endif()

if(AJI_BUILD_TRT OR AJI_BUILD_HARNESS)
  enable_language(CUDA)
  set(CMAKE_CUDA_STANDARD 17)
  find_package(CUDAToolkit REQUIRED)      # the optional MSVC/nvcc shim, OFF by default
endif()

# ---------------------------------------------------------------------------
# 1. aji (SHARED dispatcher)
# ---------------------------------------------------------------------------
add_library(aji SHARED
  "${AJI_STAGE_DIR}/src/aji_dispatch.cpp"
  "${AJI_STAGE_DIR}/src/aji_conf.cpp"
)
target_include_directories(aji PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${AJI_STAGE_DIR}/include"
  "${AJI_STAGE_DIR}/src"
)

# ---------------------------------------------------------------------------
# 2. aji_trt (SHARED TensorRT/CUDA backend)
# ---------------------------------------------------------------------------
if(AJI_BUILD_TRT)
  if(AJI_TRT_ROOT STREQUAL "")
    message(FATAL_ERROR "AJI_TRT_ROOT is required when AJI_BUILD_TRT=ON.")
  endif()
  find_library(AJI_NVINFER_LIB NAMES "${AJI_NVINFER}"
    PATHS "${AJI_TRT_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
  find_library(AJI_NVONNXPARSER_LIB NAMES nvonnxparser_11 nvonnxparser
    PATHS "${AJI_TRT_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)

  add_library(aji_trt SHARED
    "${AJI_STAGE_DIR}/src/aji_trt.cpp"
    "${AJI_STAGE_DIR}/src/aji_conf.cpp"
    "${AJI_STAGE_DIR}/src/kernels.cu"
  )
  target_include_directories(aji_trt PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${AJI_STAGE_DIR}/include"
    "${AJI_STAGE_DIR}/src"
    "${AJI_TRT_ROOT}/include"
  )
  target_link_libraries(aji_trt PRIVATE
    "${AJI_NVINFER_LIB}"
    CUDA::cuda_driver
    CUDA::cudart
  )

  # Out-of-band preparation helper CLI (M15)
  add_executable(mikai_native_prep EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_LIST_DIR}/tools/mikai_native_prep.cpp"
  )
  target_include_directories(mikai_native_prep PRIVATE
    "${AJI_TRT_ROOT}/include"
  )
  target_link_libraries(mikai_native_prep PRIVATE
    "${AJI_NVINFER_LIB}"
    "${AJI_NVONNXPARSER_LIB}"
    CUDA::cudart
    bcrypt
    version
  )

  # Out-of-band numerical/binding verifier CLI
  add_executable(mikai_native_verify EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_LIST_DIR}/tools/mikai_native_verify.cpp"
    "${AJI_STAGE_DIR}/src/kernels.cu"
  )
  target_include_directories(mikai_native_verify PRIVATE
    "${AJI_STAGE_DIR}/src"
    "${AJI_TRT_ROOT}/include"
  )
  target_link_libraries(mikai_native_verify PRIVATE
    "${AJI_NVINFER_LIB}"
    CUDA::cuda_driver
    CUDA::cudart
    bcrypt
  )

  # Prepared runtime probe
  add_executable(native_prepared_runtime_probe EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_LIST_DIR}/src/NativePreparedRuntimeProbe.cpp"
  )
  target_include_directories(native_prepared_runtime_probe PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${AJI_STAGE_DIR}/include"
  )
  target_link_libraries(native_prepared_runtime_probe PRIVATE
    CUDA::cuda_driver
    CUDA::cudart
  )
endif()

# ---------------------------------------------------------------------------
# 3. aji_dml (SHARED DirectML backend, optional)
# ---------------------------------------------------------------------------
if(AJI_BUILD_DML AND NOT AJI_ORT_ROOT STREQUAL "" AND NOT AJI_DML_ROOT STREQUAL "")
  add_library(aji_dml SHARED
    "${AJI_STAGE_DIR}/src/aji_dml.cpp"
    "${AJI_STAGE_DIR}/src/aji_conf.cpp"
  )
  target_include_directories(aji_dml PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${AJI_STAGE_DIR}/include"
    "${AJI_STAGE_DIR}/src"
    "${AJI_ORT_ROOT}/build/native/include"
    "${AJI_DML_ROOT}/include"
  )
  target_link_libraries(aji_dml PRIVATE
    d3d12
    d3d11
    dxgi
    d3dcompiler
  )
endif()

# ---------------------------------------------------------------------------
# 4. SDK-independent ABI v2 probe & optional upstream harnesses
# ---------------------------------------------------------------------------
add_executable(native_abi_probe EXCLUDE_FROM_ALL
  "${CMAKE_CURRENT_LIST_DIR}/tools/native_abi_probe.cpp"
)
target_include_directories(native_abi_probe PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${AJI_STAGE_DIR}/include"
)

if(AJI_BUILD_HARNESS)
  if(EXISTS "${AJI_STAGE_DIR}/tools/aji_harness.cpp")
    add_executable(aji_harness EXCLUDE_FROM_ALL
      "${AJI_STAGE_DIR}/tools/aji_harness.cpp"
    )
    target_include_directories(aji_harness PRIVATE "${AJI_STAGE_DIR}/include")
    target_link_libraries(aji_harness PRIVATE aji CUDA::cudart)
  endif()
  if(EXISTS "${AJI_STAGE_DIR}/tools/aji_kernel_test.cu")
    add_executable(aji_kernel_test EXCLUDE_FROM_ALL
      "${AJI_STAGE_DIR}/tools/aji_kernel_test.cu"
      "${AJI_STAGE_DIR}/src/kernels.cu"
    )
    target_include_directories(aji_kernel_test PRIVATE "${AJI_STAGE_DIR}/src")
    target_link_libraries(aji_kernel_test PRIVATE CUDA::cudart)
  endif()
endif()
