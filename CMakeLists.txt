cmake_minimum_required(VERSION 3.21)

project(SensorLink
    VERSION 0.1.0
    DESCRIPTION "End-to-end telemetry system: shared protocol library, firmware node, epoll server"
    LANGUAGES CXX C)  # C is needed for the vendored FreeRTOS-Kernel sources under node/

# --- Global C++ settings -----------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# --- Options ---------------------------------------------------------------
option(SENSORLINK_BUILD_TESTS "Build the host unit tests" ON)
option(SENSORLINK_COVERAGE "Instrument the tests for coverage (gcov)" OFF)

if(SENSORLINK_COVERAGE)
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

# --- Shared warning flags (opt-in per target) --------------------------------
# INTERFACE target: carries no code, only compile options that consumers inherit.
add_library(sensorlink_warnings INTERFACE)
target_compile_options(sensorlink_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion)

# --- Subdirectories --------------------------------------------------------
# proto/ is header-only and compiles unchanged for both targets -- it is the
# one piece of the repository both the firmware and the host build share, so
# it is configured unconditionally, before the split below.
add_subdirectory(proto)

# CMAKE_CROSSCOMPILING is set by CMake itself once the arm-none-eabi toolchain
# file (cmake/arm-none-eabi.cmake) is in effect. The firmware and the host
# build are two different targets built from two different invocations of
# CMake (see the "host" and "firmware" presets) -- never both in one
# configure, since GoogleTest and the epoll-based server code are not things
# a bare-metal Cortex-M3 build can produce.
if(CMAKE_CROSSCOMPILING)
    add_subdirectory(node)
else()
    add_subdirectory(server)
    add_subdirectory(client)
    add_subdirectory(bench)
    if(SENSORLINK_BUILD_TESTS)
        enable_testing()
        add_subdirectory(tests)
    endif()
endif()
