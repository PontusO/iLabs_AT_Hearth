# B2: Matter-only bring-up. Only main.cpp is built for now; the AT command
# surface (mt_at.c on the shared at_core) is dormant and returns in B4, so it is
# intentionally left out of SRCS here.
idf_component_register(
    SRCS
        "main.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_matter
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
