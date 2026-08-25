# IDF wrapper around the SDK-neutral core. Pure C against core/include only
# since the Task 4 port cut: no SDK PRIV_REQUIRES, no port include dir. The
# core reaches its downward port (hearth_port.h/hearth_log.h) through
# platform/esp32c6/port, an independent component neither requires nor
# includes here.
include(${CMAKE_CURRENT_LIST_DIR}/../../../core/sources.cmake)
idf_component_register(
    SRCS ${HEARTH_CORE_SOURCES}
    INCLUDE_DIRS ${HEARTH_CORE_INCLUDE_DIRS})

# MT_COMBINED_IMAGE is a build-time flag core/include/mt_at_config.h needs
# but cannot derive itself (core/ takes no sdkconfig.h, no SDK header of any
# kind). CONFIG_ENABLE_WIFI_STATION and CONFIG_OPENTHREAD_ENABLED were
# checked against the three generated sdkconfigs before picking this pair
# (task-3-report.md): the WiFi-only build sets ENABLE_WIFI_STATION alone,
# the Thread-only build sets OPENTHREAD_ENABLED alone (sdkconfig.defaults.
# thread explicitly turns ENABLE_WIFI_STATION off), and the combined build
# is the only one of the three carrying both. PUBLIC: main.cpp (PRIV_REQUIRES
# hearth_core) reads the same macro and must inherit it too.
if(CONFIG_ENABLE_WIFI_STATION AND CONFIG_OPENTHREAD_ENABLED)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC MT_COMBINED_IMAGE=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC MT_COMBINED_IMAGE=0)
endif()
