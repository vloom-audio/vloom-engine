add_library(vloom_engine)
add_library(vloom::engine ALIAS vloom_engine)

# One block per module. When you add a file, put it in its module's block.
target_sources(vloom_engine PRIVATE
    # engine
    Engine.cpp

    # core — building blocks shared by every module
    core/Version.cpp

    # time — tempo, transport, musical time
    time/MusicalTime.cpp
    time/Transport.cpp

    # dsp — sound generators and processors
    dsp/Metronome.cpp
    dsp/Sampler.cpp

    # io — hardware audio devices; each backend compiles only on its OS
    io/AlsaBackend.cpp
    io/AudioDevice.cpp
    io/CoreAudioBackend.cpp
    io/WasapiBackend.cpp

    # sample — decoded audio in memory: WAV reading, resampling
    sample/Resampler.cpp
    sample/SampleBuffer.cpp
    sample/WavReader.cpp

    # render — offline (faster than real time) rendering
    render/OfflineRenderer.cpp
    render/WavWriter.cpp)

target_include_directories(vloom_engine
    PUBLIC  ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vloom_engine PUBLIC cxx_std_20)
target_compile_definitions(vloom_engine PRIVATE VLOOM_ENGINE_VERSION="${PROJECT_VERSION}")
vloom_set_warnings(vloom_engine)

find_package(Threads REQUIRED)
target_link_libraries(vloom_engine PRIVATE Threads::Threads)

if(WIN32)
    # WASAPI: COM (ole32) and the MMCSS "Pro Audio" thread class (avrt).
    target_link_libraries(vloom_engine PRIVATE ole32 avrt)
elseif(APPLE)
    target_link_libraries(vloom_engine PRIVATE
        "-framework CoreFoundation"
        "-framework CoreAudio"
        "-framework AudioToolbox")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(vloom_engine PRIVATE ALSA::ALSA m)
endif()
