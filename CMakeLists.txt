cmake_minimum_required(VERSION 3.20)

project(MiniDB
    VERSION 0.1.0
    DESCRIPTION "An educational persistent key-value database engine in modern C++20"
    LANGUAGES CXX)

# --------------------------------------------------------------------------
# Global settings
# --------------------------------------------------------------------------
# C++20 is required, not merely preferred: CXX_EXTENSIONS OFF keeps us on
# standard C++ so the code behaves the same under GCC, Clang and MSVC.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for clangd / clang-tidy / editors.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Single-config generators (Makefiles, Ninja) default to an empty build type,
# which means "no optimisation and no -g". Pick a sane default instead.
get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT is_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

option(MINIDB_BUILD_TESTS "Build the MiniDB test suite" ON)
option(MINIDB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# --------------------------------------------------------------------------
# Warning configuration
#
# Carried as an INTERFACE target so every MiniDB target opts in with a single
# link line, and so the flags never leak to consumers of the project.
# --------------------------------------------------------------------------
add_library(minidb_warnings INTERFACE)

if(MSVC)
    target_compile_options(minidb_warnings INTERFACE
        /W4             # high warning level
        /permissive-    # strict standard conformance
        /utf-8)
    if(MINIDB_WARNINGS_AS_ERRORS)
        target_compile_options(minidb_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(minidb_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow              # a local hiding an outer name is nearly always a bug
        -Wconversion          # silent narrowing corrupts lengths read from disk
        -Wsign-conversion
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Woverloaded-virtual
        -Wnull-dereference
        -Wdouble-promotion)
    if(MINIDB_WARNINGS_AS_ERRORS)
        target_compile_options(minidb_warnings INTERFACE -Werror)
    endif()
endif()

# --------------------------------------------------------------------------
# minidb_core: the database engine, built as a static library.
#
# The engine knows nothing about the CLI. That separation is what lets the
# tests and the benchmarks link against exactly the same code the CLI runs.
# --------------------------------------------------------------------------
add_library(minidb_core STATIC
    src/command_parser.cpp
    src/database.cpp
    src/hash_table.cpp
    src/result.cpp
    src/storage.cpp)

target_include_directories(minidb_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(minidb_core PRIVATE minidb_warnings)

# Single source of truth for the version: the number lives in project() above.
target_compile_definitions(minidb_core PUBLIC MINIDB_VERSION="${PROJECT_VERSION}")

# --------------------------------------------------------------------------
# minidb: the command-line front end
# --------------------------------------------------------------------------
add_executable(minidb cli/main.cpp)
target_link_libraries(minidb PRIVATE minidb_core minidb_warnings)

# --------------------------------------------------------------------------
# Tests
#
# enable_testing() must be called from this top-level file for `ctest` to see
# the tests declared in the subdirectory.
# --------------------------------------------------------------------------
if(MINIDB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
