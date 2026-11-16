# The plugin bridge (ADR 0011): one source file with no dependency on the
# engine library, so it can also be built on its own for another
# architecture than the engine's:
#
#   cmake -S tools/vloom-plugin-bridge -B build/bridge32 -A Win32
#   cmake --build build/bridge32 --config Release
#
# Built that way (for 32 bits) it is named vloom-plugin-bridge32, the file
# the engine looks for next to the application when it meets a 32-bit plugin.
cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vloom_plugin_bridge LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

if(NOT WIN32)
    message(FATAL_ERROR "vloom-plugin-bridge is for Windows only")
endif()

add_executable(vloom-plugin-bridge main.cpp)
target_include_directories(vloom-plugin-bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_compile_features(vloom-plugin-bridge PRIVATE cxx_std_20)
target_link_libraries(vloom-plugin-bridge PRIVATE avrt ole32 user32)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set_target_properties(vloom-plugin-bridge PROPERTIES OUTPUT_NAME "vloom-plugin-bridge32")
endif()
if(COMMAND vloom_set_warnings)
    vloom_set_warnings(vloom-plugin-bridge)
endif()
