# The ESP-IDF implementation of hearth_port.h/hearth_log.h for this
# platform. Its own component, not a source of main: mt_at.c (hearth_core)
# and main.cpp both need it, and hearth_core -> main -> hearth_core would be
# a link cycle if it lived inside main. hearth_core declares no dependency
# on this component (it is pure C against core/include only); main
# PRIV_REQUIRES both directly, and the final executable link resolves
# hearth_core's calls into hearth_port_esp.c/at_uart.c across the two
# component archives the way any other component-to-component call does.
#
# PRIV_INCLUDE_DIRS reaches into core/include for hearth_port.h/hearth_log.h,
# the contract this component implements; it is a one-way include-path
# reach, not a build dependency (hearth_core is not in PRIV_REQUIRES), so it
# creates no symbol dependency from this component back onto hearth_core.
idf_component_register(
    SRCS "at_uart.c" "hearth_port_esp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../../../core/include"
    PRIV_REQUIRES driver nvs_flash esp_system)
