add_executable(vloom-cli
    main.cpp
    Args.cpp
    Commands.cpp)
target_link_libraries(vloom-cli PRIVATE vloom::engine)
vloom_set_warnings(vloom-cli)
