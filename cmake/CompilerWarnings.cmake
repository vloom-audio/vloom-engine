option(VLOOM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

function(vloom_set_warnings target)
    if(MSVC)
        # C4324 "structure was padded due to alignment specifier" is the point of
        # alignas() in core/SpscQueue.hpp: it keeps producer and consumer
        # indices on separate cache lines.
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /wd4324)
        if(VLOOM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion>)
        if(VLOOM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Werror>)
        endif()
    endif()
endfunction()
