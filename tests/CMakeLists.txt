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
    LoopTests.cpp
    PreviewTests.cpp
    RobustnessTests.cpp
    core/SnapshotExchangeTests.cpp
    core/SpscQueueTests.cpp
    dsp/SamplerTests.cpp
    dsp/SynthTests.cpp
    io/AudioDeviceTests.cpp
    mixer/MixerTests.cpp
    mixer/RoutingTests.cpp
    plugin/BuiltinProcessorsTests.cpp
    plugin/ClapHostTests.cpp
    plugin/PluginHostTests.cpp
    plugin/Vst3HostTests.cpp
    render/SongExportTests.cpp
    render/WavWriterTests.cpp
    sample/ResamplerTests.cpp
    sample/SampleLoaderTests.cpp
    sample/WavReaderTests.cpp
    sequencer/NoteTests.cpp
    sequencer/PluginNotesTests.cpp
    sequencer/SongModeTests.cpp
    sequencer/StepSequencerTests.cpp
    time/MusicalTimeTests.cpp
    time/TransportTests.cpp
    support/Allocations.cpp
    support/AllocationsTests.cpp)

target_include_directories(vloom_engine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# A real .clap for the CLAP host tests (plugin/ClapHostTests.cpp).
add_library(vloom_test_clap MODULE plugin/clap/TestPlugins.cpp)
set_target_properties(vloom_test_clap PROPERTIES PREFIX "" SUFFIX ".clap" OUTPUT_NAME "vloom-test")
target_link_libraries(vloom_test_clap PRIVATE vloom_clap_headers)
target_compile_features(vloom_test_clap PRIVATE cxx_std_20)
if(MSVC)
    target_compile_definitions(vloom_test_clap PRIVATE _CRT_SECURE_NO_WARNINGS) # strcpy into CLAP's fixed names
endif()
add_dependencies(vloom_engine_tests vloom_test_clap)
target_compile_definitions(vloom_engine_tests PRIVATE VLOOM_TEST_CLAP="$<TARGET_FILE:vloom_test_clap>")

# A real .vst3 (a single-file module) for the VST 3 host tests (plugin/Vst3HostTests.cpp).
add_library(vloom_test_vst3 MODULE plugin/vst3/TestPlugins.cpp)
set_target_properties(vloom_test_vst3 PROPERTIES PREFIX "" SUFFIX ".vst3" OUTPUT_NAME "vloom-test-vst3")
target_link_libraries(vloom_test_vst3 PRIVATE vloom_vst3_interfaces)
target_compile_features(vloom_test_vst3 PRIVATE cxx_std_20)
if(MSVC)
    target_compile_definitions(vloom_test_vst3 PRIVATE _CRT_SECURE_NO_WARNINGS) # strcpy into VST 3's fixed names
endif()
add_dependencies(vloom_engine_tests vloom_test_vst3)
target_compile_definitions(vloom_engine_tests PRIVATE VLOOM_TEST_VST3="$<TARGET_FILE:vloom_test_vst3>")
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
