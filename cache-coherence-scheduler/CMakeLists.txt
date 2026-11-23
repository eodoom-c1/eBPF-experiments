cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(cache-coherence-scheduler LANGUAGES NONE)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Skipping static-placement: select a Linux target to build this benchmark.")
    return()
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_language(C)
    include(CTest)
endif()

set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

add_executable(static-placement static-placement.c)
target_link_libraries(static-placement PRIVATE Threads::Threads)
target_compile_features(static-placement PRIVATE c_std_11)
set_target_properties(static-placement PROPERTIES C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(static-placement PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_executable(static-placement-worker-demand-options tests/worker-demand-options.c)
    target_link_libraries(static-placement-worker-demand-options PRIVATE Threads::Threads)
    target_compile_features(static-placement-worker-demand-options PRIVATE c_std_11)
    set_target_properties(static-placement-worker-demand-options PROPERTIES C_EXTENSIONS OFF)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(static-placement-worker-demand-options PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(
        NAME static-placement-worker-demand-options
        COMMAND static-placement-worker-demand-options
    )
    add_test(
        NAME static-placement-invalid-arguments
        COMMAND ${CMAKE_COMMAND}
            "-DBENCHMARK=$<TARGET_FILE:static-placement>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid-arguments.cmake"
    )
endif()
