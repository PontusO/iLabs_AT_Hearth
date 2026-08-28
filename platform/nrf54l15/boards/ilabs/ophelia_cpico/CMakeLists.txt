# The Ophelia-IV module has no 32 kHz crystal, so the LFCLK must run
# from the internal RC. Since NCS 3.3.x the legacy nrf clock driver
# (CLOCK_CONTROL_NRF, gated on the &clock devicetree node this board
# enables) owns that selection on the nRF54L series, so the board
# defconfig sets CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y, exactly as
# upstream Zephyr's ophelia4ev board does. The NCS 3.0.2-era
# zephyr_compile_definitions() hack this file used to carry is gone.
#
# Guard: if a future NCS/Zephyr bump changes the clock driver's
# dependencies so the K32SRC choice becomes invisible again, the
# defconfig line is silently ignored and the symbol below comes back
# empty. An unguarded build then clocks the GRTC from a crystal this
# module does not have and the SoC hangs in
# nrf_grtc_sys_counter_low_get() waiting on a counter that never
# ticks (paid-for knowledge from the first boot of this board). Fail
# the configure step loudly instead. Board CMake runs for every image
# built for this board, so MCUboot is covered too.
if(NOT CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC)
  message(FATAL_ERROR
    "CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC did not resolve to y for this "
    "image: the internal-RC LFCLK selection has been dropped. This "
    "module has no 32 kHz crystal; without the RC source the GRTC "
    "hangs the SoC at boot. Re-check how this NCS/Zephyr version "
    "selects the LFCLK source (board defconfig and the &clock node) "
    "and update this board definition accordingly.")
endif()
