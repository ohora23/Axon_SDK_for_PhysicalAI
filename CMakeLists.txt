cmake_minimum_required(VERSION 3.22)
project(axon
    VERSION 0.0.1
    DESCRIPTION "Data-centric zero-copy for Physical AI"
    LANGUAGES C CXX)

# Options
option(AXON_BUILD_EXAMPLES   "Build example programs (spike PoC, demos)" ON)
option(AXON_BUILD_PYTHON     "Build Python bindings (pybind11)"          OFF)
option(AXON_ENABLE_AMD_XDNA  "Enable AMD XDNA NPU backend"               OFF)
option(AXON_ENABLE_JETSON    "Enable NVIDIA Jetson Orin backend"         OFF)
option(AXON_ENABLE_RT        "Enable RT-Linux specific code paths"       ON)
option(AXON_BUILD_TESTS      "Build unit and integration tests"          ON)
option(AXON_WITH_ICEORYX2    "Use the Iceoryx2 metadata-plane backend"   OFF)
option(AXON_WITH_CUDA        "Enable the CUDA VMM accelerator pool backend" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Warning level
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)

# Platform check
if(NOT UNIX OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "axon is Linux-only (depends on dma-buf, V4L2, SCM_RIGHTS). Detected: ${CMAKE_SYSTEM_NAME}")
endif()

find_package(Threads REQUIRED)

# Metadata plane backend. The seqlock POSIX-SHM slot (src/metadata_channel.cpp)
# is always built; with -DAXON_WITH_ICEORYX2=ON the real Iceoryx2 lock-free queue
# backend (src/metadata_channel_iox2.cpp) is compiled in and selected at runtime
# via the AXON_METADATA_BACKEND env var (default: iceoryx2 when compiled in).
set(AXON_METADATA_SOURCES src/metadata_channel.cpp)
if(AXON_WITH_ICEORYX2)
    find_package(iceoryx2-cxx CONFIG REQUIRED)
    list(APPEND AXON_METADATA_SOURCES src/metadata_channel_iox2.cpp)
endif()

# Core library.
#   - FD plane      : src/sidecar.cpp          (SCM_RIGHTS / pidfd_getfd)
#   - metadata plane: src/metadata_channel.cpp (seqlock backend + factory)
#                     src/metadata_channel_iox2.cpp (Iceoryx2 backend, optional)
#   - buffer pool   : src/pool.cpp             (Custom / UDMABUF / V4L2)
#   - RT helpers    : src/rt.cpp               (mlockall, SCHED_FIFO, prefault)
#   - pub/sub       : src/publisher.cpp, src/subscriber.cpp
add_library(axon
    src/sidecar.cpp
    ${AXON_METADATA_SOURCES}
    src/pool.cpp
    src/rt.cpp
    src/publisher.cpp
    src/subscriber.cpp)
target_include_directories(axon PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(axon PUBLIC cxx_std_20)
# shm_open/shm_unlink live in librt on glibc; pthreads for std::thread in tests.
target_link_libraries(axon PUBLIC Threads::Threads rt)

if(AXON_WITH_ICEORYX2)
    target_compile_definitions(axon PUBLIC AXON_WITH_ICEORYX2=1)
    target_link_libraries(axon PUBLIC iceoryx2-cxx::static-lib-cxx)
endif()

# CUDA VMM accelerator pool (R6). Driver API only (libcuda) — no CUDA language
# enabled, so a plain g++ toolchain links it via CUDA::cuda_driver.
if(AXON_WITH_CUDA)
    find_package(CUDAToolkit REQUIRED)
    target_sources(axon PRIVATE src/accelerator_cuda.cpp)
    target_compile_definitions(axon PUBLIC AXON_WITH_CUDA=1)
    target_link_libraries(axon PUBLIC CUDA::cuda_driver)
endif()

# Enable testing before descending into subdirectories so that add_test() calls
# in examples/ (e.g. the demo smoke test) and tests/ all register.
if(AXON_BUILD_TESTS)
    enable_testing()
endif()

# Examples
if(AXON_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Python bindings (pybind11)
if(AXON_BUILD_PYTHON)
    add_subdirectory(python)
endif()

# Tests
if(AXON_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Install
install(DIRECTORY include/axon DESTINATION include)

# Summary
message(STATUS "─────────── axon build configuration ───────────")
message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  Examples:          ${AXON_BUILD_EXAMPLES}")
message(STATUS "  Python bindings:   ${AXON_BUILD_PYTHON}")
message(STATUS "  AMD XDNA backend:  ${AXON_ENABLE_AMD_XDNA}")
message(STATUS "  Jetson backend:    ${AXON_ENABLE_JETSON}")
message(STATUS "  RT code paths:     ${AXON_ENABLE_RT}")
message(STATUS "─────────────────────────────────────────────────")
