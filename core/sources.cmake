# SDK-neutral source list. Each platform's build wraps this; core knows
# neither SDK (spec section 3).
set(HEARTH_CORE_DIR ${CMAKE_CURRENT_LIST_DIR})
set(HEARTH_CORE_SOURCES
    ${HEARTH_CORE_DIR}/at/at_parser.c
    ${HEARTH_CORE_DIR}/mt/mt_at.c
    ${HEARTH_CORE_DIR}/mt/mt_cmdbox.c
    ${HEARTH_CORE_DIR}/mt/mt_composition.c
    ${HEARTH_CORE_DIR}/mt/mt_comp_store.c
    ${HEARTH_CORE_DIR}/mt/mt_rows.c
    ${HEARTH_CORE_DIR}/mt/mt_transport.c)
set(HEARTH_CORE_INCLUDE_DIRS ${HEARTH_CORE_DIR}/include)
