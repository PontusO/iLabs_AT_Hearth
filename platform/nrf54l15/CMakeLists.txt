cmake_minimum_required(VERSION 3.20.0)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})
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

if(CONFIG_CHIP)
  include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/config/nrfconnect/app/enable-gnu-std.cmake)
  include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/src/app/chip_data_model.cmake)
  target_include_directories(matter-data-model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/default_zap)
  chip_configure_data_model(matter-data-model
    BYPASS_IDL
    GEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/default_zap/zap-generated
    ZAP_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/default_zap/hearth.zap)
endif()
