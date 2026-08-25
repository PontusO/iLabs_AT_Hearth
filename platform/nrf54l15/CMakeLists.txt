cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hearth_nrf54l15)

include(${CMAKE_CURRENT_LIST_DIR}/../../core/sources.cmake)

target_sources(app PRIVATE
    src/main.c
    port/hearth_port_zephyr.c
    port/mt_matter_stub.c
    ${HEARTH_CORE_SOURCES})
target_include_directories(app PRIVATE ${HEARTH_CORE_INCLUDE_DIRS} port)
target_compile_definitions(app PRIVATE MT_COMBINED_IMAGE=0)
