# B4: Matter (main.cpp, C++) + the AT+MT interface (mt_at.c, C) on the shared
# at_core engine. mt_at.c pulls in at_core; main.cpp only touches mt_at.h,
# which is C++-safe.
idf_component_register(
    SRCS
        "main.cpp"
        "mt_at.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_matter
        nvs_flash
        at_core
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
