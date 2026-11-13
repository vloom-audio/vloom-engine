# Pinned to the commit of the v3.7.1 tag (tags can be moved).
CPMAddPackage(
    NAME Catch2
    GITHUB_REPOSITORY catchorg/Catch2
    VERSION 3.7.1
    GIT_TAG fa43b77429ba76c462b1898d6cd2f2d7a9416b14
    GIT_SHALLOW FALSE)

# Test files mirror src/: tests/<module>/<Name>Tests.cpp
add_executable(vloom_engine_tests
    EngineTests.cpp
    PreviewTests.cpp
    RobustnessTests.cpp
    core/SnapshotExchangeTests.cpp
    core/SpscQueueTests.cpp
    dsp/SamplerTests.cpp
    dsp/SynthTests.cpp
    io/AudioDeviceTests.cpp
    mixer/MixerTests.cpp
    render/WavWriterTests.cpp
    sample/ResamplerTests.cpp
    sample/SampleLoaderTests.cpp
    sample/WavReaderTests.cpp
    sequencer/NoteTests.cpp
    sequencer/SongModeTests.cpp
    sequencer/StepSequencerTests.cpp
    time/MusicalTimeTests.cpp
    time/TransportTests.cpp
    support/Allocations.cpp
    support/AllocationsTests.cpp)

target_include_directories(vloom_engine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vloom_engine_tests PRIVATE vloom::engine Catch2::Catch2WithMain)
vloom_set_warnings(vloom_engine_tests)

# Catch2's TEST_CASE uses __COUNTER__, which Clang 23+ reports as a C2y
# extension under -Wpedantic. Silence it where the flag exists.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wno-c2y-extensions VLOOM_HAS_WNO_C2Y_EXTENSIONS)
if(VLOOM_HAS_WNO_C2Y_EXTENSIONS)
    target_compile_options(vloom_engine_tests PRIVATE -Wno-c2y-extensions)
endif()

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(vloom_engine_tests)
