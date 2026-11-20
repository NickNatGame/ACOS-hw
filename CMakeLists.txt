cmake_minimum_required(VERSION 3.16)
project(philosophers LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

foreach(role server client observer)
    add_executable(philosophers_tcp_${role} src/tcp/tcp_main.c src/tcp/tcp_${role}.c
        src/tcp/tcp_io.c src/common/console.c)
    target_include_directories(philosophers_tcp_${role} PRIVATE inc/common inc/tcp)
    target_compile_definitions(philosophers_tcp_${role} PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(philosophers_tcp_${role} PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(philosophers_tcp_${role} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    )
endforeach()
target_sources(philosophers_tcp_server PRIVATE src/common/parameters.c)
target_compile_definitions(philosophers_tcp_server PRIVATE TCP_SERVER_MAIN)
target_compile_definitions(philosophers_tcp_observer PRIVATE TCP_OBSERVER_MAIN)

include(CTest)
if(BUILD_TESTING)
    add_executable(philosophers_tcp_tests tests/test_tcp.c)
    target_include_directories(philosophers_tcp_tests PRIVATE inc/common)
    target_compile_definitions(philosophers_tcp_tests PRIVATE _POSIX_C_SOURCE=200809L
        TEST_LOG_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/tests/logs")
    target_compile_options(philosophers_tcp_tests PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(philosophers_tcp_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
    add_dependencies(philosophers_tcp_tests philosophers_tcp_server philosophers_tcp_client philosophers_tcp_observer)
    add_test(NAME philosophers_tcp_tests
            COMMAND philosophers_tcp_tests
                $<TARGET_FILE:philosophers_tcp_server> $<TARGET_FILE:philosophers_tcp_client>
                $<TARGET_FILE:philosophers_tcp_observer>)
    set_tests_properties(philosophers_tcp_tests PROPERTIES TIMEOUT 90)
endif()
