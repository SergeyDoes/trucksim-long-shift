cmake_minimum_required(VERSION 3.22)
project(long_shift VERSION 1.0.0 LANGUAGES CXX RC)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Long Shift builds only with MSVC for x64: ATS/ETS2 load Windows x64 plugin DLLs")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT (/MT): the plugin must not depend on the VC++ redistributable being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(SCS_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/scs_sdk" CACHE PATH "Official SCS SDK (1.14+, with Input API)")
if(NOT EXISTS "${SCS_SDK_DIR}/include/scssdk_input.h")
  message(FATAL_ERROR "SCS SDK with Input API not found in ${SCS_SDK_DIR}")
endif()

add_library(scs_sdk INTERFACE)
target_include_directories(scs_sdk SYSTEM INTERFACE "${SCS_SDK_DIR}/include")

add_library(long_shift_options INTERFACE)
target_compile_options(long_shift_options INTERFACE /W4 /permissive- /utf-8)
target_compile_definitions(long_shift_options INTERFACE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# For scripts/package.ps1 and the release workflow.
file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${PROJECT_VERSION}")

enable_testing()

add_subdirectory(src/core)
add_subdirectory(src/plugin)
add_subdirectory(tests)
add_subdirectory(tools)
