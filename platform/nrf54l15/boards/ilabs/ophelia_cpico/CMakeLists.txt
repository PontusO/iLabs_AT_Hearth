# The Ophelia-IV module has no 32 kHz crystal, so the GRTC must clock
# from the internal low-power RC. Upstream Zephyr's ophelia4ev board
# selects CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC for this; in this NCS
# revision that Kconfig choice is only visible on SoCs using the legacy
# nrf clock driver, so the board provides the macro directly. The GRTC
# timer driver (nrf_grtc_timer.c) only tests the macro with #if defined
# and then selects NRF_GRTC_CLKSEL_LFLPRC. Board CMake runs for every
# image built for this board, so MCUboot gets it too.
zephyr_compile_definitions(CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=1)
