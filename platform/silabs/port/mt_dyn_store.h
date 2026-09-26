/*
 * mt_dyn_store.h - port-internal handle on the dynamic endpoints' external
 * attribute store, and the bump arena the port allocates from.
 *
 * The source is platform/nrf54l15/port/mt_dyn_store.h in the firmware
 * repository, transferred with the rest of the device-type side in round 2
 * task 5. Every attribute on a dynamic endpoint is declared
 * EXTERNAL_STORAGE (DECLARE_DYNAMIC_ATTRIBUTE sets that flag
 * unconditionally), so CHIP keeps no value bytes of its own for them: the
 * values live in mt_devtypes_sl.cpp's per-endpoint blocks, one arena
 * allocation per created endpoint holding that endpoint's DataVersion array
 * and its attribute slots, sized for its own device type. This header is how
 * the rest of the port reaches those blocks; it is C++ only and never leaves
 * platform/silabs/port.
 *
 * WHAT DID NOT COME WITH IT, and why, so the next batch does not go looking:
 * the nRF header's remaining type-conditional store accessors (mt_dyn_mb_store
 * and mt_dyn_temp_levels_store). Each serves a host-fed list belonging to a
 * device type this round does not build (the ModeBase families, the
 * TemperatureLevel cabinet), and every one of them is dead code without its
 * device type. They arrive with the batch that ports the first of those
 * types. The mode select and chime accessors (mt_dyn_mode_store,
 * mt_dyn_chime_store) came with catalogue batch 4's mode select (0x0027),
 * along with the kStoreWalk table mt_devtypes_sl.cpp's store_bytes() walks.
 */

#pragma once

#include <clusters/ModeSelect/Structs.h>
#include <lib/core/DataModelTypes.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hearth_log.h"

/* MT_MODES_MAX_COUNT/MT_MODES_MAX_LABEL_LEN, the core-owned bounds the mode
 * store shape below is sized by. mt_matter.h carries its own extern "C"
 * guards. */
#include "mt_matter.h"

/* mt_mode_list_t and mt_chime_store_t: the plain-C store shapes, one
 * definition shared with the C6 port. This header wraps mt_mode_list_t with
 * the CHIP structs[] tail below (that half is C++ and cannot live in the
 * shared header); the chime store needs no wrapping and is used exactly as
 * mt_stores.h defines it. */
#include "mt_stores.h"

/*
 * ---- the bump arena ---------------------------------------------------
 *
 * THE ONE PLACE THE TWO PORTS' ALLOCATION STORY DIVERGES, so it is stated
 * here rather than left to be inferred from the sizing assertions.
 *
 * The nRF arm allocates endpoint blocks and cluster objects from two
 * K_HEAP_DEFINE arenas, and prices every block through Zephyr's sys_heap
 * chunk model: a 4-byte chunk header, CHUNK_UNIT rounding, and a
 * per-allocation cost of roundup(payload + 4, 8) (the endpoint heap) or
 * roundup(payload, 8) + 8 (the 8-aligned object heap). This platform has no
 * such allocator to model, and it must not draw on the one heap it does have
 * (round 2 task 2: freertos_heap_3 forwards pvPortMalloc() to the C library's
 * malloc(), which the SDK's linker wraps into sl_memory_manager's
 * .memory_manager_heap, so kernel objects, CHIP's allocations, mbedTLS and
 * OpenThread all share one pool). Drawing endpoint blocks from that pool
 * would give up all three properties the nRF's dedicated heap was chosen for:
 * an oversized composition could starve the stack rather than fail at the
 * endpoint that does not fit, the failure log could not name what was left,
 * and the budget would stop being a line item anyone can audit.
 *
 * So the arena is a static array with a bump pointer: allocate-only,
 * 8-aligned, zeroed. That is not a downgrade from the nRF's heap, because
 * the nRF's heap is allocate-only too (nothing frees, the composition is
 * edited by AT+MTEP and applied by a reboot, and a reboot resets the arena
 * wholesale) and a first-fit allocator serving one monotonic run of
 * allocations is exactly a bump pointer with bookkeeping.
 *
 * WHAT THAT DOES TO THE COST MODEL, which every sizing assertion in
 * mt_devtypes_sl.cpp rests on: with no free list there is no per-block
 * header and no bucket table, so a block costs its payload rounded up to 8
 * and nothing else, and the arena's USABLE bytes are its gross bytes. Both
 * of the nRF's overhead derivations go with the allocator that needed them:
 * the 80-byte kHeapOverheadBytes (end-marker chunk, the round-down loss and
 * chunk 0's struct z_heap plus bucket table) and the bucket-band
 * BUILD_ASSERTs that keep that 80 honest. There is nothing here for them to
 * describe; keeping them would be arithmetic about an allocator this image
 * does not link.
 */
struct hearth_arena {
    uint8_t *base;
    size_t   cap;
    size_t   used;
};

/* The whole cost model, in one function: rounding, and nothing else.
 * constexpr because the sizing assertions in mt_devtypes_sl.cpp evaluate it
 * at compile time, exactly as the nRF's kHeapCostOf() is evaluated. */
constexpr size_t hearth_arena_cost(size_t bytes)
{
    return (bytes + 7u) & ~static_cast<size_t>(7u);
}

/*
 * Allocate from an arena. Returns nullptr when the arena is full, which is
 * the same answer an exhausted k_heap gave the nRF: the caller logs its own
 * wall and returns -1, and the boot rebuild stops at that endpoint with the
 * endpoints before it live as a prefix (AT_MT_SPEC.md 501-506).
 *
 * Zeroed, so a block starts from the same all-zero state the nRF's
 * k_heap_alloc plus memset produced. Nothing frees: see the arena comment
 * above.
 */
inline void *hearth_arena_alloc(hearth_arena &a, size_t bytes, const char *what)
{
    size_t cost = hearth_arena_cost(bytes);
    if (a.used + cost > a.cap) {
        HEARTH_LOGE("arena", "%s exhausted: %u B wanted, %u of %u B used",
                    what, (unsigned)bytes, (unsigned)a.used, (unsigned)a.cap);
        return nullptr;
    }
    void *p = a.base + a.used;
    a.used += cost;
    memset(p, 0, cost);
    return p;
}

/*
 * Threading and reporting contract
 * --------------------------------
 * PREFERRED: do not call this function to read or write attribute values.
 * Go through emberAfReadAttribute() / emberAfWriteAttribute(), which reach
 * this same store through the ember external-storage callbacks in
 * mt_devtypes_sl.cpp. That path does two things this function cannot: it
 * validates against the attribute metadata, and it raises the
 * attribute-changed notification, so subscriptions and bindings observe the
 * new value. A value poked in through mt_dyn_attr_slot() changes what a
 * later read returns and nothing else: every existing subscriber keeps
 * reporting the old one. Task 6's mt_matter_attr_read/_write bridge is
 * expected to use the ember path for exactly this reason, as the nRF's
 * does.
 *
 * This function is for the cases the ember path cannot serve. Whoever calls
 * it:
 *   - must hold chip::DeviceLayer::StackLock unless already running on the
 *     CHIP thread, since the returned pointer aliases live storage that
 *     CHIP itself reads and writes;
 *   - must not keep the returned pointer past releasing that lock, and must
 *     not assume the endpoint is still live across a release;
 *   - owns raising the attribute-changed notification after any write.
 *
 * Look up one attribute's value bytes on a dynamic endpoint. On success
 * *data points at the live storage (writable, little-endian, exactly *size
 * bytes) and the function returns true. The pointer aliases into that
 * endpoint's arena block, which is allocated once at boot and never freed,
 * so it stays valid for the life of the boot subject to the locking rules
 * above. Returns false when the endpoint is not a live dynamic endpoint, or
 * when the attribute has no slot: that is the case for every Descriptor
 * attribute and for anything ARRAY-typed, which CHIP serves from its own
 * cluster objects rather than from here.
 */
bool mt_dyn_attr_slot(chip::EndpointId ep, chip::ClusterId cluster, chip::AttributeId attr,
                      uint8_t **data, uint8_t *size);

/*
 * ---- the type-conditional host-fed stores --------------------------------
 *
 * A mode select endpoint's block carries an mt_mode_store_t after its
 * attribute slots, and a chime endpoint's block an mt_chime_store_t: the
 * host-fed lists AT+MTMODES and AT+MTCHIMESOUNDS feed and the SDK's
 * SupportedModesManager reads back. Only the endpoints that ARE those types
 * pay for them, priced into the block layout in mt_devtypes_sl_arena.inc.
 *
 * CharSpan lifetime: mt_mode_store_t's structs[] members point at its own
 * list.entries[] label bytes. Both live in the endpoint's block, which is
 * allocated once at boot and never freed (the allocate-only invariant beside
 * hearth_arena above), so the spans stay valid for the life of the boot.
 * The in-place rebuild discipline is unchanged: mt_matter_modes_set()
 * rebuilds structs[] under the StackLock it holds across its whole body, and
 * the CHIP-task readers run under that same lock.
 *
 * mt_mode_entry_t and mt_mode_list_t (the count + entries[] half) come from
 * mt_stores.h, the shared plain-C definition; this header wraps that list
 * with the CHIP structs[] tail, which is C++ and cannot live there.
 * mt_chime_entry_t and mt_chime_store_t come from mt_stores.h unchanged: the
 * chime store carries no CHIP-typed half, so it needs no wrapper.
 */

struct mt_mode_store_t {
    mt_mode_list_t list;
    chip::app::Clusters::ModeSelect::Structs::ModeOptionStruct::Type structs[MT_MODES_MAX_COUNT];
};

/*
 * Locate ep's mode store / chime store through its block. Returns nullptr
 * when ep is not a live dynamic endpoint OR its device type carries no
 * such store (no ModeSelect / Chime cluster in its declared cluster list):
 * the callers' own endpoint and cluster checks fire first on the AT
 * surface, so a nullptr from here is their defensive
 * cannot-happen-once-rebuilt arm. The pointer aliases into the endpoint's
 * arena block and follows mt_dyn_attr_slot()'s locking rules above:
 * StackLock or the CHIP thread, never kept across a release. The store is
 * value-initialized (count 0) at endpoint create, before the endpoint is
 * served.
 */
mt_mode_store_t *mt_dyn_mode_store(chip::EndpointId ep);
mt_chime_store_t *mt_dyn_chime_store(chip::EndpointId ep);

/*
 * Log the endpoint arena's occupancy on the console: handed out, capacity,
 * free, and how many of the serviceable endpoint slots are live. Called once
 * per boot from src/main.cpp's rebuild_composition(), after the loop, so the
 * line sits beside the "composition rebuilt" line it explains. Defined in
 * mt_devtypes_sl.cpp, which owns the arena.
 */
void mt_dyn_arena_report(void);

/*
 * Log the cluster-object arena's occupancy on the console: handed out,
 * capacity, free. Called once per boot from src/main.cpp's
 * rebuild_composition(), on the line after mt_dyn_arena_report(), so the two
 * arena lines sit together. Defined in mt_matter_sl.cpp, which owns this
 * arena. Not an mt_matter.h declaration, so check_decls.py does not cover
 * this pairing, the way it covers mt_dyn_arena_report's.
 */
void mt_obj_arena_report(void);
