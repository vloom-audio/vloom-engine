# Every third-party dependency of vloom-engine is declared here, in one place.
# Bump versions in this file only. Test-only dependencies live in tests/CMakeLists.txt.

# miniaudio — audio device I/O and WAV encoding (single header, public domain / MIT-0)
CPMAddPackage(
    NAME miniaudio
    GITHUB_REPOSITORY mackron/miniaudio
    GIT_TAG 0.11.21
    DOWNLOAD_ONLY YES)

if(NOT TARGET vloom_miniaudio)
    add_library(vloom_miniaudio INTERFACE)
    target_include_directories(vloom_miniaudio SYSTEM INTERFACE ${miniaudio_SOURCE_DIR})
endif()
