# Minimal runner registration. Without at least one board_runner_args()
# call, RUNNERS stays empty and Zephyr's flash/CMakeLists.txt never writes
# zephyr/runners.yaml, which sysbuild's partition_manager.cmake then fails
# to read at configure time. Mirrors the nrf54l15dk board.cmake (same SoC,
# same debug probe family); Task 7's flashing path is pyOCD, not this file.
if(CONFIG_SOC_NRF54L15_CPUAPP)
  board_runner_args(jlink "--device=cortex-m33" "--speed=4000")
endif()

include(${ZEPHYR_BASE}/boards/common/nrfutil.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
