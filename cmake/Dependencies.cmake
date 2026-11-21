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

# vloom-dsp (MIT) — the sound of the built-in effects and instruments
# (docs/adr/0008). Our own library, in its own repository so it can also ship
# as stand-alone plugins. Pinned to a commit; only the library is built (its
# tests are on only when it is the top-level project).
CPMAddPackage(
    NAME vloom_dsp
    GITHUB_REPOSITORY vloom-audio/vloom-dsp
    GIT_TAG 7c5f3a1d879bd630b31ed741afad02eb7461fd04
    GIT_SHALLOW FALSE)

# Steinberg ASIO SDK 2.3.4 (Windows only) — the host side of ASIO, the
# driver API most audio interfaces ship for low latency (docs/adr/0021).
# Licensed "under the terms of the Steinberg ASIO License, or alternatively
# under the terms of the GPL Version 3" (ASIOSDK/LICENSE.txt): we use the
# GPLv3 option. That is version 3 only, not "or later", so a binary built
# with VLOOM_ENGINE_ASIO=ON is in effect GPL-3.0-only, while the engine's
# own source stays GPL-3.0-or-later. Its host helpers (host/asiodrivers.cpp,
# host/pc/asiolist.cpp) carry a BSD-style notice that redistributions keep.
# No SDK file is in this repository: it is downloaded (pinned by hash) or
# taken from VLOOM_ENGINE_ASIO_SDK_DIR. Only the host side is compiled, into
# its own static library, without our warnings.
if(WIN32)
    set(VLOOM_ENGINE_ASIO_DEFAULT ON)
else()
    set(VLOOM_ENGINE_ASIO_DEFAULT OFF)
endif()
option(VLOOM_ENGINE_ASIO
    "Windows: ASIO driver support through the Steinberg ASIO SDK 2.3.4 under GPLv3 (makes the binary GPL-3.0-only)"
    ${VLOOM_ENGINE_ASIO_DEFAULT})
set(VLOOM_ENGINE_ASIO_SDK_DIR "" CACHE PATH
    "Extracted ASIOSDK folder (holds common/ and host/) to use instead of downloading the SDK")

if(VLOOM_ENGINE_ASIO AND NOT WIN32)
    message(STATUS "vloom-engine: VLOOM_ENGINE_ASIO is ignored: ASIO exists only on Windows")
elseif(VLOOM_ENGINE_ASIO)
    if(VLOOM_ENGINE_ASIO_SDK_DIR)
        set(VLOOM_ASIO_SDK_ROOT ${VLOOM_ENGINE_ASIO_SDK_DIR})
    else()
        # The zip's single ASIOSDK/ folder is unpacked as the source dir itself.
        CPMAddPackage(
            NAME asiosdk
            VERSION 2.3.4
            URL https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip
            URL_HASH SHA256=d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca
            DOWNLOAD_ONLY YES)
        set(VLOOM_ASIO_SDK_ROOT ${asiosdk_SOURCE_DIR})
    endif()
    foreach(file common/asio.h common/asio.cpp host/asiodrivers.cpp host/pc/asiolist.cpp)
        if(NOT EXISTS ${VLOOM_ASIO_SDK_ROOT}/${file})
            message(FATAL_ERROR "ASIO SDK: ${VLOOM_ASIO_SDK_ROOT}/${file} not found. Point "
                "VLOOM_ENGINE_ASIO_SDK_DIR at the extracted ASIOSDK folder, or set VLOOM_ENGINE_ASIO=OFF.")
        endif()
    endforeach()
    add_library(vloom_asio_sdk STATIC
        ${VLOOM_ASIO_SDK_ROOT}/common/asio.cpp
        ${VLOOM_ASIO_SDK_ROOT}/host/asiodrivers.cpp
        ${VLOOM_ASIO_SDK_ROOT}/host/pc/asiolist.cpp)
    # SYSTEM: the SDK's warnings are not ours, also under -Werror / /WX.
    target_include_directories(vloom_asio_sdk SYSTEM PUBLIC
        ${VLOOM_ASIO_SDK_ROOT}/common
        ${VLOOM_ASIO_SDK_ROOT}/host
        ${VLOOM_ASIO_SDK_ROOT}/host/pc)
    # asiolist.cpp calls the registry's TCHAR functions with char buffers:
    # it must build without UNICODE (nothing here defines it).
    target_compile_definitions(vloom_asio_sdk PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(vloom_asio_sdk PRIVATE /W0 /utf-8)
    else()
        target_compile_options(vloom_asio_sdk PRIVATE -w)
    endif()
    set_target_properties(vloom_asio_sdk PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(vloom_asio_sdk PUBLIC ole32 advapi32 user32)
endif()
