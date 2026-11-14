# Every third-party dependency of vloom-engine is declared here, in one place.
# Bump versions in this file only. Test-only dependencies live in tests/CMakeLists.txt.
#
# The engine has no third-party runtime library: audio I/O talks to each OS's
# own API (docs/adr/0004-native-audio-io.md). Only Linux needs a system package.

# ALSA (Linux) — audio output; reaches PulseAudio and PipeWire through their
# ALSA plugins. Debian/Ubuntu: libasound2-dev, Fedora: alsa-lib-devel.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(ALSA REQUIRED)
endif()

# CLAP (MIT) — the plugin format the engine hosts (docs/adr/0006). Headers
# only: nothing is linked. Pinned to the commit of the 1.2.10 tag (tags can
# be moved). DOWNLOAD_ONLY: its own CMake targets are not wanted.
CPMAddPackage(
    NAME clap
    GITHUB_REPOSITORY free-audio/clap
    VERSION 1.2.10
    GIT_TAG 195b42a004144fab0b3cf95e9c067187d15365b7
    GIT_SHALLOW FALSE
    DOWNLOAD_ONLY YES)
add_library(vloom_clap_headers INTERFACE)
# SYSTEM: their warnings are not ours.
target_include_directories(vloom_clap_headers SYSTEM INTERFACE ${clap_SOURCE_DIR}/include)

# VST 3 plugin interfaces (MIT since VST 3.8) — the plugin format the engine
# also hosts (docs/adr/0006). Only the interfaces, not the whole SDK: the
# engine implements the host side itself. Pinned to the commit of the
# v3.8.1_build_84 tag. Its two .cpp files (FUnknown/FUID and the core
# interface ids) plus ours (the VST interface ids) are built into a small
# static library, without our warnings.
CPMAddPackage(
    NAME vst3_pluginterfaces
    GITHUB_REPOSITORY steinbergmedia/vst3_pluginterfaces
    GIT_TAG 4f547e8e102b47de4a8b8aaf343c73b700786372
    GIT_SHALLOW FALSE
    DOWNLOAD_ONLY YES)
# The headers include "pluginterfaces/...": the checkout must sit in a
# directory of that name.
set(VLOOM_VST3_ROOT ${PROJECT_BINARY_DIR}/vst3)
file(MAKE_DIRECTORY ${VLOOM_VST3_ROOT})
if(NOT EXISTS ${VLOOM_VST3_ROOT}/pluginterfaces)
    file(COPY ${vst3_pluginterfaces_SOURCE_DIR}/ DESTINATION ${VLOOM_VST3_ROOT}/pluginterfaces
        PATTERN ".git" EXCLUDE)
endif()
add_library(vloom_vst3_interfaces STATIC
    ${VLOOM_VST3_ROOT}/pluginterfaces/base/funknown.cpp
    ${VLOOM_VST3_ROOT}/pluginterfaces/base/coreiids.cpp
    ${PROJECT_SOURCE_DIR}/src/plugin/vst3/Vst3Iids.cpp)
target_include_directories(vloom_vst3_interfaces SYSTEM PUBLIC ${VLOOM_VST3_ROOT})
target_compile_features(vloom_vst3_interfaces PUBLIC cxx_std_20)
set_target_properties(vloom_vst3_interfaces PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    target_link_libraries(vloom_vst3_interfaces PUBLIC ole32) # FUID::generate: CoCreateGuid
elseif(APPLE)
    target_link_libraries(vloom_vst3_interfaces PUBLIC "-framework CoreFoundation")
endif()

# vloom-dsp (MIT) � the sound of the built-in effects and instruments
# (docs/adr/0008). Our own library, in its own repository so it can also ship
# as stand-alone plugins. Pinned to a commit; only the library is built (its
# tests are on only when it is the top-level project).
CPMAddPackage(
    NAME vloom_dsp
    GITHUB_REPOSITORY vloom-audio/vloom-dsp
    GIT_TAG 243b1127000cdf45a96b004ccf3b3075d94f0616
    GIT_SHALLOW FALSE)
