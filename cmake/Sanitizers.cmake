# Optional sanitizers for debug builds (GCC / Clang only).
#   -DVLOOM_SANITIZER=address   AddressSanitizer + UndefinedBehaviorSanitizer
#   -DVLOOM_SANITIZER=thread    ThreadSanitizer (data races between UI and audio thread)
# The CMakePresets "asan" and "tsan" set this for you.

set(VLOOM_SANITIZER "" CACHE STRING "Sanitizer to enable: address, thread or empty")
set_property(CACHE VLOOM_SANITIZER PROPERTY STRINGS "" address thread)

if(VLOOM_SANITIZER AND NOT MSVC)
    if(VLOOM_SANITIZER STREQUAL "address")
        set(_vloom_san_flags -fsanitize=address,undefined -fno-omit-frame-pointer)
    elseif(VLOOM_SANITIZER STREQUAL "thread")
        set(_vloom_san_flags -fsanitize=thread)
    else()
        message(FATAL_ERROR "Unknown VLOOM_SANITIZER '${VLOOM_SANITIZER}' (use address or thread)")
    endif()
    message(STATUS "vloom: building with ${VLOOM_SANITIZER} sanitizer")
    add_compile_options(${_vloom_san_flags})
    add_link_options(${_vloom_san_flags})
endif()
