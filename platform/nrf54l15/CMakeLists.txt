cmake_minimum_required(VERSION 3.20.0)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hearth_nrf54l15)

# ---- the board contract (README.md, "Adding a board") -------------------
#
# A board on this platform has to name a UART twice, and it has to be the
# same UART both times:
#
#   the application's  hearth,at-uart      chosen node: where AT is spoken
#   MCUboot's          zephyr,uart-mcumgr  chosen node: where serial
#                                          recovery answers
#
# fw/flash.py drives ONE port and needs both of them. It straps the module
# into recovery, uploads the signed image over SMP, releases the strap and
# then waits for the application's +MTREADY, all on the same wire, because
# one wire is what a carrier brings out. A board whose two answers differ is
# not reflashable over that wire at all, and on a module whose SWD pads are a
# soldering job, that is a brick rather than an inconvenience.
#
# The two facts live in two images with two devicetrees, so no static_assert
# and no devicetree macro can see both. This checks the INPUT instead: it
# finds MCUboot's per-board overlay in this tree with the same zephyr_file()
# lookup MCUboot itself will use, reads the node label it names, and resolves
# that label against THIS image's devicetree. It catches the failure the
# round it was written for existed to remove (uart20 pinned unconditionally
# for every board, right on ophelia_cpico and the CONSOLE on both Nordic
# DKs, with nothing in the build saying so). It does not catch a
# -Dmcuboot_DTC_OVERLAY_FILE=... on the command line, which goes around the
# lookup entirely. That is the honest limit of a cross-image check here, and
# it is still worth having, because the version of this failure that has no
# check is silent until a board is on the bench.
string(REPLACE "/" "_" HEARTH_BOARD_FILE "${BOARD}${BOARD_QUALIFIERS}")

dt_chosen(HEARTH_AT_UART_NODE PROPERTY "hearth,at-uart")
if(NOT HEARTH_AT_UART_NODE)
  message(FATAL_ERROR
    "Board ${BOARD}${BOARD_QUALIFIERS} declares no `hearth,at-uart` chosen "
    "node, so nothing in this build says which UART carries the AT link.\n"
    "Add it to the board's devicetree, either a board directory under\n"
    "  ${CMAKE_CURRENT_LIST_DIR}/boards/<vendor>/<board>/\n"
    "for a board this repository defines, or an application overlay at\n"
    "  ${CMAKE_CURRENT_LIST_DIR}/boards/${HEARTH_BOARD_FILE}.overlay\n"
    "for an upstream board:\n"
    "  / { chosen { hearth,at-uart = &uartNN; }; };\n"
    "  &uartNN { status = \"okay\"; current-speed = <115200>; };\n"
    "There is deliberately no default. uart20 is the AT link on "
    "ophelia_cpico and is the CONSOLE on both Nordic DKs, so a default would "
    "be a coin toss that answers AT on the wrong pins without failing.\n"
    "See platform/nrf54l15/README.md, \"Adding a board\".")
endif()

if(CONFIG_BOOTLOADER_MCUBOOT)
  # The same two directories, in the same order, that MCUboot's own
  # DTC_OVERLAY_FILE discovery reads under its APPLICATION_CONFIG_DIR
  # (configuration_files.cmake:70-72: socs first, then boards), with the
  # SUFFIX every Zephyr-side lookup passes. Later entries win in devicetree
  # application order, and the loop below keeps the last match, so the two
  # agree about which file decides.
  set(HEARTH_MCUBOOT_IMAGE_DIR ${CMAKE_CURRENT_LIST_DIR}/sysbuild/mcuboot)
  set(HEARTH_MCUBOOT_BOARDS_DIR ${HEARTH_MCUBOOT_IMAGE_DIR}/boards)
  set(HEARTH_MCUBOOT_OVERLAYS)
  zephyr_file(CONF_FILES ${HEARTH_MCUBOOT_IMAGE_DIR}/socs DTS HEARTH_MCUBOOT_OVERLAYS
              QUALIFIERS SUFFIX ${FILE_SUFFIX})
  zephyr_file(CONF_FILES ${HEARTH_MCUBOOT_BOARDS_DIR} DTS HEARTH_MCUBOOT_OVERLAYS
              SUFFIX ${FILE_SUFFIX})

  set(HEARTH_MCUMGR_LABEL)
  set(HEARTH_MCUMGR_FILE)
  foreach(overlay ${HEARTH_MCUBOOT_OVERLAYS})
    file(READ ${overlay} hearth_overlay_text)
    # Comments come out first. CMake's MATCHES returns the FIRST match in the
    # string and these files are comment-heavy by house style, so a header
    # that quotes the property name in prose would otherwise steer the check
    # at whatever label the prose mentioned, and steer it SILENTLY, which is
    # the one failure class this whole check exists to remove.
    string(REGEX REPLACE "/\\*[^*]*\\*+([^/*][^*]*\\*+)*/" "" hearth_overlay_text
           "${hearth_overlay_text}")
    string(REGEX REPLACE "//[^\n]*" "" hearth_overlay_text "${hearth_overlay_text}")
    # `[^&]*` cannot cross the ampersand it is looking for, so this matches
    # the label of the first phandle after the property name and nothing
    # further. Whitespace and line breaks between the two are free. A value
    # written as a path phandle, &{/soc/uart@c6000}, matches nothing and
    # falls through to the "no zephyr,uart-mcumgr" error below, which still
    # points at the right file.
    if(hearth_overlay_text MATCHES "zephyr,uart-mcumgr[^&]*&([A-Za-z0-9_]+)")
      set(HEARTH_MCUMGR_LABEL ${CMAKE_MATCH_1})
      set(HEARTH_MCUMGR_FILE ${overlay})
    endif()
    unset(hearth_overlay_text)
  endforeach()

  if(NOT HEARTH_MCUMGR_LABEL)
    message(FATAL_ERROR
      "Board ${BOARD}${BOARD_QUALIFIERS} has no MCUboot serial-recovery UART.\n"
      "Nothing under\n"
      "  ${HEARTH_MCUBOOT_BOARDS_DIR}/\n"
      "sets `zephyr,uart-mcumgr` for this board; the file this build looks "
      "for is\n"
      "  ${HEARTH_MCUBOOT_BOARDS_DIR}/${HEARTH_BOARD_FILE}.overlay\n"
      "(a socs/ overlay for this SoC would do as well, and is read first)\n"
      "and it must name the same node as this board's `hearth,at-uart`, "
      "which is ${HEARTH_AT_UART_NODE}:\n"
      "  / { chosen { zephyr,uart-mcumgr = &uartNN; }; };\n"
      "  &uartNN { status = \"okay\"; };\n"
      "(Files found for this board: ${HEARTH_MCUBOOT_OVERLAYS})\n"
      "See platform/nrf54l15/README.md, \"Adding a board\".")
  endif()

  dt_nodelabel(HEARTH_MCUMGR_NODE NODELABEL "${HEARTH_MCUMGR_LABEL}")
  if(NOT HEARTH_MCUMGR_NODE)
    message(FATAL_ERROR
      "${HEARTH_MCUMGR_FILE}\n"
      "sets `zephyr,uart-mcumgr = &${HEARTH_MCUMGR_LABEL};`, but no node "
      "labelled ${HEARTH_MCUMGR_LABEL} exists in this board's devicetree. "
      "The two images share the board, so a label MCUboot can resolve is a "
      "label this image can resolve. Either the spelling is wrong, or the "
      "label is one the MCUboot overlay defines itself, which the AT link "
      "cannot be on: it has to be a node this image has too.")
  endif()

  if(NOT HEARTH_MCUMGR_NODE STREQUAL HEARTH_AT_UART_NODE)
    message(FATAL_ERROR
      "The AT link and MCUboot's serial recovery are on different UARTs on "
      "board ${BOARD}${BOARD_QUALIFIERS}, and fw/flash.py drives one port:\n"
      "  hearth,at-uart      ${HEARTH_AT_UART_NODE}\n"
      "  zephyr,uart-mcumgr  ${HEARTH_MCUMGR_NODE} (&${HEARTH_MCUMGR_LABEL})\n"
      "                      set in ${HEARTH_MCUMGR_FILE}\n"
      "Recovery would answer on a port the flasher never opens. Change "
      "whichever of the two is wrong for the hardware; on a carrier that "
      "brings out one UART, the AT link is the fixed point and recovery "
      "follows it.\n"
      "See platform/nrf54l15/README.md, \"Adding a board\".")
  endif()

  # ---- what the sysbuild/mcuboot/ directory costs, and its guard --------
  #
  # Creating that directory is what makes boards/<board>.overlay work for
  # the MCUboot image, and it is not free: sysbuild redirects the image's
  # APPLICATION_CONFIG_DIR to it (sysbuild_extensions.cmake, the
  # sysbuild_image_name_conf_dir branch), so Zephyr stops looking in
  # MCUboot's own bootloader/mcuboot/boot/zephyr for the image's
  # configuration inputs and looks here instead. Upstream ships three
  # Kconfig inputs at the paths that move:
  #
  #   prj.conf                    MCUboot's own base configuration
  #   socs/<soc>_<cluster>.conf   the per-SoC fragment
  #   boards/<board>.conf         a per-board fragment, if upstream has one
  #
  # Missing prj.conf fails the configure outright, which is at least loud.
  # A missing SoC fragment is SILENT: on the nRF54L15 it costs LTO,
  # ISR_TABLES_LOCAL_DECLARATION, the 512-byte RRAM write buffer
  # (NRF_RRAM_WRITE_BUFFER_SIZE counts 128-bit words, so 32 is 512 bytes and
  # the fallback 1 is 16) and BOOT_WATCHDOG_FEED=n, and the bootloader still
  # builds and still boots. So the two that exist for these SoCs are copied
  # under sysbuild/mcuboot/ (each file says so in its header) and this asks
  # upstream what it WOULD have contributed for the board being built, then
  # requires each answer to appear verbatim in the copy. An NCS bump that
  # edits one of them, and a new board whose SoC fragment nobody copied,
  # both fail here with the file to copy named. Same shape as the SDK patch
  # gate below: check the thing the build otherwise cannot see.
  #
  # THE REDIRECT MOVES THE DEVICETREE LOOKUPS TOO, and this guard does not
  # cover them, deliberately. Upstream also ships boot/zephyr/app.overlay,
  # boot/zephyr/boards/*.overlay and boot/zephyr/socs/*.overlay, and the
  # same APPLICATION_CONFIG_DIR redirect moves where those are looked for
  # (configuration_files.cmake:70-83). Nothing was lost there, because they
  # were already not applied before this round: sysbuild/mcuboot.overlay set
  # mcuboot_DTC_OVERLAY_FILE, which makes DTC_OVERLAY_FILE defined and skips
  # all three lookups outright. Demanding copies would therefore not restore
  # a lost setting, it would APPLY one for the first time: carrying
  # app.overlay would put zephyr,code-partition on &boot_partition where
  # this bootloader's devicetree has taken it from the board .dts since the
  # image existed, which Partition Manager overrides anyway. So the rule for
  # a new board is a reading task rather than a check: look in
  # bootloader/mcuboot/boot/zephyr/{boards,socs}/ for your target and fold
  # anything you need into your own per-board overlay by hand, because it
  # will not arrive on its own. README.md, "Adding a board", says so too.
  #
  # For the record on who does what upstream, because the first version of
  # this comment got it wrong: Nordic's Matter samples do NOT copy this
  # prj.conf. They author a bespoke, size-trimmed one and re-add LTO and
  # ISR_TABLES_LOCAL_DECLARATION in it and BOOT_WATCHDOG_FEED=n in their
  # per-board .conf, losing only NRF_RRAM_WRITE_BUFFER_SIZE. The copy-and-
  # carry-socs pattern used here is the one the SDK's own DFU samples use
  # (samples/dfu/compressed_update, samples/bluetooth/mesh/dfu/{target,
  # distributor}), so this is the SDK's other convention, not a departure
  # from it.
  if(NOT DEFINED ZEPHYR_MCUBOOT_MODULE_DIR)
    message(FATAL_ERROR
      "CONFIG_BOOTLOADER_MCUBOOT is set but the mcuboot Zephyr module is not "
      "in this workspace, so the bootloader image's configuration cannot be "
      "checked against upstream's.")
  endif()
  set(HEARTH_MCUBOOT_UPSTREAM_DIR ${ZEPHYR_MCUBOOT_MODULE_DIR}/boot/zephyr)
  set(HEARTH_MCUBOOT_LOCAL_DIR ${CMAKE_CURRENT_LIST_DIR}/sysbuild/mcuboot)

  # prj.conf is named rather than discovered, so unlike every other entry in
  # this list it is not known to exist. An upstream that ever drops it would
  # otherwise reach file(READ) and give a raw CMake error instead of this
  # block's own message.
  set(HEARTH_MCUBOOT_UPSTREAM_CONFS)
  if(EXISTS ${HEARTH_MCUBOOT_UPSTREAM_DIR}/prj.conf)
    list(APPEND HEARTH_MCUBOOT_UPSTREAM_CONFS ${HEARTH_MCUBOOT_UPSTREAM_DIR}/prj.conf)
  else()
    message(FATAL_ERROR
      "MCUboot no longer ships boot/zephyr/prj.conf, which this platform's "
      "sysbuild/mcuboot/prj.conf is a copy of:\n"
      "  looked for: ${HEARTH_MCUBOOT_UPSTREAM_DIR}/prj.conf\n"
      "Re-read how the MCUboot image gets its base configuration in this NCS "
      "before trusting the copy.")
  endif()
  zephyr_file(CONF_FILES ${HEARTH_MCUBOOT_UPSTREAM_DIR}/socs
              KCONF HEARTH_MCUBOOT_UPSTREAM_CONFS QUALIFIERS SUFFIX ${FILE_SUFFIX})
  zephyr_file(CONF_FILES ${HEARTH_MCUBOOT_UPSTREAM_DIR}/boards
              KCONF HEARTH_MCUBOOT_UPSTREAM_CONFS SUFFIX ${FILE_SUFFIX})

  # Containment, not equality, because the copies carry a header comment.
  # The consequence, stated here and in the copies: this sees upstream
  # changing, and does not see lines ADDED to a copy below the verbatim
  # block. Those apply silently and are the copies' one soft edge.
  foreach(upstream_conf ${HEARTH_MCUBOOT_UPSTREAM_CONFS})
    file(RELATIVE_PATH hearth_conf_rel ${HEARTH_MCUBOOT_UPSTREAM_DIR} ${upstream_conf})
    set(hearth_local_conf ${HEARTH_MCUBOOT_LOCAL_DIR}/${hearth_conf_rel})
    if(NOT EXISTS ${hearth_local_conf})
      message(FATAL_ERROR
        "MCUboot has a Kconfig fragment for board "
        "${BOARD}${BOARD_QUALIFIERS} that this platform does not carry, and "
        "the bootloader image will be built without it:\n"
        "  upstream: ${upstream_conf}\n"
        "  wanted:   ${hearth_local_conf}\n"
        "Copy it, under a header comment saying it is a copy and why (the "
        "files already there show the shape). It is not optional: the "
        "sysbuild/mcuboot/ directory that gives this image its per-board "
        "recovery-UART overlay also redirects where Zephyr looks for these, "
        "so what is not copied is simply not applied.")
    endif()
    file(READ ${upstream_conf} hearth_conf_upstream_text)
    file(READ ${hearth_local_conf} hearth_conf_local_text)
    string(FIND "${hearth_conf_local_text}" "${hearth_conf_upstream_text}"
           hearth_conf_found)
    if(hearth_conf_found EQUAL -1)
      message(FATAL_ERROR
        "MCUboot's own ${hearth_conf_rel} has changed and this platform's "
        "copy of it has not:\n"
        "  upstream: ${upstream_conf}\n"
        "  copy:     ${hearth_local_conf}\n"
        "Put upstream's current content back under the header comment in "
        "the copy, then rebuild the bootloader image and look at what "
        "moved.")
    endif()
    unset(hearth_conf_upstream_text)
    unset(hearth_conf_local_text)
  endforeach()
endif()

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
