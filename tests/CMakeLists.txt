cmake_minimum_required(VERSION 3.16)
project(knob_tests C)

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -Werror)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../core ${CMAKE_BINARY_DIR}/core)

enable_testing()
add_executable(test_core unit/test_core.c)
target_link_libraries(test_core PRIVATE core)
add_test(NAME core COMMAND test_core)
