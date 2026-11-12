CPMAddPackage(
    NAME Catch2
    GITHUB_REPOSITORY catchorg/Catch2
    VERSION 3.7.1)

# Test files mirror src/: tests/<module>/<Name>Tests.cpp
add_executable(vloom_engine_tests
    EngineTests.cpp
    core/SpscQueueTests.cpp
    io/AudioDeviceTests.cpp
    render/WavWriterTests.cpp
    time/MusicalTimeTests.cpp
    time/TransportTests.cpp)

target_include_directories(vloom_engine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vloom_engine_tests PRIVATE vloom::engine Catch2::Catch2WithMain)
vloom_set_warnings(vloom_engine_tests)

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(vloom_engine_tests)
