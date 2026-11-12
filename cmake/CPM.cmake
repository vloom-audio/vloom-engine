# Downloads CPM.cmake (https://github.com/cpm-cmake/CPM.cmake) on first configure.
# Set CPM_SOURCE_CACHE (env or cache var) to share downloads between builds.

set(CPM_DOWNLOAD_VERSION 0.40.2)

if(CPM_SOURCE_CACHE)
    set(CPM_DOWNLOAD_LOCATION "${CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
elseif(DEFINED ENV{CPM_SOURCE_CACHE})
    set(CPM_DOWNLOAD_LOCATION "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
else()
    set(CPM_DOWNLOAD_LOCATION "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
endif()

get_filename_component(CPM_DOWNLOAD_LOCATION ${CPM_DOWNLOAD_LOCATION} ABSOLUTE)

# The script runs inside every configure: its hash is pinned, and a cached
# copy that does not match (tampered, truncated) is fetched again.
# Bump both lines together; get the hash from a fresh download.
set(CPM_DOWNLOAD_SHA256 c8cdc32c03816538ce22781ed72964dc864b2a34a310d3b7104812a5ca2d835d)

if(EXISTS ${CPM_DOWNLOAD_LOCATION})
    file(SHA256 ${CPM_DOWNLOAD_LOCATION} CPM_CACHED_SHA256)
    if(NOT CPM_CACHED_SHA256 STREQUAL CPM_DOWNLOAD_SHA256)
        message(WARNING "Cached CPM.cmake does not match its pinned hash; downloading it again")
        file(REMOVE ${CPM_DOWNLOAD_LOCATION})
    endif()
endif()

if(NOT EXISTS ${CPM_DOWNLOAD_LOCATION})
    message(STATUS "Downloading CPM.cmake ${CPM_DOWNLOAD_VERSION}")
    file(DOWNLOAD
        https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake
        ${CPM_DOWNLOAD_LOCATION}
        EXPECTED_HASH SHA256=${CPM_DOWNLOAD_SHA256}
        STATUS CPM_DOWNLOAD_STATUS)
    list(GET CPM_DOWNLOAD_STATUS 0 CPM_DOWNLOAD_CODE)
    if(NOT CPM_DOWNLOAD_CODE EQUAL 0)
        file(REMOVE ${CPM_DOWNLOAD_LOCATION})
        message(FATAL_ERROR "Failed to download CPM.cmake: ${CPM_DOWNLOAD_STATUS}")
    endif()
endif()

include(${CPM_DOWNLOAD_LOCATION})
