cmake_minimum_required(VERSION 3.25)

project(nstu
    VERSION 0.1.0
    DESCRIPTION "Low-latency classroom lab management and screen broadcasting"
    LANGUAGES CXX
)

option(NSTU_BUILD_CLIENT "Build the Windows client" ON)
option(NSTU_BUILD_SERVER "Build the server UI" ON)
option(NSTU_BUILD_TESTS "Build unit and integration tests" ON)
option(NSTU_BUILD_VIDEO "Build DXGI and Media Foundation components" ON)
option(NSTU_ENABLE_WERROR "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(NstuWarnings)

add_subdirectory(common)

if(WIN32 AND NSTU_BUILD_VIDEO)
    add_subdirectory(video)
endif()

if(NSTU_BUILD_CLIENT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/client/CMakeLists.txt")
    add_subdirectory(client)
endif()

if(NSTU_BUILD_SERVER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/server/CMakeLists.txt")
    add_subdirectory(server)
endif()

if(NSTU_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
