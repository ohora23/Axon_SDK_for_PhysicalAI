cmake_minimum_required(VERSION 3.22)
project(dczc
    VERSION 0.0.1
    DESCRIPTION "Data-centric zero-copy for Physical AI"
    LANGUAGES C CXX)

# Options
option(DCZC_BUILD_EXAMPLES   "Build example programs (spike PoC, demos)" ON)
option(DCZC_BUILD_PYTHON     "Build Python bindings (pybind11)"          OFF)
option(DCZC_ENABLE_AMD_XDNA  "Enable AMD XDNA NPU backend"               OFF)
option(DCZC_ENABLE_JETSON    "Enable NVIDIA Jetson Orin backend"         OFF)
option(DCZC_ENABLE_RT        "Enable RT-Linux specific code paths"       ON)
option(DCZC_BUILD_TESTS      "Build unit and integration tests"          ON)

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
    message(FATAL_ERROR "dczc is Linux-only (depends on dma-buf, V4L2, SCM_RIGHTS). Detected: ${CMAKE_SYSTEM_NAME}")
endif()

# Dependencies (currently a placeholder — Iceoryx2 will be wired in after the spike PoC).
# find_package(iceoryx2 REQUIRED)

# Core library (currently header-only skeleton).
add_library(dczc INTERFACE)
target_include_directories(dczc INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(dczc INTERFACE cxx_std_20)

# Examples
if(DCZC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Tests (placeholder — will land in week 4+).
# if(DCZC_BUILD_TESTS)
#     enable_testing()
#     add_subdirectory(tests)
# endif()

# Install
install(DIRECTORY include/dczc DESTINATION include)

# Summary
message(STATUS "─────────── dczc build configuration ───────────")
message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  Examples:          ${DCZC_BUILD_EXAMPLES}")
message(STATUS "  Python bindings:   ${DCZC_BUILD_PYTHON}")
message(STATUS "  AMD XDNA backend:  ${DCZC_ENABLE_AMD_XDNA}")
message(STATUS "  Jetson backend:    ${DCZC_ENABLE_JETSON}")
message(STATUS "  RT code paths:     ${DCZC_ENABLE_RT}")
message(STATUS "─────────────────────────────────────────────────")
