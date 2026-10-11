# Optional native AnimeJaNai inference build (M04).
#
# This is a SECOND, self-contained toolchain project: MSVC + nvcc (C, CXX17,
# CUDA) producing the ABI-8 shim the MinGW/Qt GUI loads as `lib=`
# (aji.dll / aji_trt.dll / aji_dml.dll). The GUI never links these; it only
# speaks their C ABI. Nothing in here is part of the default build.
#
# Standalone (the documented route, see build-msvc.md):
#   cmake -S native -B native/build -G "Visual Studio 17 2022" \
#     -DAJI_SOURCE_DIR=C:/Users/myhli/Downloads/janai/animejanai-inference-main \
#     -DAJI_TRT_ROOT=F:/ZKS/mikai-native-sdks/tensorrt-11.3.0.99/TensorRT-11.3.0.99 \
#     -DAJI_CUDA_ROOT=F:/ZKS/mikai-native-sdks/cuda-13.4.2 \
#     -DAJI_NVINFER=nvinfer_11
#   cmake --build native/build --config Release
#
# Or through the root project, MSVC generator only:
#   cmake -S . -B build-native -DMIKAI_WITH_NATIVE=ON ...
#
# The pinned inference tree is READ-ONLY input. It is never edited in place:
# it is staged into the build directory, the patch series in native/patches/
# is applied to the staged copy, and the targets compile the staged copy.
# An unpatched shim is never silently produced - see AJI_PATCH_POLICY below.

cmake_minimum_required(VERSION 3.24)

# CMAKE_CUDA_ARCHITECTURES has to be settled before project()/enable_language(),
# because that is when CMake probes nvcc and freezes the architecture list. The
# upstream fallback is guarded by if(NOT DEFINED ...), and CMake pre-defines the
# variable during project(), so that fallback never fires and omitting the flag
# silently yields a single-architecture binary that dies on every other GPU.
# Pinned value: native/manifest/sources.json#CMAKE_CUDA_ARCHITECTURES.
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES "75-real;80-real;86-real;89-real;90-real;100-real;120-real;120-virtual")
endif()

# AJI_CUDA_ROOT arrives as a cache variable, so a -D on the command line is
# already set by the time project() runs and has to be forwarded before the
# nvcc probe rather than after it.
if(AJI_CUDA_ROOT AND NOT CUDAToolkit_ROOT)
  set(CUDAToolkit_ROOT "${AJI_CUDA_ROOT}")
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(MIKAI_NATIVE_STANDALONE TRUE)
  project(mikai_native LANGUAGES C CXX CUDA)
else()
  # Reached only through -DMIKAI_WITH_NATIVE=ON in the root project, where the
  # languages are already fixed by the parent and CUDA has to be opted into.
  set(MIKAI_NATIVE_STANDALONE FALSE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

set(AJI_SOURCE_DIR "" CACHE PATH
  "Read-only animejanai-inference source tree. Build input is the official v0.9.0 tag baseline c7f56baf966064a7855db2f82a2f1b0d0c6cfdb3 plus authorized mutable local edits; 738ce3b3020b3950c48ce3122a08f32af531d4fa is the historical baseline comparison only")
set(AJI_TRT_ROOT "" CACHE PATH
  "TensorRT root: the directory holding include/ and lib/nvinfer_11.lib")
set(AJI_NVINFER "nvinfer_11" CACHE STRING
  "nvinfer import library name (nvinfer_11 / nvinfer_10). TensorRT 11 is the pin; TensorRT 10 is not a dependency")
set(AJI_CUDA_ROOT "" CACHE PATH
  "CUDA Toolkit root (contains bin/nvcc.exe). Falls back to CUDAToolkit_ROOT")
set(AJI_ORT_ROOT "" CACHE PATH
  "Microsoft.ML.OnnxRuntime.DirectML package root, for aji_dml")
set(AJI_DML_ROOT "" CACHE PATH
  "Microsoft.AI.DirectML package root, for aji_dml")
set(AJI_PATCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/patches" CACHE PATH
  "Patch series applied to the staged copy of AJI_SOURCE_DIR")
set(AJI_PATCH_POLICY "strict" CACHE STRING
  "strict (default): refuse to configure unless a NONEMPTY applicable series applies IN FULL; warn: configure anyway and warn loudly, with no full-series stamp unless every patch applied; off: never apply the series")
set_property(CACHE AJI_PATCH_POLICY PROPERTY STRINGS strict warn off)
set(AJI_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/aji-src" CACHE PATH
  "Where the read-only source tree is staged before patching. MUST NOT equal AJI_SOURCE_DIR, contain it or sit inside it - any overlap is refused before anything is removed. Only a positively owned stage directory is ever deleted")
option(AJI_RESTAGE "Force a fresh copy of AJI_SOURCE_DIR into AJI_STAGE_DIR on this configure" OFF)
option(AJI_BUILD_TRT "Build the TensorRT backend (aji_trt)" ON)
option(AJI_BUILD_DML "Build the DirectML backend (aji_dml) when its roots are set" ON)
option(AJI_BUILD_HARNESS "Build the aji_harness / aji_kernel_test verification targets" ON)
option(AJI_REPRODUCIBLE "Apply the deterministic-link flags MSVC needs for byte-identical Release DLLs" ON)

# ---------------------------------------------------------------------------
# Toolchain preconditions
# ---------------------------------------------------------------------------

if(NOT WIN32)
  message(FATAL_ERROR
    "The native shim is a Windows (MSVC + nvcc) build. Configure it on Windows; "
    "the MinGW/Qt GUI build does not need it.")
endif()

if(NOT MSVC)
  if(MIKAI_NATIVE_STANDALONE)
    message(FATAL_ERROR
      "The native project must be configured with an MSVC generator: "
      "-G \"Visual Studio 17 2022\". See build-msvc.md.")
  endif()
  message(FATAL_ERROR
    "MIKAI_WITH_NATIVE=ON was configured with a non-MSVC compiler. nvcc on Windows "
    "requires an MSVC host compiler, so the native shim cannot be built inside the "
    "MinGW/Qt build tree. Configure native/ standalone (build-msvc.md), or drop "
    "MIKAI_WITH_NATIVE to get the default build back - the default is OFF and is "
    "unaffected by anything in this file.")
endif()

if(NOT MIKAI_NATIVE_STANDALONE)
  enable_language(CUDA)
endif()

find_package(CUDAToolkit REQUIRED)

if(AJI_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR
    "AJI_SOURCE_DIR is required: point it at the pinned animejanai-inference tree. "
    "The build input is the official v0.9.0 tag baseline at "
    "c7f56baf966064a7855db2f82a2f1b0d0c6cfdb3 plus authorized mutable local edits; "
    "the historical 738ce3b3020b3950c48ce3122a08f32af531d4fa baseline comparison "
    "predates those edits. See build-msvc.md.")
endif()
get_filename_component(AJI_SOURCE_DIR "${AJI_SOURCE_DIR}" ABSOLUTE)
if(NOT EXISTS "${AJI_SOURCE_DIR}/src/aji_dispatch.cpp")
  message(FATAL_ERROR
    "AJI_SOURCE_DIR does not look like animejanai-inference: "
    "${AJI_SOURCE_DIR}/src/aji_dispatch.cpp is missing.")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/cmake/StageInference.cmake")
  message(FATAL_ERROR
    "native/cmake/StageInference.cmake is missing: the staging and patch-policy "
    "contract lives there and is exercised by tests/cmake/"
    "NativeStageContracts.cmake. Restore that file.")
endif()

if(AJI_BUILD_TRT)
  if(AJI_TRT_ROOT STREQUAL "")
    message(FATAL_ERROR
      "AJI_TRT_ROOT is required for aji_trt. It must name the TensorRT tree that "
      "holds include/NvInfer.h and lib/nvinfer_11.lib. See build-msvc.md.")
  endif()
  get_filename_component(AJI_TRT_ROOT "${AJI_TRT_ROOT}" ABSOLUTE)
  if(NOT EXISTS "${AJI_TRT_ROOT}/include/NvInfer.h")
    message(FATAL_ERROR "AJI_TRT_ROOT/include/NvInfer.h not found: ${AJI_TRT_ROOT}")
  endif()
  if(NOT EXISTS "${AJI_TRT_ROOT}/lib/${AJI_NVINFER}.lib")
    message(FATAL_ERROR
      "AJI_TRT_ROOT/lib/${AJI_NVINFER}.lib not found: ${AJI_TRT_ROOT}. Either pass "
      "-DAJI_TRT_ROOT=<TensorRT 11.3 tree> or set -DAJI_NVINFER to the import library "
      "that tree actually ships; the TensorRT 10 default must not be relied on silently.")
  endif()
endif()

# ---------------------------------------------------------------------------
# Stage the read-only source, then apply the patch series to the staged copy.
#
# The whole contract - overlap refusal, ownership-gated deletion, complete
# content fingerprint and the strict/warn/off truthfulness rules - lives in
# native/cmake/StageInference.cmake, which is a production helper shared with
# tests/cmake/NativeStageContracts.cmake. That script exercises the same code
# paths against synthetic trees, so the behaviour configured here is the
# behaviour that is tested.

file(GLOB AJI_PATCH_FILES "${AJI_PATCH_DIR}/*.patch")
list(SORT AJI_PATCH_FILES)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/StageInference.cmake")

mikai_stage_inference_source(
  AJI_SOURCE_DIR  "${AJI_SOURCE_DIR}"
  AJI_STAGE_DIR   "${AJI_STAGE_DIR}"
  AJI_PATCH_DIR   "${AJI_PATCH_DIR}"
  AJI_PATCH_POLICY "${AJI_PATCH_POLICY}"
  AJI_RESTAGE     "${AJI_RESTAGE}"
  OUT_STAGE_VALID _MikaiStageValid
  OUT_PATCH_STATE _MikaiPatchState)

# A stage that was not produced is not a source tree: the targets below must
# not silently compile whatever happens to be sitting at AJI_STAGE_DIR.
if(NOT _MikaiStageValid)
  message(FATAL_ERROR
    "The staged tree at ${AJI_STAGE_DIR} is not valid (patch state "
    "'${_MikaiPatchState}'); the shim cannot be built from it.")
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

set(AJI_INCLUDE "${AJI_STAGE_DIR}/include")
set(AJI_SRC "${AJI_STAGE_DIR}/src")

# aji: the thin dispatcher the mpv filter loads as lib=. No CUDA/TensorRT/ORT
# dependency of its own; it forwards the C ABI to the sibling backend.
add_library(aji SHARED
  ${AJI_SRC}/aji_dispatch.cpp
  ${AJI_SRC}/aji_conf.cpp
)
target_include_directories(aji PUBLIC ${AJI_INCLUDE}
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aji PRIVATE ${CMAKE_DL_LIBS})
set_target_properties(aji PROPERTIES
  C_STANDARD 11
  CXX_STANDARD 17
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(aji PRIVATE AJI_NATIVE_BUILD)

if(AJI_BUILD_TRT)
  # aji_trt: TensorRT/CUDA backend (the former aji.dll). kernels.cu is what
  # satisfies the aji_fill_*/aji_run_* references in aji_trt.cpp.
  add_library(aji_trt SHARED
    ${AJI_SRC}/aji_trt.cpp
    ${AJI_SRC}/aji_conf.cpp
    ${AJI_SRC}/kernels.cu
  )
  target_include_directories(aji_trt
    PUBLIC ${AJI_INCLUDE} "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${AJI_TRT_ROOT}/include")
  target_link_directories(aji_trt PRIVATE "${AJI_TRT_ROOT}/lib")
  target_link_libraries(aji_trt PRIVATE
    ${AJI_NVINFER}
    CUDA::cuda_driver
    CUDA::cudart
    bcrypt)
  set_target_properties(aji_trt PROPERTIES
    CXX_STANDARD 17
    CUDA_STANDARD 17
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    CUDA_VISIBILITY_PRESET hidden)
endif()

if(AJI_BUILD_DML)
  # aji_dml: DirectML backend. DirectML.dll and onnxruntime.dll are loaded from
  # the shim's own directory at run time, so nothing is import-linked.
  if(AJI_ORT_ROOT STREQUAL "" OR AJI_DML_ROOT STREQUAL "")
    message(STATUS "AJI_ORT_ROOT/AJI_DML_ROOT not both set; skipping the aji_dml target.")
  else()
    add_library(aji_dml SHARED
      ${AJI_SRC}/aji_dml.cpp
      ${AJI_SRC}/aji_conf.cpp
    )
    target_include_directories(aji_dml
      PUBLIC ${AJI_INCLUDE} "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${AJI_ORT_ROOT}/build/native/include"
        "${AJI_DML_ROOT}/include")
    target_link_libraries(aji_dml PRIVATE d3d12 d3d11 dxgi d3dcompiler bcrypt)
    set_target_properties(aji_dml PROPERTIES
      CXX_STANDARD 17
      CXX_VISIBILITY_PRESET hidden)
  endif()
endif()

# This probe deliberately loads the produced dispatcher DLL and exercises its
# exported ABI; it has no Qt, CUDA, or TensorRT link dependency.
add_executable(native_abi_probe EXCLUDE_FROM_ALL
  ${CMAKE_CURRENT_SOURCE_DIR}/tools/native_abi_probe.cpp)
target_include_directories(native_abi_probe PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  ${AJI_INCLUDE})
set_target_properties(native_abi_probe PROPERTIES CXX_STANDARD 17)

add_executable(native_prepared_runtime_probe EXCLUDE_FROM_ALL
  ${CMAKE_CURRENT_SOURCE_DIR}/src/NativePreparedRuntimeProbe.cpp)
target_include_directories(native_prepared_runtime_probe PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  ${AJI_INCLUDE})
target_link_libraries(native_prepared_runtime_probe PRIVATE bcrypt)
set_target_properties(native_prepared_runtime_probe PROPERTIES CXX_STANDARD 17)

# Out-of-band executables keep the GPU SDK off the Qt application's link path.
if(AJI_BUILD_TRT)
  find_library(MIKAI_NVONNXPARSER_LIBRARY NAMES nvonnxparser_11
    PATHS "${AJI_TRT_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
  add_executable(mikai_native_prep EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/mikai_native_prep.cpp)
  target_include_directories(mikai_native_prep PRIVATE
    "${AJI_TRT_ROOT}/include" ${CUDAToolkit_INCLUDE_DIRS})
  target_link_directories(mikai_native_prep PRIVATE "${AJI_TRT_ROOT}/lib")
  target_link_libraries(mikai_native_prep PRIVATE
    ${AJI_NVINFER} ${MIKAI_NVONNXPARSER_LIBRARY}
    CUDA::cudart bcrypt version)
  set_target_properties(mikai_native_prep PROPERTIES CXX_STANDARD 17)

  add_executable(mikai_native_verify EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/mikai_native_verify.cpp)
  target_include_directories(mikai_native_verify PRIVATE
    "${AJI_TRT_ROOT}/include"
    ${CUDAToolkit_INCLUDE_DIRS})
  target_link_directories(mikai_native_verify PRIVATE "${AJI_TRT_ROOT}/lib")
  target_link_libraries(mikai_native_verify PRIVATE
    ${AJI_NVINFER}
    CUDA::cuda_driver
    CUDA::cudart
    bcrypt)
  set_target_properties(mikai_native_verify PROPERTIES
    CXX_STANDARD 17
    CXX_VISIBILITY_PRESET hidden)
endif()

if(AJI_BUILD_HARNESS)
  # The upstream self-checks named in M04's verification. The .manifest pins
  # <activeCodePage>UTF-8</activeCodePage> so non-ASCII paths survive argv.
  add_executable(aji_harness EXCLUDE_FROM_ALL
    ${AJI_SRC}/harness.c
    ${AJI_SRC}/utf8-argv.manifest)
  target_link_libraries(aji_harness PRIVATE aji CUDA::cudart)
  set_target_properties(aji_harness PROPERTIES C_STANDARD 11)
  if(TARGET aji_trt)
    add_dependencies(aji_harness aji_trt)
  endif()

  # Compiles the kernels directly; no TensorRT dependency.
  add_executable(aji_kernel_test EXCLUDE_FROM_ALL
    ${AJI_SRC}/kernel_test.c
    ${AJI_SRC}/kernels.cu)
  target_include_directories(aji_kernel_test PRIVATE ${AJI_INCLUDE} ${AJI_SRC})
  target_link_libraries(aji_kernel_test PRIVATE CUDA::cudart)
  set_target_properties(aji_kernel_test PROPERTIES C_STANDARD 11)
endif()

# ---------------------------------------------------------------------------
# Reproducibility
# ---------------------------------------------------------------------------

# M04's acceptance is two consecutive clean builds producing byte-identical
# aji.dll / aji_trt.dll. MSVC stamps the linker with a build time unless
# /Brepro is given; that is handled here rather than explained away afterwards.
foreach(_t aji aji_trt aji_dml)
  if(TARGET ${_t} AND AJI_REPRODUCIBLE)
    target_link_options(${_t} PRIVATE /Brepro)
    # Only cl sees /utf-8 - nvcc rejects it - so CUDA sources are excluded from
    # the generator expression on purpose.
    target_compile_options(${_t} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
  endif()
endforeach()

message(STATUS "")
message(STATUS "mikai native (M04) build")
message(STATUS "  standalone     : ${MIKAI_NATIVE_STANDALONE}")
message(STATUS "  AJI_SOURCE_DIR : ${AJI_SOURCE_DIR}")
message(STATUS "  staged tree    : ${AJI_STAGE_DIR}")
message(STATUS "  patch series   : ${AJI_PATCH_FILES} (policy=${AJI_PATCH_POLICY})")
message(STATUS "  AJI_TRT_ROOT   : ${AJI_TRT_ROOT}")
message(STATUS "  AJI_NVINFER    : ${AJI_NVINFER}")
message(STATUS "  CUDA root      : ${CUDAToolkit_ROOT}")
message(STATUS "  architectures  : ${CMAKE_CUDA_ARCHITECTURES}")
message(STATUS "")
