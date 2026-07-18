cmake_minimum_required(VERSION 3.10)

project(dmosi_posix_tests
    VERSION 1.0
    DESCRIPTION "DMOSI POSIX implementation tests"
    LANGUAGES C)

add_executable(${PROJECT_NAME} main.c)

target_link_libraries(${PROJECT_NAME}
    dmosi_posix
    dmosi_proc
    dmosi
    dmod
    Threads::Threads
)

# Use the DMOD linker script so that .dmod.inputs / .dmod.outputs sections
# are properly placed in the binary.
target_link_options(${PROJECT_NAME} PRIVATE -L ${DMOD_DIR}/scripts)
target_link_options(${PROJECT_NAME} PRIVATE -T ${CMAKE_CURRENT_SOURCE_DIR}/main.ld)

add_test(NAME dmosi_posix_tests COMMAND ${PROJECT_NAME})
