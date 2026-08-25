# The UART transport for this platform. Its own component, not a source of
# main: mt_at.c (hearth_core) and main.cpp both need it, and hearth_core ->
# main -> hearth_core would be a link cycle if it lived inside main.
idf_component_register(
    SRCS "at_uart.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver)
