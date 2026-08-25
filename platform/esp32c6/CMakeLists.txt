cmake_minimum_required(VERSION 3.16)

# esp-matter integration. Source <esp-matter>/export.sh first so ESP_MATTER_PATH
# is set (and build against IDF v5.4.1 - esp-matter release/v1.5's validated IDF).
if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR
        "ESP_MATTER_PATH is not set. Source <esp-matter>/export.sh before building.")
endif()

# C6-only: pick the ESP32-C6 device HAL.
if(NOT DEFINED ENV{ESP_MATTER_DEVICE_PATH})
    if("${IDF_TARGET}" STREQUAL "esp32c6" OR "${IDF_TARGET}" STREQUAL "")
        set(ENV{ESP_MATTER_DEVICE_PATH} $ENV{ESP_MATTER_PATH}/device_hal/device/esp32c6_devkit_c)
    else()
        message(FATAL_ERROR
            "iLabs_AT_Hearth targets the ESP32-C6 only (IDF_TARGET=${IDF_TARGET})")
    endif()
endif()

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)
include($ENV{ESP_MATTER_DEVICE_PATH}/esp_matter_device.cmake)

# hearth_core wraps the SDK-neutral core/ tree; port is the platform's own
# UART transport, its own component so hearth_core and main can both depend
# on it without a link cycle; the rest are esp-matter's component dirs.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/hearth_core"
    "${CMAKE_CURRENT_LIST_DIR}/port"
    "${ESP_MATTER_PATH}/examples/common"
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components"
    "${ESP_MATTER_PATH}/device_hal/device"
    ${extra_components_dirs_append})

project(ilabs_at_hearth)

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
# RISCV: project_include.cmake sets -Wno-format but leaves dependent flags on.
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
