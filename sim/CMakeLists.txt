# The browser simulator: the knob's screens (ui/, core/) built with Emscripten.
# Built by tools/sim.sh, which copies the sources off the network share first.
#   emcmake cmake -S sim -B sim/build -DLVGL_DIR=<lvgl 9.5.0> && cmake --build sim/build
cmake_minimum_required(VERSION 3.16)
project(knob_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXECUTABLE_SUFFIX ".js")

# LVGL: the same version as the firmware (firmware/main/idf_component.yml),
# with the firmware's own settings (lv_kconfig.h).
if(NOT LVGL_DIR)
    message(FATAL_ERROR "Set LVGL_DIR to an LVGL 9.5.0 source tree")
endif()
file(GLOB_RECURSE LVGL_SRCS ${LVGL_DIR}/src/*.c)
add_library(lvgl STATIC ${LVGL_SRCS})
target_include_directories(lvgl PUBLIC ${LVGL_DIR} ${LVGL_DIR}/src ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(lvgl PUBLIC LV_CONF_KCONFIG_EXTERNAL_INCLUDE="lv_kconfig.h" LV_CONF_SKIP)
target_compile_options(lvgl PRIVATE -O2 -w)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../core ${CMAKE_BINARY_DIR}/core)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../ui ${CMAKE_BINARY_DIR}/ui)
target_compile_options(ui PRIVATE -O2)
target_compile_options(core PRIVATE -O2)

add_executable(knob sim_main.c sim_library.c sim_voice.c)
target_compile_options(knob PRIVATE -O2 -Wall -Wextra)
target_link_libraries(knob PRIVATE ui core lvgl)
target_link_options(knob PRIVATE
    -O2
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=64MB
    -sSTACK_SIZE=1MB
    -sEXPORTED_FUNCTIONS=_main,_malloc,_free
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8,UTF8ToString)
