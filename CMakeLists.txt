cmake_minimum_required(VERSION 3.22)

project(vloom_engine
    VERSION 0.1.0
    DESCRIPTION "Real-time audio engine for VloomDAW"
    HOMEPAGE_URL "https://github.com/vloom-audio/vloom-engine"
    LANGUAGES C CXX)

# Tests and tools are on by default only when this is the top-level project,
# so VloomDAW (which adds the engine as a subdirectory) doesn't build them.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(VLOOM_ENGINE_IS_TOP_LEVEL ON)
else()
    set(VLOOM_ENGINE_IS_TOP_LEVEL OFF)
endif()

option(VLOOM_ENGINE_BUILD_TESTS "Build vloom-engine unit tests" ${VLOOM_ENGINE_IS_TOP_LEVEL})
option(VLOOM_ENGINE_BUILD_TOOLS "Build the vloom-cli tool" ${VLOOM_ENGINE_IS_TOP_LEVEL})

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/CPM.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/CompilerWarnings.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/Sanitizers.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/Dependencies.cmake)

add_subdirectory(src)

if(VLOOM_ENGINE_BUILD_TOOLS)
    add_subdirectory(tools/vloom-cli)
endif()

if(VLOOM_ENGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
