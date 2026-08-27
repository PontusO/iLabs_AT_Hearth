# The Ophelia-IV module has no 32 kHz crystal, so the GRTC must clock
# from the internal low-power RC. Upstream Zephyr's ophelia4ev board
# selects CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC for this; in this NCS
# revision that Kconfig choice is only visible on SoCs using the legacy
# nrf clock driver, so the board provides the macro directly. The GRTC
# timer driver (nrf_grtc_timer.c) only tests the macro with #if defined
# and then selects NRF_GRTC_CLKSEL_LFLPRC. Board CMake runs for every
# image built for this board, so MCUboot gets it too. The driver tests
# the macro with #if defined, not by its value: defining it as 0 would
# still satisfy #if defined and would NOT disable the RC selection.
zephyr_compile_definitions(CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=1)

# Guard against a future NCS/Zephyr bump quietly changing the driver so
# it no longer tests this macro at all: the define above would then
# become a silent no-op with no build-time signal, the GRTC would try
# to clock from a 32 kHz crystal this module does not have, and the SoC
# hangs in nrf_grtc_sys_counter_low_get() waiting on a counter that
# never ticks. Fail the configure step loudly instead of shipping that.
if(NOT DEFINED ENV{ZEPHYR_BASE})
  message(FATAL_ERROR
    "ZEPHYR_BASE is not set; cannot verify that nrf_grtc_timer.c still "
    "tests CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC. Source the NCS toolchain "
    "activation before configuring this board (see platform/nrf54l15/"
    "README.md).")
endif()

file(STRINGS "$ENV{ZEPHYR_BASE}/drivers/timer/nrf_grtc_timer.c"
     _ophelia_cpico_grtc_k32src_rc_refs
     REGEX "CLOCK_CONTROL_NRF_K32SRC_RC")
if(_ophelia_cpico_grtc_k32src_rc_refs STREQUAL "")
  message(FATAL_ERROR
    "nrf_grtc_timer.c no longer references "
    "CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC: the zephyr_compile_definitions() "
    "above has silently stopped selecting the internal RC as the GRTC "
    "clock source. This module has no 32 kHz crystal; an unguarded GRTC "
    "will hang the SoC in nrf_grtc_sys_counter_low_get() waiting on a "
    "counter that never ticks. Re-check how this NCS/Zephyr version "
    "wants the RC clock source selected and update this board's "
    "CMakeLists.txt accordingly.")
endif()
