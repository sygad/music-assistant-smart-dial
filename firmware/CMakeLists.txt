cmake_minimum_required(VERSION 3.16)

# Shared code lives outside the ESP-IDF project so the simulator and host tests can use it too.
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/../core
    ${CMAKE_CURRENT_LIST_DIR}/../ui)

# What Settings shows as the firmware. Worked out by tools/fw.sh from the
# repo's own tags, because the build runs in a local mirror with no .git.
if(DEFINED ENV{PROJECT_VER})
    set(PROJECT_VER $ENV{PROJECT_VER})
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bathroom_knob)
