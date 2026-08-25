# IDF wrapper around the SDK-neutral core. The nvs_flash/freertos
# dependencies are temporary scaffolding until the Task 4 port cut.
# port is its own component (not a relative source of main): mt_at.c calls
# at_uart_* symbols, and main also depends on hearth_core, so at_uart.c
# living inside main made a link cycle between the two component archives.
include(${CMAKE_CURRENT_LIST_DIR}/../../../core/sources.cmake)
idf_component_register(
    SRCS ${HEARTH_CORE_SOURCES}
    INCLUDE_DIRS ${HEARTH_CORE_INCLUDE_DIRS}
    PRIV_REQUIRES nvs_flash esp_system driver port)
