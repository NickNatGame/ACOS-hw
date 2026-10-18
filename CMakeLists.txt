cmake_minimum_required(VERSION 3.16)
project(philosophers LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(philosophers
    src/philosophers.c
    src/parameters.c
    src/simulation.c
    src/console.c
)

target_include_directories(philosophers PRIVATE inc)
target_compile_definitions(philosophers PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(philosophers PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(philosophers PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(philosophers_tests tests/test_philosophers.c)
    add_dependencies(philosophers_tests philosophers)
    target_include_directories(philosophers_tests PRIVATE inc)
    target_compile_definitions(philosophers_tests PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(philosophers_tests PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(philosophers_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    )
    add_test(NAME philosophers_tests
        COMMAND philosophers_tests $<TARGET_FILE:philosophers>
    )
    set_tests_properties(philosophers_tests PROPERTIES TIMEOUT 60)
endif()
