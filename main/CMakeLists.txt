# C6-only: the Matter personality is scoped to the ESP32-C6 target.
if(NOT IDF_TARGET STREQUAL "esp32c6")
    message(FATAL_ERROR
        "iLabs_AT_Matter targets the ESP32-C6 only (IDF_TARGET=${IDF_TARGET})")
endif()

# The AT engine + UART transport come from the shared at_core component
# (pulled in via EXTRA_COMPONENT_DIRS in the top-level CMakeLists.txt).
idf_component_register(
    SRCS
        "main.c"
        "mt_at.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        at_core
)
