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
    io/MidiInputTests.cpp
    mixer/MixerTests.cpp
    mixer/RoutingTests.cpp
    plugin/BuiltinCapabilitiesTests.cpp
    plugin/BuiltinProcessorsTests.cpp
    plugin/ClapHostTests.cpp
    plugin/EditorRunLoopTests.cpp
    plugin/PluginHostTests.cpp
    plugin/PluginRoutingTests.cpp
    plugin/Vst2HostTests.cpp
    plugin/Vst3HostTests.cpp
    record/AudioRecordingTests.cpp
    record/CountInPunchTests.cpp
    render/BounceTests.cpp
    render/SongExportTests.cpp
    render/WavWriterTests.cpp
    sample/ResamplerTests.cpp
    sample/SampleLoaderTests.cpp
    sample/WavReaderTests.cpp
    sequencer/MidiRecordingTests.cpp
    sequencer/NoteTests.cpp
    sequencer/PluginNotesTests.cpp
    sequencer/SongModeTests.cpp
    sequencer/StepSequencerTests.cpp
    time/MusicalTimeTests.cpp
    time/TransportTests.cpp
    support/Allocations.cpp
    support/AllocationsTests.cpp)

# src/ too: some tests reach the engine's internal helpers (plugin/EditorRunLoop).
target_include_directories(vloom_engine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)

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

# Two real VST 2 plugins for the VST 2 host tests (plugin/Vst2HostTests.cpp):
# a VST 2 file holds one plugin, so the one source is built twice.
if(WIN32)
    set(VLOOM_TEST_VST2_SUFFIX ".dll")
elseif(APPLE)
    set(VLOOM_TEST_VST2_SUFFIX ".vst")
else()
    set(VLOOM_TEST_VST2_SUFFIX ".so")
endif()
foreach(kind level keys)
    add_library(vloom_test_vst2_${kind} MODULE plugin/vst2/TestPlugins.cpp)
    set_target_properties(vloom_test_vst2_${kind} PROPERTIES
        PREFIX "" SUFFIX "${VLOOM_TEST_VST2_SUFFIX}" OUTPUT_NAME "vloom-test-vst2-${kind}")
    target_include_directories(vloom_test_vst2_${kind} PRIVATE ${PROJECT_SOURCE_DIR}/src) # plugin/Vst2Abi.hpp
    target_compile_features(vloom_test_vst2_${kind} PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_definitions(vloom_test_vst2_${kind} PRIVATE _CRT_SECURE_NO_WARNINGS) # strcpy into the host's buffers
    endif()
    add_dependencies(vloom_engine_tests vloom_test_vst2_${kind})
endforeach()
target_compile_definitions(vloom_test_vst2_keys PRIVATE VLOOM_TEST_VST2_KEYS)
# A third one for what goes beyond a plugin's main sound.
add_library(vloom_test_vst2_multi MODULE plugin/vst2/MultiPlugin.cpp)
set_target_properties(vloom_test_vst2_multi PROPERTIES
    PREFIX "" SUFFIX "${VLOOM_TEST_VST2_SUFFIX}" OUTPUT_NAME "vloom-test-vst2-multi")
target_include_directories(vloom_test_vst2_multi PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(vloom_test_vst2_multi PRIVATE cxx_std_20)
if(MSVC)
    target_compile_definitions(vloom_test_vst2_multi PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_dependencies(vloom_engine_tests vloom_test_vst2_multi)
target_compile_definitions(vloom_engine_tests PRIVATE
    VLOOM_TEST_VST2_MULTI="$<TARGET_FILE:vloom_test_vst2_multi>")
if(TARGET vloom-plugin-bridge)
    add_dependencies(vloom_engine_tests vloom-plugin-bridge)
    target_compile_definitions(vloom_engine_tests PRIVATE VLOOM_TEST_BRIDGE="$<TARGET_FILE:vloom-plugin-bridge>")
endif()
target_compile_definitions(vloom_engine_tests PRIVATE
    VLOOM_TEST_VST2_LEVEL="$<TARGET_FILE:vloom_test_vst2_level>"
    VLOOM_TEST_VST2_KEYS="$<TARGET_FILE:vloom_test_vst2_keys>")
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
