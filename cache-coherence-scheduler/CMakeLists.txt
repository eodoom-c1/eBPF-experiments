if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Skipping static-placement: select a Linux target to build this benchmark.")
    return()
endif()

set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

add_executable(static-placement static-placement.c)
target_link_libraries(static-placement PRIVATE Threads::Threads)
set_target_properties(static-placement PROPERTIES C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(static-placement PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_test(
        NAME static-placement-invalid-arguments
        COMMAND ${CMAKE_COMMAND}
            "-DBENCHMARK=$<TARGET_FILE:static-placement>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid-arguments.cmake"
    )
endif()
