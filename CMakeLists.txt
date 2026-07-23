cmake_minimum_required(VERSION 3.16)

# Reuse the shared AT-core component (engine + UART transport + link_mgr)
# from the sibling ESP-NOW repo, so both firmwares build against one source
# of truth. Both repos are expected to be checked out side by side.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../iLabs_AT_ESP-now/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ilabs_at_matter)
