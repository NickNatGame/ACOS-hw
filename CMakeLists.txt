cmake_minimum_required(VERSION 3.16)
project(philosophers LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

foreach(role server client)
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

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME philosophers_tcp_tests
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tcp.py
                $<TARGET_FILE:philosophers_tcp_server> $<TARGET_FILE:philosophers_tcp_client>)
        set_tests_properties(philosophers_tcp_tests PROPERTIES TIMEOUT 60)
    endif()
endif()
