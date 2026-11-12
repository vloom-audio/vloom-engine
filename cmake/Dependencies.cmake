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
