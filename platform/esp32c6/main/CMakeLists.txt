# B4: Matter (main.cpp, C++) + the AT+MT interface on the SDK-neutral
# hearth_core component. main.cpp only touches mt_at.h, which is C++-safe.
idf_component_register(
    SRCS
        "main.cpp"
        "mt_devtypes.cpp"
        "mt_evse.cpp"
        "mt_meter.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_matter
        nvs_flash
        driver
        hearth_core
        port
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
