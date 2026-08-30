/*
 * mt_dyn_store.h - port-internal handle on the dynamic endpoints' external
 * attribute store (Matter core spec section 5).
 *
 * Every attribute on a dynamic endpoint is declared EXTERNAL_STORAGE (the
 * DECLARE_DYNAMIC_ATTRIBUTE macro sets that flag unconditionally), so CHIP
 * keeps no value bytes of its own for them: the values live in
 * mt_devtypes_zephyr.cpp's per-endpoint blocks, one heap allocation per
 * created endpoint holding that endpoint's DataVersion array, its attribute
 * slots and (store reclaim round) any type-conditional host-fed store its
 * device type carries, sized for its own device type. This header is how
 * the rest of the port reaches those blocks; it is C++ only and never
 * leaves platform/nrf54l15/port.
 *
 * mt_dyn_attr_slot() still has no external consumer: Task 5's
 * mt_matter_attr_read/_write bridge went through the ember path this
 * header's own comment below prefers, exactly as intended. It stays as the
 * deliberate contract for a future round that genuinely cannot go through
 * emberAfReadAttribute()/emberAfWriteAttribute() (an SDK callback that
 * already holds the ember lookup, say), so that round has a documented,
 * lock-audited entry point instead of walking the header table and
 * following block pointers ad hoc. The two store accessors below it DO
 * have consumers: mt_matter_zephyr.cpp's mode select manager, chime
 * delegate and the AT+MTMODES/AT+MTCHIMESOUNDS bridges all reach their
 * per-endpoint stores through them since the store reclaim round.
 */

#pragma once

#include <clusters/ModeSelect/Structs.h>
#include <lib/core/DataModelTypes.h>

#include <stdint.h>

/* MT_MODES_MAX_COUNT/MT_MODES_MAX_LABEL_LEN and MT_CHIME_MAX_SOUNDS/
 * MT_CHIME_MAX_NAME_LEN, the core-owned bounds the store shapes below are
 * sized by. mt_matter.h carries its own extern "C" guards. */
#include "mt_matter.h"

/*
 * Threading and reporting contract
 * --------------------------------
 * PREFERRED: do not call this function to read or write attribute values.
 * Go through emberAfReadAttribute() / emberAfWriteAttribute(), which reach
 * this same store through the ember external-storage callbacks in
 * mt_devtypes_zephyr.cpp. That path does two things this function cannot:
 * it validates against the attribute metadata, and it raises the
 * attribute-changed notification, so subscriptions and bindings observe the
 * new value. A value poked in through mt_dyn_attr_slot() changes what a
 * later read returns and nothing else: every existing subscriber keeps
 * reporting the old one. Task 5's mt_matter_attr_read/_write bridge uses
 * the ember path for exactly this reason.
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
 * endpoint's heap block, which is allocated once at boot and never freed,
 * so it stays valid for the life of the boot subject to the locking rules
 * above. Returns false when the endpoint is not a live dynamic endpoint, or
 * when the attribute has no slot: that is
 * the case for every Descriptor attribute and for anything ARRAY-typed,
 * which CHIP serves from its own cluster objects rather than from here.
 */
bool mt_dyn_attr_slot(chip::EndpointId ep, chip::ClusterId cluster, chip::AttributeId attr,
                      uint8_t **data, uint8_t *size);

/*
 * ---- the type-conditional host-fed stores (store reclaim round) --------
 *
 * A mode select endpoint's block carries an mt_mode_store_t after its
 * attribute slots, and a chime endpoint's block an mt_chime_store_t: the
 * host-fed lists AT+MTMODES and AT+MTCHIMESOUNDS feed and the SDK's
 * SupportedModesManager / ChimeDelegate read back. Until this round both
 * lived in .bss as 16-deep pools (7,040 B and 4,448 B) that every
 * composition paid for whether or not it contained a single mode select or
 * chime endpoint; now only the endpoints that ARE those types pay, priced
 * into their heap cost rows in mt_devtypes_zephyr.cpp's sizing table.
 *
 * The shapes are the .bss pools' slots minus the pool bookkeeping: no
 * `used` flag and no `ep` key, because a store IS its endpoint's, located
 * through the endpoint's block. count 0 means the host has not fed a list
 * yet, exactly what an unclaimed pool slot used to mean; the readers map
 * it to the same answers (an empty SupportedModes list, UnsupportedCluster
 * from getModeOptionByMode, PROVIDER_LIST_EXHAUSTED at index 0) so the
 * wire cannot tell the storage moved.
 *
 * CharSpan lifetime: mt_mode_store_t's structs[] members point at its own
 * entries[] label bytes. Both live in the endpoint's block, which is
 * allocated once at boot and never freed (the allocate-only invariant
 * beside K_HEAP_DEFINE), so the spans stay valid for the life of the boot,
 * the same argument the .bss store made from static storage. The in-place
 * rebuild discipline is unchanged: mt_matter_modes_set() rebuilds
 * structs[] under the StackLock it holds across its whole body, and the
 * CHIP-task readers run under that same lock.
 */

struct mt_mode_entry_t {
    uint8_t mode;
    char label[MT_MODES_MAX_LABEL_LEN + 1];
};

struct mt_mode_store_t {
    uint8_t count;
    mt_mode_entry_t entries[MT_MODES_MAX_COUNT];
    chip::app::Clusters::ModeSelect::Structs::ModeOptionStruct::Type structs[MT_MODES_MAX_COUNT];
};

struct mt_chime_entry_t {
    uint8_t id;
    char name[MT_CHIME_MAX_NAME_LEN + 1];
};

struct mt_chime_store_t {
    uint8_t count;
    mt_chime_entry_t entries[MT_CHIME_MAX_SOUNDS];
};

/*
 * Catalogue batch 5: the ModeBase store, one per (endpoint, cluster) on
 * the RVC's two mode clusters (RvcRunMode and RvcCleanMode both live on
 * one endpoint, so an RVC block carries TWO of these back to back; the
 * store walk keys each by its cluster id). The AT+MTMODES cluster-aware
 * form feeds it; HearthModeBaseDelegate (mt_matter_zephyr.cpp) reads it
 * back. Same shape as the C6's mt_mb_entry_t (main.cpp) minus the pool
 * bookkeeping: mode, the spec-mandated per-mode tag, label.
 *
 * Deliberately NO ModeOptionStruct half, unlike mt_mode_store_t above:
 * ModeBase's delegate contract COPIES on every read
 * (GetModeLabelByIndex fills a caller-supplied MutableCharSpan,
 * GetModeTagsByIndex a caller-supplied List, mode-base-server.h:205,
 * :235), so no CharSpan ever aliases these bytes and the whole
 * span-lifetime apparatus that makes the mode select store 436 B does not
 * apply. sizeof is 306 (1 count + pad + 8 x 38), asserted beside the
 * sizing table in mt_devtypes_zephyr.cpp.
 *
 * count 0 means the host has not fed a list, and unlike every other
 * host-fed store that state still ANSWERS: the placeholder-mode-0 policy
 * (delegate index 0 reads mode 0 / the cluster's tag-0 default / "Mode0")
 * exists because ModeBase::Instance::Init() reads index 0 before the host
 * could possibly have fed anything. See the delegate in
 * mt_matter_zephyr.cpp.
 */
struct mt_mb_entry_t {
    uint8_t mode;
    uint16_t tag;
    char label[MT_MB_MAX_LABEL_LEN + 1];
};

struct mt_mb_store_t {
    uint8_t count;
    mt_mb_entry_t entries[MT_MB_MAX_COUNT];
};

/*
 * Locate ep's mode store / chime store through its block. Returns nullptr
 * when ep is not a live dynamic endpoint OR its device type carries no
 * such store (no ModeSelect / Chime cluster in its declared cluster list):
 * the callers' own endpoint and cluster checks fire first on the AT
 * surface, so a nullptr from here is their defensive
 * cannot-happen-once-rebuilt arm, the exact role the exhausted .bss pool
 * played. The pointer aliases into the endpoint's heap block and follows
 * mt_dyn_attr_slot()'s locking rules above: StackLock or the CHIP thread,
 * never kept across a release. The store is value-initialized (count 0) at
 * endpoint create, before the endpoint is served.
 */
mt_mode_store_t *mt_dyn_mode_store(chip::EndpointId ep);
mt_chime_store_t *mt_dyn_chime_store(chip::EndpointId ep);

/* The ModeBase store accessor is keyed by (endpoint, CLUSTER), because one
 * RVC endpoint carries two of these stores; cluster must be RvcRunMode::Id
 * or RvcCleanMode::Id. Same nullptr and locking contract as the two
 * accessors above. */
mt_mb_store_t *mt_dyn_mb_store(chip::EndpointId ep, chip::ClusterId cluster);
