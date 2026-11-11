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

if(NOT EXISTS ${CPM_DOWNLOAD_LOCATION})
    message(STATUS "Downloading CPM.cmake ${CPM_DOWNLOAD_VERSION}")
    file(DOWNLOAD
        https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake
        ${CPM_DOWNLOAD_LOCATION}
        STATUS CPM_DOWNLOAD_STATUS)
    list(GET CPM_DOWNLOAD_STATUS 0 CPM_DOWNLOAD_CODE)
    if(NOT CPM_DOWNLOAD_CODE EQUAL 0)
        file(REMOVE ${CPM_DOWNLOAD_LOCATION})
        message(FATAL_ERROR "Failed to download CPM.cmake: ${CPM_DOWNLOAD_STATUS}")
    endif()
endif()

include(${CPM_DOWNLOAD_LOCATION})
