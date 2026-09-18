/*
 * mt_devtypes_stub.c - empty since round 2 task 5.
 *
 * This file held the mt_devtypes.h quartet for the Silabs skeleton:
 * accept-all predicates and a create() that refused, so the AT+MTEP grammar
 * could be exercised end to end before any device type existed on this
 * platform (the nRF skeleton took the same shape). Task 5 replaced all four
 * with the real registry in port/mt_devtypes_sl.cpp, so there is nothing
 * left for a stub to answer: mt_devtypes.h has exactly four declarations and
 * the registry defines all four.
 *
 * THE FILE IS KEPT RATHER THAN DELETED, deliberately. Deleting it would mean
 * editing hearth.slcp's source list, test/host/Makefile's silabs-stubs target
 * and test/host/check_decls.py's pair list in the same commit, three lists
 * whose only purpose is to name this file, in exchange for one empty
 * translation unit. Keeping it costs nothing (an empty .c compiles to an
 * empty object) and leaves the gate reading exactly as it did: check_decls.py
 * proves that mt_devtypes.h's four declarations have exactly one definition
 * across the CONCATENATION of this file and mt_devtypes_sl.cpp, which is the
 * property that matters, and it goes on proving it whether the split is 4/0,
 * 0/4 or anything between.
 *
 * If a later round does delete it, all three lists move together.
 */
