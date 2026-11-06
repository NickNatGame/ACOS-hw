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

function(configure_philosophers target)
    target_link_libraries(${target} PRIVATE Threads::Threads rt)
    target_include_directories(${target} PRIVATE inc)
    target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    )
endfunction()

foreach(kind unnamed named)
    add_executable(philosophers_${kind} ${PHILOSOPHERS_SOURCES} src/semaphores_${kind}.c)
    configure_philosophers(philosophers_${kind})
endforeach()

foreach(role coordinator participant)
    add_executable(philosophers_${role} src/independent.c src/parameters.c
        src/simulation.c src/console.c src/semaphores_session.c)
    configure_philosophers(philosophers_${role})
    target_compile_definitions(philosophers_${role} PRIVATE INDEPENDENT_PROCESSES)
endforeach()
target_compile_definitions(philosophers_coordinator PRIVATE COORDINATOR_MAIN)

# Сохраняем прежнюю команду запуска неименованной версии
add_custom_command(TARGET philosophers_unnamed POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:philosophers_unnamed> "${CMAKE_CURRENT_SOURCE_DIR}/bin/philosophers"
)
add_custom_target(philosophers DEPENDS philosophers_unnamed)

include(CTest)
if(BUILD_TESTING)
    foreach(kind unnamed named)
        if(kind STREQUAL "unnamed")
            set(fault_target philosophers_faults)
        else()
            set(fault_target philosophers_named_faults)
        endif()
        add_executable(${fault_target} ${PHILOSOPHERS_SOURCES}
            src/semaphores_${kind}.c tests/faults.c)
        configure_philosophers(${fault_target})
        target_link_options(${fault_target} PRIVATE
            -Wl,--wrap=shm_open,--wrap=posix_fallocate,--wrap=mmap,--wrap=sem_init
            -Wl,--wrap=sem_open,--wrap=sem_unlink,--wrap=sem_close
            -Wl,--wrap=fork,--wrap=sem_timedwait,--wrap=sem_post,--wrap=clock_gettime
        )
    endforeach()
    add_executable(philosophers_tests
        tests/test_philosophers.c
        src/parameters.c
    )
    add_dependencies(philosophers_tests philosophers_unnamed philosophers_named
        philosophers_faults philosophers_named_faults)
    target_include_directories(philosophers_tests PRIVATE inc)
    target_compile_definitions(philosophers_tests PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(philosophers_tests PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(philosophers_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    )
    add_test(NAME philosophers_tests
        COMMAND philosophers_tests $<TARGET_FILE:philosophers_unnamed> $<TARGET_FILE:philosophers_faults>
    )
    add_test(NAME philosophers_named_tests
        COMMAND philosophers_tests $<TARGET_FILE:philosophers_named>
            $<TARGET_FILE:philosophers_named_faults> named
    )
    set_tests_properties(philosophers_tests philosophers_named_tests PROPERTIES TIMEOUT 90)
    add_executable(philosophers_session_tests tests/test_session.c)
    configure_philosophers(philosophers_session_tests)
    add_dependencies(philosophers_session_tests philosophers_coordinator philosophers_participant)
    add_test(NAME philosophers_session_tests
        COMMAND philosophers_session_tests $<TARGET_FILE:philosophers_coordinator>
            $<TARGET_FILE:philosophers_participant>)
    set_tests_properties(philosophers_session_tests PROPERTIES TIMEOUT 90)
endif()
