# core: plain C logic shared by the firmware, the simulator and the host tests.
if(ESP_PLATFORM)
    idf_component_register(SRCS "core.c" INCLUDE_DIRS ".")
else()
    add_library(core STATIC core.c)
    target_include_directories(core PUBLIC ${CMAKE_CURRENT_LIST_DIR})
    target_link_libraries(core PUBLIC m)
endif()
