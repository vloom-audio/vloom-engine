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

    # io — hardware audio devices
    io/AudioDevice.cpp
    io/miniaudio_impl.c

    # render — offline (faster than real time) rendering
    render/OfflineRenderer.cpp)

target_include_directories(vloom_engine
    PUBLIC  ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vloom_engine PUBLIC cxx_std_20)
target_compile_definitions(vloom_engine PRIVATE VLOOM_ENGINE_VERSION="${PROJECT_VERSION}")
vloom_set_warnings(vloom_engine)

find_package(Threads REQUIRED)
target_link_libraries(vloom_engine PRIVATE vloom_miniaudio Threads::Threads ${CMAKE_DL_LIBS})

if(APPLE)
    target_link_libraries(vloom_engine PRIVATE
        "-framework CoreFoundation"
        "-framework CoreAudio"
        "-framework AudioToolbox")
elseif(UNIX)
    target_link_libraries(vloom_engine PRIVATE m)
endif()
