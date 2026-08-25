# B4: Matter (main.cpp, C++) + the AT+MT interface on the SDK-neutral
# hearth_core component. main.cpp only touches mt_at.h, which is C++-safe.
#
# Task 4 audit (downward port cut): `driver` was dropped. It is not
# referenced anywhere in this component's sources (grep for "driver/" and
# gpio_*/uart_* calls came back empty; Task 3 had already flagged it as
# possibly vestigial), and `port` (which does need it) declares its own
# PRIV_REQUIRES driver.
#
# `port` itself stays, even though this component's own sources never
# include at_uart.h/hearth_port.h/hearth_log.h directly (grep confirms):
# hearth_core no longer requires it either (Task 4 drops hearth_core's
# PRIV_REQUIRES to nothing, to keep the core component pure C against
# core/include only), so nothing else pulls `port` into the component
# dependency graph. Dropping it here too excludes `port` from the build's
# reachable-component set entirely (confirmed the hard way: the first build
# attempt failed at final link with undefined references to
# hearth_link_write_line/hearth_crit_enter/hearth_kv_*/hearth_log_write,
# every symbol hearth_core's object files call and only port's
# at_uart.c/hearth_port_esp.c define) rather than merely dropping an unused
# include path, since ESP-IDF prunes the link to components reachable from
# `main` rather than linking every component found under
# EXTRA_COMPONENT_DIRS. `port` is a link-only dependency of this component:
# required so hearth_core's calls into it resolve at the final executable
# link, never referenced by a source file in this directory.
#
# `nvs_flash` stays: main.cpp and mt_evse.cpp still call the NVS API
# directly (nvs_flash_init(), the EVSE targets store).
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
        hearth_core
        port
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
