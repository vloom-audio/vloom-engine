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
