cmake_minimum_required(VERSION 3.21)

project(SensorLink
    VERSION 0.1.0
    DESCRIPTION "End-to-end telemetry system: shared protocol library, firmware node, epoll server"
    LANGUAGES CXX)

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
add_subdirectory(proto)

if(SENSORLINK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
