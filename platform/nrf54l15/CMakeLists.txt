cmake_minimum_required(VERSION 3.20.0)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hearth_nrf54l15)

include(${CMAKE_CURRENT_LIST_DIR}/../../core/sources.cmake)

target_sources(app PRIVATE
    port/hearth_port_zephyr.c
    ${HEARTH_CORE_SOURCES})
target_include_directories(app PRIVATE ${HEARTH_CORE_INCLUDE_DIRS} port)
target_compile_definitions(app PRIVATE MT_COMBINED_IMAGE=0)

if(CONFIG_CHIP)
  # ---- the SDK patch gate (sdk-patches/README.md) -----------------------
  #
  # This platform carries patches against the pinned NCS workspace, and an
  # unpatched tree must not build quietly. Every patch here is deliberately
  # written to fall back to upstream behaviour when its macro is unset, which
  # is what makes it upstreamable, and is also exactly why a MISSING patch is
  # invisible: the build succeeds, the firmware works, and several kilobytes
  # of RAM come back without a word. That is the trap this check closes, and
  # the reason it is a hard error rather than a warning in a thousand-line
  # log. The remedy is one command and it is printed here.
  #
  # The marker is a REVISION, not a presence flag. Searching for the
  # configuration macro alone would pass on a workspace still carrying an
  # OLDER cut of the same patch: it would configure, build and measure
  # cleanly while running code nobody reviewed, which is the one failure a
  # presence check cannot see. Each patch therefore defines
  # HEARTH_<name>_PATCH_REV and this gate matches the number this tree
  # expects. Bump both together whenever a patch is re-cut.
  set(HEARTH_SDK_PATCH_DIR ${CMAKE_CURRENT_LIST_DIR}/sdk-patches)
  set(HEARTH_EEM_PATCH_REV 2)
  file(READ
    ${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/src/app/clusters/electrical-energy-measurement-server/ElectricalEnergyMeasurementCluster.cpp
    hearth_eem_src)
  string(FIND "${hearth_eem_src}"
    "HEARTH_EEM_POOL_PATCH_REV ${HEARTH_EEM_PATCH_REV}" hearth_eem_patched)
  string(FIND "${hearth_eem_src}" "HEARTH_EEM_POOL_PATCH_REV" hearth_eem_any_rev)
  unset(hearth_eem_src)
  if(hearth_eem_patched EQUAL -1)
    if(hearth_eem_any_rev EQUAL -1)
      set(hearth_eem_state "is NOT patched")
    else()
      set(hearth_eem_state "carries a DIFFERENT revision of the patch than revision ${HEARTH_EEM_PATCH_REV}, which this tree expects")
    endif()
    message(FATAL_ERROR
      "The Matter module at ${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR} ${hearth_eem_state}.\n"
      "This platform's memory figures and its EEM capacity both assume the patches in\n"
      "  ${HEARTH_SDK_PATCH_DIR}\n"
      "Apply them from the west workspace (run the clean first only if the tree is\n"
      "already patched at another revision):\n"
      "  cd ${ZEPHYR_BASE}/.. \\\n"
      "    && west patch -l ${HEARTH_SDK_PATCH_DIR}/patches.yml \\\n"
      "                  -b ${HEARTH_SDK_PATCH_DIR} clean \\\n"
      "    && west patch -l ${HEARTH_SDK_PATCH_DIR}/patches.yml \\\n"
      "                  -b ${HEARTH_SDK_PATCH_DIR} apply\n"
      "then build again.\n"
      "README.md in that directory says what each patch does, how to tell whether\n"
      "a tree is patched, and how to take it back out.")
  endif()

  include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/config/nrfconnect/app/enable-gnu-std.cmake)
  include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/src/app/chip_data_model.cmake)
  target_include_directories(matter-data-model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/default_zap)
  chip_configure_data_model(matter-data-model
    BYPASS_IDL
    GEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/default_zap/zap-generated
    ZAP_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/default_zap/hearth.zap)

  set(MATTER_COMMON ${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/src)
  # src/default_zap is the ZAP_FILE's parent dir (chip_configure_data_model
  # above put the codegen output at src/default_zap/zap-generated): app's own
  # sources (main.cpp, matter_init.cpp) include <zap-generated/gen_config.h>
  # via app/util/attribute-storage.h, and that codegen dir was only ever added
  # to the matter-data-model target's own include path, not app's; mirrors
  # ZAP_PARENT_DIR in the template sample's CMakeLists.txt.
  target_include_directories(app PRIVATE src src/default_zap ${MATTER_COMMON})
  target_sources(app PRIVATE
    src/main.cpp
    port/mt_matter_stub.c
    port/mt_matter_zephyr.cpp
    port/mt_devtypes_zephyr.cpp
    ${MATTER_COMMON}/app/matter_init.cpp
    ${MATTER_COMMON}/app/matter_event_handler.cpp
    ${MATTER_COMMON}/app/task_executor.cpp
    ${MATTER_COMMON}/clusters/cluster_init.cpp)
  zephyr_linker_sources(SECTIONS ${MATTER_COMMON}/clusters/cluster_init.ld)
endif()
