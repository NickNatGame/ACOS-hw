cmake_minimum_required(VERSION 3.16)
project(philosophers LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

set(PHILOSOPHERS_SOURCES
    src/philosophers.c
    src/parameters.c
    src/simulation.c
    src/console.c
)
add_executable(philosophers ${PHILOSOPHERS_SOURCES})

target_link_libraries(philosophers PRIVATE Threads::Threads rt)
target_include_directories(philosophers PRIVATE inc)
target_compile_definitions(philosophers PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(philosophers PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(philosophers PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(philosophers_faults ${PHILOSOPHERS_SOURCES} tests/faults.c)
    target_link_libraries(philosophers_faults PRIVATE Threads::Threads rt)
    target_include_directories(philosophers_faults PRIVATE inc)
    target_compile_definitions(philosophers_faults PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(philosophers_faults PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(philosophers_faults PRIVATE
        -Wl,--wrap=shm_open,--wrap=posix_fallocate,--wrap=mmap,--wrap=sem_init
        -Wl,--wrap=fork,--wrap=sem_timedwait,--wrap=sem_post,--wrap=clock_gettime
    )
    set_target_properties(philosophers_faults PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    )
    add_executable(philosophers_tests
        tests/test_philosophers.c
        src/parameters.c
    )
    add_dependencies(philosophers_tests philosophers philosophers_faults)
    target_include_directories(philosophers_tests PRIVATE inc)
    target_compile_definitions(philosophers_tests PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(philosophers_tests PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(philosophers_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    )
    add_test(NAME philosophers_tests
        COMMAND philosophers_tests $<TARGET_FILE:philosophers> $<TARGET_FILE:philosophers_faults>
    )
    set_tests_properties(philosophers_tests PROPERTIES TIMEOUT 90)
endif()
