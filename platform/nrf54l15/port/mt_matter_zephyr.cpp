/*
 * mt_matter_zephyr.cpp - the mt_matter.h commissioning/state/network
 * slice against CHIP on Zephyr (Matter core spec section 6). The AT
 * parser thread calls these; anything touching CHIP state takes the
 * stack lock (the bridge_manager.cpp discipline).
 */

#include <app/ConcreteAttributePath.h>
#include <app/clusters/boolean-state-server/CodegenIntegration.h>
/* Catalogue batch 3, the two command-verdict types. door-lock-server.h also
 * pulls DlLockState, OperationSourceEnum, OperationErrorEnum, Nullable and
 * Optional into the global namespace itself (:46-64), which is why the
 * ember hook definitions further down can spell their signatures exactly as
 * the header declares them. */
#include <app/clusters/door-lock-server/door-lock-server.h>
#include <app/clusters/valve-configuration-and-control-server/valve-configuration-and-control-cluster.h>
#include <app/clusters/valve-configuration-and-control-server/valve-configuration-and-control-delegate.h>
/* Catalogue batch 4: the smoke/co alarm singleton server and its
 * ExpressedState priority recompute (std::array is the recompute's own
 * parameter type). */
#include <app/clusters/smoke-co-alarm-server/smoke-co-alarm-server.h>
/* Catalogue batch 4: the OperationalState Instance-plus-Delegate machinery
 * for the washer/dishwasher/dryer trio; <new> for the placement-new the
 * non-default-constructible Instance pool needs. */
#include <app/clusters/operational-state-server/operational-state-server.h>
/* Catalogue batch 4: the process-global SupportedModesManager for mode
 * select, and MatterReportingAttributeChangeCallback() for the host-fed
 * SupportedModes list's dirty-marking. */
#include <app/clusters/mode-select-server/supported-modes-manager.h>
/* Catalogue batch 4: the per-endpoint ChimeServer and its delegate. */
#include <app/clusters/chime-server/chime-server.h>
/* Catalogue batch 5: the stateless Switch event singleton for the generic
 * switch's AT+MTSWITCH bridge. */
#include <app/clusters/switch-server/switch-server.h>
/* Catalogue batch 5: the ModeBase Instance-plus-Delegate machinery for the
 * RVC's two mode clusters (RvcRunMode, RvcCleanMode share one server). The
 * RvcOperationalState Instance/Delegate come from operational-state-server.h
 * above, already included for the appliance trio. */
#include <app/clusters/mode-base-server/mode-base-server.h>
/* Catalogue batch 7a: the energy foundation. ElectricalPowerMeasurement and
 * PowerTopology are Instance-plus-Delegate served, and the port constructs
 * both itself (there is no esp-matter delegate-init callback layer on this
 * platform); ElectricalEnergyMeasurement is the free-function push server
 * plus ONE wildcard AttributeAccessInterface that nothing in the SDK ever
 * registers (declared ElectricalEnergyMeasurementCluster.h, Init() has no
 * caller anywhere in the tree), the C6's HearthEemInitCB disease, fixed in
 * mt_matter_eem_register() below. */
#include <app/clusters/electrical-power-measurement-server/electrical-power-measurement-server.h>
#include <app/clusters/power-topology-server/power-topology-server.h>
#include <app/clusters/electrical-energy-measurement-server/ElectricalEnergyMeasurementCluster.h>
#include <app/clusters/electrical-energy-measurement-server/electrical-energy-measurement-server.h>
/* Catalogue batch 7a: MeterIdentification, Instance-only (no delegate; the
 * Instance owns the attribute storage), constructed by
 * mt_meter_register_all()'s post-rebuild scan below because nothing in the
 * SDK ever calls its Init(). */
#include <app/clusters/meter-identification-server/meter-identification-server.h>
/* Catalogue batch 7a: DeviceEnergyManagement (Instance is both AAI and
 * CommandHandlerInterface; the two PA commands reach HearthDemDelegate
 * below through the Instance's CHI after the server's own pre-validation),
 * and EventLogging for the firmware-emitted PowerAdjustStart/End pair. */
#include <app/clusters/device-energy-management-server/device-energy-management-server.h>
/* Catalogue batch 7b: WaterHeaterManagement::Instance and Delegate, both
 * interface bases, the DEM shape (batch7-audit.md 2.2). */
#include <app/clusters/water-heater-management-server/water-heater-management-server.h>
/* Catalogue batch 8: RefrigeratorAlarmServer, the batch's one process-global
 * singleton with no Instance, no Delegate and no per-endpoint object at all.
 * AT+MTALARM's second cluster arm reaches it; nothing else in this file
 * does. */
#include <app/clusters/refrigerator-alarm-server/refrigerator-alarm-server.h>
/* Catalogue batch 8: the TemperatureControl iterator delegate interface and
 * its free-function SetInstance(), the SDK's "the app must supply it" hole
 * that no SDK code fills. */
#include <app/clusters/temperature-control-server/supported-temperature-levels-manager.h>
/* Catalogue batch 8: MicrowaveOvenControl::Instance and Delegate. The one
 * Instance in this file whose constructor takes references to two OTHER
 * clusters' Instances; the ordering that makes that safe is at
 * mt_matter_mwoc_register() below. */
#include <app/clusters/microwave-oven-control-server/microwave-oven-control-server.h>
#include <app/EventLogging.h>
#include <app/reporting/reporting.h>

#include <array>
#include <new>
#include <app/server/Server.h>
#include <clusters/AirQuality/Enums.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/util/attribute-storage.h>
#include <app/util/attribute-table.h>
/* AttributeBaseType(): the tree's own alias-to-base ZCL type mapping, read
 * by attr_type_info() below so the AT+MTATTR integer family cannot drift
 * from the SDK's definition of it. */
#include <app/util/ember-io-storage.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ConnectivityManager.h>
#include <platform/ThreadStackManager.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <openthread/dataset.h>
#include <openthread/link.h>
#include <openthread/thread.h>
#include <openthread/thread_ftd.h>

#include <stdio.h>
#include <string.h>

/* Memory reclaim round A: K_HEAP_DEFINE and k_heap_aligned_alloc() for the
 * cluster-object heap below. */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

extern "C" {
#include "mt_at.h"
#include "mt_composition.h"
#include "mt_matter.h"
}
/* Store reclaim round: the mode select and chime host-fed stores live in
 * each endpoint's block on the endpoint heap, reached through the
 * mt_dyn_*_store() accessors this header declares. */
#include "mt_dyn_store.h"
#include "mt_port_ids.h"

LOG_MODULE_REGISTER(hearth_matter, LOG_LEVEL_INF);

/*
 * ===================================================================
 * The cluster-object heap
 * ===================================================================
 *
 * Fourteen pools in this file used to be fixed arrays, one slot per
 * endpoint that COULD carry the family: the OperationalState,
 * RvcOperationalState, ModeBase, Chime, valve, EPM, PowerTopology, WHM,
 * DEM and MeterIdentification Delegates and the raw storage for their
 * Instances, 14,368 B of .bss and .data measured at 6c31f09. A realistic
 * six-endpoint domestic composition uses ZERO of them and the widest
 * energy composition uses a fraction, so the arrays are the defect the
 * user's principle names: something dynamic in nature, allocated
 * statically.
 *
 * They are now pointer tables into this heap, allocated during the boot
 * composition rebuild. It is the third time this port applies the
 * technique (the dyn-arena round moved the endpoint slot arena, the store
 * reclaim round the two host-fed stores) and the disciplines carry over
 * unchanged:
 *
 *   - ALLOCATE-ONLY. Nothing is ever freed. CHIP Instance and Delegate
 *     objects register endpoint-scoped CommandHandlerInterfaces and
 *     AttributeAccessInterfaces whose lifetime is the stack's, and this
 *     platform has no teardown path: AT+MTEP edits apply by reboot, which
 *     resets the heap wholesale. Placement-new into heap memory that is
 *     never freed preserves the old .bss lifetime exactly.
 *   - The DEPTH CONSTANTS STAY, and keep their meaning. kServiceableEndpoints,
 *     kModeBasePoolSlots, MT_MEAS_MAX, MT_DEM_MAX, MT_WHM_MAX and
 *     MT_METER_MAX are still each family's acceptance cap and are still
 *     checked first in every alloc; the heap is only what makes a slot
 *     cheap. Nothing about which compositions are admitted changes.
 *   - FAILURE is the established stop-at-failure prefix. A heap that
 *     cannot serve an alloc returns nullptr exactly as an exhausted pool
 *     did, mt_devtype_create() logs the wall and returns -1, and the
 *     endpoints created before it stay live as a prefix with unchanged
 *     ids (AT_MT_SPEC.md 501-506). The heap is sized so this cannot
 *     happen for any composition the other walls admit; see the
 *     arithmetic at the end of this file.
 *   - The construct-after-emberAfSetDynamicEndpoint() ordering is
 *     untouched. Only WHERE the delegate and the Instance's storage live
 *     changes; WHEN each is constructed does not. In particular the
 *     ModeBase Instance::Init() VerifyOrDie hazard is unchanged, and the
 *     block is allocated in the *_delegate_alloc() half, before anything
 *     is spent, so a heap shortfall aborts at the same point a pool
 *     shortfall always did.
 *   - The kInvalidEndpointId sentinel discipline the 7b fix round
 *     introduced for the two id-at-alloc pools (DEM, WHM) is unchanged:
 *     the alloc still stamps the sentinel and the real id still lands in
 *     the success-only second half.
 *
 * A block holds the Delegate first and, where the family has one, the raw
 * storage for its Instance immediately after at the Instance's own
 * alignment. One allocation per (endpoint, family) pair rather than two,
 * so the delegate pointer the AT bridge already carries is enough to find
 * the Instance storage; the layout is the endpoint block heap's
 * store_walk() idiom at a smaller scale.
 *
 * mt_matter_chime_delegate_unclaim() hands back the SLOT, not the block:
 * the allocate-only policy has no free. That is bounded at one stranded
 * block per boot, because the rebuild stops at the failure that provoked
 * the unclaim and nothing retries within a boot, the same bound the
 * stranded-claim policy already carries for every other pool.
 */
/* Stepped out of the anonymous namespace for the reason the endpoint heap
 * documents: K_HEAP_DEFINE emits a STRUCT_SECTION_ITERABLE that Zephyr's
 * static-heap init walks at boot, and that machinery expects a
 * section-placed object with external linkage. */
K_HEAP_DEFINE(hearth_obj_heap, HEARTH_OBJ_HEAP_BYTES);

namespace {

/*
 * What one 8-aligned allocation of `bytes` really costs this heap.
 *
 * sys_heap chunks are CHUNK_UNIT (8) aligned and carry a 4-byte header at
 * the front, so chunk_mem(c0) always lands at 8*c0+4 and an ordinary payload
 * can never be 8-aligned. sys_heap_aligned_alloc() (heap.c:309-385) rounds
 * that up to 8*(c0+1), and asks for one extra chunk to pay for the move:
 * padded_sz = bytes_to_chunksz(h, bytes, align - gap) with align - gap = 4,
 * so the allocation is roundup(bytes, 8) + 8 bytes wide.
 *
 * NO LEADING FRAGMENT IS SPLIT OFF, and it is worth saying why, because the
 * shape of the code invites the opposite conclusion. mem_to_chunkid()
 * (heap.c:157-161) is (mem - chunk_header_bytes(h) - base) / CHUNK_UNIT: it
 * subtracts the 4-byte header BEFORE dividing. With an 8-aligned base that
 * gives (8*(c0+1) - 4) / 8 = (8*c0 + 4) / 8 = c0, so the
 * `if (c > c0) { split_chunks(...); free_list_add(...); }` at heap.c:364-367
 * never fires. The eight extra bytes are the chunk header plus four bytes of
 * internal alignment slack INSIDE the one allocated chunk, not a stranded
 * prefix in bucket 0. (Fix re-review P1: an earlier version of this comment
 * claimed the prefix was split and freed. It is not, and the arithmetic
 * above is the check to re-run.)
 *
 * The SUFFIX split at heap.c:369-372 does run, because alloc_chunk() hands
 * back the whole remaining free region, and it is lossless: it returns the
 * remainder to the free list and leaves the used chunk at exactly padded_sz.
 *
 * So the model is exact rather than an upper bound. It must not be
 * "simplified" to roundup(bytes + 4, 8) by a later reader who prices only
 * the header: the alignment slack is real and the heap really does lose
 * roundup(bytes, 8) + 8 per allocation.
 *
 * Small-heap-ness (the 4-byte header) is the same derived property the
 * endpoint heap asserts; the two BUILD_ASSERTs there cover this heap too,
 * and the chunk-count bound below keeps this heap on the same side of it.
 */
constexpr size_t kObjCostOf(size_t bytes) { return ((bytes + 7) / 8) * 8 + 8; }

/*
 * Usable bytes, not the gross define, the endpoint heap's rule. For this
 * heap sys_heap_init() (heap.c:525-580) spends:
 *
 *    4  the end-marker chunk header       heap_footer_bytes(), small heap
 *    4  rounding 7164 down to a CHUNK_UNIT boundary
 *   72  chunk 0, holding struct z_heap: 28 bytes plus ten buckets x 4,
 *       so 68 rounded up to 9 chunks
 *
 * Ten buckets because bucket_idx() (heap.h:261-265) is
 * 31 - clz(heap_sz - min_chunk_size + 1) + 1 and this heap is 895 chunks,
 * which lands in the same [512, 1023] band as the 8 KB endpoint heap. The
 * BUILD_ASSERT keeps it there; it is written against heap_sz's own bounds
 * (gross/8 - 1 <= heap_sz <= gross/8) rather than the gross size, so a heap
 * one chunk the wrong side of 512 cannot slip through.
 *
 * Catalogue batch 8 moved the gross size 6528 to 7168 and the three terms
 * above are unchanged: 7168 is a whole number of chunks (896), the footer
 * still costs one chunk header, the rounding still loses the same 4 bytes,
 * and 895 chunks is still inside the ten-bucket band, so chunk 0 is still
 * 9 chunks. 80 bytes of overhead, 7,088 usable. The middle line's arithmetic
 * is the one that moved: it was "rounding 6524 down".
 */
BUILD_ASSERT((HEARTH_OBJ_HEAP_BYTES - 8) / 8 >= 512 && HEARTH_OBJ_HEAP_BYTES / 8 <= 1023,
             "the object heap left the ten-bucket band kObjHeapOverheadBytes is derived for");
constexpr size_t kObjHeapOverheadBytes = 80;
constexpr size_t kObjHeapUsableBytes   = HEARTH_OBJ_HEAP_BYTES - kObjHeapOverheadBytes;

/* Chunk-rounded bytes handed out so far, the endpoint heap's s_ep_heap_used
 * shape: handed out plus free reconciles against kObjHeapUsableBytes
 * exactly, so a capacity log can print both on one line. */
size_t s_obj_heap_used;

/*
 * The one allocation primitive. Always 8-aligned, so no caller has to think
 * about it, and always zeroed, so a heap-resident object starts from the
 * same all-zero state its .bss or .data predecessor did before its
 * constructor ran.
 */
void *obj_heap_alloc(size_t bytes)
{
    void *p = k_heap_aligned_alloc(&hearth_obj_heap, 8, bytes, K_NO_WAIT);
    if (p == nullptr) {
        /* Fix round M3: the caller's own wall message fires next and names
         * the family's CAP ("EPM delegate pool exhausted (MT_MEAS_MAX 8)"),
         * which is NOT what stopped this create if the pool still has free
         * slots. Say so here rather than leave two log lines contradicting
         * each other on an integrator's console. */
        LOG_ERR("cluster-object heap exhausted: %zu B wanted, %zu of %zu usable B used; "
                "the pool line that follows names that family's CAP, not the wall that "
                "stopped this create",
                bytes, s_obj_heap_used, kObjHeapUsableBytes);
        return nullptr;
    }
    memset(p, 0, bytes);
    s_obj_heap_used += kObjCostOf(bytes);
    return p;
}

/* Offset of the Instance's raw storage inside a delegate block. */
template <typename D, typename I>
constexpr size_t obj_inst_offset()
{
    return ((sizeof(D) + alignof(I) - 1) / alignof(I)) * alignof(I);
}

template <typename D, typename I>
constexpr size_t obj_pair_bytes()
{
    return obj_inst_offset<D, I>() + sizeof(I);
}

/* A delegate with the raw storage for its Instance behind it. The delegate
 * is constructed here (default-constructed, as the old array elements
 * were); the Instance is placement-constructed later by the family's
 * set_endpoint half, which is where its create-time arguments are known. */
template <typename D, typename I>
D *obj_pair_new()
{
    static_assert(alignof(D) <= 8 && alignof(I) <= 8,
                  "the cluster-object heap guarantees 8-byte alignment only");
    void *p = obj_heap_alloc(obj_pair_bytes<D, I>());
    return (p == nullptr) ? nullptr : new (p) D();
}

/*
 * The Instance storage inside the block this delegate starts.
 *
 * Fix round M4, stated honestly: this is NOT defined behaviour. The
 * object-representation rule gives a pointer into D's bytes an array bound
 * of sizeof(D), so stepping past that is out of bounds, not merely outside
 * the object. It is safe here because neither GCC nor Clang derives an
 * object bound from a reinterpret_cast of a heap-allocated pointer, and
 * because it is the same idiom store_walk() already uses load-bearingly for
 * the endpoint block heap. The judgement, not a claim of correctness: the
 * alternative is a second 4-byte-per-slot pointer table per family, about
 * 376 B of .bss, which is worse for the round's purpose. The zero-cost
 * hardening, if a later round wants it, is to keep the block base from
 * obj_heap_alloc() and carry it rather than recompute it from the delegate.
 */
template <typename D, typename I>
uint8_t *obj_inst_storage(D *d)
{
    return reinterpret_cast<uint8_t *>(d) + obj_inst_offset<D, I>();
}

/* A delegate with no Instance beside it (the valve, whose cluster server is
 * a free-function singleton). */
template <typename D>
D *obj_new()
{
    static_assert(alignof(D) <= 8, "the cluster-object heap guarantees 8-byte alignment only");
    void *p = obj_heap_alloc(sizeof(D));
    return (p == nullptr) ? nullptr : new (p) D();
}

/* Raw storage for an Instance that has no delegate at all (the
 * MeterIdentification pool). */
template <typename I>
uint8_t *obj_inst_new()
{
    static_assert(alignof(I) <= 8, "the cluster-object heap guarantees 8-byte alignment only");
    return static_cast<uint8_t *>(obj_heap_alloc(sizeof(I)));
}

} /* namespace */

using chip::DeviceLayer::ConnectivityMgr;
using chip::DeviceLayer::ThreadStackMgr;
using chip::DeviceLayer::ThreadStackMgrImpl;
using chip::Protocols::InteractionModel::Status;

extern "C" int mt_matter_state(void)
{
    chip::DeviceLayer::StackLock lock;
    if (chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen()) {
        return MT_STATE_COMMISSIONING;
    }
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0) {
        return MT_STATE_OPERATIONAL;
    }
    return MT_STATE_UNINIT;
}

extern "C" int mt_matter_fabric_count(void)
{
    chip::DeviceLayer::StackLock lock;
    return chip::Server::GetInstance().GetFabricTable().FabricCount();
}

extern "C" int mt_matter_open_commissioning(int timeout_s)
{
    chip::DeviceLayer::StackLock lock;
    CHIP_ERROR err = chip::Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow(
        chip::System::Clock::Seconds32(timeout_s));
    return (err == CHIP_NO_ERROR) ? 0 : -1;
}

extern "C" int mt_matter_onboarding_codes(char *qr, size_t qr_len, char *manual, size_t manual_len)
{
    /*
     * StackLock here is C6-parity discipline, not a bug fix for an observed
     * race: fix round 1's bare-ERROR bench finding turned out to be a
     * controller test-form error (AT+MTCODES, the EXEC form, sent instead
     * of the AT+MTCODES? query form that cmd_mtcodes actually requires;
     * core/mt/mt_at.c:202-215), caught and confirmed on this same build.
     * No settings-I/O race was ever observed, and none should be claimed
     * here. The original task review judged this call lock-free-safe, and
     * that judgment stands; the lock is kept anyway because the C6 takes
     * ChipStackLock for this exact function and uniform locking across
     * every CHIP-touching function in this file is cheaper to reason about
     * than a per-function safety argument for the one exception.
     *
     * The LOG_ERR calls below stay for a different, real reason: chasing
     * fix round 1's bare ERROR back to its actual cause (a caller-side
     * command-form mismatch, not this function) took longer than it should
     * have precisely because this path logged nothing on failure. Keeping
     * these means any future failure here, whatever its cause, is visible
     * on the console instead of forcing that same trace again.
     */
    if (qr_len == 0 || manual_len == 0) {
        /* qr_len - 1 / manual_len - 1 below would underflow a size_t of 0
         * into SIZE_MAX, handing MutableCharSpan a buffer size no caller
         * ever meant. Refused before the lock is even taken. */
        return -1;
    }
    chip::DeviceLayer::StackLock lock;

    chip::RendezvousInformationFlags flags(chip::RendezvousInformationFlag::kBLE);
    chip::MutableCharSpan qr_span(qr, qr_len - 1);
    chip::MutableCharSpan manual_span(manual, manual_len - 1);

    CHIP_ERROR err = GetQRCode(qr_span, flags);
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("mt_matter_onboarding_codes: GetQRCode failed: %" CHIP_ERROR_FORMAT, err.Format());
        return -1;
    }
    err = GetManualPairingCode(manual_span, flags);
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("mt_matter_onboarding_codes: GetManualPairingCode failed: %" CHIP_ERROR_FORMAT, err.Format());
        return -1;
    }
    qr[qr_span.size()] = '\0';
    manual[manual_span.size()] = '\0';
    return 0;
}

extern "C" void mt_matter_factory_reset(void)
{
    /* Schedules erase-and-reboot on the CHIP thread; cmd_mtfreset's OK
     * is already on the wire by the time the reboot lands, mirroring
     * the C6 ordering. */
    chip::Server::GetInstance().ScheduleFactoryReset();
}

extern "C" int mt_matter_net_info(int *transport, int *enabled, int *connected)
{
    chip::DeviceLayer::StackLock lock;
    if (transport) *transport = MT_NET_THREAD;
    if (enabled)   *enabled   = ConnectivityMgr().IsThreadEnabled() ? 1 : 0;
    if (connected) *connected = ConnectivityMgr().IsThreadAttached() ? 1 : 0;
    return 0;
}

extern "C" int mt_matter_transport_mismatch(void)
{
    /* Thread is the only transport this image can ever have
     * commissioned on, so the stored-credentials-on-the-wrong-transport
     * state is unreachable here. The C6's marker comparison collapses
     * to a constant. */
    return 0;
}

/*
 * OT role to Matter RoutingRoleEnum (ThreadNetworkDiagnostics), matching
 * CHIP's own derivation (thread-network-diagnostics-provider.cpp:112-118)
 * and AT_MT_SPEC.md's `<role>` table (~2894-2907): DISABLED is
 * kUnspecified (0), DETACHED is kUnassigned (1) - the two are NOT the
 * same token, unlike a plain "no dataset" or "not attached" read. A CHILD
 * with the radio off when idle is kSleepyEndDevice (2) regardless of
 * router eligibility; a CHILD with the radio on is kReed (4) when
 * router-eligible, kEndDevice (3) otherwise. Both build arms compile
 * CONFIG_OPENTHREAD_FTD=y, so otThreadIsRouterEligible() is always
 * reachable here and no #if guard is needed the way CHIP's own
 * CHIP_DEVICE_CONFIG_THREAD_FTD one is.
 */
static uint8_t ot_role_to_matter(otInstance *ot)
{
    switch (otThreadGetDeviceRole(ot)) {
    case OT_DEVICE_ROLE_DISABLED: return 0;  /* kUnspecified */
    case OT_DEVICE_ROLE_DETACHED: return 1;  /* kUnassigned */
    case OT_DEVICE_ROLE_CHILD: {
        otLinkModeConfig mode = otThreadGetLinkMode(ot);
        if (!mode.mRxOnWhenIdle) {
            return 2;  /* kSleepyEndDevice */
        }
        return otThreadIsRouterEligible(ot) ? 4 /* kReed */ : 3 /* kEndDevice */;
    }
    case OT_DEVICE_ROLE_ROUTER: return 5;    /* kRouter */
    case OT_DEVICE_ROLE_LEADER: return 6;    /* kLeader */
    default: return 0;                       /* kUnspecified */
    }
}

extern "C" int mt_matter_thread_info(mt_thread_info_t *out)
{
    if (!out) {
        return MT_ATTR_ERR_FAILED;
    }
    memset(out, 0, sizeof(*out));

    ThreadStackMgr().LockThreadStack();
    otInstance *ot = ThreadStackMgrImpl().OTInstance();
    if (!ot) {
        /* No OpenThread instance yet (should not happen once the Thread
         * stack has started, but this read must never dereference a null
         * otInstance). */
        ThreadStackMgr().UnlockThreadStack();
        return MT_ATTR_ERR_FAILED;
    }
    out->role = ot_role_to_matter(ot);
    otDeviceRole role = otThreadGetDeviceRole(ot);
    out->attached = (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER ||
                     role == OT_DEVICE_ROLE_LEADER);

    /*
     * The four id fields (and the network name) are gated on "dataset
     * installed" (otDatasetIsCommissioned()), NOT on the attached
     * predicate above: AT_MT_SPEC.md's binding derivation (~2927-2949)
     * is explicit that a device holding a dataset but still detached
     * already reports its channel, PAN ID, extended PAN ID, partition id
     * and network name, matching CHIP's own gate
     * (thread-network-diagnostics-provider.cpp:68,
     * `if (!otDatasetIsCommissioned(otInst))`). Using `attached` here
     * would make every one of those fields read null for the entire
     * window between dataset install and attachment, which is exactly
     * the false negative the spec calls out.
     */
    if (otDatasetIsCommissioned(ot)) {
        out->has_channel = true;
        out->channel = otLinkGetChannel(ot);
        out->has_panid = true;
        out->panid = otLinkGetPanId(ot);
        const otExtendedPanId *ext = otThreadGetExtendedPanId(ot);
        out->has_extpanid = true;
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) {
            v = (v << 8) | ext->m8[i];
        }
        out->extpanid = v;
        out->has_partitionid = true;
        out->partitionid = otThreadGetPartitionId(ot);

        /* Same gate as the four ids above: with no dataset installed,
         * OpenThread's otThreadGetNetworkName() still answers its
         * compiled-in default ("OpenThread"), which is not this
         * device's network name and must not reach the wire. out->name
         * is already zeroed (memset above), so leaving this block
         * unentered is what makes the unconfigured case render "". */
        const char *name = otThreadGetNetworkName(ot);
        if (name) {
            strncpy(out->name, name, sizeof(out->name) - 1);
            out->name[sizeof(out->name) - 1] = '\0';
        }
    }
    ThreadStackMgr().UnlockThreadStack();
    return MT_ATTR_OK;
}

extern "C" const char *mt_thread_role_name(uint8_t role)
{
    switch (role) {
    case 0: return "UNSPECIFIED";
    case 1: return "UNASSIGNED";
    case 2: return "SLEEPY_END_DEVICE";
    case 3: return "END_DEVICE";
    case 4: return "REED";
    case 5: return "ROUTER";
    case 6: return "LEADER";
    default: return nullptr;
    }
}

/* ---- the live endpoint table ----------------------------------------- */

/*
 * What the boot rebuild actually created, in creation order. The stored
 * composition (mt_comp_store.h) is the intent; this is the outcome, and
 * AT+MTEP? reports this one. Parallel arrays rather than a struct array,
 * mirroring the C6's s_live_* tables so the two read alike.
 *
 * Written only from the boot path in main.cpp, before mt_at_start() lets
 * any AT command run, so there is no concurrent access to guard.
 *
 * Depth is kServiceableEndpoints (capacity), not MT_COMP_MAX_ENDPOINTS
 * (acceptance): the only writer is mt_matter_record_endpoint(), which
 * main.cpp calls once per endpoint that mt_devtype_create() actually stood
 * up, so an entry here can only exist for an endpoint this build is
 * serving. A composition may DECLARE 28, but the seventeenth never gets
 * created and so is never recorded. Nothing indexes these by composition
 * index either: mt_matter_endpoint_info() bounds its caller against
 * s_live_count, which is what main.cpp's parent lookup passes through.
 */
static uint32_t s_live_devtype[kServiceableEndpoints];
static uint16_t s_live_ep_id[kServiceableEndpoints];
static uint8_t s_live_variant[kServiceableEndpoints];
static uint8_t s_live_parent[kServiceableEndpoints];
static uint16_t s_live_count;

extern "C" uint16_t mt_matter_endpoint_count(void)
{
    return s_live_count;
}

extern "C" int mt_matter_endpoint_info(uint16_t index, uint32_t *devtype, uint16_t *ep_id,
                                       uint8_t *variant, uint8_t *parent_idx)
{
    if (index >= s_live_count) {
        if (devtype) *devtype = 0;
        if (ep_id) *ep_id = 0;
        if (variant) *variant = 0;
        if (parent_idx) *parent_idx = 0;
        return -1;
    }
    if (devtype) *devtype = s_live_devtype[index];
    if (ep_id) *ep_id = s_live_ep_id[index];
    if (variant) *variant = s_live_variant[index];
    if (parent_idx) *parent_idx = s_live_parent[index];
    return 0;
}

extern "C" void mt_matter_record_endpoint(uint32_t devtype, uint16_t ep_id, uint8_t variant,
                                          uint8_t parent_idx)
{
    if (s_live_count >= kServiceableEndpoints) {
        return;
    }
    s_live_devtype[s_live_count] = devtype;
    s_live_ep_id[s_live_count] = ep_id;
    s_live_variant[s_live_count] = variant;
    s_live_parent[s_live_count] = parent_idx;
    s_live_count++;
}

/* ---- attribute bridge (Task 5, AT+MTATTR) ------------------------------ */

/*
 * ZCL type to signedness and byte width; anything outside the AT+MTATTR
 * integer family is MT_ATTR_ERR_TYPE.
 *
 * Fix round 2 (bench bug). The switch below knows only the BASE integer,
 * bool, enum and bitmap type codes. Matter also has a large family of ALIAS
 * type codes that are integers wearing a semantic name: ZCL_TEMPERATURE
 * (0xD8), ZCL_PERCENT (0xE6), ZCL_PERCENT100THS (0xE7), ZCL_EPOCH_S,
 * ZCL_ELAPSED_S, ZCL_VENDOR_ID and about thirty more. Catalogue batch 2 was
 * the first round to declare any of them (the thermostat's TEMPERATURE
 * setpoints, the fan's PERCENT settings, the window covering's
 * PERCENT100THS lift positions), and every one of those attributes fell
 * straight through to `default: return false`. The damage: an AT+MTATTR
 * read answered +MTERR:5 and MatterPostAttributeChangeCallback dropped the
 * +MTATTR URC for controller-driven changes, while the very same attributes
 * worked perfectly over a controller's IM. Bench-confirmed on all three
 * clusters.
 *
 * Nasty because at the AT layer the failure is indistinguishable from
 * correct behaviour: mt_matter.h:178-181 makes a NULL nullable read answer
 * MT_ATTR_ERR_TYPE too, so +MTERR:5 is the honest answer for a null value
 * AND the answer for "not an integer attribute". A tester reading +MTERR:5
 * on a fresh fan sees exactly what a null PercentSetting would look like.
 *
 * The fix normalises through the SDK's own alias table rather than
 * transcribing one here. There is no shared core-side helper to reuse:
 * core/include/mt_at.h exports no type-family classifier, and the C6's
 * (main.cpp's attr_val_is_unsigned()/attr_val_to_i64()) is keyed on
 * esp_matter_val_type_t, esp-matter's own enum, which is why the C6 never
 * had this bug: esp-matter maps the alias types down to its INT16/UINT8/
 * UINT16 val types before its AT bridge ever sees them. This port reads
 * ember metadata directly, so it must do that mapping itself.
 *
 * chip::app::Compatibility::Internal::AttributeBaseType()
 * (app/util/ember-io-storage.cpp:36-126, declared in ember-io-storage.h) is
 * THE definition of the family in this tree: it maps every alias to its
 * basic int(8|16|32|64)(s|u) code and, critically, returns any type it does
 * not recognise unchanged (`default: return type`, :123-124). So the switch
 * below is reached with exactly the value it used to be reached with for
 * every base type and for every string, array, struct and float:
 * base-type behaviour is unchanged by construction and no arm below is
 * touched. What changes is only that aliases now arrive pre-translated.
 *
 * The mappings this catalogue needs, from that function:
 *   ZCL_TEMPERATURE   (0xD8) -> ZCL_INT16S  (signed, 2)    :120-121
 *   ZCL_PERCENT       (0xE6) -> ZCL_INT8U   (unsigned, 1)  :40-48
 *   ZCL_PERCENT100THS (0xE7) -> ZCL_INT16U  (unsigned, 2)  :50-62
 * and, covered for free the moment a future device type needs them:
 * ENUM16/BITMAP16/VENDOR_ID/GROUP_ID/ENDPOINT_NO -> INT16U;
 * EPOCH_S/ELAPSED_S/CLUSTER_ID/ATTRIB_ID/COMMAND_ID/EVENT_ID/DEVTYPE_ID/
 * DATA_VER/BITMAP32 -> INT32U; EPOCH_US/POSIX_MS/SYSTIME_MS/SYSTIME_US/
 * NODE_ID/FABRIC_ID/EVENT_NO/BITMAP64 -> INT64U; the electrical-measurement
 * family (POWER_MW, POWER_MVA, POWER_MVAR, AMPERAGE_MA, VOLTAGE_MV,
 * ENERGY_MWH, ENERGY_MVAH, ENERGY_MVARH, MONEY) -> INT64S;
 * ACTION_ID/FABRIC_IDX/STATUS -> INT8U.
 *
 * Signedness agrees with chip::app::IsSignedAttributeType()
 * (app-common/zap-generated/attribute-type.h), the tree's other oracle:
 * TEMPERATURE is listed signed there and maps to INT16S here.
 *
 * attr_null_sentinel() below needs no change of its own. It is keyed on the
 * (is_unsigned, bytes) pair this function produces, so TEMPERATURE gets the
 * signed 2-byte sentinel 0x8000, PERCENT the unsigned 1-byte 0xFF and
 * PERCENT100THS the unsigned 2-byte 0xFFFF: exactly the bytes
 * mt_devtypes_zephyr.cpp's seed rows write for those attributes.
 */
static bool attr_type_info(EmberAfAttributeType t, bool *is_unsigned, uint8_t *bytes)
{
    switch (chip::app::Compatibility::Internal::AttributeBaseType(t)) {
    case ZAP_TYPE(BOOLEAN): case ZAP_TYPE(BITMAP8): case ZAP_TYPE(ENUM8): case ZAP_TYPE(INT8U):
        *is_unsigned = true;  *bytes = 1; return true;
    case ZAP_TYPE(BITMAP16): case ZAP_TYPE(ENUM16): case ZAP_TYPE(INT16U):
        *is_unsigned = true;  *bytes = 2; return true;
    case ZAP_TYPE(INT24U): *is_unsigned = true; *bytes = 3; return true;
    case ZAP_TYPE(BITMAP32): case ZAP_TYPE(INT32U):
        *is_unsigned = true;  *bytes = 4; return true;
    case ZAP_TYPE(INT48U): *is_unsigned = true; *bytes = 6; return true;
    case ZAP_TYPE(BITMAP64): case ZAP_TYPE(INT64U):
        *is_unsigned = true;  *bytes = 8; return true;
    case ZAP_TYPE(INT8S):  *is_unsigned = false; *bytes = 1; return true;
    case ZAP_TYPE(INT16S): *is_unsigned = false; *bytes = 2; return true;
    case ZAP_TYPE(INT32S): *is_unsigned = false; *bytes = 4; return true;
    case ZAP_TYPE(INT64S): *is_unsigned = false; *bytes = 8; return true;
    default: return false;
    }
}

/*
 * ---- the Instance-served attribute carve-out (fix round, DE397) --------
 *
 * Four attributes in this catalogue are served by a per-endpoint C++
 * object, not by the arena: OperationalState and CurrentPhase on cluster
 * 0x0060 (the appliance trio's Instance) and SelectedChime and Enabled on
 * cluster 0x0556 (the ChimeServer). Their ember slots exist only for
 * AttributeList truthfulness; without this carve-out an AT+MTATTR read
 * answered the inert seed and a write "succeeded" into it, changing
 * nothing fabric-visible while still echoing a +MTATTR URC.
 *
 * What the C6 actually does for these four, traced for this fix round
 * (controller ruling DE397 asked for a mirror of the READ behaviour):
 *
 *   READS answer the LIVE served value on the C6. Its generic read leg
 *   calls esp_matter::attribute::get_val(), whose (endpoint, cluster,
 *   attribute) overload reads through the full data model provider,
 *   provider::get_instance().ReadAttribute(request, encoder)
 *   (esp-matter esp_matter_data_model.cpp:927-968, the provider call at
 *   :957), which consults the registered Instance/server object ahead of
 *   ember. Mirrored here: the two live-read helpers below fetch the same
 *   objects this file already owns, so an AT read answers what a
 *   subscribed controller sees.
 *
 *   WRITES split by cluster, on both platforms, and the two dispositions
 *   below mirror the C6's observable wire exactly (fix round 2, DE397 as
 *   amended by the controller after the round 1 trace):
 *
 *   OperationalState pair: +MTERR:11. On the C6 these are created
 *   MANAGED_INTERNALLY without WRITABLE (esp_matter_attribute.cpp:
 *   2417-2421 CurrentPhase, :2435-2440 OperationalState), so set_val()
 *   returns ESP_ERR_NOT_SUPPORTED (esp_matter_data_model.cpp:1091-1104)
 *   and the C6 bridge maps that to MT_ATTR_ERR_READONLY (DE270).
 *   Mirrored as a refusal here; AT+MTOPSTATE is the write path.
 *
 *   Chime pair: the write ROUTES TO THE LIVE ChimeServer. On the C6
 *   these are MANAGED_INTERNALLY WITH WRITABLE plus NONVOLATILE
 *   (esp_matter_attribute.cpp:4872-4884), so set_val() takes the
 *   set_val_via_write_attribute() branch (esp_matter_data_model.cpp:
 *   1097-1098, impl :1026-1076): a real provider WriteAttribute into
 *   ChimeCluster::WriteAttribute (esp tree ChimeCluster.cpp:203-221),
 *   which decodes and calls SetSelectedChime()/SetEnabled(). The
 *   observable C6 wire, mirrored by mt_chime_attr_write_live() below:
 *     - out-of-width value (SelectedChime > 255, Enabled > 1): +MTERR:1
 *       (the C6's i64_to_attr_val() width/bool rejection, main.cpp:
 *       456-463, mapped to MT_ATTR_ERR_VALUE at its call site);
 *     - an in-width but UNINSTALLED SelectedChime id: bare ERROR, not
 *       +MTERR:1. ChimeCluster::SetSelectedChime answers Status::NotFound
 *       (esp tree ChimeCluster.cpp:223-228), which
 *       set_val_via_write_attribute() collapses to ESP_FAIL (:1069-1073)
 *       and the C6 bridge's default arm renders MT_ATTR_ERR_FAILED. Note
 *       the deliberate contrast with AT+MTCHIME, whose own contract maps
 *       the same NotFound to +MTERR:1; the two commands' wires already
 *       differ on the C6 and that difference is preserved, not invented;
 *     - success, including a same-value re-push: OK (the C6 handler
 *       returns Success without writing on same-value, and the provider
 *       absorbs NOT_FINISHED, esp_matter_data_model_provider.cpp:419-429);
 *     - NO +MTATTR URC in either notify mode: the provider path bypasses
 *       esp-matter's attribute update callback entirely (the AirQuality
 *       precedent, C6 main.cpp), and here the direct setter call bypasses
 *       emberAfWriteAttribute and so this file's
 *       MatterPostAttributeChangeCallback. The fabric still sees the
 *       change: the setter marks the path dirty itself
 *       (chime-server.cpp:224, :243). notify therefore selects nothing on
 *       this path, exactly as on the C6.
 *   KVS wear note: SetSelectedChime()/SetEnabled() persist every changed
 *   write through SafeAttributePersistenceProvider (chime-server.cpp:
 *   219-224, :238-243), so this write path is a settings_storage wear
 *   source the round 1 inert slot was not; ties to the existing 32 KB
 *   occupancy watch item. The C6's handler persists identically (esp
 *   tree ChimeCluster.cpp:235-236), so the wear parity is exact too.
 *
 *   One C6 comment caught out by this trace, recorded for the controller:
 *   the C6's DE270 note (main.cpp, the ESP_ERR_NOT_SUPPORTED mapping)
 *   claims "no device type this firmware creates today ships such an
 *   attribute" for the MANAGED_INTERNALLY+WRITABLE branch; the chime pair
 *   is exactly such an attribute and shipped five rounds before that
 *   comment was written. The branch's behaviour is what this carve-out
 *   mirrors; the stale sentence is the C6's to fix.
 *
 * Table-driven and additive: exactly these four (cluster, attribute)
 * pairs, matched only after attr_locate() has proven the endpoint carries
 * the cluster, so no existing attribute's behaviour changes and the
 * ENDPOINT/CLUSTER error division is untouched. The live-read helpers are
 * defined beside their pools further down; only the dispatch lives here.
 */
struct instance_served_attr {
    uint32_t cluster;
    uint32_t attr;
};

static const instance_served_attr k_instance_served[] = {
    { chip::app::Clusters::OperationalState::Id,
      chip::app::Clusters::OperationalState::Attributes::OperationalState::Id },
    { chip::app::Clusters::OperationalState::Id,
      chip::app::Clusters::OperationalState::Attributes::CurrentPhase::Id },
    { chip::app::Clusters::Chime::Id, chip::app::Clusters::Chime::Attributes::SelectedChime::Id },
    { chip::app::Clusters::Chime::Id, chip::app::Clusters::Chime::Attributes::Enabled::Id },
    /* Catalogue batch 5, the RVC's four. The two ModeBase CurrentModes
     * and both RvcOperationalState scalars are Instance-served (their
     * ember slots are inert shadows); reads answer the live pools below,
     * writes are refused with +MTERR:11 like the base opstate pair: on
     * the C6 all four are MANAGED_INTERNALLY without WRITABLE
     * (esp_matter_attribute.cpp:3865 for ModeBase CurrentMode,
     * :2417-2440 for the opstate pair the derived cluster reuses), so its
     * set_val() answers ESP_ERR_NOT_SUPPORTED and the DE270 mapping
     * renders the identical +MTERR:11. ChangeToMode and AT+MTOPSTATE are
     * the write paths. Note the ModeBase CLUSTER REVISION is deliberately
     * NOT here: ModeBase's AAI has no default arm, so revision reads
     * genuinely fall through to the arena and the generic path serves
     * them live (the seed rows in mt_devtypes_zephyr.cpp). */
    { chip::app::Clusters::RvcRunMode::Id,
      chip::app::Clusters::RvcRunMode::Attributes::CurrentMode::Id },
    { chip::app::Clusters::RvcCleanMode::Id,
      chip::app::Clusters::RvcCleanMode::Attributes::CurrentMode::Id },
    { chip::app::Clusters::RvcOperationalState::Id,
      chip::app::Clusters::OperationalState::Attributes::OperationalState::Id },
    { chip::app::Clusters::RvcOperationalState::Id,
      chip::app::Clusters::OperationalState::Attributes::CurrentPhase::Id },
    /* Catalogue batch 7a, the DE407 option-C rows: the seven
     * ElectricalPowerMeasurement push fields are declared with their true
     * 64-bit ZCL types (the AAI gate at CodegenDataModelProvider_Read.cpp:108-109
     * requires ember metadata), take no arena slot (attr_gets_slot()'s
     * md.size <= kSlotDataBytes refusal), and are served by the EPM
     * Instance from the HearthEpmDelegate cache below, so an AT read
     * answers the same live value a subscribed controller sees (null until
     * first pushed: +MTERR:5, the no-null-literal rule). Writes answer
     * +MTERR:11 through the non-Chime arm below, mirroring the C6, where
     * all seven are created ATTRIBUTE_FLAG_MANAGED_INTERNALLY without
     * WRITABLE (esp_matter_attribute.cpp:3918-3960, create_voltage() and
     * siblings) so its set_val() answers ESP_ERR_NOT_SUPPORTED and the
     * DE270 mapping renders the identical +MTERR:11; AT+MTMEAS is the
     * write path. PowerMode and NumberOfMeasurementTypes are deliberately
     * NOT here: their slots are seeded to the exact constants the delegate
     * serves (kAc, 1), so the generic arena read answers truthfully, the
     * FanControl agreement discipline. The EEM struct attributes need no
     * rows either: STRUCT falls out of attr_type_info() and answers
     * +MTERR:5 on the generic path, the same code the C6 answers. */
    { chip::app::Clusters::ElectricalPowerMeasurement::Id,
      chip::app::Clusters::ElectricalPowerMeasurement::Attributes::Voltage::Id },
    { chip::app::Clusters::ElectricalPowerMeasurement::Id,
      chip::app::Clusters::ElectricalPowerMeasurement::Attributes::ActiveCurrent::Id },
    { chip::app::Clusters::ElectricalPowerMeasurement::Id,
      chip::app::Clusters::ElectricalPowerMeasurement::Attributes::ActivePower::Id },
    { chip::app::Clusters::ElectricalPowerMeasurement::Id,
      chip::app::Clusters::ElectricalPowerMeasurement::Attributes::RMSVoltage::Id },
    { chip::app::Clusters::ElectricalPowerMeasurement::Id,
      chip::app::Clusters::ElectricalPowerMeasurement::Attributes::RMSCurrent::Id },
    { chip::app::Clusters::ElectricalPowerMeasurement::Id,
      chip::app::Clusters::ElectricalPowerMeasurement::Attributes::Frequency::Id },
    { chip::app::Clusters::ElectricalPowerMeasurement::Id,
      chip::app::Clusters::ElectricalPowerMeasurement::Attributes::PowerFactor::Id },
    /* MeterIdentification MeterType (batch 7a): the one integer the AT
     * read reaches on this cluster (AT_MT_SPEC.md 3.9's 0x0511 row),
     * served live from the Instance's own storage (null until
     * AT+MTMETERID: +MTERR:5). The strings and the struct need no rows:
     * CHAR_STRING/STRUCT fall out of attr_type_info() and answer
     * +MTERR:5 on the generic path, the C6's wire for all four. Writes
     * answer +MTERR:11 (MANAGED_INTERNALLY without WRITABLE on the C6,
     * esp_matter_attribute.cpp:5296-5300); AT+MTMETERID is the write
     * path. */
    { chip::app::Clusters::MeterIdentification::Id,
      chip::app::Clusters::MeterIdentification::Attributes::MeterType::Id },
    /* DeviceEnergyManagement (batch 7a): all six delegate-served scalars,
     * the four enums/bool with inert shadows plus the two DE407
     * metadata-only power_mw declarations. Reads answer the
     * HearthDemDelegate cache below; writes +MTERR:11 (MANAGED_INTERNALLY
     * without WRITABLE on the C6, esp_matter_attribute.cpp:4255-4290);
     * AT+MTMEAS 0x98 is the write path. The DEMMode CurrentMode row is
     * the RVC ModeBase pair's rule on the third alias; its ClusterRevision
     * stays out (LIVE seed, the ModeBase no-default-arm note above). */
    { chip::app::Clusters::DeviceEnergyManagement::Id,
      chip::app::Clusters::DeviceEnergyManagement::Attributes::ESAType::Id },
    { chip::app::Clusters::DeviceEnergyManagement::Id,
      chip::app::Clusters::DeviceEnergyManagement::Attributes::ESACanGenerate::Id },
    { chip::app::Clusters::DeviceEnergyManagement::Id,
      chip::app::Clusters::DeviceEnergyManagement::Attributes::ESAState::Id },
    { chip::app::Clusters::DeviceEnergyManagement::Id,
      chip::app::Clusters::DeviceEnergyManagement::Attributes::AbsMinPower::Id },
    { chip::app::Clusters::DeviceEnergyManagement::Id,
      chip::app::Clusters::DeviceEnergyManagement::Attributes::AbsMaxPower::Id },
    { chip::app::Clusters::DeviceEnergyManagement::Id,
      chip::app::Clusters::DeviceEnergyManagement::Attributes::OptOutState::Id },
    { chip::app::Clusters::DeviceEnergyManagementMode::Id,
      chip::app::Clusters::DeviceEnergyManagementMode::Attributes::CurrentMode::Id },
    /* WaterHeaterManagement (batch 7b): all six delegate-served values,
     * the five narrow ones with inert shadows plus the DE407 metadata-only
     * energy_mwh declaration (EstimatedHeatRequired). Reads answer the
     * HearthWhmDelegate cache below; writes +MTERR:11 (MANAGED_INTERNALLY
     * without WRITABLE on the C6 for all six,
     * esp_matter_attribute.cpp:4485-4519); AT+MTMEAS 0x94 is the write
     * path. The three feature-gated rows are declared on variant 0 only;
     * on a variant-1 endpoint attr_locate()'s metadata miss answers
     * +MTERR:4 before this table is ever consulted, so the rows are
     * harmless there. The WaterHeaterMode CurrentMode row is the RVC
     * ModeBase pair's rule on the fourth alias; its ClusterRevision stays
     * out (LIVE seed, the ModeBase no-default-arm note above), while
     * WHM's ClusterRevision stays out for the OPPOSITE reason: its seed
     * is an inert shadow kept equal to the value Instance::Read() serves
     * itself (water-heater-management-server.cpp:146-147), so the generic
     * arena read answers the same 2 either way and needs no carve-out. */
    { chip::app::Clusters::WaterHeaterManagement::Id,
      chip::app::Clusters::WaterHeaterManagement::Attributes::HeaterTypes::Id },
    { chip::app::Clusters::WaterHeaterManagement::Id,
      chip::app::Clusters::WaterHeaterManagement::Attributes::HeatDemand::Id },
    { chip::app::Clusters::WaterHeaterManagement::Id,
      chip::app::Clusters::WaterHeaterManagement::Attributes::TankVolume::Id },
    { chip::app::Clusters::WaterHeaterManagement::Id,
      chip::app::Clusters::WaterHeaterManagement::Attributes::EstimatedHeatRequired::Id },
    { chip::app::Clusters::WaterHeaterManagement::Id,
      chip::app::Clusters::WaterHeaterManagement::Attributes::TankPercentage::Id },
    { chip::app::Clusters::WaterHeaterManagement::Id,
      chip::app::Clusters::WaterHeaterManagement::Attributes::BoostState::Id },
    { chip::app::Clusters::WaterHeaterMode::Id,
      chip::app::Clusters::WaterHeaterMode::Attributes::CurrentMode::Id },
    /* RefrigeratorAndTemperatureControlledCabinetMode (batch 8): the RVC
     * ModeBase pair's rule on the fifth alias, read live from the pool and
     * refused +MTERR:11 on write. Its ClusterRevision stays out for the same
     * reason every ModeBase alias's does (the AAI has no revision case, so
     * the arena seed is the live answer). RefrigeratorAlarm needs no rows at
     * all, and that is the interesting half: Mask, State and Supported are
     * plain arena integers that the singleton server itself reads and writes
     * through the generated Accessors, so an AT+MTATTR read of any of them
     * already answers exactly what a controller sees. Only State's WRITE
     * path is diverted, and it is diverted to a different COMMAND
     * (AT+MTALARM) rather than to a carve-out row here, because what the
     * diversion buys is the Notify event, not a different value. */
    { chip::app::Clusters::RefrigeratorAndTemperatureControlledCabinetMode::Id,
      chip::app::Clusters::RefrigeratorAndTemperatureControlledCabinetMode::Attributes::
          CurrentMode::Id },
    /* OvenMode and OvenCavityOperationalState (batch 8, the heater cabinet):
     * the ModeBase pair's rule on the sixth alias, and the opstate pair's on
     * the derived cluster, all read live from the shared pools and refused
     * +MTERR:11 on write. The cavity's two rows are the RvcOperationalState
     * rows one cluster over and exist for the same reason: its Instance's
     * AAI intercepts both scalars, so the arena slots beneath them are inert
     * shadows. AT+MTOPSTATE and AT+MTMODES are the write paths. The cavity's
     * ClusterRevision stays out for the WHM reason rather than the ModeBase
     * one: Instance::Read() serves it, and the seed is a shadow kept equal
     * to what it serves, so the generic arena read answers the same 1. */
    { chip::app::Clusters::OvenMode::Id,
      chip::app::Clusters::OvenMode::Attributes::CurrentMode::Id },
    /* MicrowaveOvenMode CurrentMode (batch 8), the seventh and last ModeBase
     * alias. Note what is NOT here and why, because the microwave is the one
     * device type where the omissions are the interesting part: the three
     * MicrowaveOvenControl attributes are Instance-served too, and are
     * deliberately left to the arena, because AT_MT_SPEC.md 1441-1456 says a
     * host reads CookTime and PowerSetting back only through a commissioned
     * controller; and OperationalState's CountdownTime is left to the arena
     * because its seeded null and the delegate's NullNullable agree, so both
     * paths answer +MTERR:5 already. */
    { chip::app::Clusters::MicrowaveOvenMode::Id,
      chip::app::Clusters::MicrowaveOvenMode::Attributes::CurrentMode::Id },
    { chip::app::Clusters::OvenCavityOperationalState::Id,
      chip::app::Clusters::OperationalState::Attributes::OperationalState::Id },
    { chip::app::Clusters::OvenCavityOperationalState::Id,
      chip::app::Clusters::OperationalState::Attributes::CurrentPhase::Id },
};

static bool instance_attr_served(uint32_t cluster, uint32_t attr)
{
    for (auto &e : k_instance_served) {
        if (e.cluster == cluster && e.attr == attr) {
            return true;
        }
    }
    return false;
}

/* Defined in the OperationalState, chime, ModeBase and RVC opstate
 * sections below, beside the pools they read. All run under the caller's
 * StackLock. */
static int mt_opstate_attr_read_live(uint16_t ep, uint32_t cluster, uint32_t attr, int64_t *out,
                                     bool *is_unsigned);
static int mt_chime_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out, bool *is_unsigned);
static int mt_chime_attr_write_live(uint16_t ep, uint32_t attr, int64_t val);
static int mt_mb_attr_read_live(uint16_t ep, uint32_t cluster, int64_t *out, bool *is_unsigned);
static int mt_rvc_opstate_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out,
                                         bool *is_unsigned);
static int mt_epm_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out, bool *is_unsigned);
static int mt_meter_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out, bool *is_unsigned);
static int mt_dem_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out, bool *is_unsigned);
static int mt_whm_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out, bool *is_unsigned);
/* The RVC opstate pool's Instance lookup (defined beside the pool below),
 * shared by mt_matter_opstate_set()'s RVC branch and the live reader. */
static chip::app::Clusters::OperationalState::Instance *mt_rvc_opstate_instance(uint16_t ep);

/*
 * Locate an attribute's metadata, splitting endpoint/cluster/attribute
 * absence the way mt_matter.h's mt_attr_result_t comments require it split:
 * an unknown endpoint index is MT_ATTR_ERR_ENDPOINT (emberAfIndexFromEndpoint
 * against kEmberInvalidEndpointIndex, attribute-storage.h), an endpoint that
 * exists but does not carry the cluster is MT_ATTR_ERR_CLUSTER
 * (emberAfContainsServer, attribute-storage.h:98-104), and a present cluster
 * with no such attribute is MT_ATTR_ERR_ATTRIBUTE (emberAfLocateAttributeMetadata).
 *
 * emberAfContainsServer(), not emberAfFindServerCluster(): the latter is not
 * declared in attribute-storage.h (this file's only ember include), only in
 * app/util/endpoint-config-api.h; emberAfContainsServer() is its exact
 * wrapper (attribute-storage.cpp:828-830, `return
 * emberAfFindServerCluster(endpoint, clusterId) != nullptr;`), so calling it
 * is equivalent and needs no extra include.
 */
static mt_attr_result_t attr_locate(uint16_t ep, uint32_t cluster, uint32_t attr,
                                    const EmberAfAttributeMetadata **out_md)
{
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, cluster)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    const EmberAfAttributeMetadata *md = emberAfLocateAttributeMetadata(ep, cluster, attr);
    if (md == nullptr) {
        return MT_ATTR_ERR_ATTRIBUTE;
    }
    *out_md = md;
    return MT_ATTR_OK;
}

/*
 * NumericAttributeTraits::GetNullValue() (attribute-storage-null-handling.h):
 * the type maximum for unsigned, the type minimum for signed - same
 * sentinels mt_devtypes_zephyr.cpp's s_seeds table documents and uses.
 * Shared by mt_matter_attr_read()'s null check and
 * MatterPostAttributeChangeCallback()'s (fix round 1, C2) so the two cannot
 * drift apart.
 */
static uint64_t attr_null_sentinel(bool is_unsigned, uint8_t bytes)
{
    return is_unsigned
        ? (bytes >= 8 ? (uint64_t)-1 : ((1ULL << (8 * bytes)) - 1))
        : (bytes >= 8 ? (1ULL << 63) : (1ULL << (8 * bytes - 1)));
}

/*
 * out is a required, non-null out-parameter (unlike is_unsigned, which
 * mt_matter.h documents as "may be NULL"): mt_at.c's cmd_mtattr always
 * passes the address of a stack local, the same precondition the C6's
 * mt_matter_attr_read() (main.cpp) relies on without a guard of its own.
 */
extern "C" int mt_matter_attr_read(uint16_t ep, uint32_t cluster, uint32_t attr, int64_t *out,
                                   bool *is_unsigned)
{
    chip::DeviceLayer::StackLock lock;

    const EmberAfAttributeMetadata *md = nullptr;
    mt_attr_result_t r = attr_locate(ep, cluster, attr, &md);
    if (r != MT_ATTR_OK) {
        return r;
    }

    /* DE397: the Instance-served attributes answer the live object, not
     * the arena; see the carve-out comment above attr_locate(). Batch 5
     * turned the two-way ternary into a per-cluster dispatch. Every
     * cluster in k_instance_served has its own explicit arm, and default:
     * fails loudly (batch 5 fix round, review Minor 2): a future table
     * row whose case someone forgets would otherwise be silently answered
     * by whichever reader default routed to, and a bare ERROR at the
     * bench is the drift alarm this arm exists to be. */
    if (instance_attr_served(cluster, attr)) {
        switch (cluster) {
        case chip::app::Clusters::OperationalState::Id:
        case chip::app::Clusters::OvenCavityOperationalState::Id:
            return mt_opstate_attr_read_live(ep, cluster, attr, out, is_unsigned);
        case chip::app::Clusters::RvcRunMode::Id:
        case chip::app::Clusters::RvcCleanMode::Id:
        case chip::app::Clusters::DeviceEnergyManagementMode::Id:
        case chip::app::Clusters::WaterHeaterMode::Id:
        case chip::app::Clusters::RefrigeratorAndTemperatureControlledCabinetMode::Id:
        case chip::app::Clusters::OvenMode::Id:
        case chip::app::Clusters::MicrowaveOvenMode::Id:
            return mt_mb_attr_read_live(ep, cluster, out, is_unsigned);
        case chip::app::Clusters::RvcOperationalState::Id:
            return mt_rvc_opstate_attr_read_live(ep, attr, out, is_unsigned);
        case chip::app::Clusters::Chime::Id:
            return mt_chime_attr_read_live(ep, attr, out, is_unsigned);
        case chip::app::Clusters::ElectricalPowerMeasurement::Id:
            return mt_epm_attr_read_live(ep, attr, out, is_unsigned);
        case chip::app::Clusters::MeterIdentification::Id:
            return mt_meter_attr_read_live(ep, attr, out, is_unsigned);
        case chip::app::Clusters::DeviceEnergyManagement::Id:
            return mt_dem_attr_read_live(ep, attr, out, is_unsigned);
        case chip::app::Clusters::WaterHeaterManagement::Id:
            return mt_whm_attr_read_live(ep, attr, out, is_unsigned);
        default:
            return MT_ATTR_ERR_FAILED;
        }
    }

    bool unsigned_type;
    uint8_t bytes;
    if (!attr_type_info(md->attributeType, &unsigned_type, &bytes)) {
        /* Genuinely not an AT+MTATTR type (string/array/struct/float): no
         * definite signedness exists, but the header's "the flag is also
         * valid when the result is MT_ATTR_ERR_TYPE" covers this arm too,
         * and cmd_mtattr (mt_at.c) always lets MT_ATTR_ERR_TYPE fall through
         * to a write that reads this flag before it re-derives the same
         * failure. Same choice as the C6 (main.cpp's mt_matter_attr_read(),
         * the ESP_ERR_NOT_SUPPORTED/ARRAY arm): false selects the signed
         * parse, and the write fails on its own attr_type_info() check for
         * the identical reason. */
        if (is_unsigned) {
            *is_unsigned = false;
        }
        return MT_ATTR_ERR_TYPE;
    }
    /* Set before the null check below, so the flag stays valid even when
     * this read goes on to answer MT_ATTR_ERR_TYPE for a null nullable
     * value (core/include/mt_matter.h:172-176, binding). */
    if (is_unsigned) {
        *is_unsigned = unsigned_type;
    }

    uint8_t buf[8] = { 0 };
    Status st = emberAfReadAttribute(ep, cluster, attr, buf, sizeof(buf));
    if (st != Status::Success) {
        /* Cannot happen once attr_locate() has already proven the
         * endpoint/cluster/attribute triple exists; defensive only. */
        return MT_ATTR_ERR_FAILED;
    }

    uint64_t raw = 0;
    for (uint8_t i = 0; i < bytes; i++) {
        raw |= ((uint64_t)buf[i]) << (8 * i);
    }

    if (md->IsNullable() && raw == attr_null_sentinel(unsigned_type, bytes)) {
        /* The AT grammar has no null literal: answer MT_ATTR_ERR_TYPE
         * with is_unsigned already set above, so a caller about to
         * WRITE can still fetch the signedness first. */
        return MT_ATTR_ERR_TYPE;
    }

    if (unsigned_type) {
        *out = (int64_t)raw;  /* reinterpret through uint64_t, per the header */
    } else {
        int shift = 64 - 8 * bytes;
        *out = (int64_t)(raw << shift) >> shift;
    }
    return MT_ATTR_OK;
}

extern "C" int mt_matter_attr_write(uint16_t ep, uint32_t cluster, uint32_t attr, int64_t val, bool notify)
{
    chip::DeviceLayer::StackLock lock;

    const EmberAfAttributeMetadata *md = nullptr;
    mt_attr_result_t r = attr_locate(ep, cluster, attr, &md);
    if (r != MT_ATTR_OK) {
        return r;
    }

    /* DE397 as amended (fix round 2): the two clusters part ways here,
     * each mirroring its own C6 wire. The opstate pair is refused with
     * +MTERR:11 (not writable over AT anywhere; AT+MTOPSTATE is the write
     * path); the chime pair routes to the live ChimeServer's own setters,
     * the C6's set_val_via_write_attribute() branch in this port's terms.
     * Full traced evidence in the carve-out comment above attr_locate(). */
    if (instance_attr_served(cluster, attr)) {
        if (cluster != chip::app::Clusters::Chime::Id) {
            /* The opstate pair (batch 4), the RVC's four (batch 5) and
             * the energy rows (batch 7a): +MTERR:11, mirroring the C6's
             * MANAGED_INTERNALLY-without-WRITABLE refusal for every one
             * of them. ChangeToMode, AT+MTOPSTATE, AT+MTMODES and
             * AT+MTMEAS are the write paths. */
            return MT_ATTR_ERR_READONLY;
        }
        return mt_chime_attr_write_live(ep, attr, val);
    }

    bool unsigned_type;
    uint8_t bytes;
    if (!attr_type_info(md->attributeType, &unsigned_type, &bytes)) {
        return MT_ATTR_ERR_TYPE;
    }

    /* Width bounds check ahead of the write: a value outside the
     * attribute's own width is MT_ATTR_ERR_VALUE, not a silent truncation
     * (core/include/mt_matter.h:185-188, binding). */
    if (bytes < 8) {
        uint64_t u = (uint64_t)val;
        if (unsigned_type) {
            uint64_t max_u = (1ULL << (8 * bytes)) - 1;
            if (u > max_u) {
                return MT_ATTR_ERR_VALUE;
            }
        } else {
            int64_t min_s = -(int64_t)(1ULL << (8 * bytes - 1));
            int64_t max_s = (int64_t)((1ULL << (8 * bytes - 1)) - 1);
            if (val < min_s || val > max_s) {
                return MT_ATTR_ERR_VALUE;
            }
        }
    }

    /*
     * No metadata->IsWritable() gate here - a deliberate delta from the
     * brief's sketch, recorded in the task report with tree evidence on
     * both platforms:
     *
     * emAfWriteAttribute()'s writable/data-type guard runs only when
     * overrideReadOnlyAndDataType is false (NCS v3.3.4
     * attribute-table.cpp:350-364), and BOTH public emberAfWriteAttribute()
     * overloads pass true for it (attribute-table.cpp:189-199): "This
     * function will not check to see if the attribute is writable since the
     * read only/writable characteristic of an attribute only pertains to
     * external devices writing over the air... it assumes the device knows
     * what it is doing" (the function's own doc comment). A locally
     * originated write is trusted the same way on the C6: esp-matter's
     * set_val()/update() (esp_matter_data_model.cpp:1091) skip their
     * ATTRIBUTE_FLAG_WRITABLE-less refusal entirely for a plain,
     * not-managed-internally attribute, which is what TemperatureMeasurement
     * MeasuredValue and Identify IdentifyType both are on the C6 too
     * (esp_matter_attribute.cpp: create_measured_value() is
     * ATTRIBUTE_FLAG_NULLABLE only, create_identify_type() is
     * ATTRIBUTE_FLAG_NONE - neither carries WRITABLE or
     * MANAGED_INTERNALLY). This bridge's write is that same "the caller is
     * the local host, not a controller over the air" concession, which is
     * exactly why the controller bench gate's own MeasuredValue write has to
     * succeed even though ZAP declares it without WRITABLE
     * (mt_devtypes_zephyr.cpp:163-164).
     *
     * mt_matter.h's MT_ATTR_ERR_READONLY comment is scoped narrower than "no
     * WRITABLE flag": it names a specific mechanism, "served by a cluster
     * Instance (ATTRIBUTE_FLAG_MANAGED_INTERNALLY without
     * ATTRIBUTE_FLAG_WRITABLE)". This milestone's registry has no
     * Instance-managed cluster at all (no delegate pool exists yet on this
     * platform), so that condition cannot occur here - MT_ATTR_ERR_READONLY
     * is unreachable from this bridge today, the same as it was on the C6
     * before its first Instance-managed cluster shipped. A gate on
     * IsWritable() alone would answer READONLY for MeasuredValue too, which
     * the bench gate explicitly requires to succeed; it is not implemented
     * here for that reason. Concretely: the brief's own bench gate line "a
     * write to IdentifyType (read-only) answers the read-only error" will
     * NOT reproduce with this bridge - IdentifyType is locally writable, at
     * parity with the C6 at the same point in its history. Flagged for the
     * controller in the task report; the fix, if this parity is not what is
     * wanted here, is a future round's Instance-managed IdentifyCluster (or
     * an explicit non-generic carve-out), not a blanket IsWritable() check
     * that breaks MeasuredValue.
     */

    uint8_t buf[8] = { 0 };
    uint64_t u = (uint64_t)val;
    for (uint8_t i = 0; i < bytes; i++) {
        buf[i] = (uint8_t)(u >> (8 * i));
    }

    chip::app::ConcreteAttributePath path(ep, cluster, attr);
    EmberAfWriteDataInput input(buf, md->attributeType);
    input.SetMarkDirty(notify ? chip::app::MarkAttributeDirty::kIfChanged
                              : chip::app::MarkAttributeDirty::kNo);

    /*
     * notify selects ONLY MarkAttributeDirty - kIfChanged (notify=true) marks
     * the change for the CHIP reporting engine, so subscribed/bound
     * controllers see it; kNo (notify=false) changes local storage without
     * that fabric-facing report. It does NOT gate the +MTATTR URC: fix round
     * 1 (controller ruling, C1) overturned this file's original
     * s_suppress_attr_urc design, which assumed notify=false host writes
     * should echo silently. The wire spec says otherwise on both platforms:
     * AT_MT_SPEC.md 3.8 states plainly, with no mode carve-out, "A write to
     * an ember-managed attribute that actually changes its value echoes a
     * +MTATTR URC ... then OK", and 3.25's AirQuality write is called out as
     * the ONE exception that "never echoes the +MTATTR URC, in either mode"
     * - an exception that only makes sense if the general rule already
     * covers both modes. On the C6, esp-matter's set_val()
     * (esp_matter_data_model.cpp:784-788) fires its POST_UPDATE callback
     * unconditionally whenever call_callbacks defaults true, with no
     * per-mode gate either; mt_matter_attr_write() there never suppresses
     * app_attribute_update_cb(). So a notify=false reflection of a
     * controller-driven change DOES echo back to the host as a +MTATTR URC
     * on both platforms - the host is expected to see its own reflection
     * confirmed, not silenced, and de-duplication (if ever needed) is a host
     * concern, not this bridge's.
     */
    Status st = emberAfWriteAttribute(path, input);

    switch (st) {
    case Status::Success:
        /* A same-value re-push also answers Success in both notify modes:
         * emAfWriteAttribute()'s AttributeValueIsChanging() early-out
         * (attribute-table.cpp:408-426) returns Success without touching
         * MarkAttributeDirty's callback path at all. OK by construction; no
         * special case needed here (binding per core/include/mt_matter.h's
         * same-value comment). */
        return MT_ATTR_OK;
    case Status::ConstraintError:
        /* ember's own MIN_MAX bounds check (attribute-table.cpp:366-406)
         * runs unconditionally, independent of the override flag noted
         * above. */
        return MT_ATTR_ERR_VALUE;
    default:
        return MT_ATTR_ERR_FAILED;
    }
}

/*
 * Strong override of the weak default in NCS's generic-callback-stubs.cpp.
 * Every attribute write that actually changes a value passes through here,
 * whether it came from this bridge's write (either notify mode - see the
 * comment in mt_matter_attr_write() above, fix round 1/C1) or from a
 * controller's own IM write - so any change surfaces to the host as a
 * +MTATTR URC the same way the C6's app_attribute_update_cb() does. Format
 * is byte-identical to platform/esp32c6/main/main.cpp:838-842.
 */
void MatterPostAttributeChangeCallback(const chip::app::ConcreteAttributePath &path, uint8_t type,
                                       uint16_t size, uint8_t *value)
{
    if (path.mEndpointId == 0 || path.mEndpointId == kCatalogueEndpointId) return;
    bool is_unsigned; uint8_t bytes;
    if (!attr_type_info((EmberAfAttributeType)type, &is_unsigned, &bytes) || size < bytes) return;
    uint64_t raw = 0;
    for (uint8_t i = 0; i < bytes; i++) raw |= ((uint64_t)value[i]) << (8 * i);

    /*
     * Fix round 1, C2: a transition TO null must not echo the raw sentinel
     * as if it were a real value (a host reading +MTATTR:...,255 has no way
     * to tell that from a genuine 255, and a follow-up AT+MTATTR read
     * answers +MTERR:5 for the same attribute, an inconsistency the C6 does
     * not have - attr_val_to_i64()'s nullable arms there return false for a
     * null value, so main.cpp's app_attribute_update_cb() never builds a URC
     * line for one, see attr_val_to_i64() and its callers in main.cpp).
     * emberAfLocateAttributeMetadata() is a plain table lookup (no I/O, no
     * lock of its own), safe to call here under whatever lock the caller
     * already holds (this bridge's own StackLock for a local write, or
     * CHIP's own lock for a controller-driven one - the same lock
     * discipline this whole file follows). Shares attr_null_sentinel() with
     * mt_matter_attr_read() so the two null tests cannot drift apart.
     */
    const EmberAfAttributeMetadata *md =
        emberAfLocateAttributeMetadata(path.mEndpointId, path.mClusterId, path.mAttributeId);
    if (md != nullptr && md->IsNullable() && raw == attr_null_sentinel(is_unsigned, bytes)) {
        return;
    }

    /*
     * Fix round 2, C1: the BooleanState bridge (Contact/Rain/Water Freeze/
     * Water Leak). BooleanState is one of the clusters CHIP serves through
     * the newer registered ServerClusterInterface path
     * (src/app/clusters/boolean-state-server/): CodegenDataModelProvider_
     * Read.cpp:116 checks that registry before ember's external-storage
     * fallback is ever consulted, so a real controller's read of
     * StateValue was answering from the registered BooleanStateCluster
     * object's own mStateValue, not this bridge's arena - bench-confirmed
     * (an AT write of StateValue=1 fired the URC below, but chip-tool
     * still read FALSE) and mechanically confirmed against the tree cited
     * above. This block bridges the two: BooleanState::
     * FindClusterOnEndpoint() (CodegenIntegration.h) returns the very
     * object emberAfSetDynamicEndpoint()'s init dispatch already
     * constructed for this endpoint (see the comment on booleanStateAttrs
     * in mt_devtypes_zephyr.cpp for why that construction happens at all),
     * and SetStateValue() pushes this write into it AND emits the
     * StateChange event (boolean-state-cluster.cpp:57-64) - strictly
     * better than an ember-only fix, since a bare ember write never emits
     * that event.
     *
     * No recursion risk. SetStateValue() only touches the object's own
     * mStateValue plus the event/reporting path; it never calls back into
     * emberAfWriteAttribute, so it cannot re-enter this callback. And a
     * controller's own IM write to StateValue never reaches this function
     * in the first place: StateValue is read-only in the Matter sense and
     * BooleanStateCluster does not override WriteAttribute, so the
     * registry rejects the write before ember's external-storage path
     * (and this callback) is ever reached. The only writer that reaches
     * here for this attribute is this bridge's own AT+MTATTR path.
     */
    if (path.mClusterId == chip::app::Clusters::BooleanState::Id &&
        path.mAttributeId == chip::app::Clusters::BooleanState::Attributes::StateValue::Id) {
        auto *booleanState = chip::app::Clusters::BooleanState::FindClusterOnEndpoint(path.mEndpointId);
        if (booleanState != nullptr) {
            booleanState->SetStateValue(raw != 0);
        }
    }

    char line[64];
    if (is_unsigned) {
        snprintf(line, sizeof(line), "+MTATTR:%u,%lu,%lu,%llu", path.mEndpointId,
                 (unsigned long)path.mClusterId, (unsigned long)path.mAttributeId,
                 (unsigned long long)raw);
    } else {
        int64_t sv = (int64_t)(raw << (64 - 8 * bytes)) >> (64 - 8 * bytes);
        snprintf(line, sizeof(line), "+MTATTR:%u,%lu,%lu,%lld", path.mEndpointId,
                 (unsigned long)path.mClusterId, (unsigned long)path.mAttributeId,
                 (long long)sv);
    }
    mt_at_urc(line);
}

/*
 * Catalogue batch 2, the air quality sensor (0x002C). Until this batch this
 * function was a stub in mt_matter_stub.c returning 0, because no device
 * type on this platform carried the AirQuality cluster.
 *
 * mt_matter.h's contract for it: one accessor, two callers, so the ember
 * feature map and a server Instance's BitMask<Feature> cannot be edited
 * apart. On the C6 those two callers are mk_air_quality_sensor() and
 * mt_air_quality_register_all(). On this platform only the first exists:
 * mt_devtypes_zephyr.cpp's seed_slots() fills the AirQuality FeatureMap slot
 * from this value, and nothing constructs an AirQuality::Instance (see the
 * audit note on airQualityAttrs for why - with none registered, ember serves
 * the arena). If a later round adds one, it reads its BitMask from here and
 * the two agree by construction.
 *
 * All four optional features are enabled. The base cluster only mandates
 * Unknown/Good/Poor; Fair, Moderate, VeryPoor and ExtremelyPoor are each
 * behind their own bit, and the host library's AirQuality_t publishes all
 * seven SDK values, so anything less would let the library report a value
 * this endpoint's feature map does not admit. Values are the CHIP enum's own
 * (clusters/AirQuality/Enums.h:49-55), never transcribed literals.
 */
extern "C" uint32_t mt_air_quality_feature_mask(void)
{
    using chip::app::Clusters::AirQuality::Feature;
    return chip::to_underlying(Feature::kFair) | chip::to_underlying(Feature::kModerate) |
           chip::to_underlying(Feature::kVeryPoor) | chip::to_underlying(Feature::kExtremelyPoor);
}

/*
 * ============ catalogue batch 3: the command verdict frame ==============
 *
 * The door lock (0x000A) and the water valve (0x0042) are the first device
 * types on this platform whose commands need an APPLICATION verdict, and
 * this section is where that verdict is fetched. Everything about the
 * verdict protocol itself, the sequence numbers, the 1000 ms window, the
 * default-deny, the +MTCMDTO URC, belongs to core (mt_at.h, mt_cmdbox.h)
 * and is identical on both platforms; nothing here reimplements any of it.
 * All this code does is call mt_cmd_forward() from the right SDK hook and
 * translate the boolean it returns into whatever that hook's caller expects.
 *
 * Both types forward the PAYLOAD-LESS form. AT_MT_SPEC.md 3.17 registers
 * the door lock as cluster 257 commands 0/1 and the valve as cluster 129
 * commands 0/1, with no trailing fields on either, so mt_cmd_forward() is
 * the right entry point and neither mt_cmd_forward_payload() nor
 * mt_cmd_forward_fields() is involved.
 *
 * NO StackLock in the command hooks, unlike every mt_matter_* bridge
 * function in this file. They already run ON the CHIP event-loop task: the
 * lock's two are ember command callbacks and the valve's two are Delegate
 * methods the SDK calls synchronously from that same task.
 * mt_cmd_forward() takes no lock of its own either, for the same reason,
 * and mt_at.c's AT+MTCMDRESP handler must never take the stack lock on the
 * reply path (see its comment) or the two would deadlock. The AT+MTLOCK
 * and AT+MTVALVE bridges below DO take it, because those are called from
 * the AT parser thread like every other bridge function here.
 */

/* ---- door lock (0x000A) ----------------------------------------------- */

/*
 * One helper for both commands, differing only in the command id forwarded,
 * mirroring the C6's mt_door_lock_adjudicate() exactly.
 *
 * The verdict-to-status mapping is the SDK's, not a choice made here:
 * HandleRemoteLockOperation() answers the controller
 * `success ? Status::Success : Status::Failure` (door-lock-server.cpp:3726)
 * straight off this function's return value, and reads `err` only to
 * annotate the LockOperationError event it emits on the failure path
 * (:3762-3765). So an allow is Success and a deny is Failure on the wire,
 * with an event carrying the reason.
 *
 * kUnspecified is the reason on every deny, because mt_cmd_forward()'s
 * false is deliberately undifferentiated: it covers an explicit host deny,
 * a missed 1000 ms window, the AT link not being up, and no host callback
 * registered. A lock fails closed, and the firmware has no honest way to
 * tell a controller which of those four it was. Not kInvalidCredential in
 * particular: that value additionally triggers HandleWrongCodeEntry()
 * (:3716) and would count a host deny towards the wrong-code lockout, which
 * would be a lie about what happened.
 */
static bool mt_door_lock_adjudicate(chip::EndpointId endpointId, uint32_t command,
                                    OperationErrorEnum &err)
{
    if (mt_cmd_forward(endpointId, chip::app::Clusters::DoorLock::Id, command)) {
        return true;
    }
    err = OperationErrorEnum::kUnspecified;
    return false;
}

/*
 * Strong overrides of the weak defaults in door-lock-server-callback.cpp
 * (:46 and :55). Signatures verbatim from door-lock-server.h:1112-1114 and
 * :1128-1130; note the argument order, fabricIdx and nodeId BEFORE the PIN,
 * and that pinCode is an Optional, not a Nullable.
 *
 * fabricIdx/nodeId/pinCode go unused. With FeatureMap 0 this endpoint
 * advertises no credential feature at all, so a PIN-carrying invoke is
 * refused by the server before the app is consulted, and fabricIdx/nodeId
 * are event-annotation fields with no adjudication use: the host is told
 * which endpoint and which command, which is what the +MTCMD frame carries.
 */
bool emberAfPluginDoorLockOnDoorLockCommand(chip::EndpointId endpointId,
                                            const Nullable<chip::FabricIndex> &fabricIdx,
                                            const Nullable<chip::NodeId> &nodeId,
                                            const Optional<chip::ByteSpan> &pinCode,
                                            OperationErrorEnum &err)
{
    (void)fabricIdx;
    (void)nodeId;
    (void)pinCode;
    return mt_door_lock_adjudicate(endpointId, chip::app::Clusters::DoorLock::Commands::LockDoor::Id,
                                   err);
}

bool emberAfPluginDoorLockOnDoorUnlockCommand(chip::EndpointId endpointId,
                                              const Nullable<chip::FabricIndex> &fabricIdx,
                                              const Nullable<chip::NodeId> &nodeId,
                                              const Optional<chip::ByteSpan> &pinCode,
                                              OperationErrorEnum &err)
{
    (void)fabricIdx;
    (void)nodeId;
    (void)pinCode;
    return mt_door_lock_adjudicate(endpointId,
                                   chip::app::Clusters::DoorLock::Commands::UnlockDoor::Id, err);
}

/*
 * The lock's per-endpoint init hook, emberAfDoorLockClusterInitCallback(),
 * is deliberately NOT here. It lives in mt_devtypes_zephyr.cpp, next to the
 * door lock's device type declaration, because its failure has to abort
 * mt_devtype_create() and that file owns the create. See the comment on the
 * override there for the InitEndpoint-versus-InitServer reasoning.
 */

/*
 * AT+MTLOCK bridge (AT_MT_SPEC.md 3.18). Reports the host's OWN actuation
 * as LockState, through the source-taking SetLockState() overload so the
 * LockOperation event a controller subscribes to is actually emitted; the
 * 2-arg overload (door-lock-server.h:160) writes the attribute silently and
 * is deliberately not used. mt_matter.h calls this the 6-arg form after the
 * C6's SDK; in this tree it is 7-arg with defaults
 * (door-lock-server.h:144-148), same call.
 *
 * The firmware never calls this itself after an allowed +MTCMD verdict:
 * actuation timing belongs to the host, which calls AT+MTLOCK once its own
 * mechanism confirms the bolt moved.
 */
extern "C" int mt_matter_lock_state_set(uint16_t ep, uint8_t state, uint8_t source)
{
    chip::DeviceLayer::StackLock lock;
    /* Same two lookups, in the same order, that attr_locate() above uses to
     * separate MT_ATTR_ERR_ENDPOINT from MT_ATTR_ERR_CLUSTER, so AT+MTLOCK's
     * error division (AT_MT_SPEC.md 3.18) matches AT+MTATTR's by
     * construction rather than by two hand-written lookups agreeing. */
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, chip::app::Clusters::DoorLock::Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    bool ok = DoorLockServer::Instance().SetLockState(ep, (DlLockState)state,
                                                      (OperationSourceEnum)source);
    return ok ? MT_ATTR_OK : MT_ATTR_ERR_FAILED;
}

extern "C" uint8_t mt_matter_lock_source_manual(void)
{
    return (uint8_t)OperationSourceEnum::kManual;
}

extern "C" uint8_t mt_matter_lock_source_max(void)
{
    /* kUnknownEnumValue is one past the last real source and is not itself
     * a valid one to report (mt_matter.h); kAliro is the highest that is.
     * Read from the pinned header at call time rather than transcribed, so
     * an SDK that grows the enum grows AT+MTLOCK's accepted range with it.
     * This tree agrees with the C6's: AT_MT_SPEC.md 3.18's documented
     * 0..10 range holds on both. */
    return (uint8_t)OperationSourceEnum::kAliro;
}

/* ---- water valve (0x0042) ---------------------------------------------
 *
 * ValveConfigurationAndControl::Delegate (delegate.h:34) has exactly three
 * pure virtuals (valve-configuration-and-control-delegate.h:40-42):
 *
 *   virtual DataModel::Nullable<chip::Percent> HandleOpenValve(DataModel::Nullable<chip::Percent> level) = 0;
 *   virtual CHIP_ERROR HandleCloseValve()                                                                = 0;
 *   virtual void HandleRemainingDurationTick(uint32_t duration)                                          = 0;
 *
 * The verdict cannot fail either command on the wire, and this is an SDK
 * property rather than a firmware choice (AT_MT_SPEC.md 3.19). Traced in
 * this tree, not assumed from the C6's note: CloseValve() calls
 * HandleCloseValve() (cluster.cpp:303) and neither assigns, tests nor logs
 * the CHIP_ERROR it returns, then returns CHIP_NO_ERROR unconditionally
 * (:306), so the Close command handler's Failure arm (:520-522) is
 * unreachable through the delegate. Open is worse: HandleOpenValve() has no
 * error channel at all, since it returns a LEVEL rather than a status, and
 * SetValveLevel() reads that level only to decide whether to republish
 * CurrentLevel (:361-365) before returning CHIP_NO_ERROR (:370); the Open
 * handler's status Optional (:440) is only ever filled from argument
 * constraint checks (:451, :456, :462, :483), never from anything the
 * delegate did.
 *
 * So the verdict here gates ONE thing: whether the host actually moves the
 * valve. That is still the whole point of forwarding it, which is why both
 * hooks forward anyway.
 *
 * One caller of HandleCloseValve() is NOT a controller invoke: the server's
 * own auto-close timer re-enters it when a timed open expires, so the host
 * sees an unsolicited 129/1 forward. Fix round I1; the full mechanism and
 * why it is documented rather than fixed are on the water valve's audit
 * note in mt_devtypes_zephyr.cpp, and the host-facing consequence is in the
 * platform README.
 */
class HearthValveDelegate : public chip::app::Clusters::ValveConfigurationAndControl::Delegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }

    /*
     * Return the level asked for on allow (the caller republishes it as
     * CurrentLevel when the Level feature is present, which it is not on
     * this build), and NullNullable on deny: "no level to report", which is
     * the nearest thing to a refusal this signature can express. Not an
     * error, because there is no error to express.
     */
    chip::app::DataModel::Nullable<chip::Percent> HandleOpenValve(
        chip::app::DataModel::Nullable<chip::Percent> level) override
    {
        if (mt_cmd_forward(m_ep, chip::app::Clusters::ValveConfigurationAndControl::Id,
                           chip::app::Clusters::ValveConfigurationAndControl::Commands::Open::Id)) {
            return level;
        }
        return chip::app::DataModel::NullNullable;
    }

    /*
     * Forward for adjudication and answer CHIP_NO_ERROR regardless: the
     * caller discards this return (see the section comment), so returning
     * an error on deny would change nothing on the wire while making this
     * function's contract with its caller a lie.
     */
    CHIP_ERROR HandleCloseValve() override
    {
        mt_cmd_forward(m_ep, chip::app::Clusters::ValveConfigurationAndControl::Id,
                       chip::app::Clusters::ValveConfigurationAndControl::Commands::Close::Id);
        return CHIP_NO_ERROR;
    }

    /*
     * Fires once a second from a SystemLayer timer on the CHIP event loop
     * while a timed open counts down (cluster.cpp:214, :235, :245).
     * Nothing in this firmware's AT surface exposes that countdown as an
     * event to forward, so there is nothing to do; RemainingDuration itself
     * still ticks in the server's shadow store and is what a subscribed
     * controller reads (see the seed note in mt_devtypes_zephyr.cpp).
     *
     * THIS FUNCTION must stay non-blocking: it runs on the event loop once
     * per second, and a mt_cmd_forward() from here would stall the stack
     * for up to 1000 ms on every tick.
     *
     * That rule is about this function only, and it is worth being exact,
     * because the TERMINAL tick does reach mt_cmd_forward() anyway, just
     * not through here. At zero the tick does not call this callback at
     * all: startRemainingDurationTick() takes the else branch and calls
     * CloseValve() (:250-252), which calls HandleCloseValve() (:303), which
     * forwards and blocks. So the auto-close stall exists, it is one
     * blocking forward per timed open rather than one per second, and it is
     * the SDK's structure rather than something this class chooses.
     * Documented, not fixed; fix round I1, full mechanism on the water
     * valve's audit note in mt_devtypes_zephyr.cpp.
     */
    void HandleRemainingDurationTick(uint32_t duration) override { (void)duration; }

private:
    chip::EndpointId m_ep = chip::kInvalidEndpointId;
};

/*
 * Pool of delegate objects, handed out in composition order by
 * mt_devtype_create() (mt_devtypes_zephyr.cpp). Memory reclaim round A:
 * the pool is a table of POINTERS and the objects themselves come from the
 * cluster-object heap, so a composition with no water valve pays four
 * bytes per unused slot instead of a whole delegate. The heap block is
 * never freed; see the heap's own comment for the standing policy.
 *
 * Sized kServiceableEndpoints, not MT_COMP_MAX_ENDPOINTS. mt_matter.h's
 * contract names the latter because it was written for the C6, which
 * serves all 28; this port serves 16 (mt_port_ids.h, acceptance versus
 * capacity), so a seventeenth endpoint of ANY device type fails its create
 * before it could ask for a delegate. Sizing for 28 would reserve twelve
 * slots no composition on this part can reach. The exhaustion behaviour
 * the contract specifies is unchanged: nullptr, and the caller aborts the
 * boot rebuild.
 */
/* A count, not a table: nothing ever looks a valve delegate up again. The
 * cluster server keeps the pointer itself (SetDefaultDelegate below) and
 * the AT+MTVALVE bridge goes through the server, so the only thing this
 * pool has to do is enforce the cap. The families that ARE looked up by
 * endpoint keep their pointer tables. */
static size_t s_valve_delegate_next;

extern "C" void *mt_matter_valve_delegate_alloc(void)
{
    if (s_valve_delegate_next >= kServiceableEndpoints) {
        return nullptr;
    }
    HearthValveDelegate *d = obj_new<HearthValveDelegate>();
    if (d == nullptr) {
        return nullptr;
    }
    s_valve_delegate_next++;
    return d;
}

/*
 * Fixes the slot's endpoint AND registers it with the cluster server. Both
 * halves belong here because both need the endpoint id, which is not known
 * until emberAfSetDynamicEndpoint() has succeeded; mt_devtype_create()'s
 * comment has the full ordering argument and why it differs from the C6's.
 *
 * SetDefaultDelegate() returns void and drops the registration silently if
 * the endpoint index does not resolve (cluster.cpp:264-269: a lone
 * `if (ep < table size)` with no else and no log). A valve in that state
 * still answers Open and Close with Success and still looks healthy to a
 * controller, while no +MTCMD is ever raised and the server's auto-close
 * tick never runs. So the registration is read back rather than trusted,
 * and a failure is logged loudly at the moment it happens. It does not
 * abort the create; mt_devtype_create()'s own comment says why that is
 * consistent with the pool check aborting rather than at odds with it.
 */
extern "C" void mt_matter_valve_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    auto *d = static_cast<HearthValveDelegate *>(delegate);
    d->set_endpoint(ep);
    chip::app::Clusters::ValveConfigurationAndControl::SetDefaultDelegate(ep, d);
    if (chip::app::Clusters::ValveConfigurationAndControl::GetDefaultDelegate(ep) != d) {
        LOG_ERR("valve delegate registration dropped for endpoint %u: commands on it will not "
                "reach the host and auto-close will not run",
                (unsigned)ep);
    }
}

/*
 * AT+MTVALVE bridge (AT_MT_SPEC.md 3.19). Reports the host's own actuation
 * through the cluster's own UpdateCurrentState()/UpdateCurrentLevel() calls
 * rather than a raw attribute write, so the ValveStateChanged event a
 * subscribed controller expects is emitted. Same split as AT+MTLOCK, and
 * doubly so here: the verdict never reached the wire anyway, so only the
 * host can say the valve moved.
 *
 * level == -1 means absent; mt_at.c has already validated 0..100 for any
 * value it passes through. As on the C6, <level> publishes nowhere on this
 * build: FeatureMap is 0, so CurrentLevel is not a declared attribute and
 * UpdateCurrentLevel() checks the feature before touching it and returns
 * success either way. The argument is accepted and does nothing observable,
 * which is exactly what AT_MT_SPEC.md 3.19 documents.
 */
extern "C" int mt_matter_valve_state_set(uint16_t ep, uint8_t state, int level)
{
    chip::DeviceLayer::StackLock lock;
    namespace Valve = chip::app::Clusters::ValveConfigurationAndControl;
    /* attr_locate()'s two lookups again, for AT_MT_SPEC.md 3.19's error
     * division; see the note in mt_matter_lock_state_set() above. */
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, Valve::Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    CHIP_ERROR err = Valve::UpdateCurrentState(ep, (Valve::ValveStateEnum)state);
    if (err != CHIP_NO_ERROR) {
        return MT_ATTR_ERR_FAILED;
    }
    if (level >= 0) {
        err = Valve::UpdateCurrentLevel(ep, (chip::Percent)level);
        if (err != CHIP_NO_ERROR) {
            return MT_ATTR_ERR_FAILED;
        }
    }
    return MT_ATTR_OK;
}

/* ============ catalogue batch 4: the appliance and notification types ====
 *
 * The smoke/co alarm (0x0076), the OperationalState trio (0x0073, 0x0075,
 * 0x007C), the mode select (0x0027) and the chime (0x0146) land here in
 * build order. The same file discipline as batch 3 holds throughout: SDK
 * hooks that run on the CHIP task take no StackLock, AT bridges called from
 * the parser thread always do, and nothing here reimplements any of core's
 * verdict protocol.
 */

/* ---- smoke/co alarm (0x0076) ------------------------------------------ */

/*
 * The cluster's one mandatory link symbol: declared at
 * smoke-co-alarm-server.h:178, called from smoke-co-alarm-server.cpp:112
 * (RequestSelfTest) and :450 (HandleRemoteSelfTestRequest), and defined
 * nowhere under src/, weak or otherwise; the only definitions in the tree
 * are example apps'. Leaving it out is a link error, the same shape the C6
 * documents for its own copy (design spec F4).
 *
 * Notify-only, the +MTCMD seq-0 form's first consumer on this platform
 * (mt_cmd_notify(), core/mt/mt_at.c): no mailbox slot, never blocks, no
 * verdict to give. In THIS tree HandleRemoteSelfTestRequest() has already
 * set TestInProgress true (:447) and ExpressedState kTesting (:448) when it
 * calls this hook (:450), and it answers the controller Status::Success
 * only AFTER the hook returns (:452). That is the reverse of the C6
 * comment's "already answered Success before calling this" ordering, re-read
 * here rather than copied; the functional conclusion is unchanged, because
 * the Success is hardcoded and nothing this hook does can influence it, so
 * fire-and-forget notify remains the correct shape. The busy pre-check
 * (:438-445) answers Status::Busy without ever reaching this hook, so a
 * self-test requested while ExpressedState is any alarm/testing state
 * raises no +MTCMD at all.
 *
 * Runs on the CHIP task (generated IMClusterCommandHandler dispatch), so no
 * StackLock, the same reasoning as the door lock hooks above.
 *
 * The host is expected to run its own test and report completion with
 * AT+MTALARM=<ep>,5,0 (TestInProgress false), which fires SelfTestComplete
 * and recomputes ExpressedState below (bug B165).
 */
void emberAfPluginSmokeCoAlarmSelfTestRequestCommand(chip::EndpointId endpointId)
{
    mt_cmd_notify(endpointId, chip::app::Clusters::SmokeCoAlarm::Id,
                  chip::app::Clusters::SmokeCoAlarm::Commands::SelfTestRequest::Id);
}

/*
 * ExpressedState priority order (bug B165, C6 bench finding F-C10-2). The
 * SDK's recompute contract, SetExpressedStateByPriority
 * (smoke-co-alarm-server.h:54-55), leaves the order entirely to the
 * application and ships NO default; every reference app in this tree pairs
 * its state-changing setters with a recompute call. Without one the
 * endpoint wedges: SetTestInProgress() fires SelfTestComplete but never
 * touches ExpressedState, so a completed self-test would stay stuck at
 * kTesting forever. Order ported from the C6
 * (platform/esp32c6/main/main.cpp:5598-5608), which itself copied the SDK's
 * dedicated example; the identical eight entries appear in THIS tree at
 * examples/smoke-co-alarm-app/silabs/src/SmokeCoAlarmManager.cpp:32-36,
 * verified rather than assumed: smoke alarm and its interconnect echo
 * outrank CO alarm and its echo, then hardware fault, an in-progress self
 * test, end-of-service, and low battery last.
 */
static const std::array<SmokeCoAlarmServer::ExpressedStateEnum, SmokeCoAlarmServer::kPriorityOrderLength>
    s_alarm_expressed_state_priority = {
        SmokeCoAlarmServer::ExpressedStateEnum::kSmokeAlarm,
        SmokeCoAlarmServer::ExpressedStateEnum::kInterconnectSmoke,
        SmokeCoAlarmServer::ExpressedStateEnum::kCOAlarm,
        SmokeCoAlarmServer::ExpressedStateEnum::kInterconnectCO,
        SmokeCoAlarmServer::ExpressedStateEnum::kHardwareFault,
        SmokeCoAlarmServer::ExpressedStateEnum::kTesting,
        SmokeCoAlarmServer::ExpressedStateEnum::kEndOfService,
        SmokeCoAlarmServer::ExpressedStateEnum::kBatteryAlert,
    };

/*
 * AT+MTALARM bridge, ported from the C6's cluster-dispatched form
 * (platform/esp32c6/main/main.cpp mt_matter_alarm_set()). mt_at.c's
 * cmd_mtalarm only checks the UNION bound 0..11 before calling this (it
 * cannot know which cluster <ep> carries); this bridge dispatches on the
 * cluster the endpoint actually has. On THIS platform only SmokeCoAlarm
 * exists so far: RefrigeratorAlarm arrives with the composed-appliance
 * types in a later batch, and until then an endpoint carrying neither
 * answers MT_ATTR_ERR_CLUSTER through the single fall-through below, the
 * same code the C6 answers. The dispatch shape is kept so that batch adds
 * a branch rather than restructuring this function.
 *
 * Field 0 (ExpressedState) is derived by the server from the other fields
 * and never settable directly; rejected here rather than in mt_at.c because
 * field 0 is a legal RefrigeratorAlarm bit on the C6 and the union bound
 * must stay shared (core/include/mt_matter.h:647-651, binding).
 *
 * Every legal field maps to the matching SmokeCoAlarmServer setter, never a
 * raw attribute write: the setters emit the cluster's spec-mandated events
 * (SmokeAlarm, COAlarm, LowBattery, HardwareFault, EndOfService,
 * SelfTestComplete, AllClear) which a raw write to the same ember storage
 * would silently skip. <value> is range-checked against THAT field's own
 * enum bound before the cast, the two boolean fields against 0/1. Fields
 * whose attributes this composition does not declare (4, 8..11) pass their
 * range check and then fail inside the setter's attribute write,
 * MT_ATTR_ERR_FAILED, identical to the C6.
 *
 * needs_recompute mirrors the C6 row for row (B165): every field that
 * feeds s_alarm_expressed_state_priority recomputes after a successful
 * set; DeviceMuted, ContaminationState and SmokeSensitivityLevel do not,
 * on the reference apps' own evidence (no ExpressedStateEnum value
 * corresponds to any of the three). SetExpressedState() is idempotent
 * against the current value, so field 5's completion path emits
 * SelfTestComplete once and the recompute's AllClear only when the
 * priority verdict actually changes.
 */
extern "C" int mt_matter_alarm_set(uint16_t ep, uint8_t field, uint8_t value)
{
    using namespace chip::app::Clusters;
    chip::DeviceLayer::StackLock lock;
    /* attr_locate()'s two lookups, the same error division as every other
     * bridge in this file. */
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }

    if (emberAfContainsServer(ep, SmokeCoAlarm::Id)) {
        if (field == 0) {
            /* ExpressedState is derived; see the function comment. */
            return MT_ATTR_ERR_VALUE;
        }

        SmokeCoAlarmServer &srv = SmokeCoAlarmServer::Instance();
        bool ok;
        bool needs_recompute = false;
        switch (field) {
        case 1: /* SmokeState */
            if (value >= (uint8_t)SmokeCoAlarmServer::AlarmStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetSmokeState(ep, (SmokeCoAlarmServer::AlarmStateEnum)value);
            needs_recompute = true;
            break;
        case 2: /* COState */
            if (value >= (uint8_t)SmokeCoAlarmServer::AlarmStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetCOState(ep, (SmokeCoAlarmServer::AlarmStateEnum)value);
            needs_recompute = true;
            break;
        case 3: /* BatteryAlert */
            if (value >= (uint8_t)SmokeCoAlarmServer::AlarmStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetBatteryAlert(ep, (SmokeCoAlarmServer::AlarmStateEnum)value);
            needs_recompute = true;
            break;
        case 4: /* DeviceMuted: not declared here; the setter fails. Not an
                 * ExpressedState priority, no recompute. */
            if (value >= (uint8_t)SmokeCoAlarmServer::MuteStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetDeviceMuted(ep, (SmokeCoAlarmServer::MuteStateEnum)value);
            break;
        case 5: /* TestInProgress: bool, the self-test completion path at 0 */
            if (value > 1) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetTestInProgress(ep, value != 0);
            needs_recompute = true;
            break;
        case 6: /* HardwareFaultAlert: bool */
            if (value > 1) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetHardwareFaultAlert(ep, value != 0);
            needs_recompute = true;
            break;
        case 7: /* EndOfServiceAlert */
            if (value >= (uint8_t)SmokeCoAlarmServer::EndOfServiceEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetEndOfServiceAlert(ep, (SmokeCoAlarmServer::EndOfServiceEnum)value);
            needs_recompute = true;
            break;
        case 8: /* InterconnectSmokeAlarm: not declared here; setter fails */
            if (value >= (uint8_t)SmokeCoAlarmServer::AlarmStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetInterconnectSmokeAlarm(ep, (SmokeCoAlarmServer::AlarmStateEnum)value);
            needs_recompute = true;
            break;
        case 9: /* InterconnectCOAlarm: not declared here; setter fails */
            if (value >= (uint8_t)SmokeCoAlarmServer::AlarmStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetInterconnectCOAlarm(ep, (SmokeCoAlarmServer::AlarmStateEnum)value);
            needs_recompute = true;
            break;
        case 10: /* ContaminationState: not declared here; setter fails */
            if (value >= (uint8_t)SmokeCoAlarmServer::ContaminationStateEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetContaminationState(ep, (SmokeCoAlarmServer::ContaminationStateEnum)value);
            break;
        case 11: /* SmokeSensitivityLevel: not declared here; setter fails */
            if (value >= (uint8_t)SmokeCoAlarmServer::SensitivityEnum::kUnknownEnumValue) {
                return MT_ATTR_ERR_VALUE;
            }
            ok = srv.SetSmokeSensitivityLevel(ep, (SmokeCoAlarmServer::SensitivityEnum)value);
            break;
        default:
            /* cmd_mtalarm caps field at 11; unreachable, kept defensive. */
            return MT_ATTR_ERR_VALUE;
        }
        if (ok && needs_recompute) {
            srv.SetExpressedStateByPriority(ep, s_alarm_expressed_state_priority);
        }
        return ok ? MT_ATTR_OK : MT_ATTR_ERR_FAILED;
    }

    /*
     * Catalogue batch 8: the second cluster arm the pre-batch comment
     * promised, and it is a different shape from the one above rather than
     * a tenth case in the same switch.
     *
     * <field> is not an attribute selector here, it is an alarm BIT NUMBER
     * (AT_MT_SPEC.md 2312-2317): 0 is DoorOpen, the only bit the Matter spec
     * defines in any revision through 1.5.1, and 1..7 exist only because the
     * union bound mt_at.c enforces is 0..11 and this bridge has to narrow it
     * somewhere. Bits 8..11 are legal for SmokeCoAlarm and out of range
     * here.
     *
     * The Supported check is what makes an in-range but undefined bit
     * answer +MTERR:1 rather than quietly setting a bit no controller can
     * interpret. It reads the endpoint's OWN Supported attribute rather than
     * a compiled-in mask, so a future host-configurable Supported (there is
     * no AT path for it today; the seed is bit 0) narrows this check with
     * it.
     *
     * ONE CALL, BOTH EFFECTS: SetStateValue() writes State and emits Notify
     * itself (refrigerator-alarm-server.cpp:115-149), which is the whole
     * reason AT+MTATTR is not the write path for State. Read-modify-write of
     * the current State rather than a bare assignment, because <field> names
     * one bit and the other bits must survive; GetStateValue() answers from
     * the same arena slot the write lands in.
     */
    if (emberAfContainsServer(ep, RefrigeratorAlarm::Id)) {
        if (field > 7 || value > 1) {
            return MT_ATTR_ERR_VALUE;
        }
        RefrigeratorAlarmServer &srv = RefrigeratorAlarmServer::Instance();
        const chip::BitMask<RefrigeratorAlarm::AlarmBitmap> bit(
            static_cast<uint32_t>(1u) << field);

        chip::BitMask<RefrigeratorAlarm::AlarmBitmap> supported;
        if (srv.GetSupportedValue(ep, &supported) != Status::Success) {
            return MT_ATTR_ERR_FAILED;
        }
        if (!supported.HasAll(bit)) {
            return MT_ATTR_ERR_VALUE;
        }

        chip::BitMask<RefrigeratorAlarm::AlarmBitmap> state;
        if (srv.GetStateValue(ep, &state) != Status::Success) {
            return MT_ATTR_ERR_FAILED;
        }
        if (value != 0) {
            state.Set(bit);
        } else {
            state.Clear(bit);
        }
        return (srv.SetStateValue(ep, state) == Status::Success) ? MT_ATTR_OK
                                                                 : MT_ATTR_ERR_FAILED;
    }

    return MT_ATTR_ERR_CLUSTER;
}
/* ---- laundry washer / dishwasher / laundry dryer (OperationalState) ----
 *
 * One Instance PLUS one Delegate object per endpoint; the audit trail for
 * why (SetInstance's VerifyOrDie, the AAI-served attribute set, the forced
 * construct-after-create ordering, the never-destroy policy) lives on
 * opStateAttrs in mt_devtypes_zephyr.cpp. This section owns the objects.
 *
 * The verdict here IS the wire response, unlike the valve: the only caller
 * of each Handle*StateCallback copies err straight into the
 * OperationalCommandResponse (Instance::HandlePauseState,
 * operational-state-server.cpp:414-447, "mDelegate->HandlePauseStateCallback
 * (err); ... response.commandResponseState = err", and the
 * Stop/Start/Resume handlers at :449, :467, :485 follow the same shape). So
 * allow sets kNoError and deny sets kUnableToCompleteOperation, the same
 * pair the C6 maps (kUnableToCompleteOperation rather than
 * kUnableToStartOrResume matches the SDK's own dishwasher placeholder's
 * choice for a generic delegate-side denial).
 *
 * The delegate deliberately does NOT call SetOperationalState() on allow.
 * Same split-ownership rule as AT+MTLOCK and AT+MTVALVE: the verdict says
 * the host MAY act; only the host knows when the physical appliance
 * actually completed the transition, and it reports that separately with
 * AT+MTOPSTATE (mt_matter_opstate_set below), which emits the attribute
 * report a subscribed controller expects.
 *
 * The command hooks run on the CHIP task (the Instance's own
 * CommandHandlerInterface; this cluster is CHI-only, config-data.yaml:63),
 * so no StackLock in them, the batch 3 discipline.
 *
 * Instance reachability needs no registry: the Instance constructor calls
 * mDelegate->SetInstance(this) (operational-state-server.cpp:48), the base
 * Delegate stores it, and the protected GetInstance() accessor
 * (operational-state-server.h:364-371) is reachable from this subclass;
 * instance() below is the public passthrough mt_matter_opstate_set() uses,
 * the C6's exact mechanism.
 */
class HearthOpStateDelegate : public chip::app::Clusters::OperationalState::Delegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }
    void set_cluster(chip::ClusterId cluster) { m_cluster = cluster; }
    chip::EndpointId endpoint() const { return m_ep; }
    chip::ClusterId cluster() const { return m_cluster; }

    /* GetInstance() is protected in the base Delegate; this passthrough is
     * how mt_matter_opstate_set() reaches the Instance the constructor
     * attached via SetInstance(). */
    chip::app::Clusters::OperationalState::Instance *instance() { return GetInstance(); }

    chip::app::DataModel::Nullable<uint32_t> GetCountdownTime() override
    {
        return chip::app::DataModel::NullNullable;
    }

    /* Exactly the four base states, in enum order. The SDK reads this list
     * to serve OperationalStateList; kError is a member because the spec
     * requires the error state to be listed even though AT+MTOPSTATE can
     * never set it directly (kError is reserved for the error-detection
     * path, and SetOperationalState() refuses it independently,
     * operational-state-server.cpp:101-107). */
    CHIP_ERROR GetOperationalStateAtIndex(
        size_t index,
        chip::app::Clusters::OperationalState::GenericOperationalState &operationalState) override
    {
        using chip::app::Clusters::OperationalState::OperationalStateEnum;
        static const OperationalStateEnum states[] = {
            OperationalStateEnum::kStopped,
            OperationalStateEnum::kRunning,
            OperationalStateEnum::kPaused,
            OperationalStateEnum::kError,
        };
        if (index >= (sizeof(states) / sizeof(states[0]))) {
            return CHIP_ERROR_NOT_FOUND;
        }
        operationalState.Set(chip::to_underlying(states[index]));
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetOperationalPhaseAtIndex(size_t index, chip::MutableCharSpan &operationalPhase) override
    {
        /* NOT_FOUND at index 0 makes PhaseList read null: these appliances
         * publish no phases (operational-state-server.h:305-317 documents
         * the convention). */
        (void)index;
        (void)operationalPhase;
        return CHIP_ERROR_NOT_FOUND;
    }

    void HandlePauseStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::OperationalState::Commands::Pause::Id, err);
    }

    void HandleResumeStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::OperationalState::Commands::Resume::Id, err);
    }

    void HandleStartStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::OperationalState::Commands::Start::Id, err);
    }

    void HandleStopStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::OperationalState::Commands::Stop::Id, err);
    }

private:
    void forward(uint32_t command, chip::app::Clusters::OperationalState::GenericOperationalError &err)
    {
        using chip::app::Clusters::OperationalState::ErrorStateEnum;
        bool allow = mt_cmd_forward(m_ep, m_cluster, command);
        err.Set(chip::to_underlying(allow ? ErrorStateEnum::kNoError
                                          : ErrorStateEnum::kUnableToCompleteOperation));
    }

    chip::EndpointId m_ep = chip::kInvalidEndpointId;
    /* Only the base cluster exists in this catalogue; carried anyway so the
     * +MTCMD cluster field always comes from the slot, not a literal, and a
     * future derived-cluster consumer (the oven cavity) reuses this class
     * unchanged, the C6's shape. */
    chip::ClusterId m_cluster = chip::app::Clusters::OperationalState::Id;
};

/*
 * The delegate pool, kServiceableEndpoints deep like the valve's (a
 * seventeenth endpoint of ANY type fails its create before it could ask
 * for a delegate; mt_matter.h's MT_COMP_MAX_ENDPOINTS sizing is the C6's,
 * which serves all 28). Memory reclaim round A: the pool is a table of
 * POINTERS into the cluster-object heap, and the Instance's raw storage
 * sits BEHIND the delegate in the same heap block rather than in a
 * parallel .bss array. OperationalState::Instance has no default
 * constructor (it takes the delegate and the endpoint id, neither known
 * until create time), so it is placement-constructed exactly once in
 * mt_matter_opstate_delegate_set_endpoint() and never destroyed, the
 * allocate-only policy the valve pool and the endpoint block heap already
 * follow: blocks are handed out monotonically by the one boot rebuild and
 * a reboot resets the heap wholesale, so no block is ever re-constructed
 * over a live Instance and no explicit destructor call is needed (or made;
 * see the opStateAttrs audit note for what ~Instance() would do).
 */
static HearthOpStateDelegate *s_opstate_delegates[kServiceableEndpoints];
static size_t s_opstate_delegate_next;

extern "C" void *mt_matter_opstate_delegate_alloc(uint32_t cluster_id)
{
    if (s_opstate_delegate_next >= kServiceableEndpoints) {
        return nullptr;
    }
    HearthOpStateDelegate *d =
        obj_pair_new<HearthOpStateDelegate, chip::app::Clusters::OperationalState::Instance>();
    if (d == nullptr) {
        return nullptr;
    }
    d->set_cluster(cluster_id);
    s_opstate_delegates[s_opstate_delegate_next++] = d;
    return d;
}

/*
 * The second half of the handout, and on this platform it is where the
 * Instance is born: constructed into the raw storage behind the delegate
 * in its own heap block (memory reclaim round A; the delegate pointer IS
 * the block) and Init()ed, which registers the endpoint-scoped
 * CommandHandlerInterface and AttributeAccessInterface. Must run below a
 * successful emberAfSetDynamicEndpoint(): Init() opens with an
 * emberAfContainsServer() check and bails otherwise
 * (operational-state-server.cpp:65-70).
 *
 * An Init() failure is logged loudly and does not abort the create, the
 * valve read-back's reasoning exactly: below a successful
 * emberAfSetDynamicEndpoint() the contains-server check cannot fail, and
 * the registry Register() calls fail only on a duplicate registration,
 * which the construct-once pool rules out. If it ever fired anyway the
 * endpoint is live and correctly seeded, and an appliance that reports
 * state but does not adjudicate, plus a shouting log, beats tearing down
 * a healthy endpoint.
 */
/*
 * Catalogue batch 8 gave this function its second Instance type, and the
 * delegate class was written for it three batches ago: m_cluster exists
 * precisely so "a future derived-cluster consumer (the oven cavity) reuses
 * this class unchanged".
 *
 * OvenCavityOperationalState::Instance is a public subclass of
 * OperationalState::Instance whose entire body is a two-argument constructor
 * forwarding to the protected three-argument base with the cluster id baked
 * in (operational-state-server.h:459-478). It declares NO data members and
 * NO overrides, so its sizeof and alignof equal the base's and the raw
 * storage obj_pair_new() reserved for the base holds it exactly. That is
 * asserted rather than trusted: a future SDK that gave the subclass a member
 * would otherwise placement-construct past the end of the block, silently.
 */
static_assert(sizeof(chip::app::Clusters::OvenCavityOperationalState::Instance) ==
                  sizeof(chip::app::Clusters::OperationalState::Instance),
              "OvenCavityOperationalState::Instance grew past the base Instance's storage in "
              "the opstate pool's blocks");
static_assert(alignof(chip::app::Clusters::OvenCavityOperationalState::Instance) ==
                  alignof(chip::app::Clusters::OperationalState::Instance),
              "OvenCavityOperationalState::Instance out-aligns the base Instance's storage");

/* Catalogue batch 8 split the body out so the microwave's ordering function
 * can CHECK this Init()'s return rather than only hear about it in a log.
 * mt_matter.h's setter signature is void and that header is read-only, so
 * the public entry point below stays void and this helper carries the
 * error; the log text is emitted here so both callers produce the identical
 * line. */
static CHIP_ERROR opstate_construct_and_init(void *delegate, uint16_t ep)
{
    namespace OpState     = chip::app::Clusters::OperationalState;
    namespace OvenCavity  = chip::app::Clusters::OvenCavityOperationalState;
    auto *d = static_cast<HearthOpStateDelegate *>(delegate);
    d->set_endpoint(ep);
    uint8_t *storage = obj_inst_storage<HearthOpStateDelegate, OpState::Instance>(d);
    /* The cluster id was fixed at alloc time, before anything could be
     * spent, and it is what picks the Instance type here. Both arms yield an
     * OperationalState::Instance * (the derived one by upcast), which is
     * what the delegate's own GetInstance() passthrough hands back later, so
     * everything downstream of this point is cluster-agnostic. */
    OpState::Instance *inst = nullptr;
    if (d->cluster() == OvenCavity::Id) {
        inst = new (storage) OvenCavity::Instance(d, ep);
    } else {
        inst = new (storage) OpState::Instance(d, ep);
    }
    CHIP_ERROR err = inst->Init();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("opstate Instance::Init failed for endpoint %u cluster 0x%08X: "
                "%" CHIP_ERROR_FORMAT
                "; commands on it will not reach the host and its attributes will not be served",
                (unsigned)ep, (unsigned)d->cluster(), err.Format());
    }
    return err;
}

extern "C" void mt_matter_opstate_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    (void)opstate_construct_and_init(delegate, ep);
}

/*
 * AT+MTOPSTATE bridge. mt_at.c's cmd_mtopstate has already rejected
 * anything outside the UNION of every opstate cluster family's legal set
 * (and 3, kError, outright) before this is called; this bridge narrows to
 * the legal set of the cluster ep actually carries, two branches since
 * catalogue batch 5: the base cluster's {0 Stopped, 1 Running, 2 Paused}
 * for the washer/dishwasher/dryer trio, and RvcOperationalState's widened
 * {0, 1, 2, 0x40 kSeekingCharger, 0x41 kCharging, 0x42 kDocked} for the
 * RVC. A union-legal RVC state (0x40..0x42) on a washer answers
 * MT_ATTR_ERR_VALUE in the base branch, exactly as the C6's does; the
 * oven-cavity branch arrives with its device type in the
 * composed-appliance batch. SetOperationalState() enforces each cluster's
 * rule independently (operational-state-server.cpp:101-107, kError or an
 * unsupported state answers CHIP_ERROR_INVALID_ARGUMENT), so a state that
 * slipped both checks would still map to MT_ATTR_ERR_FAILED rather than
 * being silently accepted.
 *
 * Called from the AT parser thread, so the StackLock IS taken, unlike the
 * delegate hooks above.
 */
extern "C" int mt_matter_opstate_set(uint16_t ep, uint8_t state)
{
    namespace OpState = chip::app::Clusters::OperationalState;
    namespace RvcOpState = chip::app::Clusters::RvcOperationalState;
    namespace OvenCavityOperationalState = chip::app::Clusters::OvenCavityOperationalState;
    chip::DeviceLayer::StackLock lock;
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }

    if (emberAfContainsServer(ep, OpState::Id)) {
        if (state > 2) {
            return MT_ATTR_ERR_VALUE;
        }
        OpState::Instance *inst = nullptr;
        for (size_t i = 0; i < s_opstate_delegate_next; i++) {
            HearthOpStateDelegate *d = s_opstate_delegates[i];
            if (d->endpoint() == ep && d->cluster() == OpState::Id) {
                inst = d->instance();
                break;
            }
        }
        if (inst == nullptr) {
            /* Cannot happen once the boot rebuild has run: every endpoint
             * carrying the cluster got its pair in mt_devtype_create().
             * Defensive, the C6's same arm. */
            return MT_ATTR_ERR_FAILED;
        }
        return (inst->SetOperationalState(state) == CHIP_NO_ERROR) ? MT_ATTR_OK
                                                                   : MT_ATTR_ERR_FAILED;
    }

    /* Catalogue batch 5: the RVC branch the pre-batch comment promised.
     * The derived cluster's state space widens to {0, 1, 2, 0x40
     * kSeekingCharger, 0x41 kCharging, 0x42 kDocked} (RvcOperationalState/
     * Enums.h:67-69); mt_at.c's union check has already admitted these,
     * and this is where a union-legal value lands on the RIGHT cluster
     * (0x40 on a washer still answers MT_ATTR_ERR_VALUE in the base
     * branch above, mirroring the C6's mt_matter_opstate_set() branch
     * order exactly). SetOperationalState() enforces the same set
     * independently through IsSupportedOperationalState(), so a state
     * that slipped both checks maps to MT_ATTR_ERR_FAILED rather than
     * being silently accepted. The Instance is reached through the RVC's
     * own delegate pool below, keyed by endpoint alone (one
     * RvcOperationalState per RVC endpoint), returned upcast to the base
     * Instance the way the base GetInstance() passthrough hands it over. */
    if (emberAfContainsServer(ep, RvcOpState::Id)) {
        if (!(state <= 2 || state == 0x40 || state == 0x41 || state == 0x42)) {
            return MT_ATTR_ERR_VALUE;
        }
        OpState::Instance *inst = mt_rvc_opstate_instance(ep);
        if (inst == nullptr) {
            /* Cannot happen once the boot rebuild has run; defensive. */
            return MT_ATTR_ERR_FAILED;
        }
        return (inst->SetOperationalState(state) == CHIP_NO_ERROR) ? MT_ATTR_OK
                                                                   : MT_ATTR_ERR_FAILED;
    }

    /* Catalogue batch 8: the third branch this function's own comment has
     * promised since batch 5, and it is the RVC's mirror image
     * (AT_MT_SPEC.md 2078-2090). OvenCavityOperationalState is a DERIVED
     * cluster that adds no derived-number-space states at all, so its legal
     * set is identical to the base cluster's {0 Stopped, 1 Running,
     * 2 Paused} and 0x40..0x42 answer +MTERR:1 on a cavity exactly as they
     * do on a washer. mt_at.c's union check needed no edit for this branch
     * for the same reason: the cavity's set is a subset of the union it
     * already admits. What is narrower than the base is the COMMAND set
     * (Stop and Start only), and that is the declared list's business, not
     * this function's: a host may report a Paused cavity even though no
     * controller can ask for one. The Instance comes from the shared opstate
     * pool, matched on both endpoint and cluster like the base branch. */
    if (emberAfContainsServer(ep, OvenCavityOperationalState::Id)) {
        if (state > 2) {
            return MT_ATTR_ERR_VALUE;
        }
        OpState::Instance *inst = nullptr;
        for (size_t i = 0; i < s_opstate_delegate_next; i++) {
            HearthOpStateDelegate *d = s_opstate_delegates[i];
            if (d->endpoint() == ep && d->cluster() == OvenCavityOperationalState::Id) {
                inst = d->instance();
                break;
            }
        }
        if (inst == nullptr) {
            /* Cannot happen once the boot rebuild has run; defensive, the
             * base branch's arm. */
            return MT_ATTR_ERR_FAILED;
        }
        return (inst->SetOperationalState(state) == CHIP_NO_ERROR) ? MT_ATTR_OK
                                                                   : MT_ATTR_ERR_FAILED;
    }

    return MT_ATTR_ERR_CLUSTER;
}

/*
 * DE397 live read for the trio's two Instance-served scalars, dispatched
 * from mt_matter_attr_read() (see the carve-out comment above
 * attr_locate()). Runs under that bridge's StackLock; the pool scan is
 * the same one mt_matter_opstate_set() uses. Both attributes are unsigned
 * (OperationalStateEnum enum8, CurrentPhase uint8), and a null
 * CurrentPhase answers MT_ATTR_ERR_TYPE with the flag already set, the
 * generic null-nullable contract (the AT grammar has no null literal).
 * These appliances publish no phases, so CurrentPhase in practice always
 * answers +MTERR:5 here, matching what a controller reads as null.
 */
static int mt_opstate_attr_read_live(uint16_t ep, uint32_t cluster, uint32_t attr, int64_t *out,
                                     bool *is_unsigned)
{
    namespace OpState = chip::app::Clusters::OperationalState;
    if (is_unsigned) {
        *is_unsigned = true;
    }
    /* Catalogue batch 8: keyed on (endpoint, CLUSTER) rather than endpoint
     * and the base cluster id, because the shared pool now serves the oven
     * cavity's derived cluster too. The base cluster's answers are
     * unchanged: no device type carries both, so the same slot is found
     * either way. */
    OpState::Instance *inst = nullptr;
    for (size_t i = 0; i < s_opstate_delegate_next; i++) {
        HearthOpStateDelegate *d = s_opstate_delegates[i];
        if (d->endpoint() == ep && d->cluster() == cluster) {
            inst = d->instance();
            break;
        }
    }
    if (inst == nullptr) {
        /* Cannot happen once the boot rebuild has run; defensive, the
         * same arm as mt_matter_opstate_set(). */
        return MT_ATTR_ERR_FAILED;
    }
    if (attr == OpState::Attributes::OperationalState::Id) {
        *out = inst->GetCurrentOperationalState();
        return MT_ATTR_OK;
    }
    chip::app::DataModel::Nullable<uint8_t> phase = inst->GetCurrentPhase();
    if (phase.IsNull()) {
        return MT_ATTR_ERR_TYPE;
    }
    *out = phase.Value();
    return MT_ATTR_OK;
}

/* ---- mode select (0x0027) ---------------------------------------------
 *
 * ONE process-global manager for the whole device, not a pool: the SDK
 * fetches it fresh through getSupportedModesManager() on every
 * SupportedModes read and every ChangeToMode
 * (mode-select-server.cpp:84, :123, :469), and setSupportedModesManager()
 * (:62) stores a bare global pointer, so a second mode select endpoint
 * re-registering it is harmless. The manager dispatches on endpoint id
 * itself against that endpoint's host-fed store.
 *
 * The store (mt_mode_store_t, mt_dyn_store.h) lives in the endpoint's own
 * heap block since the store reclaim round, reached through
 * mt_dyn_mode_store(): only a composition that declares mode select
 * endpoints pays for it, and the .bss pool this section used to hold
 * (7,040 B for 16 slots) is gone. Capacity did not move: the block heap
 * serves at most kServiceableEndpoints (16) endpoints, and an endpoint the
 * create path refused is unaddressable by AT+MTMODES anyway, the
 * acceptance-versus-capacity split every pool in this file follows. A
 * fresh store has count 0, which every reader below maps to exactly the
 * answers the pool's no-used-slot arm used to give, so the wire cannot
 * tell the storage moved.
 *
 * CharSpan lifetime and the in-place rebuild, the C6's argument re-checked
 * against THIS tree: both the label bytes and the ModeOptionStruct array
 * live in the endpoint's block, which is allocated once at boot and never
 * freed (the allocate-only invariant beside K_HEAP_DEFINE in
 * mt_devtypes_zephyr.cpp), and the struct array is rebuilt IN PLACE on
 * every AT+MTMODES write to the same store, never reallocated. What rules
 * out a reader observing a half-rebuilt entry is mutual exclusion, not the
 * rebuild order: mt_matter_modes_set() holds the StackLock for the whole
 * rebuild, and the reads run on the CHIP task under the same lock. The SDK
 * also re-fetches the provider per access rather than caching one, which
 * rules out a stale provider outliving a rebuild; secondary, the lock is
 * the guarantee.
 */

class HearthSupportedModesManager : public chip::app::Clusters::ModeSelect::SupportedModesManager
{
private:
    /*
     * SupportedModesManager declares its ModeOptionStructType alias BEFORE
     * its "public:" (supported-modes-manager.h:36), so the name is private
     * to the base class even though a public virtual's signature uses it:
     * a derived class inherits the member but not access to the name. The
     * same private-alias shadow the esp32 platform's own
     * StaticSupportedModesManager uses, mirrored from the C6.
     */
    using ModeOptionStructType = chip::app::Clusters::ModeSelect::Structs::ModeOptionStruct::Type;

public:
    ModeOptionsProvider getModeOptionsProvider(chip::EndpointId endpointId) const override
    {
        mt_mode_store_t *store = mt_dyn_mode_store(endpointId);
        if (store != nullptr && store->count > 0) {
            return ModeOptionsProvider(store->structs, store->structs + store->count);
        }
        /* begin == end == nullptr: no store on this endpoint, or the host
         * has not fed a list yet (count 0, what the pool's unclaimed slot
         * used to mean), an empty SupportedModes list either way. */
        return ModeOptionsProvider();
    }

    chip::Protocols::InteractionModel::Status getModeOptionByMode(
        chip::EndpointId endpointId, uint8_t mode, const ModeOptionStructType **dataPtr) const override
    {
        mt_mode_store_t *store = mt_dyn_mode_store(endpointId);
        if (store == nullptr || store->count == 0) {
            /* count 0 lands here, NOT in the InvalidCommand arm below:
             * before the reclaim round an endpoint the host had not fed
             * had no used pool slot at all and answered UnsupportedCluster,
             * and a ChangeToMode against a fresh mode select endpoint must
             * keep answering exactly that. */
            return chip::Protocols::InteractionModel::Status::UnsupportedCluster;
        }
        for (uint8_t i = 0; i < store->count; i++) {
            if (store->structs[i].mode == mode) {
                *dataPtr = &store->structs[i];
                return chip::Protocols::InteractionModel::Status::Success;
            }
        }
        return chip::Protocols::InteractionModel::Status::InvalidCommand;
    }
};
static HearthSupportedModesManager s_mode_select_manager;

/*
 * The accessor doubles as the registration: on this platform there is no
 * esp_matter init-callback pass to defer to, so the global is (re)set here,
 * each call, before mt_devtype_create() spends anything on the endpoint.
 * Idempotent by construction (same pointer every time), and safe at any
 * point in boot: setSupportedModesManager() is a bare pointer store with
 * no CHIP state behind it.
 */
extern "C" void *mt_matter_mode_select_manager(void)
{
    chip::app::Clusters::ModeSelect::setSupportedModesManager(&s_mode_select_manager);
    return &s_mode_select_manager;
}

/*
 * AT+MTMODES bridge (the ModeSelect form). Grammar and content rules are
 * enforced by cmd_mtmodes() in mt_at.c before this is called; the bounds
 * re-checked here are defensive. Mirrors the C6's
 * mt_matter_modes_set() with the ember lookups in place of esp_matter's
 * and the same terminal MatterReportingAttributeChangeCallback() so an
 * active subscription sees the new list; CurrentMode is deliberately not
 * touched here (a controller's ChangeToMode owns it, and the host
 * observes that as a +MTATTR URC).
 */
extern "C" int mt_matter_modes_set(uint16_t ep, const uint8_t *modes, const char *const *labels,
                                   uint8_t count)
{
    chip::DeviceLayer::StackLock lock;
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, chip::app::Clusters::ModeSelect::Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (count < 1 || count > MT_MODES_MAX_COUNT) {
        return MT_ATTR_ERR_FAILED;
    }

    mt_mode_store_t *slot = mt_dyn_mode_store(ep);
    if (!slot) {
        /* Cannot happen: the two lookups above proved ep live with a
         * ModeSelect server, and every such endpoint's block carries its
         * store from create. Defensive, the role the exhausted pool arm
         * used to play. */
        return MT_ATTR_ERR_FAILED;
    }

    /* Fix round, review M5: every label is validated BEFORE any entry is
     * overwritten. The old single loop returned mid-write on a bad
     * length, leaving entries[0..i-1] holding new bytes while
     * slot->structs[] still published the previous list's CharSpan
     * lengths over them: no overrun (everything stays in-buffer), but a
     * served label could carry a stale tail. Unreachable in practice,
     * because cmd_mtmodes has already enforced these exact bounds before
     * the bridge is called; hardened anyway so the defensive check can
     * never publish a half-written list, rather than relying on a
     * comment to say why it could not. The chime twin needs no such
     * split: its names are re-measured with strlen() on every read, not
     * cached in a span. */
    for (uint8_t i = 0; i < count; i++) {
        size_t len = strlen(labels[i]);
        if (len < 1 || len > MT_MODES_MAX_LABEL_LEN) {
            return MT_ATTR_ERR_FAILED;
        }
    }
    for (uint8_t i = 0; i < count; i++) {
        slot->entries[i].mode = modes[i];
        memcpy(slot->entries[i].label, labels[i], strlen(labels[i]) + 1);
    }
    slot->count = count;

    /* Rebuild the struct array in place, each CharSpan pointing into the
     * store's own label buffer (block-resident, never freed; the section
     * comment's lifetime argument); the lock held across this whole
     * function is what makes the rebuild atomic against CHIP-task readers
     * (see the section comment). SemanticTags publishes a
     * default-constructed empty List per entry, mandatory-but-empty-legal. */
    for (uint8_t i = 0; i < count; i++) {
        slot->structs[i].mode = slot->entries[i].mode;
        slot->structs[i].label = chip::CharSpan::fromCharString(slot->entries[i].label);
        slot->structs[i].semanticTags = chip::app::DataModel::List<
            const chip::app::Clusters::ModeSelect::Structs::SemanticTagStruct::Type>();
    }

    MatterReportingAttributeChangeCallback(
        ep, chip::app::Clusters::ModeSelect::Id,
        chip::app::Clusters::ModeSelect::Attributes::SupportedModes::Id);
    return MT_ATTR_OK;
}

/* ---- chime (0x0146) ---------------------------------------------------
 *
 * Per-endpoint ChimeServer plus ChimeDelegate; the composition-side audit
 * (CHI-only, no ServerInit, the AAI-shadowed KVS-persisted pair, the
 * missing NotFound pre-check and event) is on chimeAttrs in
 * mt_devtypes_zephyr.cpp. This section owns the objects and the two AT
 * bridges.
 *
 * The verdict is the one on this firmware's whole +MTCMD surface that
 * reaches the controller exactly as given: HandlePlayChimeSound() copies
 * whatever Status the delegate returns straight into the command's
 * response (chime-server.cpp:260-274), with no SDK-side remapping.
 * Status::Success on allow, Status::Failure on deny.
 *
 * The payload, DECIDED (user ruling DE396): this tree's PlayChimeSound()
 * takes NO argument (chime-server.h:164; the command XML declares none),
 * so the C6's invoke-supplied chimeID does not exist to forward. The
 * single trailing +MTCMD field instead carries the owning server's
 * GetSelectedChime(): the wire arity stays byte-identical to the C6 and
 * the payload means "the chime id that will play", which is also what the
 * server will play by its own rules. The server pointer comes from the
 * base class's own back-reference (the ctor calls SetChimeServer(this),
 * chime-server.cpp:46, readable through the protected GetChimeServer(),
 * chime-server.h:173), the OperationalState GetInstance() mechanism under
 * another name.
 */
/* The sounds store (mt_chime_store_t, mt_dyn_store.h) lives in the chime
 * endpoint's own heap block since the store reclaim round, reached through
 * mt_dyn_chime_store(); the 4,448 B .bss pool this section used to hold is
 * gone, and only compositions that declare chime endpoints pay for the
 * store. A fresh store has count 0, which both delegate lookups below
 * already answer with PROVIDER_LIST_EXHAUSTED at any index, exactly what
 * the pool's no-slot arm produced. */

class HearthChimeDelegate : public chip::app::Clusters::ChimeDelegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }
    chip::EndpointId endpoint() const { return m_ep; }

    /* GetChimeServer() is protected in the base; this passthrough is how
     * the AT bridges below reach each endpoint's server object. */
    chip::app::Clusters::ChimeServer *server() { return GetChimeServer(); }

    CHIP_ERROR GetChimeSoundByIndex(uint8_t chimeIndex, uint8_t &chimeID,
                                    chip::MutableCharSpan &name) override
    {
        mt_chime_store_t *slot = mt_dyn_chime_store(m_ep);
        if (slot == nullptr || chimeIndex >= slot->count) {
            /* PROVIDER_LIST_EXHAUSTED, NOT the CHIP_ERROR_NOT_FOUND the
             * header's doc comment names (chime-server.h:137-138): the
             * implementation's two iteration loops terminate ONLY on
             * PROVIDER_LIST_EXHAUSTED (EncodeSupportedChimeSounds,
             * chime-server.cpp:148-151, and IsSupportedChimeID, :168).
             * Answering NOT_FOUND would fail every InstalledChimeSounds
             * read outright and, worse, spin IsSupportedChimeID forever
             * on an unmatched id (its uint8_t index wraps and nothing
             * else stops it), hanging the CHIP task. The code wins over
             * its comment; same sentinel the C6 returns. */
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        chimeID = slot->entries[chimeIndex].id;
        chip::CharSpan src(slot->entries[chimeIndex].name, strlen(slot->entries[chimeIndex].name));
        return chip::CopyCharSpanToMutableCharSpan(src, name);
    }

    CHIP_ERROR GetChimeIDByIndex(uint8_t chimeIndex, uint8_t &chimeID) override
    {
        mt_chime_store_t *slot = mt_dyn_chime_store(m_ep);
        if (slot == nullptr || chimeIndex >= slot->count) {
            /* Same sentinel note as GetChimeSoundByIndex above. */
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        chimeID = slot->entries[chimeIndex].id;
        return CHIP_NO_ERROR;
    }

    /* Only reached when Enabled is true (chime-server.cpp:266-271; a
     * disabled chime answers Success with no delegate call and no
     * +MTCMD). Runs on the CHIP task, no StackLock. */
    chip::Protocols::InteractionModel::Status PlayChimeSound() override
    {
        using chip::Protocols::InteractionModel::Status;
        chip::app::Clusters::ChimeServer *srv = GetChimeServer();
        if (srv == nullptr) {
            /* Unreachable by construction (the server's ctor set the
             * back-reference before any command could dispatch);
             * defensive, and failing closed beats forwarding a payload
             * this delegate cannot know. */
            return Status::Failure;
        }
        bool allow = mt_cmd_forward_payload(m_ep, chip::app::Clusters::Chime::Id,
                                            chip::app::Clusters::Chime::Commands::PlayChimeSound::Id,
                                            srv->GetSelectedChime());
        return allow ? Status::Success : Status::Failure;
    }

private:
    chip::EndpointId m_ep = chip::kInvalidEndpointId;
};

/* Delegate pool with the raw storage for the non-default-constructible
 * ChimeServer behind each delegate in its own cluster-object heap block,
 * the OperationalState pools' exact shape since memory reclaim round A and
 * the same construct-once, never-destroy policy (chime's ~ChimeServer
 * would unregister both interfaces, chime-server.cpp:49-57, and no
 * teardown path exists on this platform). */
static HearthChimeDelegate *s_chime_delegates[kServiceableEndpoints];
static size_t s_chime_delegate_next;

extern "C" void *mt_matter_chime_delegate_alloc(void)
{
    if (s_chime_delegate_next >= kServiceableEndpoints) {
        return nullptr;
    }
    HearthChimeDelegate *d =
        obj_pair_new<HearthChimeDelegate, chip::app::Clusters::ChimeServer>();
    if (d == nullptr) {
        return nullptr;
    }
    s_chime_delegates[s_chime_delegate_next++] = d;
    return d;
}

/*
 * The second half: placement-constructs the ChimeServer (its ctor stores
 * the delegate reference and sets the delegate's back-reference) and runs
 * Init(), which loads the two KVS-persisted attributes and registers the
 * endpoint-scoped AAI and command handler (chime-server.cpp:59-66). Below
 * a successful emberAfSetDynamicEndpoint() by the same reasoning as the
 * OperationalState pool; an Init() failure is logged loudly and does not
 * abort, the same unreachable-by-ordering argument.
 */
extern "C" void mt_matter_chime_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    auto *d = static_cast<HearthChimeDelegate *>(delegate);
    d->set_endpoint(ep);
    auto *srv =
        new (obj_inst_storage<HearthChimeDelegate, chip::app::Clusters::ChimeServer>(d))
            chip::app::Clusters::ChimeServer(ep, *d);
    CHIP_ERROR err = srv->Init();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("ChimeServer::Init failed for endpoint %u: %" CHIP_ERROR_FORMAT
                "; the chime cluster on it will not be served",
                (unsigned)ep, err.Format());
    }
}

/*
 * Fix round M3: hand back the slot mt_matter_chime_delegate_alloc() gave
 * out, for the create paths that fail AFTER the claim and BEFORE
 * mt_matter_chime_delegate_set_endpoint(). Without this, every post-claim
 * -1 return in mt_devtype_create() stranded one pool slot per boot
 * (bounded, because the rebuild stops at the failure, but the unwind was
 * incomplete where the door lock's is complete). Only the MOST RECENT
 * claim can come back, which is the only case the caller can produce:
 * claims and unclaims both run under the StackLock mt_devtype_create()
 * holds, one endpoint at a time, and a slot that reached set_endpoint
 * (its ChimeServer constructed and registered) is only reachable from the
 * success path, so it can never be offered here. Anything else is a
 * caller bug worth shouting about, not silently absorbing.
 *
 * Deliberately NOT declared in core/include/mt_matter.h (read-only this
 * round): the pair contract there is unchanged, and the one caller
 * declares this port-local extension itself (mt_devtypes_zephyr.cpp,
 * beside the chime handout).
 *
 * Memory reclaim round A: what comes back is the SLOT, not the heap block.
 * The cluster-object heap is allocate-only and has no free, so the
 * unclaimed delegate's block stays out. That is bounded at one stranded
 * block per boot for the same reason the stranded-CLAIM policy is bounded
 * at one: the rebuild stops at the failure that provoked the unclaim and
 * nothing retries within a boot.
 */
extern "C" void mt_matter_chime_delegate_unclaim(void *delegate)
{
    if (s_chime_delegate_next > 0 &&
        delegate == s_chime_delegates[s_chime_delegate_next - 1]) {
        s_chime_delegates[--s_chime_delegate_next] = nullptr;
        return;
    }
    LOG_ERR("chime delegate unclaim ignored: not the most recent claim");
}

/*
 * AT+MTCHIMESOUNDS bridge. Grammar and content rules are enforced by
 * cmd_mtchimesounds() in mt_at.c; the bounds re-checked here are
 * defensive. Ends by marking InstalledChimeSounds dirty with
 * MatterReportingAttributeChangeCallback(), and NOT with the SDK's
 * ReportInstalledChimeSoundsChange(): that method is DECLARED on
 * ChimeServer (chime-server.h:92) and its own doc comments demand
 * calling it, but it has NO DEFINITION anywhere in this tree (grep over
 * src/, examples/ and zzz_generated/ finds only the declaration and the
 * two doc mentions), so calling it is a link error. The same
 * documented-but-nonexistent hole the C6 recorded against ITS tree
 * (core/include/mt_matter.h's note), one revision later and one symbol
 * shape over; the substitute is the same one, and it works identically
 * here because the reporting engine's dirty-marking is not
 * ember-specific.
 */
extern "C" int mt_matter_chime_sounds_set(uint16_t ep, const uint8_t *ids,
                                          const char *const *names, uint8_t count)
{
    chip::DeviceLayer::StackLock lock;
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, chip::app::Clusters::Chime::Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (count < 1 || count > MT_CHIME_MAX_SOUNDS) {
        return MT_ATTR_ERR_FAILED;
    }

    mt_chime_store_t *slot = mt_dyn_chime_store(ep);
    if (!slot) {
        /* Cannot happen: the two lookups above proved ep live with a
         * Chime server, and every such endpoint's block carries its store
         * from create. Defensive. */
        return MT_ATTR_ERR_FAILED;
    }

    for (uint8_t i = 0; i < count; i++) {
        size_t len = strlen(names[i]);
        if (len < 1 || len > MT_CHIME_MAX_NAME_LEN) {
            return MT_ATTR_ERR_FAILED;
        }
        slot->entries[i].id = ids[i];
        memcpy(slot->entries[i].name, names[i], len + 1);
    }
    slot->count = count;

    MatterReportingAttributeChangeCallback(
        ep, chip::app::Clusters::Chime::Id,
        chip::app::Clusters::Chime::Attributes::InstalledChimeSounds::Id);
    return MT_ATTR_OK;
}

/*
 * AT+MTCHIME bridge. mt_at.c's cmd_mtchime has already checked <what> is
 * 0 or 1 and <value> fits a u8. Goes through the server object's own
 * SetSelectedChime()/SetEnabled(), never a raw attribute write: the
 * members are the served truth (the ember slots are inert shadows), and
 * the setters persist to KVS and mark the path dirty themselves
 * (chime-server.cpp:206-248). SetSelectedChime answers Status::NotFound
 * for an id AT+MTCHIMESOUNDS never installed, mapped to MT_ATTR_ERR_VALUE
 * (+MTERR:1), the C6's mapping.
 */
extern "C" int mt_matter_chime_set(uint16_t ep, uint8_t what, uint8_t value)
{
    using chip::Protocols::InteractionModel::Status;
    chip::DeviceLayer::StackLock lock;
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, chip::app::Clusters::Chime::Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }

    chip::app::Clusters::ChimeServer *srv = nullptr;
    for (size_t i = 0; i < s_chime_delegate_next; i++) {
        HearthChimeDelegate *d = s_chime_delegates[i];
        if (d->endpoint() == ep) {
            srv = d->server();
            break;
        }
    }
    if (srv == nullptr) {
        /* Cannot happen once the boot rebuild has run. Defensive. */
        return MT_ATTR_ERR_FAILED;
    }

    Status st;
    switch (what) {
    case 0: /* SelectedChime */
        st = srv->SetSelectedChime(value);
        break;
    case 1: /* Enabled */
        st = srv->SetEnabled(value != 0);
        break;
    default:
        /* cmd_mtchime already rejects <what> outside 0..1. Defensive. */
        return MT_ATTR_ERR_VALUE;
    }
    return (st == Status::Success) ? MT_ATTR_OK : MT_ATTR_ERR_VALUE;
}

/*
 * DE397 live read for the chime's two AAI-shadowed, KVS-persisted
 * attributes, dispatched from mt_matter_attr_read() (see the carve-out
 * comment above attr_locate()). Runs under that bridge's StackLock. Both
 * unsigned, neither nullable; the values are the ChimeServer members a
 * subscribed controller reads, KVS restore and all, so an AT read after a
 * reboot answers the persisted pair rather than the arena's fresh seeds.
 */
static int mt_chime_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out, bool *is_unsigned)
{
    if (is_unsigned) {
        *is_unsigned = true;
    }
    chip::app::Clusters::ChimeServer *srv = nullptr;
    for (size_t i = 0; i < s_chime_delegate_next; i++) {
        HearthChimeDelegate *d = s_chime_delegates[i];
        if (d->endpoint() == ep) {
            srv = d->server();
            break;
        }
    }
    if (srv == nullptr) {
        /* Cannot happen once the boot rebuild has run; defensive. */
        return MT_ATTR_ERR_FAILED;
    }
    if (attr == chip::app::Clusters::Chime::Attributes::SelectedChime::Id) {
        *out = srv->GetSelectedChime();
    } else {
        *out = srv->GetEnabled() ? 1 : 0;
    }
    return MT_ATTR_OK;
}

/*
 * DE397 as amended, fix round 2: the AT+MTATTR write leg for the chime
 * pair, routed to the live server the way the C6's
 * MANAGED_INTERNALLY+WRITABLE branch routes through the data model
 * provider. Dispatched from mt_matter_attr_write() under its StackLock;
 * the status-to-wire mapping mirrors the traced C6 behaviour point for
 * point (see the carve-out comment above attr_locate()): out-of-width
 * +MTERR:1, uninstalled SelectedChime id a bare ERROR (NotFound collapsed,
 * exactly as the C6's ESP_FAIL default arm renders it, and deliberately
 * NOT AT+MTCHIME's +MTERR:1 for the same NotFound), success and
 * same-value OK, no +MTATTR URC in either notify mode, KVS persistence
 * on every changed write (the wear note is in the carve-out comment).
 */
static int mt_chime_attr_write_live(uint16_t ep, uint32_t attr, int64_t val)
{
    using chip::Protocols::InteractionModel::Status;
    chip::app::Clusters::ChimeServer *srv = nullptr;
    for (size_t i = 0; i < s_chime_delegate_next; i++) {
        HearthChimeDelegate *d = s_chime_delegates[i];
        if (d->endpoint() == ep) {
            srv = d->server();
            break;
        }
    }
    if (srv == nullptr) {
        /* Cannot happen once the boot rebuild has run; defensive. */
        return MT_ATTR_ERR_FAILED;
    }
    Status st;
    if (attr == chip::app::Clusters::Chime::Attributes::SelectedChime::Id) {
        if (val < 0 || val > 0xFF) {
            return MT_ATTR_ERR_VALUE;
        }
        st = srv->SetSelectedChime((uint8_t)val);
    } else {
        if (val < 0 || val > 1) {
            return MT_ATTR_ERR_VALUE;
        }
        st = srv->SetEnabled(val != 0);
    }
    return (st == Status::Success) ? MT_ATTR_OK : MT_ATTR_ERR_FAILED;
}

/* ============ catalogue batch 5: the standalone remainder ================
 *
 * The generic switch (0x000F) lands here first; the RVC's ModeBase and
 * RvcOperationalState machinery follows in its own commit. Same file
 * discipline as batches 3 and 4: SDK hooks on the CHIP task take no
 * StackLock, AT bridges from the parser thread always do.
 */

/* ---- generic switch (0x000F) ------------------------------------------ */

/*
 * AT+MTSWITCH bridge (AT_MT_SPEC.md 3.15): emit the Switch cluster's
 * InitialPress event at position 1, the upstream arduino-esp32 class's
 * click(). mt_at.c's cmd_mtswitch has already rejected any action other
 * than 0 with +MTERR:1, so this bridge only ever emits InitialPress.
 *
 * The StackLock is load-bearing, not just uniformity: EventLogging.h says
 * "The consumer has to either lock the Matter stack lock or queue the
 * event to the Matter event queue when using LogEvent. This function is
 * not safe to call outside of the main Matter processing context", and
 * OnInitialPress() is a bare LogEvent() passthrough
 * (switch-server.cpp:66-78). Same quote the C6 cites at its own call site
 * (platform/esp32c6/main/main.cpp mt_matter_switch_click()).
 *
 * The two lookups are attr_locate()'s, for the usual error division.
 * Unlike the C6's esp-matter helper (send_initial_press, which returns
 * esp_err_t), this tree's OnInitialPress() returns VOID: a LogEvent
 * failure is logged by the server and not reported to the caller, so
 * mt_matter.h's MT_ATTR_ERR_FAILED arm ("the event send itself fails") is
 * unreachable on this platform and the checks above are the whole failure
 * surface. Nothing echoes on the AT link; subscribed controllers see the
 * event. CurrentPosition is deliberately not written (the C6 does not
 * either; the cluster's event path never touches it).
 */
extern "C" int mt_matter_switch_click(uint16_t ep)
{
    chip::DeviceLayer::StackLock lock;
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, chip::app::Clusters::Switch::Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    chip::app::Clusters::SwitchServer::Instance().OnInitialPress(ep, 1);
    return MT_ATTR_OK;
}

/* ---- temperature levels (TemperatureControl 0x0056) --------------------
 *
 * Catalogue batch 8. The SupportedTemperatureLevels list a
 * TemperatureLevel-variant cabinet or cook surface publishes, fed by
 * AT+MTTEMPLEVELS (AT_MT_SPEC.md 3.16) and read back by the SDK's own
 * wildcard AttributeAccessInterface.
 *
 * ONE process-global iterator delegate for the whole device, not a pool, and
 * that is the SDK's shape rather than a choice: the cluster keeps a single
 * `SupportedTemperatureLevelsIteratorDelegate *sInstance` behind
 * free-function GetInstance()/SetInstance()
 * (temperature-control-server.cpp:37, :56-66), and dispatches per endpoint
 * by calling Reset(endpoint) on it before every iteration
 * (:82, :195). So the object below is stateless between calls except for
 * the endpoint and index the base class's Reset() sets, and it answers out
 * of whichever endpoint's block-resident store mEndpoint names. The mode
 * select manager above has the identical shape for the identical reason.
 *
 * SetInstance() has NO CALLER anywhere in the SDK: the port must register
 * this itself, exactly as the C6 does (main.cpp:1829). Registration is
 * idempotent (a bare pointer store), so mt_matter_temp_levels_register()
 * below is safe to call once per TemperatureControl-bearing endpoint from
 * the create path, and it is called for BOTH variants deliberately: a
 * TemperatureNumber endpoint never reaches the iterator, but registering
 * only on the level variant would make the registration depend on
 * composition order in a way nothing else here does.
 *
 * The store lives in the endpoint's own heap block (mt_temp_levels_store_t,
 * mt_dyn_store.h), so only endpoints that actually carry the level variant
 * pay its 273 B. The C6's equivalent is a 28-slot .bss array of about
 * 7,728 B that every composition pays; that array is the single largest
 * thing this batch declined to transplant.
 */
class HearthTempLevelsDelegate
    : public chip::app::Clusters::TemperatureControl::SupportedTemperatureLevelsIteratorDelegate
{
public:
    /* Both overrides run on the CHIP task under the stack lock (the AAI read
     * and the SetTemperature callback are both stack-thread work), so they
     * take no lock of their own, the standing hook discipline. */
    uint8_t Size() override
    {
        mt_temp_levels_store_t *store = mt_dyn_temp_levels_store(mEndpoint);
        return (store == nullptr) ? 0 : store->count;
    }

    CHIP_ERROR Next(chip::MutableCharSpan &item) override
    {
        mt_temp_levels_store_t *store = mt_dyn_temp_levels_store(mEndpoint);
        if (store == nullptr || mIndex >= store->count) {
            /* Any non-CHIP_NO_ERROR ends the AAI's encode loop
             * (temperature-control-server.cpp:85-90); NOT_FOUND is the same
             * code the SDK's own sample manager answers past its end. */
            return CHIP_ERROR_NOT_FOUND;
        }
        CHIP_ERROR err = chip::CopyCharSpanToMutableCharSpan(
            chip::CharSpan::fromCharString(store->labels[mIndex]), item);
        if (err == CHIP_NO_ERROR) {
            mIndex++;
        }
        return err;
    }
};

static HearthTempLevelsDelegate s_temp_levels_delegate;

/* The create path's one-liner, port-local for the reason every register in
 * this file is (core/include/mt_matter.h is read-only and names no such
 * function; the C6 needs no equivalent because it registers once at boot
 * from its own main). Called for any TemperatureControl-bearing endpoint,
 * after the endpoint is live for tidiness rather than necessity: this
 * registration resolves no endpoint index and could legally run at any
 * point. */
extern "C" void mt_matter_temp_levels_register(void)
{
    chip::app::Clusters::TemperatureControl::SetInstance(&s_temp_levels_delegate);
}

/*
 * AT+MTTEMPLEVELS (AT_MT_SPEC.md 3.16). Grammar, count and label content are
 * enforced by cmd_mttemplevels() in mt_at.c before this is called; the
 * bounds re-checked here are defensive, the AT+MTMODES bridge's discipline.
 *
 * The error division is the header's (core/include/mt_matter.h:247-251) and
 * needs BOTH lookups, because this port can be in a state no earlier command
 * could reach: the endpoint carries TemperatureControl and still has no
 * label store, because the store belongs to the cluster's TemperatureLevel
 * VARIANT. emberAfContainsServer() answers the cluster question
 * (MT_ATTR_ERR_CLUSTER, +MTERR:3) and mt_dyn_temp_levels_store() answers the
 * variant question (MT_ATTR_ERR_ATTRIBUTE, +MTERR:4). On the C6 the second
 * question is asked of esp-matter's attribute::get() instead
 * (main.cpp:1846-1852); here it is asked of the declared list, through the
 * same predicate the store walk and the create-path construction use.
 *
 * Full replacement per call, never a merge, and not persisted: the store
 * starts empty every boot and the host re-sends, the standing contract for
 * host-fed state. Ends with the dirty mark, which is the ONLY way a
 * subscribed controller learns the list changed, since SupportedTemperatureLevels
 * is served by an AAI and never passes through
 * MatterPostAttributeChangeCallback(): no +MTATTR URC fires for it either,
 * and there is no AT+MTATTR path to it at all (AT_MT_SPEC.md 1249-1251).
 */
extern "C" int mt_matter_temp_levels_set(uint16_t ep, const char *const *labels, uint8_t count)
{
    namespace TemperatureControl = chip::app::Clusters::TemperatureControl;
    chip::DeviceLayer::StackLock lock;
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, TemperatureControl::Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    mt_temp_levels_store_t *store = mt_dyn_temp_levels_store(ep);
    if (store == nullptr) {
        /* The cluster is there and the store is not: a TemperatureNumber
         * variant. The one arm of this function that is a normal answer
         * rather than a defensive one. */
        return MT_ATTR_ERR_ATTRIBUTE;
    }
    if (labels == nullptr || count < 1 || count > MT_TEMP_LEVEL_MAX_COUNT) {
        return MT_ATTR_ERR_FAILED;
    }
    /* Every label validated before any entry is overwritten, the AT+MTMODES
     * fix-round shape. Strictly optional here (the iterator copies per call
     * and caches no span, so a half-written list could not publish stale
     * bytes) and kept anyway so the host-fed bridges reason identically. */
    for (uint8_t i = 0; i < count; i++) {
        if (labels[i] == nullptr) {
            return MT_ATTR_ERR_FAILED;
        }
        size_t len = strlen(labels[i]);
        if (len < 1 || len > MT_TEMP_LEVEL_MAX_LEN) {
            return MT_ATTR_ERR_FAILED;
        }
    }
    for (uint8_t i = 0; i < count; i++) {
        memcpy(store->labels[i], labels[i], strlen(labels[i]) + 1);
    }
    store->count = count;

    MatterReportingAttributeChangeCallback(
        ep, TemperatureControl::Id, TemperatureControl::Attributes::SupportedTemperatureLevels::Id);
    return MT_ATTR_OK;
}

/* ---- robotic vacuum cleaner: the two ModeBase clusters ----------------
 *
 * RvcRunMode (0x0054) and RvcCleanMode (0x0055) are concrete derivations
 * of the abstract ModeBase cluster, one shared Delegate interface and one
 * shared Instance class parameterised by cluster id at CONSTRUCTION (the
 * four-argument ctor, mode-base-server.h:46: delegate, endpoint, cluster,
 * feature). One delegate OBJECT plus one Instance per (endpoint, cluster)
 * PAIR: an RVC endpoint carries both clusters at once, so it consumes two
 * pool slots, unlike every earlier per-endpoint pool in this file.
 *
 * THE TWO WAYS THIS MACHINERY KILLS OR SILENTLY CRIPPLES THE DEVICE, and
 * why the code below is shaped the way it is. Instance::Init()
 * (mode-base-server.cpp:71-84) does, in order:
 *
 *   1. ReturnErrorOnFailure(mDelegate->GetModeValueByIndex(0, mCurrentMode))
 *      (:74). This runs BEFORE anything else, before any AT+MTMODES could
 *      possibly have fed a list. If the delegate answers
 *      PROVIDER_LIST_EXHAUSTED at index 0, Init() returns early and the
 *      Instance is NEVER REGISTERED: no abort, no log from the SDK, reads
 *      and ChangeToMode on that cluster simply go unanswered. That is why
 *      the placeholder-mode-0 policy below is MANDATORY, not a nicety: an
 *      empty store still answers index 0 with mode 0, the cluster's tag-0
 *      default and label "Mode0" (mirrored from the C6, main.cpp:
 *      2429-2470). The port checks Init()'s return and logs loudly
 *      besides, which the C6's SDK init-callback path cannot (it discards
 *      the return outright).
 *
 *   2. VerifyOrDie(emberAfContainsServer(mEndpointId, mClusterId)) (:77).
 *      A PANIC, not the soft CHIP_ERROR_INVALID_ARGUMENT bail the
 *      OperationalState machinery gives the same mistake
 *      (operational-state-server.cpp:63-70). Constructing and Init()ing a
 *      ModeBase Instance before emberAfSetDynamicEndpoint() succeeds
 *      ABORTS THE DEVICE. The construct-and-Init therefore lives only in
 *      mt_matter_modebase_delegate_set_endpoint(), which
 *      mt_devtype_create() calls strictly below a successful
 *      emberAfSetDynamicEndpoint(); nothing else may ever construct one.
 *
 * One-delegate-per-Instance is DISCIPLINE here, not an enforced abort:
 * ModeBase::Delegate::SetInstance() is a plain setter
 * (`void SetInstance(Instance * aInstance) { mInstance = aInstance; }`,
 * mode-base-server.h:256), with NO VerifyOrDie. This contradicts the C6's
 * own comment (main.cpp:2408-2412 claims the identical VerifyOrDie
 * contract as OperationalState, whose SetInstance really does die,
 * operational-state-server.h:349-353); verified against this tree and the
 * C6's citation deliberately NOT copied. A shared delegate would not
 * abort, it would silently answer through the wrong Instance, which is
 * worse; the one-pair-per-slot pool below is what keeps the rule.
 *
 * Init() also LoadPersistentAttributes() (:79): three KVS reads per
 * (endpoint, cluster) at every boot, and UpdateCurrentMode() persists on
 * every controller ChangeToMode (:178). Two ModeBase clusters per RVC
 * endpoint makes this a second ZMS wear source after the chime's, on the
 * settings_storage partition already under a sizing watch. Also live but
 * inert: Init()'s OnOff coupling block compiles here
 * (MATTER_DM_PLUGIN_ON_OFF_SERVER is defined, OnOff is in hearth.zap) and
 * is skipped at :132 because no RVC endpoint carries an OnOff server.
 *
 * The header defines `static IntrusiveList<Instance>
 * gModeBaseAliasesInstances` IN THE HEADER (mode-base-server.h:279), so
 * this translation unit gets a private copy; only the SDK's own
 * GetModeBaseInstanceList() accessor touches the real one, and this file
 * must never name the symbol.
 *
 * The mode store is block-resident (mt_mb_store_t, mt_dyn_store.h),
 * reached through mt_dyn_mb_store(ep, cluster): only RVC endpoints pay,
 * and count 0 is the placeholder state. Unlike mt_mode_store_t there is
 * no struct/CharSpan half: every ModeBase read COPIES into caller-owned
 * memory (GetModeLabelByIndex fills a MutableCharSpan, GetModeTagsByIndex
 * a caller-supplied List, mode-base-server.h:205, :235), so no lifetime
 * argument is needed beyond the lock.
 */
class HearthModeBaseDelegate : public chip::app::Clusters::ModeBase::Delegate
{
public:
    void set_cluster(chip::ClusterId cluster) { m_cluster = cluster; }
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }
    chip::EndpointId endpoint() const { return m_ep; }
    chip::ClusterId cluster() const { return m_cluster; }

    /* GetInstance() is protected in the base Delegate; this passthrough
     * is how mt_matter_modebase_set() clamps CurrentMode and how the
     * DE397 live reader answers it, the HearthOpStateDelegate mechanism. */
    chip::app::Clusters::ModeBase::Instance *instance() { return GetInstance(); }

    CHIP_ERROR Init() override { return CHIP_NO_ERROR; }

    CHIP_ERROR GetModeLabelByIndex(uint8_t modeIndex, chip::MutableCharSpan &label) override
    {
        mt_mb_store_t *slot = mt_dyn_mb_store(m_ep, m_cluster);
        if (slot == nullptr || slot->count == 0) {
            if (modeIndex != 0) {
                return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
            }
            /* The placeholder: index 0 must answer (see the section
             * comment; Instance::Init() reads it before any host list can
             * exist). Superseded the moment AT+MTMODES stores a real
             * list; no separate flag, count > 0 simply wins. */
            return chip::CopyCharSpanToMutableCharSpan(chip::CharSpan::fromCharString("Mode0"),
                                                       label);
        }
        if (modeIndex >= slot->count) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        return chip::CopyCharSpanToMutableCharSpan(
            chip::CharSpan::fromCharString(slot->entries[modeIndex].label), label);
    }

    CHIP_ERROR GetModeValueByIndex(uint8_t modeIndex, uint8_t &value) override
    {
        mt_mb_store_t *slot = mt_dyn_mb_store(m_ep, m_cluster);
        if (slot == nullptr || slot->count == 0) {
            if (modeIndex != 0) {
                return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
            }
            value = 0;
            return CHIP_NO_ERROR;
        }
        if (modeIndex >= slot->count) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        value = slot->entries[modeIndex].mode;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetModeTagsByIndex(
        uint8_t modeIndex,
        chip::app::DataModel::List<chip::app::Clusters::detail::Structs::ModeTagStruct::Type> &tags)
        override
    {
        mt_mb_store_t *slot = mt_dyn_mb_store(m_ep, m_cluster);
        uint16_t tag_value;
        if (slot == nullptr || slot->count == 0) {
            if (modeIndex != 0) {
                return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
            }
            tag_value = placeholder_tag();
        } else {
            if (modeIndex >= slot->count) {
                return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
            }
            tag_value = slot->entries[modeIndex].tag;
        }
        /* The SDK hands in a buffer sized for its own tag maximum; this
         * delegate publishes exactly one tag per mode, so size 1 always
         * fits. Guarded anyway, the C6's defensive shape. */
        if (tags.size() < 1) {
            return CHIP_ERROR_INVALID_ARGUMENT;
        }
        tags[0].value = tag_value;
        tags.reduce_size(1);
        return CHIP_NO_ERROR;
    }

    /*
     * ChangeToMode::Id is 0x00 for both RVC mode clusters (each generated
     * CommandIds.h declares it), so one constant serves both and
     * m_cluster, fixed at alloc time, is what routes the +MTCMD to the
     * right wire cluster. The SDK's own pre-check answers UnsupportedMode
     * before this is ever called (Instance::HandleChangeToMode validates
     * membership first, mode-base-server.cpp:382-421), and a
     * same-as-current mode answers success without calling it either
     * (:406), so only genuine transitions reach the host, and only
     * kSuccess/kGenericFailure are this function's business. The verdict
     * IS the wire response: HandleChangeToMode copies response.status
     * straight out. Runs on the CHIP task (the Instance's own
     * CommandHandlerInterface; both clusters are CHI-only), no StackLock,
     * the standing hook discipline.
     */
    void HandleChangeToMode(
        uint8_t NewMode,
        chip::app::Clusters::ModeBase::Commands::ChangeToModeResponse::Type &response) override
    {
        bool allow = mt_cmd_forward_payload(
            m_ep, m_cluster, chip::app::Clusters::RvcRunMode::Commands::ChangeToMode::Id, NewMode);
        response.status =
            chip::to_underlying(allow ? chip::app::Clusters::ModeBase::StatusCode::kSuccess
                                      : chip::app::Clusters::ModeBase::StatusCode::kGenericFailure);
    }

private:
    /* Tag-0 defaults, placeholder case only (index 0 of an unfed store):
     * RvcRunMode kIdle (0x4000), RvcCleanMode kVacuum (0x4001), the
     * AT_MT_SPEC.md 3.20 table's exact policy, read from the enums rather
     * than transcribed. mt_matter_modebase_set() applies the same table
     * to real host entries at store time. */
    uint16_t placeholder_tag() const
    {
        using namespace chip::app::Clusters;
        if (m_cluster == RvcRunMode::Id) {
            return chip::to_underlying(RvcRunMode::ModeTag::kIdle);
        }
        if (m_cluster == DeviceEnergyManagementMode::Id) {
            /* Batch 7a: kNoOptimization on every mode, the AT_MT_SPEC.md
             * 3.20 table row (no mandatory tag in the cluster XML; a mode
             * the host has not described makes no optimization
             * promise). */
            return chip::to_underlying(DeviceEnergyManagementMode::ModeTag::kNoOptimization);
        }
        if (m_cluster == WaterHeaterMode::Id) {
            /* Batch 7b: kManual (0x4001) on every mode, the AT_MT_SPEC.md
             * 3.20 table row (Mode_WaterHeater.xml names no mandatory tag
             * either; Manual is the everyday operating mode a host that
             * does not care about tags most plausibly means, the OvenMode
             * reasoning the spec row records). */
            return chip::to_underlying(WaterHeaterMode::ModeTag::kManual);
        }
        if (m_cluster == MicrowaveOvenMode::Id) {
            /* Batch 8: kNormal (0x4000) on every mode, the AT_MT_SPEC.md
             * 3.20 table row, and THE ONE ARM IN THIS FUNCTION THAT IS
             * LOAD-BEARING BEYOND ITS OWN CLUSTER. MicrowaveOvenControl's
             * SetCookingParameters resolves an omitted cookMode through
             * GetModeValueByModeTag(kNormal), so this value is what makes a
             * freshly composed microwave answer cooking commands before the
             * host has ever sent AT+MTMODES. Get it wrong and every
             * SetCookingParameters answers InvalidCommand with no +MTCMD
             * raised, forever and silently. mt_matter_mwoc_register() reads
             * it back at create time precisely so that cannot happen
             * unnoticed. */
            return chip::to_underlying(MicrowaveOvenMode::ModeTag::kNormal);
        }
        if (m_cluster == OvenMode::Id) {
            /* Batch 8: kBake (0x4000) on every mode, the AT_MT_SPEC.md 3.20
             * table row. Mode_Oven.xml mandates no tag, and Bake is the
             * everyday oven mode a host that does not care about tags most
             * plausibly means, the WaterHeaterMode kManual reasoning. */
            return chip::to_underlying(OvenMode::ModeTag::kBake);
        }
        if (m_cluster == RefrigeratorAndTemperatureControlledCabinetMode::Id) {
            /* Batch 8: kAuto on every mode, the AT_MT_SPEC.md 3.20 table
             * row. Worth pointing at, because it is the one arm here whose
             * substituted tag VALUE is itself 0: kAuto is a ModeBase COMMON
             * tag (0x0000), not a cluster-specific one in the 0x4000 range
             * like every other arm in this function. The substitution is
             * still real and still the right answer; it just happens to be
             * a no-op on the wire, and a reader who assumes every default
             * tag is 0x4000-something will misread this line. */
            return chip::to_underlying(
                RefrigeratorAndTemperatureControlledCabinetMode::ModeTag::kAuto);
        }
        return chip::to_underlying(RvcCleanMode::ModeTag::kVacuum);
    }

    chip::EndpointId m_ep = chip::kInvalidEndpointId;
    chip::ClusterId m_cluster = chip::kInvalidClusterId;
};

/*
 * Pool sizing, RULED (graph DE404): 18 slots, which is 2 x 9, two
 * ModeBase pairs per RVC endpoint times the NINE RVC endpoints the
 * endpoint block heap can actually serve (8,112 usable / 864 B per RVC
 * block; the arithmetic beside K_HEAP_DEFINE in mt_devtypes_zephyr.cpp).
 * Not kServiceableEndpoints like every other pool here, because this is
 * the port's first two-objects-per-endpoint pool and 16 would cap the
 * composition at eight RVCs, one below what the heap allows, while 32
 * (2 x 16) would reserve Instances for seven endpoints the heap can never
 * stand up. Exhaustion aborts the create before anything is spent, so a
 * tenth RVC fails on whichever wall it hits first (this pool or the
 * heap), loudly either way. MT_MB_MAX_LISTS (16, core) bounds the C6's
 * flat store, not this pool: on this platform the stores are
 * block-resident per endpoint and the core constant only shapes the
 * entry layout (the mt_matter.h staleness note batch 4 already logged).
 *
 * The Instance pool is alignas raw storage (the four-argument ctor needs
 * create-time values), placement-constructed exactly once per slot in
 * mt_matter_modebase_delegate_set_endpoint() and never destroyed:
 * ~Instance() would Shutdown() and unregister cleanly
 * (mode-base-server.cpp:55-69), but this platform has no teardown path
 * (AT+MTEP edits apply by reboot, which resets every pool wholesale), the
 * standing allocate-only policy.
 */
/* Catalogue batch 7a: 18 -> 20, the batch brief's ruling, to admit
 * DEM/DEMMode-bearing endpoints beyond the RVC arithmetic. The arena
 * arithmetic itself says 18 still suffices with MT_DEM_MAX at 4: the
 * heaviest feasible mix is 7 RVCs (14 slots, 6,048 B of block heap) plus
 * 4 DEM endpoints (4 slots, 1,856 B) = 18 slots in 7,904 of 8,112 usable
 * B, and every other mix is at or under 18 before a heap or MT_DEM_MAX
 * wall lands; the two extra slots are the brief's deliberate headroom for
 * 160 B of .bss.
 *
 * Catalogue batch 7b redid the arithmetic with WaterHeaterMode as the
 * fourth consumer, as 7a's note promised, and 20 STANDS: the ceiling is
 * set by heap cost per ModeBase slot, and the RVC's 432 B/slot (864 B
 * block, two slots) beats every 7b provider (DEM v1 456 B/slot, water
 * heater v1 632, water heater v0 808, battery storage v0 856), so the
 * slot-maximizing mixes are still RVC-led: 9 RVCs = 18 slots in 7,776 B,
 * or 8 RVCs + 2 DEM v1 = 18 in 7,824 B, or 7 RVCs + 4 DEM v1 = 18 in
 * 7,872 B; folding a water heater in always costs more heap per slot and
 * lands at or under 18 before the heap or an MT_*_MAX wall does
 * (MT_WHM_MAX caps water heaters at 4, MT_DEM_MAX caps DEM-bearing
 * endpoints, battery storage included, at 4). No growth; the 7a headroom
 * still covers exactly the two slots it always did. EnergyEvseMode would
 * be the fifth consumer and is out by ruling DE408 (LM20 tier).
 *
 * Catalogue batch 8 added THREE more consumers at once (0x0052 on the
 * refrigerator and the cooler cabinet, OvenMode on the heater cabinet,
 * MicrowaveOvenMode on the microwave) and 20 STILL STANDS, for the reason
 * that has decided this number every time: the ceiling is set by endpoint
 * block-heap cost per ModeBase slot, and the RVC's 432 B/slot (864 B block,
 * two slots on one endpoint) still beats every batch-8 provider by a wide
 * margin: cooler cabinet v0 472 B/slot, refrigerator 520, microwave 536,
 * heater cabinet v0 536, every variant-1 shape worse again. The
 * slot-maximising mixes are therefore still RVC-led and still top out at 18
 * (9 RVCs, 7,776 B of block heap; 8 RVCs plus 2 cooler cabinets, 18 slots in
 * 7,856 B; 8 RVCs plus 2 DEM v1, 18 in 7,824), with nothing MB-bearing cheap
 * enough to fit any of their remainders. Re-derived by exhaustive search
 * over the full batch-8 catalogue rather than argued by inspection, the
 * batch's own worst-object-mix discipline. The 7a headroom is still exactly
 * the two slots it always was. */
constexpr size_t kModeBasePoolSlots = 20;

/* Memory reclaim round A: a table of pointers into the cluster-object
 * heap, each block holding the delegate with the raw storage for its
 * ModeBase Instance behind it. kModeBasePoolSlots is unchanged and is
 * still checked first, so the acceptance cap DE404 argued for is exactly
 * what it was. */
static HearthModeBaseDelegate *s_mb_delegates[kModeBasePoolSlots];
static size_t s_mb_delegate_next;

extern "C" void *mt_matter_modebase_delegate_alloc(uint32_t cluster_id)
{
    if (s_mb_delegate_next >= kModeBasePoolSlots) {
        return nullptr;
    }
    HearthModeBaseDelegate *d =
        obj_pair_new<HearthModeBaseDelegate, chip::app::Clusters::ModeBase::Instance>();
    if (d == nullptr) {
        return nullptr;
    }
    d->set_cluster(cluster_id);
    s_mb_delegates[s_mb_delegate_next++] = d;
    return d;
}

/*
 * The second half, and where the ModeBase Instance is BORN. Everything
 * about the placement of this call is the section comment's two-failure
 * story: it may only run below a successful emberAfSetDynamicEndpoint()
 * (Init()'s VerifyOrDie on emberAfContainsServer PANICS otherwise, unlike
 * every soft-bailing Init this file already hosts), and the endpoint's
 * block-resident store must already be constructed (Init()'s first act
 * reads the delegate's index 0, which the placeholder policy answers for
 * an unfed store). The Instance is constructed with feature 0:
 * DirectModeChange, the aliases' only feature, is optional and not taken,
 * matching the FeatureMap 0 arena seed so the AAI's FeatureMap answer and
 * the inert slot agree.
 *
 * Init()'s return is CHECKED and a failure logged loudly, the shape the
 * opstate handout set: the C6's SDK init path discards it, and the
 * silent-deregistration failure mode (placeholder missing, index-0 read
 * fails, Instance never registers, no diagnostic anywhere) is exactly the
 * kind of quiet cripple this port refuses to ship without a shout. It
 * does not abort: below a successful create the contains-server check
 * cannot fail, the placeholder answers index 0 by construction, and the
 * registry Register() calls fail only on a duplicate registration the
 * construct-once pool rules out; if it ever fired anyway, the endpoint is
 * live and correct in every other respect, the standing lesser-evil
 * argument.
 */
/* Split for the microwave's ordering function, the opstate helper's reason
 * exactly (mt_matter.h's setter is void and read-only). */
static CHIP_ERROR modebase_construct_and_init(void *delegate, uint16_t ep)
{
    auto *d = static_cast<HearthModeBaseDelegate *>(delegate);
    d->set_endpoint(ep);
    auto *inst =
        new (obj_inst_storage<HearthModeBaseDelegate, chip::app::Clusters::ModeBase::Instance>(d))
            chip::app::Clusters::ModeBase::Instance(d, ep, d->cluster(), 0);
    CHIP_ERROR err = inst->Init();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("ModeBase Instance::Init failed for endpoint %u cluster 0x%08X: "
                "%" CHIP_ERROR_FORMAT
                "; the cluster is NOT registered and will answer nothing on the fabric",
                (unsigned)ep, (unsigned)d->cluster(), err.Format());
    }
    return err;
}

extern "C" void mt_matter_modebase_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    (void)modebase_construct_and_init(delegate, ep);
}

/*
 * AT+MTMODES's cluster-aware form (AT_MT_SPEC.md 3.20). Grammar and
 * content rules (count bounds, mode uniqueness, tag range, label content)
 * are enforced by cmd_mtmodes() in mt_at.c; the bounds re-checked here
 * are defensive. This bridge is the sole place that validates cluster
 * against the ModeBase ids this build serves, since mt_at.c has no CHIP
 * header to read them from: on this platform that is RvcRunMode and
 * RvcCleanMode only, and the other five ModeBase aliases the C6 already
 * accepts answer MT_ATTR_ERR_CLUSTER here until their device types land
 * in later batches (the composed-appliance and energy rounds).
 *
 * Tag-0 defaults are substituted at STORE time so every read is
 * branch-free, the C6's policy verbatim (main.cpp:2762-2766): RvcRunMode's
 * FIRST declared mode gets kIdle and every later one kCleaning (which is
 * what keeps a host that always writes tag 0 conformant, since a
 * RvcRunMode list must contain an Idle-tagged mode), RvcCleanMode gets
 * kVacuum on every mode. A nonzero tag passes through unvalidated beyond
 * the u16 range check mt_at.c performed; tag semantics are the host's.
 *
 * All labels are validated before any entry is overwritten (the fix-round
 * M5 shape). Strictly optional here, unlike the ModeSelect twin: ModeBase
 * reads copy per access and cache no spans, so a half-written list could
 * never publish stale bytes; kept anyway so the two bridges reason
 * identically.
 *
 * Ends with the SupportedModes dirty-mark and the CurrentMode CLAMP: if
 * the replacement list dropped the mode the Instance currently holds,
 * UpdateCurrentMode(first entry) through the SDK's own setter, which
 * persists and reports the change itself when it actually changes. A
 * clamp reaches the fabric as that report only, never as a +MTATTR URC:
 * CurrentMode's ember slot is an inert shadow (the k_instance_served
 * split), and the host that just replaced the list can read the clamped
 * value back live over AT+MTATTR.
 */
extern "C" int mt_matter_modebase_set(uint16_t ep, uint32_t cluster, const uint8_t *modes,
                                      const uint16_t *tags, const char *const *labels,
                                      uint8_t count)
{
    using namespace chip::app::Clusters;
    chip::DeviceLayer::StackLock lock;
    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (cluster != RvcRunMode::Id && cluster != RvcCleanMode::Id &&
        cluster != DeviceEnergyManagementMode::Id && cluster != WaterHeaterMode::Id &&
        cluster != RefrigeratorAndTemperatureControlledCabinetMode::Id &&
        cluster != OvenMode::Id && cluster != MicrowaveOvenMode::Id) {
        /* Batch 7a widened the accept set to three of the C6's seven
         * ModeBase ids, batch 7b to four (WaterHeaterMode; EnergyEvseMode
         * stays out with its device type, ruling DE408 LM20-tier), batch 8
         * to five with the refrigerator and cooler cabinet's 0x0052. OvenMode
         * and MicrowaveOvenMode join it later in the same batch, with the
         * device types that carry them. */
        return MT_ATTR_ERR_CLUSTER;
    }
    if (!emberAfContainsServer(ep, cluster)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (count < 1 || count > MT_MB_MAX_COUNT) {
        return MT_ATTR_ERR_FAILED;
    }

    mt_mb_store_t *slot = mt_dyn_mb_store(ep, cluster);
    if (!slot) {
        /* Cannot happen: the lookups above proved ep live with this
         * cluster, and every such endpoint's block carries both stores
         * from create. Defensive. */
        return MT_ATTR_ERR_FAILED;
    }

    for (uint8_t i = 0; i < count; i++) {
        size_t len = strlen(labels[i]);
        if (len < 1 || len > MT_MB_MAX_LABEL_LEN) {
            return MT_ATTR_ERR_FAILED;
        }
    }
    for (uint8_t i = 0; i < count; i++) {
        uint16_t tag = tags[i];
        if (tag == 0) {
            if (cluster == RvcRunMode::Id) {
                tag = chip::to_underlying(i == 0 ? RvcRunMode::ModeTag::kIdle
                                                 : RvcRunMode::ModeTag::kCleaning);
            } else if (cluster == DeviceEnergyManagementMode::Id) {
                /* Batch 7a: kNoOptimization on every mode, first or not
                 * (the placeholder_tag() arm's reasoning). */
                tag = chip::to_underlying(DeviceEnergyManagementMode::ModeTag::kNoOptimization);
            } else if (cluster == WaterHeaterMode::Id) {
                /* Batch 7b: kManual on every mode, first or not (the
                 * placeholder_tag() arm's reasoning; the AT_MT_SPEC.md
                 * 3.20 table row). */
                tag = chip::to_underlying(WaterHeaterMode::ModeTag::kManual);
            } else if (cluster == MicrowaveOvenMode::Id) {
                /* Batch 8: kNormal on every mode, first or not. A host that
                 * writes tag 0 for every mode still gets a list in which
                 * SetCookingParameters can resolve a default cookMode,
                 * which is the placeholder arm's reason applied to real
                 * entries. */
                tag = chip::to_underlying(MicrowaveOvenMode::ModeTag::kNormal);
            } else if (cluster == OvenMode::Id) {
                /* Batch 8: kBake on every mode, first or not (the
                 * placeholder_tag() arm's reasoning). */
                tag = chip::to_underlying(OvenMode::ModeTag::kBake);
            } else if (cluster == RefrigeratorAndTemperatureControlledCabinetMode::Id) {
                /* Batch 8: kAuto on every mode, first or not. The one
                 * substitution in this loop whose result is itself 0 (a
                 * ModeBase COMMON tag, not a 0x4000-range cluster-specific
                 * one), so it is a no-op on the wire; see placeholder_tag()'s
                 * matching arm. */
                tag = chip::to_underlying(
                    RefrigeratorAndTemperatureControlledCabinetMode::ModeTag::kAuto);
            } else {
                tag = chip::to_underlying(RvcCleanMode::ModeTag::kVacuum);
            }
        }
        slot->entries[i].mode = modes[i];
        slot->entries[i].tag = tag;
        memcpy(slot->entries[i].label, labels[i], strlen(labels[i]) + 1);
    }
    slot->count = count;

    /* One Instance lookup serves both tail steps (batch 5 fix round,
     * review Minor 3): the dirty-mark goes through the SDK's own
     * Instance::ReportSupportedModesChange(), the method the delegate
     * contract says the device SHALL call (mode-base-server.h:109, the
     * SHALL at :203/:215/:233; its body is exactly the raw
     * MatterReportingAttributeChangeCallback this bridge used to make,
     * mode-base-server.cpp:241-244, so the wire is unchanged and a future
     * SDK revision that adds bookkeeping there is picked up for free).
     * The raw callback survives only in the defensive no-Instance arm, so
     * a host feed still reaches subscriptions even in the
     * cannot-happen-once-rebuilt state. */
    ModeBase::Instance *inst = nullptr;
    for (size_t i = 0; i < s_mb_delegate_next; i++) {
        HearthModeBaseDelegate *d = s_mb_delegates[i];
        if (d->endpoint() == ep && d->cluster() == cluster) {
            inst = d->instance();
            break;
        }
    }
    if (inst == nullptr) {
        MatterReportingAttributeChangeCallback(ep, cluster,
                                               ModeBase::Attributes::SupportedModes::Id);
        return MT_ATTR_OK;
    }
    inst->ReportSupportedModesChange();

    if (!inst->IsSupportedMode(inst->GetCurrentMode())) {
        /* The clamp target is entries[0].mode, stored two loops above, so
         * UpdateCurrentMode()'s ConstraintError arm (an unsupported mode)
         * is unreachable here; checked and logged anyway rather than
         * discarded, this file's convention for SDK returns (batch 5 fix
         * round, review Minor 4). */
        Status st = inst->UpdateCurrentMode(slot->entries[0].mode);
        if (st != Status::Success) {
            LOG_ERR("modebase CurrentMode clamp to %u failed on endpoint %u cluster 0x%08X "
                    "(status 0x%02X)",
                    (unsigned)slot->entries[0].mode, (unsigned)ep, (unsigned)cluster,
                    (unsigned)chip::to_underlying(st));
        }
    }
    return MT_ATTR_OK;
}

/*
 * DE397 live read for the two ModeBase CurrentModes, dispatched from
 * mt_matter_attr_read() under its StackLock. CurrentMode is unsigned,
 * never nullable; the Instance's member is the served truth (KVS-restored
 * at Init, updated by ChangeToMode and the clamp above), the inert arena
 * seed is not.
 */
static int mt_mb_attr_read_live(uint16_t ep, uint32_t cluster, int64_t *out, bool *is_unsigned)
{
    if (is_unsigned) {
        *is_unsigned = true;
    }
    for (size_t i = 0; i < s_mb_delegate_next; i++) {
        HearthModeBaseDelegate *d = s_mb_delegates[i];
        if (d->endpoint() == ep && d->cluster() == cluster) {
            chip::app::Clusters::ModeBase::Instance *inst = d->instance();
            if (inst == nullptr) {
                break;
            }
            *out = inst->GetCurrentMode();
            return MT_ATTR_OK;
        }
    }
    /* Cannot happen once the boot rebuild has run; defensive. */
    return MT_ATTR_ERR_FAILED;
}

/* ---- microwave oven: MicrowaveOvenControl (0x005F) ---------------------
 *
 * THE SHARPEST HAZARD IN THE COMPOSED-APPLIANCE BATCH, and it deserves the
 * space. Every per-endpoint object this port has built so far is
 * INDEPENDENT: claim a pool slot before emberAfSetDynamicEndpoint(),
 * placement-construct and Init() after it, in any order among themselves.
 * The microwave breaks that. MicrowaveOvenControl::Instance's constructor is
 *
 *   Instance(Delegate *, EndpointId, ClusterId, BitMask<Feature>,
 *            OperationalState::Instance &, ModeBase::Instance &)
 *
 * (microwave-oven-control-server.h:58-59) and those last two are C++
 * REFERENCES to two OTHER clusters' live Instances. So the OperationalState
 * Instance and the MicrowaveOvenMode Instance must both exist before this
 * one can be constructed at all, and its handlers dereference both on the
 * very first invoke (microwave-oven-control-server.cpp:250, :277-281).
 *
 * WHAT MAKES IT DANGEROUS RATHER THAN MERELY FIDDLY is that every way of
 * getting it wrong is QUIET. A reference bound to storage that has not been
 * constructed yet compiles, links, boots, commissions and answers reads; the
 * failure surfaces only when a controller invokes SetCookingParameters, and
 * it surfaces as InvalidInState or InvalidCommand, statuses the server
 * legitimately produces for perfectly ordinary reasons. There is no
 * VerifyOrDie to catch it, no log, and no +MTCMD for a host to notice
 * missing.
 *
 * THE THREE MANDATORY MITIGATIONS, all of them in this one function:
 *
 *   1. ONE function owns the order, and the order is fixed and commented.
 *      The create path calls this instead of the three independent-looking
 *      setters, and its else-branch is what stops the generic opstate setter
 *      from constructing the same Instance twice.
 *   2. ALL THREE Init() returns are checked and logged, not just this
 *      cluster's. The two neighbours' setters were split into
 *      construct-and-init helpers for exactly this, because mt_matter.h's
 *      public setters return void and that header is read-only.
 *   3. A GetModeValueByModeTag(kNormal) READBACK after construction, logged
 *      loudly on failure. That call is the difference between a working
 *      microwave and one that refuses every cooking command in silence
 *      forever: SetCookingParameters resolves its default cookMode through
 *      it (microwave-oven-control-server.cpp:277-281), so if the ModeBase
 *      placeholder tag is not kNormal the command answers InvalidCommand
 *      with nothing raised to the host. It succeeds against the unfed
 *      placeholder by design (placeholder_tag()'s MicrowaveOvenMode arm),
 *      which is what makes a freshly composed microwave usable before the
 *      host has ever sent AT+MTMODES; the readback is here so that the day
 *      that arm is edited wrongly, the bench sees it at boot.
 *
 * A failed neighbour Init() does NOT abort the MWOC construction. Both
 * Instances still exist as constructed objects after a failed Init (Init
 * only registers interfaces), so the references are sound and a microwave
 * with an unregistered opstate cluster is still better than one with no
 * cooking control at all; the loud summary line below is what a bench sees.
 * A NULL delegate is different and does abort: there is no object to bind a
 * reference to, and binding one would be undefined rather than degraded.
 *
 * WHAT THE SERVER GUARANTEES BEFORE THE HOST EVER SEES A COMMAND, which is
 * why the forwards below carry only legitimate adjudication requests:
 * SetCookingParameters requires the operational state to be kStopped or
 * answers InvalidInState (:250); if startAfterSetting is present it asks the
 * data model provider whether OperationalState's Start is accepted on this
 * endpoint and answers InvalidCommand if not (:252-273, which is why
 * kOpStateIncoming keeps Start on the microwave's list); cookTime is
 * range-checked against GetMaxCookTimeSec(); powerSetting is range- and
 * step-checked. AddMoreTime requires the state not be kError and range-checks
 * the sum.
 *
 * CookTime and PowerSetting ownership: CookTime lives in the Instance
 * (SetCookTimeSec(), which this delegate calls on an allowed command, the
 * SDK's own reference implementation's pattern), PowerSetting lives in this
 * delegate object because the Delegate interface has no setter for it at
 * all, only GetPowerSettingNum(). Both are applied only on ALLOW: a denied
 * command is a refusal to accept the new cooking parameters, so nothing is
 * written for the host to have refused.
 *
 * The verdict IS the wire response, the chime's shape rather than the
 * OperationalState family's GenericOperationalError indirection:
 * HandleSetCookingParameters() copies whatever Status these two methods
 * return straight into the InvokeResponse through AddStatus(), no remapping,
 * so allow is Status::Success and deny is Status::Failure (AT_MT_SPEC.md
 * 1391-1409). Both hooks run on the CHIP task (this cluster is CHI-only), so
 * no StackLock, the standing hook discipline.
 */
class HearthMwocDelegate : public chip::app::Clusters::MicrowaveOvenControl::Delegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }
    chip::EndpointId endpoint() const { return m_ep; }

    /* GetInstance() is protected in the base Delegate and const-qualified
     * here, unlike the OperationalState and ModeBase passthroughs: the base
     * only offers `const Instance *GetInstance() const`
     * (microwave-oven-control-server.h:195). SetCookTimeSec() is non-const,
     * so the cast is needed; it is safe because the object it names is this
     * delegate's own non-const Instance, constructed below. */
    chip::app::Clusters::MicrowaveOvenControl::Instance *instance()
    {
        return const_cast<chip::app::Clusters::MicrowaveOvenControl::Instance *>(GetInstance());
    }

    /*
     * SetCookingParameters: the four-field forward,
     * cookMode,cookTime,power,startAfter, in that fixed order (AT_MT_SPEC.md
     * 1391-1400 and 3.17's field-arity rule). THREE of the four arrive
     * already resolved rather than Optional, and that is traced against the
     * server rather than assumed from the Delegate interface's own doc
     * comments: cookMode defaults to whichever mode carries the kNormal tag,
     * cookTime to 30 s and startAfterSetting to false, all before this
     * callback runs, which is why the signature declares them as plain
     * values. Only powerSettingNum is still Optional at this boundary, and
     * on a PowerAsNumber-only build it always has a value too (the server
     * defaults it to MaxPower, 100). So none of the four fields is ever
     * empty on this firmware's actual traffic; the empty-field wire
     * convention still applies verbatim, it simply never triggers here.
     * wattSettingIndex is not forwarded at all: it is always NullOptional on
     * this build and carries the host nothing.
     */
    chip::Protocols::InteractionModel::Status HandleSetCookingParametersCallback(
        uint8_t cookMode, uint32_t cookTimeSec, bool startAfterSetting,
        chip::Optional<uint8_t> powerSettingNum, chip::Optional<uint8_t> wattSettingIndex) override
    {
        using chip::Protocols::InteractionModel::Status;
        (void)wattSettingIndex;
        char fields[40];
        int n = 0;
        n += snprintf(fields + n, sizeof(fields) - n, "%u,", (unsigned)cookMode);
        n += snprintf(fields + n, sizeof(fields) - n, "%lu,", (unsigned long)cookTimeSec);
        n += powerSettingNum.HasValue()
                 ? snprintf(fields + n, sizeof(fields) - n, "%u,",
                            (unsigned)powerSettingNum.Value())
                 : snprintf(fields + n, sizeof(fields) - n, ",");
        n += snprintf(fields + n, sizeof(fields) - n, "%u", startAfterSetting ? 1u : 0u);
        (void)n;
        bool allow = mt_cmd_forward_fields(
            m_ep, chip::app::Clusters::MicrowaveOvenControl::Id,
            chip::app::Clusters::MicrowaveOvenControl::Commands::SetCookingParameters::Id, fields);
        if (allow) {
            if (instance() != nullptr) {
                instance()->SetCookTimeSec(cookTimeSec);
            }
            if (powerSettingNum.HasValue()) {
                m_power_setting = powerSettingNum.Value();
            }
        }
        return allow ? Status::Success : Status::Failure;
    }

    /* AddMoreTime: the single-field forward, finalCookTimeSec. Same
     * allow-applies, deny-refuses and raw-passthrough-verdict shape. */
    chip::Protocols::InteractionModel::Status
    HandleModifyCookTimeSecondsCallback(uint32_t finalCookTimeSec) override
    {
        using chip::Protocols::InteractionModel::Status;
        char fields[16];
        snprintf(fields, sizeof(fields), "%lu", (unsigned long)finalCookTimeSec);
        bool allow = mt_cmd_forward_fields(
            m_ep, chip::app::Clusters::MicrowaveOvenControl::Id,
            chip::app::Clusters::MicrowaveOvenControl::Commands::AddMoreTime::Id, fields);
        if (allow && instance() != nullptr) {
            instance()->SetCookTimeSec(finalCookTimeSec);
        }
        return allow ? Status::Success : Status::Failure;
    }

    /* PowerAsNumber-only. GetWattSettingByIndex() answering NOT_FOUND is not
     * a stub: Init() only counts watt levels when PowerInWatts is set, so it
     * is never called on this build, and the same is true of
     * GetCurrentWattIndex() and GetWattRating(). The three power-limit
     * getters ARE dead for a different and more interesting reason: the
     * server only consults them when PowerNumberLimits is set, which
     * conformance forbids without PowerAsNumber and esp-matter's own helper
     * inverts, so MinPower/MaxPower/PowerStep always read the SDK's
     * compiled-in 10/100/10 on both platforms. They are implemented anyway
     * because the pure-virtual contract requires it, and they return the
     * same constants the SDK would, so a future PowerNumberLimits build
     * would not change behaviour by accident. */
    CHIP_ERROR GetWattSettingByIndex(uint8_t index, uint16_t &wattSetting) override
    {
        (void)index;
        (void)wattSetting;
        return CHIP_ERROR_NOT_FOUND;
    }

    uint32_t GetMaxCookTimeSec() const override { return 86400; }
    uint8_t GetPowerSettingNum() const override { return m_power_setting; }
    uint8_t GetMinPowerNum() const override
    {
        return chip::app::Clusters::MicrowaveOvenControl::kDefaultMinPowerNum;
    }
    uint8_t GetMaxPowerNum() const override
    {
        return chip::app::Clusters::MicrowaveOvenControl::kDefaultMaxPowerNum;
    }
    uint8_t GetPowerStepNum() const override
    {
        return chip::app::Clusters::MicrowaveOvenControl::kDefaultPowerStepNum;
    }
    uint8_t GetCurrentWattIndex() const override { return 0; }
    uint16_t GetWattRating() const override { return 0; }

private:
    chip::EndpointId m_ep = chip::kInvalidEndpointId;
    /* kDefaultMaxPowerNum, the value the server itself defaults an omitted
     * powerSetting to and the value mwocAttrs seeds its shadow with. */
    uint8_t m_power_setting = chip::app::Clusters::MicrowaveOvenControl::kDefaultMaxPowerNum;
};

/* One slot per serviceable endpoint, the opstate and chime pools' depth: one
 * MicrowaveOvenControl cluster per microwave endpoint, so a seventeenth
 * endpoint of any type fails its create before it could ask. Memory reclaim
 * round A's shape, a table of pointers into the cluster-object heap with the
 * Instance's raw storage behind each delegate in the same block.
 *
 * The Delegate's own SetInstance() carries the OperationalState shape, not
 * the ModeBase one: VerifyOrDie(mInstance == nullptr || aInstance == nullptr
 * || mInstance == aInstance) (microwave-oven-control-server.h:183-186), an
 * ABORT on sharing. One delegate per Instance is enforced by the SDK here,
 * not merely by this pool's discipline. */
static HearthMwocDelegate *s_mwoc_delegates[kServiceableEndpoints];
static size_t s_mwoc_delegate_next;

extern "C" void *mt_matter_mwoc_delegate_alloc(void)
{
    if (s_mwoc_delegate_next >= kServiceableEndpoints) {
        return nullptr;
    }
    HearthMwocDelegate *d =
        obj_pair_new<HearthMwocDelegate, chip::app::Clusters::MicrowaveOvenControl::Instance>();
    if (d == nullptr) {
        return nullptr;
    }
    s_mwoc_delegates[s_mwoc_delegate_next++] = d;
    return d;
}

/* The header's second half, and on this platform it stamps the endpoint and
 * nothing else: the Instance cannot be born here, because this signature has
 * no way to reach the other two Instances its constructor needs. It is
 * called by mt_matter_mwoc_register() below as the first step of the ordered
 * sequence, which is the only caller. */
extern "C" void mt_matter_mwoc_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    static_cast<HearthMwocDelegate *>(delegate)->set_endpoint(ep);
}

/*
 * THE ORDERED CONSTRUCTION. Read the section comment above before changing a
 * line of this; the order below is a correctness requirement that nothing at
 * runtime enforces and every violation of which is silent.
 */
extern "C" void mt_matter_mwoc_register(void *mwoc_delegate, void *opstate_delegate,
                                        void *mode_delegate, uint16_t ep)
{
    namespace Mwoc    = chip::app::Clusters::MicrowaveOvenControl;
    namespace OpState = chip::app::Clusters::OperationalState;
    namespace MwoMode = chip::app::Clusters::MicrowaveOvenMode;

    /* A null delegate here is unrecoverable rather than degrading: there
     * would be no object for the Instance constructor to bind a reference
     * to. Cannot happen (all three claims aborted the create before anything
     * was spent), so this is the drift alarm for a future edit that adds a
     * claim without adding it to this call. */
    if (mwoc_delegate == nullptr || opstate_delegate == nullptr || mode_delegate == nullptr) {
        LOG_ERR("microwave endpoint %u: a delegate is missing (mwoc %p, opstate %p, mode %p); "
                "no MicrowaveOvenControl cluster will be registered and every cooking command "
                "will fail",
                (unsigned)ep, mwoc_delegate, opstate_delegate, mode_delegate);
        return;
    }

    /* STEP 1 and STEP 2, in this order and before anything else: the two
     * Instances the MWOC constructor takes references to. Both are
     * placement-constructed and Init()ed by these helpers, both returns are
     * kept. */
    CHIP_ERROR ops_err  = opstate_construct_and_init(opstate_delegate, ep);
    CHIP_ERROR mode_err = modebase_construct_and_init(mode_delegate, ep);

    OpState::Instance *ops_inst =
        static_cast<HearthOpStateDelegate *>(opstate_delegate)->instance();
    chip::app::Clusters::ModeBase::Instance *mode_inst =
        static_cast<HearthModeBaseDelegate *>(mode_delegate)->instance();
    if (ops_inst == nullptr || mode_inst == nullptr) {
        /* Each Instance's constructor calls SetInstance(this) on its own
         * delegate, so a null here means a constructor did not run, which
         * the two steps above just did. Unreachable, and checked because
         * binding a reference to it would be undefined rather than merely
         * broken. */
        LOG_ERR("microwave endpoint %u: neighbour Instance missing after construction "
                "(opstate %p, mode %p); MicrowaveOvenControl will NOT be registered",
                (unsigned)ep, (void *)ops_inst, (void *)mode_inst);
        return;
    }

    /* STEP 3: only now can the MWOC Instance be built. The feature mask is
     * kPowerAsNumber and is mandatory rather than chosen: Init() below
     * refuses anything else, and it must agree with the FeatureMap seeded on
     * mwocAttrs, which it does by both being derived from the same
     * conformance fact. */
    mt_matter_mwoc_delegate_set_endpoint(mwoc_delegate, ep);
    auto *d = static_cast<HearthMwocDelegate *>(mwoc_delegate);
    auto *mwoc_inst = new (obj_inst_storage<HearthMwocDelegate, Mwoc::Instance>(d))
        Mwoc::Instance(d, ep, Mwoc::Id, chip::BitMask<Mwoc::Feature>(Mwoc::Feature::kPowerAsNumber),
                       *ops_inst, *mode_inst);
    CHIP_ERROR mwoc_err = mwoc_inst->Init();

    /* All three returns, in one place. Each helper has already logged its
     * own failure in its own words; this line exists so a bench sees the
     * ENDPOINT named as degraded rather than three unrelated cluster
     * complaints, and so a failure of this Init() (which nothing else logs)
     * is never silent. */
    if (mwoc_err != CHIP_NO_ERROR) {
        LOG_ERR("microwave endpoint %u: MicrowaveOvenControl Instance::Init failed: "
                "%" CHIP_ERROR_FORMAT
                "; SetCookingParameters and AddMoreTime will not reach the host",
                (unsigned)ep, mwoc_err.Format());
    }
    if (ops_err != CHIP_NO_ERROR || mode_err != CHIP_NO_ERROR || mwoc_err != CHIP_NO_ERROR) {
        LOG_ERR("microwave endpoint %u is DEGRADED: opstate Init %" CHIP_ERROR_FORMAT
                ", mode Init %" CHIP_ERROR_FORMAT ", control Init %" CHIP_ERROR_FORMAT,
                (unsigned)ep, ops_err.Format(), mode_err.Format(), mwoc_err.Format());
    }

    /* THE READBACK. GetModeValueByModeTag(kNormal) is what
     * SetCookingParameters uses to resolve an omitted cookMode, and a
     * failure of it makes every cooking command answer InvalidCommand
     * forever with nothing raised to the host. It succeeds here against the
     * unfed ModeBase placeholder because placeholder_tag()'s
     * MicrowaveOvenMode arm answers kNormal, which is the whole reason a
     * freshly composed microwave works before the host has sent
     * AT+MTMODES. Read back rather than trusted, because that arm is one
     * edit away from being wrong and nothing else would notice. */
    uint8_t normal_mode = 0;
    CHIP_ERROR tag_err = mode_inst->GetModeValueByModeTag(
        chip::to_underlying(MwoMode::ModeTag::kNormal), normal_mode);
    if (tag_err != CHIP_NO_ERROR) {
        LOG_ERR("microwave endpoint %u: NO MODE CARRIES THE kNormal TAG "
                "(%" CHIP_ERROR_FORMAT "); every SetCookingParameters will answer "
                "InvalidCommand and raise no +MTCMD. Check the MicrowaveOvenMode placeholder "
                "tag and any host-fed AT+MTMODES list",
                (unsigned)ep, tag_err.Format());
    } else {
        LOG_INF("microwave endpoint %u ready: kNormal resolves to mode %u", (unsigned)ep,
                (unsigned)normal_mode);
    }
}

/* ---- robotic vacuum cleaner: RvcOperationalState ----------------------
 *
 * The derived opstate cluster needs its own Delegate SUBCLASS
 * (RvcOperationalState::Delegate adds HandleGoHomeCommandCallback and
 * stubs Start/Stop to kUnknownEnumValue, operational-state-server.h:
 * 378-406) and therefore its own pool: the base Delegate::SetInstance()
 * VerifyOrDies when a second Instance shares a delegate
 * (operational-state-server.h:349-353), so lending trio delegates to RVC
 * Instances is fatal, and the trio's HearthOpStateDelegate has no GoHome
 * hook anyway (mt_matter.h:526-528 states the split rule directly).
 *
 * No new translation unit: RvcOperationalState::Instance lives in the
 * already-compiled operational-state-server.cpp (:520-570), constructed
 * Instance(Delegate*, ep), which forwards RvcOperationalState::Id to the
 * protected three-argument base ctor. Init() is the base's, and
 * soft-bails on emberAfContainsServer (operational-state-server.cpp:
 * 63-70), so this half of the RVC is the trio's ordering story, NOT the
 * ModeBase panic above.
 *
 * Command routing (the reason the metadata list in mt_devtypes_zephyr.cpp
 * is exactly {Pause, Resume, GoHome}): the base InvokeCommand() switches
 * on Pause/Resume/Start/Stop unconditionally and sends everything else to
 * InvokeDerivedClusterCommand(), where GoHome is handled (:296-334,
 * :534-546). The server-side guards resolve the state cases before any
 * delegate call (Pause also compatible with kSeekingCharger, Resume with
 * kCharging/kDocked; GoHome from kCharging/kDocked answers
 * kCommandInvalidInState and from kSeekingCharger answers success, both
 * WITHOUT calling the delegate, :522-531 and :548-567), so the three
 * forwards below only carry genuine adjudication requests. Allow is
 * kNoError, deny kUnableToCompleteOperation, copied by the SDK straight
 * into the OperationalCommandResponse: the verdict IS the wire response,
 * the trio's contract. GoHome forwards payload-less (the command has no
 * fields), via mt_cmd_forward like its siblings.
 *
 * Instance reachability: the base GetInstance() passthrough, the smaller
 * change against the C6's typed m_instance member (which exists there
 * only because the C6 writes its own init callback); the pointer is the
 * identical one, upcast to OperationalState::Instance, and everything
 * this file calls on it (SetOperationalState, GetCurrentOperationalState,
 * GetCurrentPhase) is base API.
 */
class HearthRvcOpStateDelegate : public chip::app::Clusters::RvcOperationalState::Delegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }
    chip::EndpointId endpoint() const { return m_ep; }

    chip::app::Clusters::OperationalState::Instance *instance() { return GetInstance(); }

    chip::app::DataModel::Nullable<uint32_t> GetCountdownTime() override
    {
        return chip::app::DataModel::NullNullable;
    }

    /* Seven states in the RVC's own enum: the four base states plus the
     * three derived-number-space ones AT+MTOPSTATE may set (0x40
     * kSeekingCharger, 0x41 kCharging, 0x42 kDocked,
     * RvcOperationalState/Enums.h:67-69), mirroring the C6's list
     * (main.cpp:3300-3313). kEmptyingDustBin and friends (0x43-0x46)
     * exist in the enum but are outside the AT contract's union and not
     * published. kError is listed because the spec requires it even
     * though AT+MTOPSTATE can never set it directly. */
    CHIP_ERROR GetOperationalStateAtIndex(
        size_t index,
        chip::app::Clusters::OperationalState::GenericOperationalState &operationalState) override
    {
        using chip::app::Clusters::RvcOperationalState::OperationalStateEnum;
        static const OperationalStateEnum states[] = {
            OperationalStateEnum::kStopped,        OperationalStateEnum::kRunning,
            OperationalStateEnum::kPaused,         OperationalStateEnum::kError,
            OperationalStateEnum::kSeekingCharger, OperationalStateEnum::kCharging,
            OperationalStateEnum::kDocked,
        };
        if (index >= (sizeof(states) / sizeof(states[0]))) {
            return CHIP_ERROR_NOT_FOUND;
        }
        operationalState.Set(chip::to_underlying(states[index]));
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetOperationalPhaseAtIndex(size_t index,
                                          chip::MutableCharSpan &operationalPhase) override
    {
        /* NOT_FOUND at index 0 makes PhaseList read null, the trio's
         * convention; this firmware publishes no phases. */
        (void)index;
        (void)operationalPhase;
        return CHIP_ERROR_NOT_FOUND;
    }

    void HandlePauseStateCallback(
        chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::RvcOperationalState::Commands::Pause::Id, err);
    }

    void HandleResumeStateCallback(
        chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::RvcOperationalState::Commands::Resume::Id, err);
    }

    void HandleGoHomeCommandCallback(
        chip::app::Clusters::OperationalState::GenericOperationalError &err) override
    {
        forward(chip::app::Clusters::RvcOperationalState::Commands::GoHome::Id, err);
    }

private:
    void forward(uint32_t command,
                 chip::app::Clusters::OperationalState::GenericOperationalError &err)
    {
        using chip::app::Clusters::OperationalState::ErrorStateEnum;
        bool allow = mt_cmd_forward(m_ep, chip::app::Clusters::RvcOperationalState::Id, command);
        err.Set(chip::to_underlying(allow ? ErrorStateEnum::kNoError
                                          : ErrorStateEnum::kUnableToCompleteOperation));
    }

    chip::EndpointId m_ep = chip::kInvalidEndpointId;
};

/* One RvcOperationalState per RVC endpoint, so this pool is
 * kServiceableEndpoints deep like the trio's (the heap admits only nine
 * RVC endpoints, but a slot here is 260-odd bytes against the ModeBase
 * pool's DE404 argument for departing from 16; not worth a second special
 * sizing). Memory reclaim round A: the pool is a table of POINTERS into
 * the cluster-object heap, and the raw storage for the
 * non-default-constructible Instance sits behind the delegate in the same
 * block, placement-constructed once and never destroyed: the standing pool
 * policy. */
static HearthRvcOpStateDelegate *s_rvc_opstate_delegates[kServiceableEndpoints];
static size_t s_rvc_opstate_delegate_next;

extern "C" void *mt_matter_rvc_opstate_delegate_alloc(void)
{
    if (s_rvc_opstate_delegate_next >= kServiceableEndpoints) {
        return nullptr;
    }
    HearthRvcOpStateDelegate *d =
        obj_pair_new<HearthRvcOpStateDelegate,
                     chip::app::Clusters::RvcOperationalState::Instance>();
    if (d == nullptr) {
        return nullptr;
    }
    s_rvc_opstate_delegates[s_rvc_opstate_delegate_next++] = d;
    return d;
}

/*
 * The second half: constructs the RvcOperationalState::Instance in its
 * slot and runs Init(), the trio's shape verbatim (Init soft-bails on
 * emberAfContainsServer, so this belongs below the successful
 * emberAfSetDynamicEndpoint(); a failure is logged loudly and does not
 * abort, the unreachable-by-ordering argument).
 */
extern "C" void mt_matter_rvc_opstate_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    auto *d = static_cast<HearthRvcOpStateDelegate *>(delegate);
    d->set_endpoint(ep);
    auto *inst =
        new (obj_inst_storage<HearthRvcOpStateDelegate,
                              chip::app::Clusters::RvcOperationalState::Instance>(d))
            chip::app::Clusters::RvcOperationalState::Instance(d, ep);
    CHIP_ERROR err = inst->Init();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("RvcOperationalState Instance::Init failed for endpoint %u: %" CHIP_ERROR_FORMAT
                "; commands on it will not reach the host and its attributes will not be served",
                (unsigned)ep, err.Format());
    }
}

/* The pool's Instance lookup, shared by mt_matter_opstate_set()'s RVC
 * branch and the DE397 live reader below. */
static chip::app::Clusters::OperationalState::Instance *mt_rvc_opstate_instance(uint16_t ep)
{
    for (size_t i = 0; i < s_rvc_opstate_delegate_next; i++) {
        HearthRvcOpStateDelegate *d = s_rvc_opstate_delegates[i];
        if (d->endpoint() == ep) {
            return d->instance();
        }
    }
    return nullptr;
}

/*
 * DE397 live read for the RVC opstate pair, the trio's reader against the
 * RVC pool: OperationalState answers the live enum (including the
 * derived-number-space values), CurrentPhase answers +MTERR:5 while null,
 * which is its steady state here since no phases are published.
 */
static int mt_rvc_opstate_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out,
                                         bool *is_unsigned)
{
    namespace OpState = chip::app::Clusters::OperationalState;
    if (is_unsigned) {
        *is_unsigned = true;
    }
    OpState::Instance *inst = mt_rvc_opstate_instance(ep);
    if (inst == nullptr) {
        /* Cannot happen once the boot rebuild has run; defensive. */
        return MT_ATTR_ERR_FAILED;
    }
    if (attr == OpState::Attributes::OperationalState::Id) {
        *out = inst->GetCurrentOperationalState();
        return MT_ATTR_OK;
    }
    chip::app::DataModel::Nullable<uint8_t> phase = inst->GetCurrentPhase();
    if (phase.IsNull()) {
        return MT_ATTR_ERR_TYPE;
    }
    *out = phase.Value();
    return MT_ATTR_OK;
}

/*
 * ============ catalogue batch 7a: the energy foundation =================
 *
 * ElectricalPowerMeasurement (0x0090), PowerTopology (0x009C) and
 * ElectricalEnergyMeasurement (0x0091), the three clusters behind the
 * Electrical Sensor (0x0510) and Electrical Meter (0x0514) device types and
 * every later energy type's sensor graft. The AT surface is AT+MTMEAS
 * (mt_matter_meas_set() below, AT_MT_SPEC.md 3.25); the storage ruling is
 * DE407 option C (no arena slot widening anywhere: the 64-bit attributes are
 * metadata-only declarations served from the delegate caches here, and
 * AT+MTATTR reads reach them through the k_instance_served carve-out above).
 *
 * Three clusters, two models, one bridge, the C6's shape
 * (platform/esp32c6/main/main.cpp, the energy round A block) with one
 * structural delta: on the C6, esp-matter's own delegate-init callbacks
 * construct the EPM and PowerTopology Instances at endpoint enable
 * (ElectricalPowerMeasurementDelegateInitCB and PowerTopologyDelegateInitCB,
 * esp_matter_delegate_callbacks.cpp); this platform has no such layer, so
 * mt_matter_meas_delegate_set_endpoint() below placement-constructs and
 * Init()s both Instances itself, the ModeBase handout's shape. Both Init()s
 * are SOFT (AttributeAccessInterfaceRegistry registration only, no
 * emberAfContainsServer check and no VerifyOrDie anywhere in this batch's
 * energy clusters: electrical-power-measurement-server.cpp:43-47,
 * power-topology-server.cpp:42-46), so a misordered call cannot panic; it
 * would only leave the endpoint unserved, which is why the setter checks and
 * logs the return like every other handout in this file.
 *
 *   ElectricalPowerMeasurement is PULL-model: the Instance is an
 *   AttributeAccessInterface answering every read from the Delegate's
 *   Get*() methods (its Read() has a case for every attribute the cluster
 *   defines and no default arm, electrical-power-measurement-server.cpp:
 *   65-221, so of the declared set only ClusterRevision ever reaches
 *   ember). The host's pushed values live in HearthEpmDelegate members and
 *   one MatterReportingAttributeChangeCallback per applied field makes
 *   subscriptions fire.
 *
 *   PowerTopology with the NodeTopology feature needs only a constructible
 *   Delegate: its two pure virtuals are the endpoint-list iterators backing
 *   AvailableEndpoints/ActiveEndpoints, which exist only under the SET/TREE
 *   features, so HearthPtopDelegate answers both PROVIDER_LIST_EXHAUSTED
 *   and is never actually consulted.
 *
 *   ElectricalEnergyMeasurement is PUSH-model free functions:
 *   NotifyCumulativeEnergyMeasured() stores the timestamped structs into
 *   the server's own per-endpoint MeasurementData AND emits the
 *   CumulativeEnergyMeasured event in one call (ElectricalEnergyMeasurement
 *   Cluster.cpp:191-218, the LogEvent at :207), this port's second
 *   event emitter after batch 5's InitialPress. Its per-endpoint storage
 *   covers dynamic endpoints: gMeasurements is sized
 *   MATTER_DM_..._ENDPOINT_COUNT + CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT
 *   (:42-43), 1 + 16 = 17 entries here, indexed through
 *   emberAfGetClusterServerEndpointIndex(). Measured in the batch 7a step-0
 *   build: 8,432 B of .bss (17 x 496 B), charged the moment this
 *   translation unit is referenced, for every composition, whether or not
 *   an EEM endpoint is ever declared; the audit's headline cost, accepted
 *   by the batch brief with measurement.
 *
 * Accuracy: both electrical clusters must serve a MeasurementAccuracyStruct.
 * This firmware is a bridge and cannot know the host's real metering
 * hardware; the figures served (0.1% to 1%, 0.5% typical, one range over the
 * full XML value span) are the middle band of the SDK reference
 * implementation's table, the C6's identical documented assumption
 * (AT_MT_SPEC.md 3.25, "Accuracy is a fixed firmware constant").
 */

/* The XML value bounds mt_matter_meas_set() validates against
 * (electrical-power-measurement-cluster.xml /
 * electrical-energy-measurement-cluster.xml, pinned tree). Not available
 * from any generated header, so cited literals rather than transcribed
 * enum values, the C6's identical constants. */
static constexpr int64_t kMeasValueAbsMax = 4611686018427387904LL; /* +-2^62 */
static constexpr int64_t kMeasFreqMax     = 1000000;   /* Frequency, mHz  */
static constexpr int64_t kMeasPfAbsMax    = 10000;     /* PowerFactor, 1/100 % */

static const chip::app::Clusters::ElectricalPowerMeasurement::Structs::MeasurementAccuracyRangeStruct::Type
    s_meas_power_accuracy_ranges[] = {
    {
        .rangeMin       = -kMeasValueAbsMax,
        .rangeMax       = kMeasValueAbsMax,
        .percentMax     = chip::MakeOptional(static_cast<chip::Percent100ths>(1000)),
        .percentMin     = chip::MakeOptional(static_cast<chip::Percent100ths>(100)),
        .percentTypical = chip::MakeOptional(static_cast<chip::Percent100ths>(500)),
    },
};

/* The one mandatory EPM accuracy entry: ActivePower, the cluster's one
 * mandatory measured value. */
static const chip::app::Clusters::ElectricalPowerMeasurement::Structs::MeasurementAccuracyStruct::Type
    s_meas_epm_accuracy = {
    .measurementType  = chip::app::Clusters::ElectricalPowerMeasurement::MeasurementTypeEnum::kActivePower,
    .measured         = true,
    .minMeasuredValue = -kMeasValueAbsMax,
    .maxMeasuredValue = kMeasValueAbsMax,
    .accuracyRanges   = chip::app::DataModel::List<
        const chip::app::Clusters::ElectricalPowerMeasurement::Structs::MeasurementAccuracyRangeStruct::Type>(
        s_meas_power_accuracy_ranges),
};

static const chip::app::Clusters::ElectricalEnergyMeasurement::Structs::MeasurementAccuracyRangeStruct::Type
    s_meas_energy_accuracy_ranges[] = {
    {
        .rangeMin       = 0,
        .rangeMax       = kMeasValueAbsMax,
        .percentMax     = chip::MakeOptional(static_cast<chip::Percent100ths>(1000)),
        .percentMin     = chip::MakeOptional(static_cast<chip::Percent100ths>(100)),
        .percentTypical = chip::MakeOptional(static_cast<chip::Percent100ths>(500)),
    },
};

/* Same tolerance figures as the EPM entry above, but typed kElectricalEnergy
 * over the energy value span: EEM's Accuracy attribute describes the energy
 * measurement itself, so serving the ActivePower-typed struct verbatim would
 * satisfy the letter of "the same accuracy" while violating the attribute's
 * meaning (the C6's own note, kept). */
static const chip::app::Clusters::ElectricalEnergyMeasurement::Structs::MeasurementAccuracyStruct::Type
    s_meas_eem_accuracy = {
    .measurementType  = chip::app::Clusters::ElectricalEnergyMeasurement::MeasurementTypeEnum::kElectricalEnergy,
    .measured         = true,
    .minMeasuredValue = 0,
    .maxMeasuredValue = kMeasValueAbsMax,
    .accuracyRanges   = chip::app::DataModel::List<
        const chip::app::Clusters::ElectricalEnergyMeasurement::Structs::MeasurementAccuracyRangeStruct::Type>(
        s_meas_energy_accuracy_ranges),
};

/*
 * The EPM delegate: a store for host-pushed values, all null until the host
 * first pushes them (AT_MT_SPEC.md 3.25's null-until-pushed contract), plus
 * the fixed answers the pull-model server needs. Mirrored from the C6's
 * HearthEpmDelegate (main.cpp) with no semantic change; base-class
 * SetEndpointId()/mEndpointId (electrical-power-measurement-server.h:36)
 * carry the endpoint, set ONCE, by the Instance constructor that
 * mt_matter_meas_delegate_set_endpoint() runs (the ctor's
 * mDelegate.SetEndpointId(aEndpointId), server.h:107): the setter itself
 * makes no SetEndpointId() call of its own for this pool, so endpoint()
 * and meas_epm_for() resolve only from the construction onward, which is
 * always before the first push can name the endpoint. The PTOP branch
 * differs: its set_endpoint() writes the port delegate's OWN member (the
 * base PowerTopology::Delegate has none), and the Instance ctor there
 * sets nothing back.
 */
class HearthEpmDelegate : public chip::app::Clusters::ElectricalPowerMeasurement::Delegate
{
public:
    /* Host-pushed values, written only by mt_matter_meas_set() below. */
    chip::app::DataModel::Nullable<int64_t> m_voltage, m_active_current, m_active_power,
        m_frequency, m_power_factor, m_rms_voltage, m_rms_current;

    chip::EndpointId endpoint() const { return mEndpointId; }

    chip::app::Clusters::ElectricalPowerMeasurement::PowerModeEnum GetPowerMode() override
    {
        /* kAc, agreeing with the AlternatingCurrent feature every EPM
         * endpoint this port builds advertises AND with the PowerMode
         * arena seed (mt_devtypes_zephyr.cpp), which the AT read answers. */
        return chip::app::Clusters::ElectricalPowerMeasurement::PowerModeEnum::kAc;
    }
    uint8_t GetNumberOfMeasurementTypes() override { return 1; }

    /* Accuracy: exactly one entry (ActivePower, the mandatory one), backed
     * by a static const table, so the Start/End read brackets have nothing
     * to lock (the SDK reference delegate's own reasoning). */
    CHIP_ERROR StartAccuracyRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetAccuracyByIndex(
        uint8_t index,
        chip::app::Clusters::ElectricalPowerMeasurement::Structs::MeasurementAccuracyStruct::Type &accuracy) override
    {
        if (index > 0) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        accuracy = s_meas_epm_accuracy;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR EndAccuracyRead() override { return CHIP_NO_ERROR; }

    /* Ranges and harmonics: empty lists (the attributes are not declared
     * on any endpoint this port builds; these satisfy the pure-virtual
     * contract). */
    CHIP_ERROR StartRangesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetRangeByIndex(
        uint8_t,
        chip::app::Clusters::ElectricalPowerMeasurement::Structs::MeasurementRangeStruct::Type &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR EndRangesRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicCurrentsByIndex(
        uint8_t,
        chip::app::Clusters::ElectricalPowerMeasurement::Structs::HarmonicMeasurementStruct::Type &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR EndHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartHarmonicPhasesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicPhasesByIndex(
        uint8_t,
        chip::app::Clusters::ElectricalPowerMeasurement::Structs::HarmonicMeasurementStruct::Type &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR EndHarmonicPhasesRead() override { return CHIP_NO_ERROR; }

    /* The seven field-table attributes, served from the pushed store. */
    chip::app::DataModel::Nullable<int64_t> GetVoltage() override { return m_voltage; }
    chip::app::DataModel::Nullable<int64_t> GetActiveCurrent() override { return m_active_current; }
    chip::app::DataModel::Nullable<int64_t> GetActivePower() override { return m_active_power; }
    chip::app::DataModel::Nullable<int64_t> GetFrequency() override { return m_frequency; }
    chip::app::DataModel::Nullable<int64_t> GetPowerFactor() override { return m_power_factor; }
    chip::app::DataModel::Nullable<int64_t> GetRMSVoltage() override { return m_rms_voltage; }
    chip::app::DataModel::Nullable<int64_t> GetRMSCurrent() override { return m_rms_current; }

    /* Everything the field table does not carry: permanently null. These
     * attributes are never declared on any endpoint this port builds, so
     * these answers exist only to satisfy the pure-virtual contract. */
    chip::app::DataModel::Nullable<int64_t> GetReactiveCurrent() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetApparentCurrent() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetReactivePower() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetApparentPower() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetRMSPower() override { return {}; }
    chip::app::DataModel::Nullable<int64_t> GetNeutralCurrent() override { return {}; }
};

/*
 * The PowerTopology delegate: NodeTopology only, so both pure virtuals
 * answer "empty" and the object exists purely so the Instance has something
 * to hold a reference to. The endpoint member mirrors the other pools'
 * shape for the by-endpoint lookup; nothing else reads it.
 */
class HearthPtopDelegate : public chip::app::Clusters::PowerTopology::Delegate
{
public:
    void set_endpoint(chip::EndpointId ep) { m_ep = ep; }
    chip::EndpointId endpoint() const { return m_ep; }

    CHIP_ERROR GetAvailableEndpointAtIndex(size_t, chip::EndpointId &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    CHIP_ERROR GetActiveEndpointAtIndex(size_t, chip::EndpointId &) override
    {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }

private:
    chip::EndpointId m_ep = chip::kInvalidEndpointId;
};

/*
 * The two pools, MT_MEAS_MAX (8, core/include/mt_matter.h:819, the C6's
 * depth: cite-checked, "measurement-capable endpoints per composition")
 * each, deliberately NOT kServiceableEndpoints: the DE407 ruling pins the
 * C6 pool constants, and eight measurement-capable endpoints already
 * exceeds any host this firmware targets. Exhaustion aborts the create
 * before anything is spent (mt_devtype_create()'s two-halves rule).
 *
 * Each pool is a table of POINTERS into the cluster-object heap since
 * memory reclaim round A, one block per delegate with the raw storage for
 * its Instance behind it: the four-argument Instance ctor needs create-time
 * values, so the Instances are placement-constructed exactly once per block
 * in mt_matter_meas_delegate_set_endpoint() and never destroyed (no
 * teardown path on this platform; AT+MTEP edits apply by reboot), the
 * standing allocate-only pool policy. MT_MEAS_MAX is unchanged and is still
 * checked first.
 */
static HearthEpmDelegate  *s_meas_epm_delegates[MT_MEAS_MAX];
static size_t             s_meas_epm_next;
static HearthPtopDelegate *s_meas_ptop_delegates[MT_MEAS_MAX];
static size_t             s_meas_ptop_next;

extern "C" void *mt_matter_epm_delegate_alloc(void)
{
    if (s_meas_epm_next >= MT_MEAS_MAX) {
        return nullptr;
    }
    HearthEpmDelegate *d =
        obj_pair_new<HearthEpmDelegate,
                     chip::app::Clusters::ElectricalPowerMeasurement::Instance>();
    if (d == nullptr) {
        return nullptr;
    }
    s_meas_epm_delegates[s_meas_epm_next++] = d;
    return d;
}

extern "C" void *mt_matter_ptop_delegate_alloc(void)
{
    if (s_meas_ptop_next >= MT_MEAS_MAX) {
        return nullptr;
    }
    HearthPtopDelegate *d =
        obj_pair_new<HearthPtopDelegate, chip::app::Clusters::PowerTopology::Instance>();
    if (d == nullptr) {
        return nullptr;
    }
    s_meas_ptop_delegates[s_meas_ptop_next++] = d;
    return d;
}

/*
 * The second half of the measurement handout, and where both Instances are
 * BORN on this platform (the structural delta from the C6 in the section
 * comment above). One setter serves both pools, mt_matter.h's documented
 * contract: the void* is matched against each pool's own objects, so
 * mt_devtypes_zephyr.cpp needs neither a second name nor any knowledge of
 * which class it is holding.
 *
 * Must run below a successful emberAfSetDynamicEndpoint(): not for safety
 * (both Init()s are soft, the section comment), but because registering the
 * AAI for an endpoint that then fails to enable would strand a registration
 * nothing serves. Instance feature masks are fixed here and must agree with
 * the FeatureMap arena seeds in mt_devtypes_zephyr.cpp, which the audit
 * notes on epmAttrs/ptopAttrs cross-reference: EPM AlternatingCurrent
 * (0x2) with the six optional attributes the declared metadata carries
 * (Voltage, ActiveCurrent, RMSVoltage, RMSCurrent, Frequency, PowerFactor;
 * ActivePower is mandatory and carries no optional-attribute bit), and
 * PowerTopology NodeTopology (0x1) with no optional attributes, so the
 * endpoint-list reads genuinely do not exist.
 */
extern "C" void mt_matter_meas_delegate_set_endpoint(void *delegate, uint16_t ep)
{
    using namespace chip::app::Clusters;
    for (size_t i = 0; i < s_meas_epm_next; i++) {
        HearthEpmDelegate *d = s_meas_epm_delegates[i];
        if (d == delegate) {
            auto *inst = new (obj_inst_storage<HearthEpmDelegate,
                                               ElectricalPowerMeasurement::Instance>(d))
                ElectricalPowerMeasurement::Instance(
                ep, *d,
                chip::BitMask<ElectricalPowerMeasurement::Feature>(
                    ElectricalPowerMeasurement::Feature::kAlternatingCurrent),
                chip::BitMask<ElectricalPowerMeasurement::OptionalAttributes>(
                    ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeVoltage,
                    ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeActiveCurrent,
                    ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeRMSVoltage,
                    ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeRMSCurrent,
                    ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeFrequency,
                    ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributePowerFactor));
            CHIP_ERROR err = inst->Init();
            if (err != CHIP_NO_ERROR) {
                LOG_ERR("EPM Instance::Init failed for endpoint %u: %" CHIP_ERROR_FORMAT
                        "; every ElectricalPowerMeasurement read on it will fail",
                        (unsigned)ep, err.Format());
            }
            return;
        }
    }
    for (size_t i = 0; i < s_meas_ptop_next; i++) {
        HearthPtopDelegate *d = s_meas_ptop_delegates[i];
        if (d == delegate) {
            d->set_endpoint(ep);
            auto *inst =
                new (obj_inst_storage<HearthPtopDelegate, PowerTopology::Instance>(d))
                    PowerTopology::Instance(
                ep, *d, chip::BitMask<PowerTopology::Feature>(PowerTopology::Feature::kNodeTopology),
                chip::BitMask<PowerTopology::OptionalAttributes>());
            CHIP_ERROR err = inst->Init();
            if (err != CHIP_NO_ERROR) {
                LOG_ERR("PowerTopology Instance::Init failed for endpoint %u: %" CHIP_ERROR_FORMAT
                        "; every PowerTopology read on it will fail",
                        (unsigned)ep, err.Format());
            }
            return;
        }
    }
    LOG_ERR("meas_delegate_set_endpoint: pointer belongs to neither pool (endpoint %u)",
            (unsigned)ep);
}

/* The pool lookup the AT+MTMEAS EPM branch and the DE397 live reader use. */
static HearthEpmDelegate *meas_epm_for(chip::EndpointId ep)
{
    for (size_t i = 0; i < s_meas_epm_next; i++) {
        if (s_meas_epm_delegates[i]->endpoint() == ep) {
            return s_meas_epm_delegates[i];
        }
    }
    return nullptr;
}

/*
 * Per-EEM-endpoint registration, called by mt_devtype_create() below its
 * successful emberAfSetDynamicEndpoint() for every endpoint whose declared
 * cluster list carries ElectricalEnergyMeasurement. The port's
 * HearthEemInitCB equivalent (C6 main.cpp:4066-4088), two jobs:
 *
 *   1. Once, on the first EEM endpoint of the boot: construct and Init()
 *      the server's ONE wildcard ElectricalEnergyMeasurementAttrAccess.
 *      Nothing in the SDK ever registers it (Init() has no caller in the
 *      pinned tree), so without this every EEM attribute read answers
 *      Failure while the ember metadata advertises the attributes, the
 *      ARCHITECTURE.md 8.6 disease in its serving-path flavour. THE MASK IS
 *      LOAD-BEARING: this one object answers FeatureMap for EVERY EEM
 *      endpoint (its Read() encodes mFeature,
 *      ElectricalEnergyMeasurementCluster.cpp:66-67), so it is exactly
 *      Imported|Exported|Cumulative, the three bits every EEM cluster list
 *      in mt_devtypes_zephyr.cpp seeds and AT_MT_SPEC.md 3.25 promises; a
 *      different mask here would make FeatureMap lie on every EEM endpoint
 *      and CumulativeEnergyImported vanish behind its feature gate
 *      (Read()'s VerifyOrReturnError, :75-80). Static storage plus
 *      placement-new, never destroyed, the standing pool policy (the C6
 *      uses a one-time heap new; this platform avoids the heap).
 *   2. Per endpoint: serve the Accuracy attribute via
 *      SetMeasurementAccuracy(), which resolves the endpoint through
 *      emberAfGetClusterServerEndpointIndex() and therefore needs the
 *      endpoint configured and enabled first, which the call site's
 *      below-the-successful-create placement guarantees.
 */
extern "C" void mt_matter_eem_register(uint16_t ep)
{
    using namespace chip::app::Clusters::ElectricalEnergyMeasurement;

    alignas(ElectricalEnergyMeasurementAttrAccess) static uint8_t
        s_eem_attr_access_storage[sizeof(ElectricalEnergyMeasurementAttrAccess)];
    static bool s_eem_registered;

    if (!s_eem_registered) {
        auto *aai = new (s_eem_attr_access_storage) ElectricalEnergyMeasurementAttrAccess(
            chip::BitMask<Feature, uint32_t>(Feature::kImportedEnergy, Feature::kExportedEnergy,
                                             Feature::kCumulativeEnergy),
            chip::BitMask<OptionalAttributes, uint32_t>());
        CHIP_ERROR err = aai->Init();
        if (err != CHIP_NO_ERROR) {
            LOG_ERR("EEM AttrAccess registration failed: %" CHIP_ERROR_FORMAT
                    "; every ElectricalEnergyMeasurement read will fail",
                    err.Format());
        }
        s_eem_registered = true;
    }

    CHIP_ERROR err = SetMeasurementAccuracy(ep, s_meas_eem_accuracy);
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("EEM SetMeasurementAccuracy failed on endpoint %u: %" CHIP_ERROR_FORMAT,
                (unsigned)ep, err.Format());
    }
}

/*
 * DE397 live read for the seven EPM push fields, dispatched from
 * mt_matter_attr_read() under its StackLock: the delegate cache is the
 * served truth, the metadata-only 64-bit declarations have no arena slot
 * to answer from. All seven are signed (the electrical-measurement alias
 * family maps to INT64S, attr_type_info()'s doc note above); a null value
 * (never pushed) answers MT_ATTR_ERR_TYPE with the flag already set, the
 * no-null-literal contract every nullable read in this file follows.
 */
static int mt_epm_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out, bool *is_unsigned)
{
    namespace EPM = chip::app::Clusters::ElectricalPowerMeasurement;
    if (is_unsigned) {
        *is_unsigned = false;
    }
    HearthEpmDelegate *d = meas_epm_for(ep);
    if (d == nullptr) {
        /* Cannot happen once the boot rebuild has run; defensive. */
        return MT_ATTR_ERR_FAILED;
    }
    const chip::app::DataModel::Nullable<int64_t> *v = nullptr;
    switch (attr) {
    case EPM::Attributes::Voltage::Id:       v = &d->m_voltage; break;
    case EPM::Attributes::ActiveCurrent::Id: v = &d->m_active_current; break;
    case EPM::Attributes::ActivePower::Id:   v = &d->m_active_power; break;
    case EPM::Attributes::RMSVoltage::Id:    v = &d->m_rms_voltage; break;
    case EPM::Attributes::RMSCurrent::Id:    v = &d->m_rms_current; break;
    case EPM::Attributes::Frequency::Id:     v = &d->m_frequency; break;
    case EPM::Attributes::PowerFactor::Id:   v = &d->m_power_factor; break;
    default:
        /* Unreachable: only the seven k_instance_served rows route here. */
        return MT_ATTR_ERR_FAILED;
    }
    if (v->IsNull()) {
        return MT_ATTR_ERR_TYPE;
    }
    *out = v->Value();
    return MT_ATTR_OK;
}

/*
 * The AT+MTMEAS bridge (AT_MT_SPEC.md 3.25). Grammar (pair count bounds,
 * per-field signedness at parse) is core's business (cmd_mtmeas and
 * mt_meas_field_signed(), mt_at.c); this bridge owns everything that needs
 * the data model: endpoint and cluster lookup, per-field range validation
 * against the cluster XML bounds, and the atomic apply. Two passes, the
 * family's hard contract: every pair is validated before any pair is
 * applied, so a bad third pair leaves the first two unapplied.
 *
 * Cluster admission is a batch boundary, not machinery: of the five
 * push-served ids the spec names, this build serves 0x0090, 0x0091 and
 * 0x0098 (batch 7a) and 0x0094 (batch 7b); EnergyEvse (0x0099) answers
 * MT_ATTR_ERR_CLUSTER until its device type lands (out of batch 7b by
 * ruling DE408, the LM20 tier), exactly as an unlisted cluster id does,
 * and its branch slots into the dispatch below without reshaping it.
 */
/* The DeviceEnergyManagement (0x0098) and WaterHeaterManagement (0x0094)
 * branch bodies: defined in their sections below, after HearthDemDelegate
 * and HearthWhmDelegate and their pools exist to look into. Both run
 * under the StackLock mt_matter_meas_set() already holds. */
static int mt_meas_dem_apply(uint16_t ep, const uint8_t *fields, const int64_t *values,
                             uint8_t count);
static int mt_meas_whm_apply(uint16_t ep, const uint8_t *fields, const int64_t *values,
                             uint8_t count);

extern "C" int mt_matter_meas_set(uint16_t ep, uint32_t cluster, const uint8_t *fields,
                                  const int64_t *values, uint8_t count)
{
    using namespace chip::app::Clusters;
    chip::DeviceLayer::StackLock lock;

    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (cluster != ElectricalPowerMeasurement::Id && cluster != ElectricalEnergyMeasurement::Id &&
        cluster != DeviceEnergyManagement::Id && cluster != WaterHeaterManagement::Id) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (!emberAfContainsServer(ep, cluster)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    if (count < 1) {
        /* Defensive; mt_at.c never passes an empty list. */
        return MT_ATTR_ERR_FAILED;
    }

    if (cluster == DeviceEnergyManagement::Id) {
        return mt_meas_dem_apply(ep, fields, values, count);
    }
    if (cluster == WaterHeaterManagement::Id) {
        return mt_meas_whm_apply(ep, fields, values, count);
    }

    if (cluster == ElectricalPowerMeasurement::Id) {
        HearthEpmDelegate *d = meas_epm_for(ep);
        if (d == nullptr) {
            /* Cluster present but no pool slot serves this endpoint: cannot
             * happen once the boot rebuild has run; defensive, the pool
             * bridges' standing answer. */
            return MT_ATTR_ERR_FAILED;
        }

        /* Pass 1: validate everything. */
        for (uint8_t i = 0; i < count; i++) {
            switch (fields[i]) {
            case MT_MEAS_F_VOLTAGE:
            case MT_MEAS_F_ACTIVE_CURRENT:
            case MT_MEAS_F_ACTIVE_POWER:
            case MT_MEAS_F_RMS_VOLTAGE:
            case MT_MEAS_F_RMS_CURRENT:
                if (values[i] < -kMeasValueAbsMax || values[i] > kMeasValueAbsMax) {
                    return MT_ATTR_ERR_VALUE;
                }
                break;
            case MT_MEAS_F_FREQUENCY:
                if (values[i] < 0 || values[i] > kMeasFreqMax) {
                    return MT_ATTR_ERR_VALUE;
                }
                break;
            case MT_MEAS_F_POWER_FACTOR:
                if (values[i] < -kMeasPfAbsMax || values[i] > kMeasPfAbsMax) {
                    return MT_ATTR_ERR_VALUE;
                }
                break;
            default:
                return MT_ATTR_ERR_VALUE;
            }
        }

        /* Pass 2: apply, one subscription report per applied field. */
        for (uint8_t i = 0; i < count; i++) {
            chip::app::DataModel::Nullable<int64_t> v =
                chip::app::DataModel::MakeNullable(values[i]);
            uint32_t attr_id;
            switch (fields[i]) {
            case MT_MEAS_F_VOLTAGE:
                d->m_voltage = v;
                attr_id = ElectricalPowerMeasurement::Attributes::Voltage::Id;
                break;
            case MT_MEAS_F_ACTIVE_CURRENT:
                d->m_active_current = v;
                attr_id = ElectricalPowerMeasurement::Attributes::ActiveCurrent::Id;
                break;
            case MT_MEAS_F_ACTIVE_POWER:
                d->m_active_power = v;
                attr_id = ElectricalPowerMeasurement::Attributes::ActivePower::Id;
                break;
            case MT_MEAS_F_FREQUENCY:
                d->m_frequency = v;
                attr_id = ElectricalPowerMeasurement::Attributes::Frequency::Id;
                break;
            case MT_MEAS_F_POWER_FACTOR:
                d->m_power_factor = v;
                attr_id = ElectricalPowerMeasurement::Attributes::PowerFactor::Id;
                break;
            case MT_MEAS_F_RMS_VOLTAGE:
                d->m_rms_voltage = v;
                attr_id = ElectricalPowerMeasurement::Attributes::RMSVoltage::Id;
                break;
            default: /* MT_MEAS_F_RMS_CURRENT, pass 1 admits nothing else */
                d->m_rms_current = v;
                attr_id = ElectricalPowerMeasurement::Attributes::RMSCurrent::Id;
                break;
            }
            MatterReportingAttributeChangeCallback(ep, ElectricalPowerMeasurement::Id, attr_id);
        }
        return MT_ATTR_OK;
    }

    /* ---- ElectricalEnergyMeasurement ---- */

    /* Pass 1: validate everything. Values are cumulative mWh counters,
     * unsigned on the wire; anything negative here is either a negative
     * input or a u64 pattern above INT64_MAX, both outside the XML's
     * 0..2^62. */
    for (uint8_t i = 0; i < count; i++) {
        if (fields[i] != MT_ENERGY_F_IMPORTED && fields[i] != MT_ENERGY_F_EXPORTED) {
            return MT_ATTR_ERR_VALUE;
        }
        if (values[i] < 0 || values[i] > kMeasValueAbsMax) {
            return MT_ATTR_ERR_VALUE;
        }
    }

    ElectricalEnergyMeasurement::MeasurementData *data =
        ElectricalEnergyMeasurement::MeasurementDataForEndpoint(ep);
    if (data == nullptr) {
        /* Cannot happen once the endpoint is enabled: gMeasurements covers
         * every dynamic endpoint (the section comment above); defensive. */
        return MT_ATTR_ERR_FAILED;
    }

    bool    have_imp = false, have_exp = false;
    int64_t imp_val = 0, exp_val = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (fields[i] == MT_ENERGY_F_IMPORTED) {
            have_imp = true;
            imp_val  = values[i]; /* duplicate fields: last one wins */
        } else {
            have_exp = true;
            exp_val  = values[i];
        }
    }

    /* Timestamp policy (AT_MT_SPEC.md 3.25:2713-2730, binding): end is
     * taken by the firmware at push time, Matter epoch seconds when wall
     * time is synced, milliseconds since boot otherwise; each push's start
     * is the previous push's end, carried per endpoint by the server's own
     * stored structs, so consecutive pushes chain into contiguous
     * measurement periods without the host supplying any time at all. */
    uint32_t now_ts = 0;
    bool     wall   = (chip::System::Clock::GetClock_MatterEpochS(now_ts) == CHIP_NO_ERROR);
    uint64_t now_ms = static_cast<uint64_t>(
        chip::System::SystemClock().GetMonotonicMilliseconds64().count());

    auto build = [&](int64_t energy,
                     const chip::Optional<ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type> &prev) {
        ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type s;
        s.energy = energy;
        if (prev.HasValue()) {
            s.startTimestamp = prev.Value().endTimestamp;
            s.startSystime   = prev.Value().endSystime;
        }
        if (wall) {
            s.endTimestamp.SetValue(now_ts);
        } else {
            s.endSystime.SetValue(now_ms);
        }
        return s;
    };

    /* A side this call does not mention is carried forward unchanged:
     * NotifyCumulativeEnergyMeasured() REPLACES both stored sides
     * (ElectricalEnergyMeasurementCluster.cpp:197-198), so passing Missing
     * for the un-pushed one would null its attribute out from under any
     * subscriber. The cost is that the event restates the carried side's
     * previous reading, which the spec's "imported, exported, or both"
     * event wording tolerates (the C6's identical note). */
    chip::Optional<ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type> imported =
        data->cumulativeImported;
    chip::Optional<ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type> exported =
        data->cumulativeExported;
    if (have_imp) {
        imported.SetValue(build(imp_val, data->cumulativeImported));
    }
    if (have_exp) {
        exported.SetValue(build(exp_val, data->cumulativeExported));
    }

    if (!ElectricalEnergyMeasurement::NotifyCumulativeEnergyMeasured(ep, imported, exported)) {
        /* The one narrow post-validation failure the spec discloses: the
         * store may already hold the new values when the event emission
         * fails, so "error answered" does not imply "attributes unchanged"
         * on this path (3.25's event-buffer-exhaustion note). */
        return MT_ATTR_ERR_FAILED;
    }

    /* Notify writes the store and emits the event but reports no attribute
     * change (ElectricalEnergyMeasurementCluster.cpp:191-218 has no
     * MatterReportingAttributeChangeCallback), so subscriptions on the
     * attributes themselves are fired here, one per pushed side, the same
     * one-report-per-applied-field contract as the EPM branch. */
    if (have_imp) {
        MatterReportingAttributeChangeCallback(
            ep, ElectricalEnergyMeasurement::Id,
            ElectricalEnergyMeasurement::Attributes::CumulativeEnergyImported::Id);
    }
    if (have_exp) {
        MatterReportingAttributeChangeCallback(
            ep, ElectricalEnergyMeasurement::Id,
            ElectricalEnergyMeasurement::Attributes::CumulativeEnergyExported::Id);
    }
    return MT_ATTR_OK;
}

/*
 * ---- electrical utility meter: the MeterIdentification pool (batch 7a) ----
 *
 * The gap this section fills, the audit's third organ of the
 * declared-but-never-called disease: nothing in the SDK ever constructs a
 * MeterIdentification::Instance (Init() has no caller anywhere in the
 * pinned tree), so without this scan the cluster's five attributes would
 * advertise in the metadata and answer nothing. The C6 fixed it with its
 * own mt_meter.cpp; this is that file's pool rendered in this port's
 * idiom.
 *
 * The Instance is AttributeAccessInterface only, NO delegate, and OWNS its
 * attribute storage (three 64-byte string buffers plus five Nullables,
 * meter-identification-server.h:60-78; 304 B measured in the step-0
 * build), so the pool is raw aligned storage plus placement-new, the
 * ModeBase Instance shape, MT_METER_MAX (2, core/include/mt_matter.h:1273,
 * the C6 depth per DE407) deep.
 *
 * Reservation versus construction, two different times on purpose (the
 * C6's fix-round-2 lesson, mt_meter.cpp): capacity is claimed at CREATE
 * time by mt_meter_reserve() from mt_devtype_create()'s pre-create claim
 * block, because only a thunk failure can abort the composition rebuild;
 * construction waits for mt_meter_register_all()'s one scan, run by
 * main.cpp after rebuild_composition() and before mt_at_start(). On this
 * platform the CHIP server is already running during the rebuild, so
 * "pre-start" means before +MTREADY lets the host ask, and the scan takes
 * the StackLock; Instance::Init() is SOFT (nulls the five attributes,
 * registers the AAI, meter-identification-server.cpp:59-68).
 *
 * s_meter_reserved counts attempts ADMITTED, not completions: a claim
 * stranded by a later create failure expires with the boot (any create
 * failure aborts the whole rebuild; nothing retries within one boot), the
 * same monotonic-claim reasoning as every pool in this file.
 */
namespace {

/* Memory reclaim round A: the raw Instance storage is no longer a .bss
 * array. mt_meter_reserve() takes it from the cluster-object heap at CLAIM
 * time and parks the pointer here, so a heap shortfall fails the claim and
 * aborts the create like an exhausted pool, rather than surfacing in
 * mt_meter_register_all()'s post-rebuild scan where nothing could abort
 * anything. */
struct mt_meter_entry {
    bool used;
    uint16_t ep;
    chip::app::Clusters::MeterIdentification::Instance *instance;
    uint8_t *storage;
};

mt_meter_entry s_meter[MT_METER_MAX];
uint16_t s_meter_reserved;

chip::app::Clusters::MeterIdentification::Instance *mt_meter_find(uint16_t ep)
{
    for (uint16_t i = 0; i < MT_METER_MAX; i++) {
        if (s_meter[i].used && s_meter[i].ep == ep) {
            return s_meter[i].instance;
        }
    }
    return nullptr;
}

} /* namespace */

/* The one value both the thunk's FeatureMap seed and this pool's Instance
 * construction read (mt_matter.h's one-accessor-two-callers contract), so
 * the ember shadow and the Instance's own BitMask cannot drift. Neither
 * caller snapshots the other's output, so their order is immaterial:
 * Instance::Read() answers FeatureMap from the BitMask given at
 * construction, never from ember. */
extern "C" uint32_t mt_meter_feature_mask(void)
{
    return chip::to_underlying(chip::app::Clusters::MeterIdentification::Feature::kPowerThreshold);
}

extern "C" bool mt_meter_reserve(void)
{
    if (s_meter_reserved >= MT_METER_MAX) {
        return false;
    }
    uint8_t *raw = obj_inst_new<chip::app::Clusters::MeterIdentification::Instance>();
    if (raw == nullptr) {
        return false;
    }
    s_meter[s_meter_reserved].storage = raw;
    s_meter_reserved++;
    return true;
}

extern "C" void mt_meter_register_all(void)
{
    using namespace chip::app::Clusters::MeterIdentification;
    chip::DeviceLayer::StackLock lock;

    chip::BitMask<Feature> features(mt_meter_feature_mask());
    uint16_t slot = 0;

    /* slot < MT_METER_MAX is a defensive backstop, not the primary gate:
     * mt_meter_reserve() already bounded how many meter endpoints the
     * rebuild could create, and a composition that exceeded it aborted
     * before this function runs. The walk is the live endpoint table (the
     * C6's own scan shape), filtered by the real ember composition. */
    for (uint16_t i = 0; i < mt_matter_endpoint_count() && slot < MT_METER_MAX; i++) {
        uint32_t devtype;
        uint16_t ep;
        uint8_t variant, parent_idx;
        if (mt_matter_endpoint_info(i, &devtype, &ep, &variant, &parent_idx) != 0) {
            continue;
        }
        if (!emberAfContainsServer(ep, chip::app::Clusters::MeterIdentification::Id)) {
            continue;
        }
        /* Memory reclaim round A: every meter-bearing endpoint in this walk
         * passed mt_meter_reserve(), which allocated this slot's storage,
         * so a null here is an internal invariant violation rather than a
         * capacity outcome. Stop rather than fault. */
        if (s_meter[slot].storage == nullptr) {
            LOG_ERR("MeterIdentification slot %u has no storage: reserve/scan disagree; "
                    "endpoint %u will serve nothing",
                    (unsigned)slot, (unsigned)ep);
            break;
        }
        auto *inst = new (s_meter[slot].storage) Instance(ep, features);
        CHIP_ERROR err = inst->Init();
        if (err != CHIP_NO_ERROR) {
            /* Logged, not aborted: Init() refusing an (endpoint, cluster)
             * pair this same boot's rebuild just created is an internal
             * invariant violation no host action can trigger, the
             * mt_meter.cpp reasoning. The slot is not consumed. */
            LOG_ERR("MeterIdentification Instance::Init failed for endpoint %u: "
                    "%" CHIP_ERROR_FORMAT "; its five attributes will answer nothing",
                    (unsigned)ep, err.Format());
            continue;
        }
        s_meter[slot].used = true;
        s_meter[slot].ep = ep;
        s_meter[slot].instance = inst;
        slot++;
    }
}

/*
 * DE397 live read for MeterType, dispatched from mt_matter_attr_read()
 * under its StackLock: unsigned enum8, null until the host's first
 * AT+MTMETERID (+MTERR:5, the no-null-literal rule).
 */
static int mt_meter_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out, bool *is_unsigned)
{
    using namespace chip::app::Clusters::MeterIdentification;
    if (is_unsigned) {
        *is_unsigned = true;
    }
    if (attr != Attributes::MeterType::Id) {
        /* Unreachable: MeterType is the cluster's only carve-out row. */
        return MT_ATTR_ERR_FAILED;
    }
    Instance *inst = mt_meter_find(ep);
    if (inst == nullptr) {
        /* Cannot happen once mt_meter_register_all() has run; defensive. */
        return MT_ATTR_ERR_FAILED;
    }
    if (inst->GetMeterType().IsNull()) {
        return MT_ATTR_ERR_TYPE;
    }
    *out = chip::to_underlying(inst->GetMeterType().Value());
    return MT_ATTR_OK;
}

/*
 * AT+MTMETERID (AT_MT_SPEC.md 3.29): push the full MeterIdentification
 * identity in one call, the only write path any of the five attributes
 * has, all-or-nothing (every field validated before any SetXxx() runs).
 * Grammar (the hand-parse, quoting, printability, the 64-byte scan bound)
 * is cmd_mtmeterid's business in mt_at.c; this bridge owns the lookups and
 * the semantic checks. The C6 splits this across main.cpp (lock, lookups)
 * and mt_meter.cpp (validate, apply) only because ChipStackLock lives in
 * its main.cpp; here one function owns the whole path.
 *
 * The cluster lookup answers MT_ATTR_ERR_ATTRIBUTE, not the generic
 * MT_ATTR_ERR_CLUSTER: this command's own error table maps "endpoint
 * exists but carries no MeterIdentification" to +MTERR:4, the AT+MTROW
 * family's no-payload-of-this-kind code (3.29's lookup-errors paragraph),
 * and attr_err_to_mterr() renders MT_ATTR_ERR_ATTRIBUTE as exactly that.
 *
 * There is deliberately no read-back verb: the identity is
 * host-originated and re-pushed on reconcile, and a full identity line
 * (worst case 265 B) exceeds the host library's 255-byte usable receive
 * line, so a read-back would be silently discarded by the host's own line
 * reader rather than fail loudly (the C6's measured arithmetic,
 * mt_meter.cpp, restated in 3.29).
 */
extern "C" int mt_matter_meter_set_identity(uint16_t ep, const mt_meter_identity_t *id)
{
    using namespace chip::app::Clusters::MeterIdentification;
    using chip::app::Clusters::Globals::PowerThresholdSourceEnum;
    /* PowerThresholdStruct is a namespace (the payload type is
     * PowerThresholdStruct::Type), so a namespace alias rather than a
     * using-declaration. */
    namespace PowerThresholdStruct = chip::app::Clusters::Globals::Structs::PowerThresholdStruct;
    using chip::app::DataModel::MakeNullable;

    chip::DeviceLayer::StackLock lock;

    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, chip::app::Clusters::MeterIdentification::Id)) {
        return MT_ATTR_ERR_ATTRIBUTE;
    }
    Instance *inst = mt_meter_find(ep);
    if (inst == nullptr) {
        /* Cluster present but no pool slot: cannot happen once
         * mt_meter_register_all() has run; defensive. */
        return MT_ATTR_ERR_FAILED;
    }

    /* ---- validate everything before applying anything ---- */

    if (id->meter_type > static_cast<uint8_t>(MeterTypeEnum::kGeneric)) {
        return MT_ATTR_ERR_VALUE;
    }
    if (!id->pwr_present && !id->apparent_present) {
        /* PowerThresholdStruct's "choice b": at least one of the two power
         * optionals. cmd_mtmeterid already rejects this at parse; the
         * double gate is the demcap precedent. */
        return MT_ATTR_ERR_VALUE;
    }
    if (id->src_present && id->src > static_cast<uint8_t>(PowerThresholdSourceEnum::kEquipment)) {
        return MT_ATTR_ERR_VALUE;
    }
    size_t pod_len      = strlen(id->pod);
    size_t serial_len   = strlen(id->serial);
    size_t protocol_len = strlen(id->protocol);
    if (pod_len > MT_METERID_MAX_STR || serial_len > MT_METERID_MAX_STR ||
        protocol_len > MT_METERID_MAX_STR) {
        /* Defensive: cmd_mtmeterid enforces the length while scanning the
         * quoted string; re-checked as the last point before any SetXxx()
         * call, the all-or-nothing contract's whole purpose. */
        return MT_ATTR_ERR_VALUE;
    }

    /* ---- apply ---- */

    PowerThresholdStruct::Type pt;
    if (id->pwr_present) {
        pt.powerThreshold.SetValue(id->pwr);
    }
    if (id->apparent_present) {
        pt.apparentPowerThreshold.SetValue(id->apparent);
    }
    if (id->src_present) {
        pt.powerThresholdSource.SetNonNull(static_cast<PowerThresholdSourceEnum>(id->src));
    } else {
        pt.powerThresholdSource.SetNull();
    }

    /* Every call below is expected to return CHIP_NO_ERROR: the block
     * above already checked everything each SetXxx() would reject. A
     * failure here is an internal SDK invariant violation, logged rather
     * than propagated so one unexpected failure does not also abandon the
     * fields that would have applied cleanly after it (the C6's identical
     * disposition; the Instance copies every span into its own fixed
     * buffers before returning, so the caller's stack strings are not
     * held). */
    CHIP_ERROR err;
    err = inst->SetMeterType(MakeNullable(static_cast<MeterTypeEnum>(id->meter_type)));
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("SetMeterType failed for endpoint %u: %" CHIP_ERROR_FORMAT, (unsigned)ep,
                err.Format());
    }
    err = inst->SetPointOfDelivery(MakeNullable(chip::CharSpan(id->pod, pod_len)));
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("SetPointOfDelivery failed for endpoint %u: %" CHIP_ERROR_FORMAT, (unsigned)ep,
                err.Format());
    }
    err = inst->SetMeterSerialNumber(MakeNullable(chip::CharSpan(id->serial, serial_len)));
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("SetMeterSerialNumber failed for endpoint %u: %" CHIP_ERROR_FORMAT, (unsigned)ep,
                err.Format());
    }
    err = inst->SetProtocolVersion(MakeNullable(chip::CharSpan(id->protocol, protocol_len)));
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("SetProtocolVersion failed for endpoint %u: %" CHIP_ERROR_FORMAT, (unsigned)ep,
                err.Format());
    }
    err = inst->SetPowerThreshold(MakeNullable(pt));
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("SetPowerThreshold failed for endpoint %u: %" CHIP_ERROR_FORMAT, (unsigned)ep,
                err.Format());
    }

    return MT_ATTR_OK;
}

/*
 * ---- device energy management: HearthDemDelegate and its pool (batch 7a) ----
 *
 * The C6's HearthDemDelegate (main.cpp:4865-5266) ported whole: the cached
 * attribute store the Instance's AAI reads, the owned
 * PowerAdjustmentCapability list AT+MTDEMCAP replaces, the two PA command
 * forwards, and the firmware-owned event policy. The policy, binding per
 * AT_MT_SPEC.md 3.17:1487-1533 and 3.25:2755-2768:
 *
 *   - An ALLOWED PowerAdjustRequest sets ESAState kPowerAdjustActive
 *     itself, arms the duration clock and emits the fieldless
 *     PowerAdjustStart (the host does NOT push the entry transition, the
 *     contrast to the water heater's Boost where the host pushes
 *     BoostState).
 *   - Re-adjust rule: a second accept while already PowerAdjustActive
 *     emits NO second Start and does not re-arm the clock (certification
 *     behaviour: TC_DEM_2_2 step 14 requires "SUCCESS and no event sent"
 *     and asserts the eventual End's duration against the FIRST accept).
 *     The forward still reaches the host either way.
 *   - A host ESAState push LEAVING kPowerAdjustActive emits
 *     PowerAdjustEnd(NormalCompletion, measured duration, cached field-6
 *     energyUse, cache consumed); a same-state push emits nothing and
 *     reports nothing (ESAState reports on change only, unlike the WHM's
 *     per-sample BoostState).
 *   - An accepted CancelPowerAdjustRequest emits
 *     PowerAdjustEnd(Cancelled) and resets to Online through the RAW
 *     setter, never SetESAState(): the derivation there would emit a
 *     second End with the wrong cause for a session already ended.
 *
 * The server pre-validates EVERY PowerAdjustRequest against the pushed
 * capability and the OptOutState x cause matrix BEFORE the delegate runs
 * (device-energy-management-server.cpp:254-359: a null capability answers
 * ConstraintError, an out-of-range request likewise, and
 * CancelPowerAdjustRequest has a server-side in-state guard answering
 * InvalidInState, :371-390), so a null capability never wakes the host and
 * no +MTCMD is ever raised for a cancel in the wrong state: AT+MTDEMCAP's
 * whole reason for existing.
 *
 * Cause tracking (the C6's task-review F2 contract): the capability
 * struct's LIVE cause is firmware-owned while an adjustment runs (stamped
 * on accept with the request's adjustment reason, reported dirty); the
 * BASELINE member is the host-pushed resting value every PowerAdjustEnd
 * restores.
 *
 * Lock discipline, the file's standing rule: the two command forwards run
 * on the CHIP task from the Instance's CHI invoke path, NO StackLock
 * taken; the push-path derivation (SetESAState) runs from
 * mt_matter_meas_set()/mt_matter_demcap_set(), which already hold it.
 */
class HearthDemDelegate : public chip::app::Clusters::DeviceEnergyManagement::Delegate
{
public:
    /* Host-pushed cached state, written only by mt_meas_dem_apply(). The
     * defaults are the spec's pre-first-push answers (3.25): ESAType 0
     * (kEvse), canGenerate false, ESAState Online, powers 0, OptOutState
     * NoOptOut. */
    uint8_t m_esa_type     = 0;
    bool    m_can_generate = false;
    chip::app::Clusters::DeviceEnergyManagement::ESAStateEnum m_esa_state =
        chip::app::Clusters::DeviceEnergyManagement::ESAStateEnum::kOnline;
    int64_t m_abs_min_power = 0;
    int64_t m_abs_max_power = 0;
    chip::app::Clusters::DeviceEnergyManagement::OptOutStateEnum m_opt_out =
        chip::app::Clusters::DeviceEnergyManagement::OptOutStateEnum::kNoOptOut;

    /* The session's approximate energy use (mWh), the PowerAdjustEnd
     * event's energyUse field: MT_DEM_F_ADJ_ENERGY_USE, an event carrier
     * and not an attribute (mt_matter.h). Host-pushed while an adjustment
     * runs, consumed and reset by every PowerAdjustEnd emission. */
    int64_t m_energy_use = 0;

    /* The owned PowerAdjustmentCapability store, AT+MTDEMCAP's surface:
     * fixed backing array, entry count, the Nullable the getter hands
     * out (null until the host installs entries), and the baseline cause
     * (the F2 contract in the section comment). */
    chip::app::Clusters::DeviceEnergyManagement::Structs::PowerAdjustStruct::Type
        m_pa_entries[MT_DEM_CAP_MAX_ENTRIES];
    uint8_t m_pa_count = 0;
    chip::app::DataModel::Nullable<
        chip::app::Clusters::DeviceEnergyManagement::Structs::PowerAdjustCapabilityStruct::Type>
        m_pa_capability;
    chip::app::Clusters::DeviceEnergyManagement::PowerAdjustReasonEnum m_pa_cause_baseline =
        chip::app::Clusters::DeviceEnergyManagement::PowerAdjustReasonEnum::kNoAdjustment;

    chip::EndpointId endpoint() const { return mEndpointId; }

    /* Whether this endpoint's variant seeded the PowerAdjustment bit:
     * stamped by mt_matter_dem_register() from the create path's variant
     * predicate, the single source the FeatureMap seed shares. The
     * AT+MTDEMCAP feature gate reads it here (the C6 reads its
     * esp-matter FeatureMap attribute back for the same answer; this
     * port's ember shadow is a seed, so the port record is the honest
     * source). */
    void set_with_pa(bool pa) { m_with_pa = pa; }
    bool with_pa() const { return m_with_pa; }

    void SetAdjEnergyUse(int64_t mwh) { m_energy_use = mwh; }

    chip::Protocols::InteractionModel::Status PowerAdjustRequest(
        const int64_t power, const uint32_t duration,
        chip::app::Clusters::DeviceEnergyManagement::AdjustmentCauseEnum cause) override
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;
        using chip::Protocols::InteractionModel::Status;

        char fields[48];
        snprintf(fields, sizeof(fields), "%lld,%lu,%u", (long long)power, (unsigned long)duration,
                 (unsigned)chip::to_underlying(cause));

        bool allow = mt_cmd_forward_fields(mEndpointId, Id, Commands::PowerAdjustRequest::Id,
                                           fields);
        if (!allow) {
            return Status::Failure;
        }

        /* Whether this accept STARTS an adjustment or RE-ADJUSTS a
         * running one, decided before the state write (the section
         * comment's re-adjust rule). */
        bool in_progress = (m_esa_state == ESAStateEnum::kPowerAdjustActive);

        /* Stamp the capability's live cause with the accepted request's
         * adjustment reason (F2). The server has already rejected
         * kUnknownEnumValue; the default arm is defensive. */
        switch (cause) {
        case AdjustmentCauseEnum::kLocalOptimization:
            SetCapabilityCause(PowerAdjustReasonEnum::kLocalOptimizationAdjustment);
            break;
        case AdjustmentCauseEnum::kGridOptimization:
            SetCapabilityCause(PowerAdjustReasonEnum::kGridOptimizationAdjustment);
            break;
        default:
            break;
        }

        SetStateRaw(ESAStateEnum::kPowerAdjustActive);
        if (!in_progress) {
            m_pa_start_ms = static_cast<uint64_t>(
                chip::System::SystemClock().GetMonotonicMilliseconds64().count());

            /* Emission discipline: COMMAND PATH, CHIP task, no StackLock
             * (the section comment). A LogEvent failure is logged and
             * deliberately does not fail the command: the host has
             * already accepted and the state is applied and served. */
            Events::PowerAdjustStart::Type event;
            chip::EventNumber n;
            CHIP_ERROR err = chip::app::LogEvent(event, mEndpointId, n);
            if (err != CHIP_NO_ERROR) {
                LOG_ERR("DEM ep %u: PowerAdjustStart event failed: %" CHIP_ERROR_FORMAT,
                        (unsigned)mEndpointId, err.Format());
            }
        }
        /* else: re-adjust while active, no second Start, the clock keeps
         * measuring from the first accept. */
        return Status::Success;
    }

    chip::Protocols::InteractionModel::Status CancelPowerAdjustRequest() override
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;
        using chip::Protocols::InteractionModel::Status;

        /* Payload-less forward (NULL fields reproduces mt_cmd_forward()'s
         * exact four-field +MTCMD line). No firmware in-state guard: the
         * server refused the command with InvalidInState unless ESAState
         * was kPowerAdjustActive before this ran (the section comment). */
        bool allow = mt_cmd_forward_fields(mEndpointId, Id, Commands::CancelPowerAdjustRequest::Id,
                                           NULL);
        if (!allow) {
            return Status::Failure;
        }

        EmitPowerAdjustEnd(CauseEnum::kCancelled);
        SetStateRaw(ESAStateEnum::kOnline);
        return Status::Success;
    }

    /* The six non-PowerAdjustment command handlers. Unreachable while
     * this firmware is PA-only: the server's InvokeCommand dispatch
     * answers UnsupportedCommand for every one of them unless the
     * matching feature bit is in the Instance's mask
     * (device-energy-management-server.cpp:181-251), and this port only
     * ever constructs with kPowerAdjustment or nothing. Failure rather
     * than Success so a future feature bit without a handler is a
     * visible command failure, not a silent lie (the C6's reasoning,
     * kept). */
    chip::Protocols::InteractionModel::Status StartTimeAdjustRequest(
        const uint32_t requestedStartTime,
        chip::app::Clusters::DeviceEnergyManagement::AdjustmentCauseEnum cause) override
    {
        (void)requestedStartTime;
        (void)cause;
        return chip::Protocols::InteractionModel::Status::Failure;
    }
    chip::Protocols::InteractionModel::Status PauseRequest(
        const uint32_t duration,
        chip::app::Clusters::DeviceEnergyManagement::AdjustmentCauseEnum cause) override
    {
        (void)duration;
        (void)cause;
        return chip::Protocols::InteractionModel::Status::Failure;
    }
    chip::Protocols::InteractionModel::Status ResumeRequest() override
    {
        return chip::Protocols::InteractionModel::Status::Failure;
    }
    chip::Protocols::InteractionModel::Status ModifyForecastRequest(
        const uint32_t forecastID,
        const chip::app::DataModel::DecodableList<
            chip::app::Clusters::DeviceEnergyManagement::Structs::SlotAdjustmentStruct::Type>
            &slotAdjustments,
        chip::app::Clusters::DeviceEnergyManagement::AdjustmentCauseEnum cause) override
    {
        (void)forecastID;
        (void)slotAdjustments;
        (void)cause;
        return chip::Protocols::InteractionModel::Status::Failure;
    }
    chip::Protocols::InteractionModel::Status RequestConstraintBasedForecast(
        const chip::app::DataModel::DecodableList<
            chip::app::Clusters::DeviceEnergyManagement::Structs::ConstraintsStruct::Type>
            &constraints,
        chip::app::Clusters::DeviceEnergyManagement::AdjustmentCauseEnum cause) override
    {
        (void)constraints;
        (void)cause;
        return chip::Protocols::InteractionModel::Status::Failure;
    }
    chip::Protocols::InteractionModel::Status CancelRequest() override
    {
        return chip::Protocols::InteractionModel::Status::Failure;
    }

    /* The eight getters, serving the host-pushed cache. */
    chip::app::Clusters::DeviceEnergyManagement::ESATypeEnum GetESAType() override
    {
        return static_cast<chip::app::Clusters::DeviceEnergyManagement::ESATypeEnum>(m_esa_type);
    }
    bool GetESACanGenerate() override { return m_can_generate; }
    chip::app::Clusters::DeviceEnergyManagement::ESAStateEnum GetESAState() override
    {
        return m_esa_state;
    }
    int64_t GetAbsMinPower() override { return m_abs_min_power; }
    int64_t GetAbsMaxPower() override { return m_abs_max_power; }
    chip::app::Clusters::DeviceEnergyManagement::OptOutStateEnum GetOptOutState() override
    {
        return m_opt_out;
    }
    const chip::app::DataModel::Nullable<
        chip::app::Clusters::DeviceEnergyManagement::Structs::PowerAdjustCapabilityStruct::Type> &
    GetPowerAdjustmentCapability() override
    {
        return m_pa_capability;
    }
    /* Permanently null: PFR/SFR out of this batch's scope. */
    const chip::app::DataModel::Nullable<
        chip::app::Clusters::DeviceEnergyManagement::Structs::ForecastStruct::Type> &
    GetForecast() override
    {
        return m_forecast;
    }

    /*
     * SetESAState: the push-path transition entry. mt_meas_dem_apply()
     * calls this for every MT_DEM_F_ESA_STATE pair; the derivation lives
     * here (the section comment's push rules). Rejects kUnknownEnumValue
     * and up, reports the attribute dirty ON CHANGE ONLY, so callers do
     * not report ESAState again themselves. The one SDK caller of this
     * virtual is the server's Pause path, unreachable while PA-only, so
     * in practice every call site is a bridge holding the StackLock.
     */
    CHIP_ERROR SetESAState(chip::app::Clusters::DeviceEnergyManagement::ESAStateEnum next) override
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;

        if (next >= ESAStateEnum::kUnknownEnumValue) {
            return CHIP_IM_GLOBAL_STATUS(ConstraintError);
        }
        if (next == m_esa_state) {
            return CHIP_NO_ERROR;
        }
        ESAStateEnum prev = m_esa_state;
        if (prev == ESAStateEnum::kPowerAdjustActive) {
            /* Emission discipline: PUSH PATH, the bridge already holds
             * the StackLock (the section comment). */
            EmitPowerAdjustEnd(CauseEnum::kNormalCompletion);
        }
        if (next == ESAStateEnum::kPowerAdjustActive) {
            /* Entering via a push is off-design (the firmware owns the
             * entry transition on an accepted PowerAdjustRequest, which
             * arms this clock itself), but nothing stops a host doing
             * it; arm the clock here too so a later End measures from
             * this moment instead of from boot (3.25's rule). */
            m_pa_start_ms = static_cast<uint64_t>(
                chip::System::SystemClock().GetMonotonicMilliseconds64().count());
        }
        SetStateRaw(next);
        return CHIP_NO_ERROR;
    }

    /* AT+MTDEMCAP's apply half needs the private helpers; the bridge is a
     * free function, so it goes through these two rather than friending. */
    void demcap_apply(chip::app::Clusters::DeviceEnergyManagement::PowerAdjustReasonEnum baseline,
                      uint8_t n, const int64_t *quads)
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;

        /* The live cause carried into the rebuilt struct: the pushed
         * baseline normally, the firmware-stamped value while an
         * adjustment is running (the F2 contract: replacing the
         * capability mid-adjustment must not overwrite the stamped cause;
         * the eventual PowerAdjustEnd restores the NEW baseline). */
        PowerAdjustReasonEnum live = baseline;
        if (m_esa_state == ESAStateEnum::kPowerAdjustActive && !m_pa_capability.IsNull()) {
            live = m_pa_capability.Value().cause;
        }
        m_pa_cause_baseline = baseline;

        if (n == 0) {
            m_pa_count = 0;
            m_pa_capability.SetNull();
        } else {
            for (uint8_t i = 0; i < n; i++) {
                m_pa_entries[i].minPower    = quads[4 * i];
                m_pa_entries[i].maxPower    = quads[4 * i + 1];
                m_pa_entries[i].minDuration = static_cast<uint32_t>(quads[4 * i + 2]);
                m_pa_entries[i].maxDuration = static_cast<uint32_t>(quads[4 * i + 3]);
            }
            m_pa_count = n;
            Structs::PowerAdjustCapabilityStruct::Type cap;
            cap.powerAdjustCapability.SetNonNull(
                chip::app::DataModel::List<const Structs::PowerAdjustStruct::Type>(m_pa_entries,
                                                                                  n));
            cap.cause = live;
            m_pa_capability.SetNonNull(cap);
        }
        MatterReportingAttributeChangeCallback(mEndpointId, Id,
                                               Attributes::PowerAdjustmentCapability::Id);
    }

private:
    bool m_with_pa = false;

    /* Forecast: permanently null, never written. */
    chip::app::DataModel::Nullable<
        chip::app::Clusters::DeviceEnergyManagement::Structs::ForecastStruct::Type>
        m_forecast;

    /* Monotonic ms at the last accepted PowerAdjustRequest, the duration
     * clock. */
    uint64_t m_pa_start_ms = 0;

    /* Stamp the owned capability struct's cause and report the attribute
     * dirty. A null capability (AT+MTDEMCAP never sent) has no cause to
     * stamp; that state cannot carry a running adjustment anyway (the
     * server refuses PowerAdjustRequest on a null capability), so
     * skipping is right on the restore path too. */
    void SetCapabilityCause(chip::app::Clusters::DeviceEnergyManagement::PowerAdjustReasonEnum r)
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;
        if (m_pa_capability.IsNull() || m_pa_capability.Value().cause == r) {
            return;
        }
        m_pa_capability.Value().cause = r;
        MatterReportingAttributeChangeCallback(mEndpointId, Id,
                                               Attributes::PowerAdjustmentCapability::Id);
    }

    /* Cache write + dirty report, no event derivation: the command paths
     * use this directly because they emit their own event (or none)
     * before transitioning; SetESAState() is the derived-emission entry.
     * Reports on change only. */
    void SetStateRaw(chip::app::Clusters::DeviceEnergyManagement::ESAStateEnum next)
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;
        if (m_esa_state != next) {
            m_esa_state = next;
            MatterReportingAttributeChangeCallback(mEndpointId, Id, Attributes::ESAState::Id);
        }
    }

    /* Fill and log PowerAdjustEnd: cause per caller, duration measured
     * against the accept timestamp, energyUse from the host-pushed
     * field-6 cache, consumed (reset to 0) by the emission; the
     * capability cause restored to the host-pushed baseline (F2). Lock
     * discipline is the CALLER's, commented at each call site; a
     * LogEvent failure is logged and does not propagate (the transition
     * the event describes has already been decided). */
    void EmitPowerAdjustEnd(chip::app::Clusters::DeviceEnergyManagement::CauseEnum cause)
    {
        using namespace chip::app::Clusters::DeviceEnergyManagement;

        uint64_t now_ms = static_cast<uint64_t>(
            chip::System::SystemClock().GetMonotonicMilliseconds64().count());
        Events::PowerAdjustEnd::Type event;
        event.cause     = cause;
        event.duration  = static_cast<uint32_t>((now_ms - m_pa_start_ms) / 1000);
        event.energyUse = m_energy_use;

        chip::EventNumber n;
        CHIP_ERROR err = chip::app::LogEvent(event, mEndpointId, n);
        if (err != CHIP_NO_ERROR) {
            LOG_ERR("DEM ep %u: PowerAdjustEnd event failed: %" CHIP_ERROR_FORMAT,
                    (unsigned)mEndpointId, err.Format());
        }
        m_energy_use = 0;
        SetCapabilityCause(m_pa_cause_baseline);
    }
};

/*
 * The pool, MT_DEM_MAX (4, core/include/mt_matter.h:1017, the C6 depth per
 * DE407) delegates plus raw aligned Instance storage: unlike the C6, where
 * esp-matter's DeviceEnergyManagementDelegateInitCB news the Instance at
 * endpoint enable from a FeatureMap snapshot, this port constructs it in
 * mt_matter_dem_register() with the variant's own mask (measured 40 B per
 * Instance in the step-0 build). Endpoint id stamped in the SUCCESS-ONLY
 * second half by the Instance constructor, never at handout (batch 7b
 * fix round M1, the reasoning at the alloc below). Exhaustion aborts the
 * create before anything is spent.
 */
static HearthDemDelegate *s_dem_delegates[MT_DEM_MAX];
static size_t            s_dem_next;

/* Batch 7b fix round M1: the id is NOT stamped at handout any more. The
 * WHM alloc's comment carries the full reasoning, one discipline for
 * both id-at-alloc pools: a claim stranded by a create failure after
 * this point used to keep the s_next_ep_id stamp the NEXT successful
 * create then took, and dem_for()'s first-match walk would bind the
 * 0x98 pushes, AT+MTDEMCAP and the carve-out reads to the dead delegate
 * (inherited 7a pattern, fixed with its double). The sentinel is
 * unmatchable; the real id lands in mt_matter_dem_register()'s Instance
 * construction, whose ctor runs mDelegate.SetEndpointId(aEndpointId)
 * (device-energy-management-server.h:210-216), success-only. */
extern "C" void *mt_matter_dem_delegate_alloc(uint16_t ep)
{
    (void)ep;
    if (s_dem_next >= MT_DEM_MAX) {
        return nullptr;
    }
    HearthDemDelegate *d =
        obj_pair_new<HearthDemDelegate,
                     chip::app::Clusters::DeviceEnergyManagement::Instance>();
    if (d == nullptr) {
        return nullptr;
    }
    d->SetEndpointId(chip::kInvalidEndpointId);
    s_dem_delegates[s_dem_next++] = d;
    return d;
}

/* The pool lookup the 0x98 push branch, the AT+MTDEMCAP bridge and the
 * DE397 live reader use. */
static HearthDemDelegate *dem_for(chip::EndpointId ep)
{
    for (size_t i = 0; i < s_dem_next; i++) {
        if (s_dem_delegates[i]->endpoint() == ep) {
            return s_dem_delegates[i];
        }
    }
    return nullptr;
}

/*
 * The DEM second half: construct the Instance with the variant's feature
 * mask and Init() it (CHI registration then AAI, SOFT on both,
 * device-energy-management-server.cpp:40-47: no emberAfContainsServer
 * check anywhere, so ordering mistakes cannot panic; below the successful
 * create by the same strand-nothing reasoning as the measurement
 * halves). with_pa is the create path's variant predicate, the single
 * source shared with seed_slots()'s FeatureMap special case
 * (mt_devtypes_zephyr.cpp), and is recorded on the delegate for
 * mt_matter_demcap_set()'s feature gate.
 */
extern "C" void mt_matter_dem_register(void *delegate, uint16_t ep, bool with_pa)
{
    using namespace chip::app::Clusters::DeviceEnergyManagement;
    auto *d = static_cast<HearthDemDelegate *>(delegate);
    d->set_with_pa(with_pa);
    auto *inst = new (obj_inst_storage<HearthDemDelegate, Instance>(d))
        Instance(ep, *d, with_pa ? Feature::kPowerAdjustment : static_cast<Feature>(0));
    CHIP_ERROR err = inst->Init();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("DEM Instance::Init failed for endpoint %u: %" CHIP_ERROR_FORMAT
                "; the cluster will serve nothing and its commands will not dispatch",
                (unsigned)ep, err.Format());
    }
}

/*
 * AT+MTMEAS's DeviceEnergyManagement (0x0098) branch. Called only from
 * mt_matter_meas_set(), which already holds the StackLock and resolved
 * the endpoint and cluster lookups; this function owns the field table
 * (AT_MT_SPEC.md 3.25's 0x0098 rows). Value bounds from the cluster XML
 * and the generated enums: ESAType is 0..0x0D plus 0xFF (kOther);
 * ESACanGenerate bool; ESAState ESAStateEnum 0..4; OptOutState 0..3; the
 * two powers carry the full int64 width (the XML constrains neither
 * beyond its type; the AbsMaxPower>=AbsMinPower relation is a
 * cross-field data-model property between independently pushed fields,
 * deliberately not enforced per push). Field 6 is the event carrier:
 * caches only, never dirty. Two passes, the family's hard contract.
 */
static int mt_meas_dem_apply(uint16_t ep, const uint8_t *fields, const int64_t *values,
                             uint8_t count)
{
    using namespace chip::app::Clusters::DeviceEnergyManagement;

    HearthDemDelegate *d = dem_for(ep);
    if (d == nullptr) {
        /* Cluster present but no pool slot serves this endpoint: cannot
         * happen once the boot rebuild has run; defensive. */
        return MT_ATTR_ERR_FAILED;
    }

    /* Pass 1: validate everything. */
    for (uint8_t i = 0; i < count; i++) {
        switch (fields[i]) {
        case MT_DEM_F_ESA_TYPE:
            if (!((values[i] >= 0 && values[i] <= 0x0D) || values[i] == 0xFF)) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_DEM_F_ESA_CAN_GEN:
            if (values[i] < 0 || values[i] > 1) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_DEM_F_ESA_STATE:
            if (values[i] < 0 ||
                values[i] >= chip::to_underlying(ESAStateEnum::kUnknownEnumValue)) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_DEM_F_ABS_MIN_POWER:
        case MT_DEM_F_ABS_MAX_POWER:
        case MT_DEM_F_ADJ_ENERGY_USE:
            /* int64 full width, no XML bound beyond the type. */
            break;
        case MT_DEM_F_OPT_OUT_STATE:
            if (values[i] < 0 ||
                values[i] >= chip::to_underlying(OptOutStateEnum::kUnknownEnumValue)) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        default:
            return MT_ATTR_ERR_VALUE;
        }
    }

    /* Pass 2: apply, in order (sequential last-writer semantics).
     * ESAState transitions derive events as they are applied. */
    for (uint8_t i = 0; i < count; i++) {
        uint32_t attr_id;
        switch (fields[i]) {
        case MT_DEM_F_ESA_TYPE:
            d->m_esa_type = (uint8_t)values[i];
            attr_id = Attributes::ESAType::Id;
            break;
        case MT_DEM_F_ESA_CAN_GEN:
            d->m_can_generate = (values[i] != 0);
            attr_id = Attributes::ESACanGenerate::Id;
            break;
        case MT_DEM_F_ESA_STATE:
            /* SetESAState owns derivation AND the on-change dirty report;
             * pass 1 already cut the enum range, so its ConstraintError
             * arm is unreachable here. */
            (void)d->SetESAState(static_cast<ESAStateEnum>(values[i]));
            continue;
        case MT_DEM_F_ABS_MIN_POWER:
            d->m_abs_min_power = values[i];
            attr_id = Attributes::AbsMinPower::Id;
            break;
        case MT_DEM_F_ABS_MAX_POWER:
            d->m_abs_max_power = values[i];
            attr_id = Attributes::AbsMaxPower::Id;
            break;
        case MT_DEM_F_OPT_OUT_STATE:
            d->m_opt_out = static_cast<OptOutStateEnum>(values[i]);
            attr_id = Attributes::OptOutState::Id;
            break;
        default: /* MT_DEM_F_ADJ_ENERGY_USE, pass 1 admits nothing else */
            /* Event carrier: cache only, never dirty (3.25's field-6
             * row). */
            d->SetAdjEnergyUse(values[i]);
            continue;
        }
        MatterReportingAttributeChangeCallback(ep, Id, attr_id);
    }
    return MT_ATTR_OK;
}

/*
 * The AT+MTDEMCAP bridge (AT_MT_SPEC.md 3.26). Grammar (the 3+4n arity,
 * integer parses, the u32 duration bound) is cmd_mtdemcap's business;
 * this bridge owns everything semantic, in the established
 * lookup-then-value order: endpoint and cluster lookups, the
 * PowerAdjustment feature gate (the delegate's with_pa record, stamped
 * from the same variant predicate the FeatureMap seed derives from; a
 * variant-1 endpoint has no PowerAdjustmentCapability attribute to set
 * and answers MT_ATTR_ERR_ATTRIBUTE, the spec's +MTERR:4 row), the cause
 * enum range, and per-entry minPower<=maxPower / minDuration<=maxDuration
 * ordering (the server's request walk takes each entry as an interval; a
 * mis-ordered entry would be an empty one). Full replacement per call,
 * not persisted, reported dirty on success; the store lives in the
 * delegate and starts null every boot.
 */
extern "C" int mt_matter_demcap_set(uint16_t ep, uint8_t cause, uint8_t n, const int64_t *quads)
{
    using namespace chip::app::Clusters::DeviceEnergyManagement;
    chip::DeviceLayer::StackLock lock;

    if (emberAfIndexFromEndpoint(ep) == kEmberInvalidEndpointIndex) {
        return MT_ATTR_ERR_ENDPOINT;
    }
    if (!emberAfContainsServer(ep, Id)) {
        return MT_ATTR_ERR_CLUSTER;
    }
    HearthDemDelegate *d = dem_for(ep);
    if (d == nullptr) {
        /* Cluster present but no pool slot: defensive, the push branches'
         * answer. */
        return MT_ATTR_ERR_FAILED;
    }
    if (!d->with_pa()) {
        return MT_ATTR_ERR_ATTRIBUTE;
    }

    if (n > MT_DEM_CAP_MAX_ENTRIES) {
        /* Defensive; cmd_mtdemcap's <n> range check answers first. */
        return MT_ATTR_ERR_VALUE;
    }
    if (cause >= chip::to_underlying(PowerAdjustReasonEnum::kUnknownEnumValue)) {
        return MT_ATTR_ERR_VALUE;
    }
    for (uint8_t i = 0; i < n; i++) {
        if (quads[4 * i] > quads[4 * i + 1] ||     /* minPower > maxPower */
            quads[4 * i + 2] > quads[4 * i + 3]) { /* minDuration > maxDuration */
            return MT_ATTR_ERR_VALUE;
        }
    }

    d->demcap_apply(static_cast<PowerAdjustReasonEnum>(cause), n, quads);
    return MT_ATTR_OK;
}

/*
 * DE397 live read for the six DEM scalars, dispatched from
 * mt_matter_attr_read() under its StackLock: the delegate cache is the
 * served truth (the enum shadows are seeds, the two powers have no slot
 * at all). None of the six is nullable; signedness per attribute.
 */
static int mt_dem_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out, bool *is_unsigned)
{
    namespace DEM = chip::app::Clusters::DeviceEnergyManagement;
    HearthDemDelegate *d = dem_for(ep);
    if (d == nullptr) {
        /* Cannot happen once the boot rebuild has run; defensive. */
        if (is_unsigned) {
            *is_unsigned = false;
        }
        return MT_ATTR_ERR_FAILED;
    }
    bool u = true;
    int64_t v;
    switch (attr) {
    case DEM::Attributes::ESAType::Id:       v = d->m_esa_type; break;
    case DEM::Attributes::ESACanGenerate::Id: v = d->m_can_generate ? 1 : 0; break;
    case DEM::Attributes::ESAState::Id:      v = chip::to_underlying(d->m_esa_state); break;
    case DEM::Attributes::OptOutState::Id:   v = chip::to_underlying(d->m_opt_out); break;
    case DEM::Attributes::AbsMinPower::Id:   v = d->m_abs_min_power; u = false; break;
    case DEM::Attributes::AbsMaxPower::Id:   v = d->m_abs_max_power; u = false; break;
    default:
        /* Unreachable: only the six k_instance_served rows route here. */
        if (is_unsigned) {
            *is_unsigned = false;
        }
        return MT_ATTR_ERR_FAILED;
    }
    if (is_unsigned) {
        *is_unsigned = u;
    }
    *out = v;
    return MT_ATTR_OK;
}

/*
 * ---- water heater: HearthWhmDelegate and its pool (batch 7b) --------------
 *
 * The C6's HearthWhmDelegate (main.cpp:4405-4564) ported whole: the cached
 * attribute store the Instance's AAI reads, the Boost/CancelBoost command
 * forwards, and the firmware-derived Boost event pair. The cluster is FULLY
 * delegate-served (its six values answer from the Delegate getters,
 * water-heater-management-server.cpp:114-152; the C6 creates all six
 * MANAGED_INTERNALLY, esp_matter_attribute.cpp:4485-4519); the split
 * ownership rule is the AT+MTLOCK/AT+MTVALVE one: an ALLOWED Boost does NOT
 * touch the cached BoostState here, because "the host decided" (the command
 * verdict) and "the host actually did it" (the AT+MTMEAS BoostState push)
 * are different moments, and only the second one moves the served state.
 *
 * =========================================================================
 * THE BOOST EVENT DERIVATION STATE MACHINE, exhaustively, because it is
 * stateful firmware logic with no SDK support and no build check
 * (AT_MT_SPEC.md 3.25:2734-2749 and core/include/mt_matter.h:865-874,
 * both binding; the C6's mt_meas_whm_apply() implements it identically).
 *
 * State: the delegate's cached BoostState (kInactive/kActive,
 * WaterHeaterManagement/Enums.h:32-36), written ONLY by the AT+MTMEAS 0x94
 * push path below. Input: each applied MT_WHM_F_BOOST_STATE pair, IN ORDER
 * within one command (sequential last-writer semantics, so one push
 * carrying 2,1,2,0 legitimately emits BoostStarted then BoostEnded).
 * Transitions, evaluated per applied pair against the cache value it is
 * about to replace:
 *
 *   Inactive -> Active: emit BoostStarted, its boostInfo carrying the
 *     CACHED PARAMETERS OF THE LAST HOST-ACCEPTED BOOST COMMAND (the
 *     m_boost lifecycle below), then CONSUME the cache (reset to the
 *     duration-0/no-optionals default).
 *   Active -> Inactive: emit BoostEnded (no payload beyond the event).
 *   Same state -> same state: emit NOTHING. This is also what makes
 *     HandleCancelBoost()'s in-state guard's "no event sent" claim hold by
 *     construction: no forward means no host actuation, no BoostState
 *     push, no transition.
 *
 * Attribute reporting is SEPARATE from event derivation and fires per
 * sample: every applied pair, BoostState included and SAME-STATE pushes
 * included, is reported dirty (one MatterReportingAttributeChangeCallback
 * per applied pair, the EPM rule), the deliberate CONTRAST to DEM's
 * ESAState, which reports on change only. Two clusters, two spec'd
 * shapes; do not "fix" one to match the other.
 *
 * THE PARAMETER-CACHE LIFECYCLE (m_boost), the round's delicate piece:
 *
 *   SET: in HandleBoost(), on ALLOW only, after the host's verdict comes
 *     back positive. A denied Boost never started anything, so there is
 *     nothing for a later BoostStarted to describe, and caching on deny
 *     would attribute a stale command's parameters to some future
 *     host-initiated boost.
 *   CONSUMED: by the Inactive-to-Active emission, exactly once. The
 *     parameters describe the boost that just started; a LATER Active
 *     transition without a fresh accepted Boost in between (a
 *     host-initiated boost from a physical button, no controller
 *     involved) must emit duration 0 and no optionals, "parameters
 *     unknown", rather than restate a finished boost's numbers. That is
 *     the round B rule mt_matter.h:865-874 fixes, and the reset after
 *     emission is what implements it.
 *   ALSO CONSUMED: by the guarded (Inactive-state) CancelBoost, RULED
 *     this round (ruled at B410; batch 7b review I1). An accepted Boost a
 *     controller then cancels before the host ever pushes Active is a
 *     boost that never started, and the spec's own no-cache-on-deny
 *     rationale ("a denied Boost never started anything") applies to it
 *     verbatim: without this reset the next genuinely unrelated
 *     host-initiated boost would emit BoostStarted carrying the
 *     CANCELLED command's duration and optionals. DELIBERATE C6
 *     DIVERGENCE: the C6's guard leaves its cache set and has the exact
 *     stale-parameter window, parked as bug B410; AT_MT_SPEC.md is being
 *     amended to this consume-on-guarded-cancel rule (the coordinator's
 *     spec pass), so this port implements the amended rule and the C6
 *     catches up with B410. Only the guarded arm consumes: an ALLOWED
 *     forwarded CancelBoost (Active state) still touches nothing, since
 *     the boost it cancels already consumed the cache at its
 *     BoostStarted.
 *   REPLACED: a second accepted Boost before any Active push simply
 *     overwrites the cache; the last accepted command is the one whose
 *     parameters the eventual BoostStarted carries.
 *   NEVER cleared by BoostEnded, a forwarded CancelBoost or a deny: only
 *     an allow writes it, and only the two consumers above (emission,
 *     the guarded cancel) reset it.
 *
 *   ONE DISCLOSED WIRE TENSION rides on the duration-0 emission, spec-
 *   mandated and deliberately NOT "fixed" here (review M2):
 *   WaterHeaterBoostInfoStruct declares Duration min="1"
 *   (water-heater-management-cluster.xml:42), so the "parameters unknown"
 *   BoostStarted sends a value the struct's own constraint excludes. The
 *   wire rule stands (AT_MT_SPEC.md 3.25 and mt_matter.h pin duration 0;
 *   the C6 emits the same 0), but a certification harness or a strict
 *   controller validating the decoded event against the constraint may
 *   reject it: a named TH risk for the bench, not a port defect.
 *
 * Emission discipline (the DEM section's rule, restated per path): the
 * derivation runs ONLY from mt_meas_whm_apply(), i.e. under the StackLock
 * mt_matter_meas_set() already holds, so GenerateBoostStartedEvent()/
 * GenerateBoostEndedEvent() (Delegate base helpers wrapping LogEvent,
 * water-heater-management-server.cpp:48-86) run lock-held. A Generate*
 * failure (event buffer exhaustion) is logged by the helper itself and
 * deliberately does NOT fail the push: the pushed state is applied and
 * served either way, so answering an error would tell the host "nothing
 * changed" about a change that took (the EEM branch's error-after-apply
 * caveat, without the misleading answer).
 * =========================================================================
 *
 * The CancelBoost in-state guard, and WHERE it lives, traced on both
 * platforms because the batch brief asks exactly that: the SDK server has
 * NO guard (Instance::HandleCancelBoost, water-heater-management-server
 * .cpp:232-241, is a bare delegate call; contrast DEM, where the server
 * itself answers InvalidInState). The guard is DELEGATE property on both
 * platforms: the SDK's own reference delegate skips the cancel body and
 * answers Success when not boosting (WhmDelegateImpl::HandleCancelBoost,
 * examples/energy-management-app), and the C6's HearthWhmDelegate returns
 * Status::Success without waking the host when the cached BoostState is
 * already Inactive, because the cluster test plan requires "status
 * SUCCESS and no event sent" for exactly that case (TC_EWATERHTR_2_2 step
 * 26; AT_MT_SPEC.md 3.17:1487-1495). This port mirrors the observable
 * behaviour (Success, no +MTCMD, no event) and ADDS the ruled
 * cache consume the lifecycle block above records (the B410 ruling): the one
 * deliberate divergence from both the C6 and the SDK reference delegate,
 * neither of which clears its cache there (the C6's window is parked as
 * B410). The guard reads the host-pushed BoostState cache, the same
 * value a controller reads.
 *
 * The ALLOWED (Active-state) CancelBoost emits no BoostEnded here, BY
 * DESIGN (review M4): the split-ownership rule again, the host owns the
 * state, so the event fires only when the host's BoostState 0 push
 * reaches the derivation. The SDK's own reference delegate emits
 * BoostEnded inside HandleCancelBoost instead; a host that actuates the
 * cancel and never pushes leaves the served BoostState Active with no
 * event, which is the host library documentation's line to carry, not a
 * firmware guard's.
 *
 * Lock discipline, the file's standing rule: HandleBoost/HandleCancelBoost
 * run on the CHIP task from the Instance's CHI invoke path, NO StackLock
 * taken; the push-path derivation runs from mt_meas_whm_apply(), which
 * inherits mt_matter_meas_set()'s.
 */
class HearthWhmDelegate : public chip::app::Clusters::WaterHeaterManagement::Delegate
{
public:
    /* Host-pushed cached state, written only by mt_meas_whm_apply(). The
     * defaults are the spec's pre-first-push answers (AT_MT_SPEC.md 3.25):
     * everything 0, BoostState Inactive; they match the zero-filled inert
     * shadows in mt_devtypes_zephyr.cpp's whmAttrs seeds. */
    uint8_t  m_heater_types = 0;
    uint8_t  m_heat_demand  = 0;
    chip::app::Clusters::WaterHeaterManagement::BoostStateEnum m_boost_state =
        chip::app::Clusters::WaterHeaterManagement::BoostStateEnum::kInactive;
    uint16_t m_tank_volume  = 0;
    int64_t  m_est_heat_req = 0;
    uint8_t  m_tank_percent = 0;

    /* The parameters of the last host-ACCEPTED Boost command, cached for
     * the derived BoostStarted event. The full lifecycle contract is the
     * section comment's PARAMETER-CACHE block; the value-initialized
     * default (duration 0, every Optional missing) IS the "parameters
     * unknown" emission, so consuming the cache is one aggregate reset. */
    struct BoostParams {
        uint32_t                      duration = 0;
        chip::Optional<bool>          one_shot;
        chip::Optional<bool>          emergency;
        chip::Optional<int16_t>       setpoint;
        chip::Optional<chip::Percent> target_pct;
        chip::Optional<chip::Percent> reheat;
    } m_boost;

    chip::EndpointId endpoint() const { return mEndpointId; }

    /* The endpoint's WHM feature bits, stamped by mt_matter_whm_register()
     * from the create path's variant predicate, the single source the
     * FeatureMap seed and the Instance mask share (the DEM with_pa
     * divergence's reasoning: the C6 reads its live FeatureMap attribute
     * back, but this port's ember copy is a seed shadow, so the port
     * record stamped from the same predicate is the honest source). Read
     * by mt_meas_whm_apply()'s field gates. */
    void set_features(uint32_t mask) { m_features = mask; }
    bool has_feature(chip::app::Clusters::WaterHeaterManagement::Feature f) const
    {
        return (m_features & chip::to_underlying(f)) != 0;
    }

    /*
     * Boost: pack the five Optionals into the wire form
     * "<duration>,<mask>[,<v1>[,<v2>[,<v3>]]]" (AT_MT_SPEC.md 3.17's
     * five-field tail, the first five-field +MTCMD consumer):
     * MT_BOOST_P_* presence bits in canonical order, MT_BOOST_V_* carrying
     * the two bools' VALUES when present (a present bool needs no appended
     * field), then ONLY the present numeric optionals appended in
     * canonical order, temporarySetpoint / targetPercentage / targetReheat
     * (core/include/mt_matter.h:973-979; worked example: duration 3600,
     * oneShot true, targetPercentage 80 forwards as "3600,265,80"). The
     * Instance has already range-checked targetPercentage/targetReheat and
     * their feature conformance before calling this
     * (water-heater-management-server.cpp:171-222), so the packing needs
     * no validation of its own. The verdict is the wire response raw:
     * HandleBoost's Status lands in AddStatus unmapped (:224-225).
     * Accepted parameters are cached ON ALLOW ONLY (the lifecycle
     * contract above).
     */
    chip::Protocols::InteractionModel::Status HandleBoost(
        uint32_t duration, chip::Optional<bool> oneShot, chip::Optional<bool> emergencyBoost,
        chip::Optional<int16_t> temporarySetpoint, chip::Optional<chip::Percent> targetPercentage,
        chip::Optional<chip::Percent> targetReheat) override
    {
        using chip::Protocols::InteractionModel::Status;

        unsigned mask = 0;
        if (oneShot.HasValue()) {
            mask |= MT_BOOST_P_ONESHOT;
            if (oneShot.Value()) {
                mask |= MT_BOOST_V_ONESHOT;
            }
        }
        if (emergencyBoost.HasValue()) {
            mask |= MT_BOOST_P_EMERGENCY;
            if (emergencyBoost.Value()) {
                mask |= MT_BOOST_V_EMERGENCY;
            }
        }
        if (temporarySetpoint.HasValue()) {
            mask |= MT_BOOST_P_SETPOINT;
        }
        if (targetPercentage.HasValue()) {
            mask |= MT_BOOST_P_TARGET_PCT;
        }
        if (targetReheat.HasValue()) {
            mask |= MT_BOOST_P_REHEAT;
        }

        char fields[48];
        int n = snprintf(fields, sizeof(fields), "%lu,%u", (unsigned long)duration, mask);
        if (temporarySetpoint.HasValue()) {
            n += snprintf(fields + n, sizeof(fields) - n, ",%d", (int)temporarySetpoint.Value());
        }
        if (targetPercentage.HasValue()) {
            n += snprintf(fields + n, sizeof(fields) - n, ",%u",
                          (unsigned)targetPercentage.Value());
        }
        if (targetReheat.HasValue()) {
            n += snprintf(fields + n, sizeof(fields) - n, ",%u", (unsigned)targetReheat.Value());
        }
        (void)n;

        bool allow = mt_cmd_forward_fields(
            mEndpointId, chip::app::Clusters::WaterHeaterManagement::Id,
            chip::app::Clusters::WaterHeaterManagement::Commands::Boost::Id, fields);
        if (!allow) {
            return Status::Failure;
        }
        m_boost.duration   = duration;
        m_boost.one_shot   = oneShot;
        m_boost.emergency  = emergencyBoost;
        m_boost.setpoint   = temporarySetpoint;
        m_boost.target_pct = targetPercentage;
        m_boost.reheat     = targetReheat;
        return Status::Success;
    }

    /*
     * CancelBoost: in-state guard FIRST, without waking the host, since
     * there is nothing for the host to adjudicate (the section comment
     * traces where the guard lives on each platform; the SDK server has
     * none). The guarded answer is SUCCESS-and-silence, not a failure,
     * AND it consumes the parameter cache (B410 ruling, the lifecycle
     * block's ALSO CONSUMED arm: a cancelled not-yet-started boost is a
     * boost that never started, so its parameters must not survive to
     * describe some future unrelated one; the C6's identical stale
     * window is parked as B410). The "no event" half holds by
     * construction: no forward means no host actuation, no BoostState
     * push, no Active-to-Inactive transition, so the derivation never
     * emits BoostEnded. Otherwise forward command 1 payload-less (NULL
     * fields reproduces mt_cmd_forward()'s exact four-field +MTCMD line)
     * and pass the verdict through raw, same as Boost above; the
     * forwarded arm touches no state and emits nothing here, the M4
     * design note in the section comment.
     */
    chip::Protocols::InteractionModel::Status HandleCancelBoost() override
    {
        using chip::Protocols::InteractionModel::Status;
        using chip::app::Clusters::WaterHeaterManagement::BoostStateEnum;

        if (m_boost_state == BoostStateEnum::kInactive) {
            m_boost = BoostParams{};
            return Status::Success;
        }
        bool allow = mt_cmd_forward_fields(
            mEndpointId, chip::app::Clusters::WaterHeaterManagement::Id,
            chip::app::Clusters::WaterHeaterManagement::Commands::CancelBoost::Id, NULL);
        return allow ? Status::Success : Status::Failure;
    }

    /* The six getters, serving the host-pushed cache. */
    chip::BitMask<chip::app::Clusters::WaterHeaterManagement::WaterHeaterHeatSourceBitmap>
    GetHeaterTypes() override
    {
        return chip::BitMask<
            chip::app::Clusters::WaterHeaterManagement::WaterHeaterHeatSourceBitmap>(
            m_heater_types);
    }
    chip::BitMask<chip::app::Clusters::WaterHeaterManagement::WaterHeaterHeatSourceBitmap>
    GetHeatDemand() override
    {
        return chip::BitMask<
            chip::app::Clusters::WaterHeaterManagement::WaterHeaterHeatSourceBitmap>(
            m_heat_demand);
    }
    uint16_t GetTankVolume() override { return m_tank_volume; }
    chip::Energy_mWh GetEstimatedHeatRequired() override { return m_est_heat_req; }
    chip::Percent GetTankPercentage() override { return m_tank_percent; }
    chip::app::Clusters::WaterHeaterManagement::BoostStateEnum GetBoostState() override
    {
        return m_boost_state;
    }

private:
    uint32_t m_features = 0;
};

/*
 * The pool, MT_WHM_MAX (4, core/include/mt_matter.h:939, the C6 depth per
 * DE407) delegates plus raw aligned Instance storage, the DEM pool's exact
 * shape: this port constructs the Instance in mt_matter_whm_register()
 * with the variant's own mask, where the C6 lets esp-matter's
 * WaterHeaterManagementDelegateInitCB new it from a FeatureMap snapshot at
 * endpoint enable. Endpoint id stamped in the SUCCESS-ONLY second half by
 * the Instance constructor (water-heater-management-server.h:142-149),
 * never at handout: the alloc takes the id per the header's contract
 * (mt_matter.h:1003) and discards it, fix round M1, the full reasoning at
 * the alloc below. Exhaustion aborts the create before anything is spent;
 * never destroyed (~Instance() would Shutdown() cleanly, but this
 * platform has no teardown path, the standing allocate-only policy).
 */
static HearthWhmDelegate *s_whm_delegates[MT_WHM_MAX];
static size_t            s_whm_next;

/* Fix round M1, one discipline for both id-at-alloc pools (this one and
 * the DEM's): the id passed per the header's alloc(ep) contract is
 * DELIBERATELY NOT stamped here. s_next_ep_id is only the id this create
 * WILL assign if nothing after the claim fails, and a claim stranded by a
 * later failure kept that stamp while the NEXT successful create took the
 * same id: whm_for()'s first-match walk would then bind pushes and
 * carve-out reads to the dead delegate while the fabric served the live
 * Instance, a silent split. The sentinel makes a stranded claim
 * unmatchable (dynamic endpoints are 1..N, never kInvalidEndpointId), and
 * the REAL id lands in the success-only second half, where
 * mt_matter_whm_register()'s Instance construction runs
 * mDelegate.SetEndpointId(ep) (water-heater-management-server.h:142-149):
 * the id-in-the-second-half discipline every other pool in this file
 * already follows (ModeBase, opstate, valve, EPM/PTOP), now uniform. The
 * header's stamp-at-handout rationale is C6 init-callback timing that
 * does not exist here (header read-only, the standing staleness
 * disposition). */
extern "C" void *mt_matter_whm_delegate_alloc(uint16_t ep)
{
    (void)ep;
    if (s_whm_next >= MT_WHM_MAX) {
        return nullptr;
    }
    HearthWhmDelegate *d =
        obj_pair_new<HearthWhmDelegate,
                     chip::app::Clusters::WaterHeaterManagement::Instance>();
    if (d == nullptr) {
        return nullptr;
    }
    d->SetEndpointId(chip::kInvalidEndpointId);
    s_whm_delegates[s_whm_next++] = d;
    return d;
}

/* The pool lookup the AT+MTMEAS 0x94 branch and the DE397 live reader
 * use, the meas_epm_for() shape. */
static HearthWhmDelegate *whm_for(chip::EndpointId ep)
{
    for (size_t i = 0; i < s_whm_next; i++) {
        if (s_whm_delegates[i]->endpoint() == ep) {
            return s_whm_delegates[i];
        }
    }
    return nullptr;
}

/*
 * The WHM second half: construct the Instance with the variant's feature
 * mask and Init() it (CHI registration then AAI, SOFT on both,
 * water-heater-management-server.cpp:94-100: no emberAfContainsServer
 * check anywhere, so ordering mistakes cannot panic; below the successful
 * create by the same strand-nothing reasoning as the measurement halves).
 * with_em_tp is the create path's variant predicate, the single source
 * shared with seed_slots()'s WHM FeatureMap special case
 * (mt_devtypes_zephyr.cpp), and is recorded on the delegate for
 * mt_meas_whm_apply()'s field gates. The ctor takes a single Feature value
 * that seeds a BitMask (server.h:142-149), so the two-bit mask is cast
 * through the underlying type, the DEM register's own idiom.
 */
extern "C" void mt_matter_whm_register(void *delegate, uint16_t ep, bool with_em_tp)
{
    using namespace chip::app::Clusters::WaterHeaterManagement;
    auto *d = static_cast<HearthWhmDelegate *>(delegate);
    uint32_t mask = with_em_tp ? (chip::to_underlying(Feature::kEnergyManagement) |
                                  chip::to_underlying(Feature::kTankPercent))
                               : 0;
    d->set_features(mask);
    auto *inst = new (obj_inst_storage<HearthWhmDelegate, Instance>(d))
        Instance(ep, *d, static_cast<Feature>(mask));
    CHIP_ERROR err = inst->Init();
    if (err != CHIP_NO_ERROR) {
        LOG_ERR("WHM Instance::Init failed for endpoint %u: %" CHIP_ERROR_FORMAT
                "; the cluster will serve nothing and its commands will not dispatch",
                (unsigned)ep, err.Format());
    }
}

/*
 * AT+MTMEAS's WaterHeaterManagement (0x0094) branch. Called only from
 * mt_matter_meas_set(), which already holds the StackLock and resolved the
 * endpoint and cluster lookups; this function owns the field table
 * (AT_MT_SPEC.md 3.25's 0x0094 rows) and the derivation state machine the
 * section comment above fixes.
 *
 * Feature gate, bridge-side by design (mt_at.c only classifies
 * signedness): TankVolume and EstimatedHeatRequired exist only under
 * EnergyManagement, TankPercentage only under TankPercent
 * (water-heater-management-cluster.xml:73-87), and a push to a gated field
 * on an endpoint without the feature answers MT_ATTR_ERR_CLUSTER
 * (+MTERR:3), the same data-model code an energy push gets on a
 * power-only electrical endpoint: "this endpoint does not serve that
 * data" is one condition, not two (the spec's own words). The bits come
 * from the delegate's port record, stamped from the identical variant
 * predicate the FeatureMap seed and the Instance mask derive from, so the
 * gate and the served FeatureMap cannot disagree (the section comment's
 * honest-source note; the C6 reads its live FeatureMap attribute instead
 * because esp-matter's copy is authoritative there).
 *
 * Value bounds, the cluster XML: HeaterTypes/HeatDemand are
 * WaterHeaterHeatSourceBitmap (defined bits 0x01..0x10, so 0..0x1F);
 * BoostState is enum8 0/1; TankVolume u16; EstimatedHeatRequired is
 * energy_mwh with min 0 (int64 on the wire for pipeline symmetry, so the
 * negative half is cut here, +MTERR:1 through MT_ATTR_ERR_VALUE);
 * TankPercentage is percent 0..100. Two passes, the family's hard
 * contract: every pair is validated (range AND feature gate) before any
 * pair is applied.
 */
static int mt_meas_whm_apply(uint16_t ep, const uint8_t *fields, const int64_t *values,
                             uint8_t count)
{
    using namespace chip::app::Clusters::WaterHeaterManagement;

    HearthWhmDelegate *d = whm_for(ep);
    if (d == nullptr) {
        /* Cluster present but no pool slot serves this endpoint: cannot
         * happen once the boot rebuild has run; defensive, the pool
         * bridges' standing answer. */
        return MT_ATTR_ERR_FAILED;
    }

    /* Pass 1: validate everything, range and feature gate both. */
    for (uint8_t i = 0; i < count; i++) {
        switch (fields[i]) {
        case MT_WHM_F_HEATER_TYPES:
        case MT_WHM_F_HEAT_DEMAND:
            if (values[i] < 0 || values[i] > 0x1F) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_WHM_F_BOOST_STATE:
            if (values[i] < 0 || values[i] > 1) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_WHM_F_TANK_VOLUME:
            if (!d->has_feature(Feature::kEnergyManagement)) {
                return MT_ATTR_ERR_CLUSTER;
            }
            if (values[i] < 0 || values[i] > 0xFFFF) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_WHM_F_EST_HEAT_REQ:
            if (!d->has_feature(Feature::kEnergyManagement)) {
                return MT_ATTR_ERR_CLUSTER;
            }
            if (values[i] < 0 || values[i] > kMeasValueAbsMax) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        case MT_WHM_F_TANK_PERCENT:
            if (!d->has_feature(Feature::kTankPercent)) {
                return MT_ATTR_ERR_CLUSTER;
            }
            if (values[i] < 0 || values[i] > 100) {
                return MT_ATTR_ERR_VALUE;
            }
            break;
        default:
            return MT_ATTR_ERR_VALUE;
        }
    }

    /* Pass 2: apply, one subscription report per applied field (same-state
     * BoostState pushes included, the per-sample rule the section comment
     * contrasts with DEM), deriving the boost events on BoostState
     * transitions AS THEY ARE APPLIED, in pair order. */
    for (uint8_t i = 0; i < count; i++) {
        uint32_t attr_id;
        switch (fields[i]) {
        case MT_WHM_F_HEATER_TYPES:
            d->m_heater_types = (uint8_t)values[i];
            attr_id = Attributes::HeaterTypes::Id;
            break;
        case MT_WHM_F_HEAT_DEMAND:
            d->m_heat_demand = (uint8_t)values[i];
            attr_id = Attributes::HeatDemand::Id;
            break;
        case MT_WHM_F_BOOST_STATE: {
            /* THE STATE MACHINE (the section comment's transition table).
             * prev is the cache value this pair replaces; the write
             * happens before the emission so a Generate* helper that
             * somehow read the attribute back would see the new state,
             * and the same-state arm falls through to the per-sample
             * dirty report below with no event. */
            BoostStateEnum next = (values[i] != 0) ? BoostStateEnum::kActive
                                                   : BoostStateEnum::kInactive;
            BoostStateEnum prev = d->m_boost_state;
            d->m_boost_state    = next;
            if (prev != next) {
                if (next == BoostStateEnum::kActive) {
                    /* Inactive -> Active: BoostStarted with the cached
                     * last-accepted parameters, then CONSUME the cache
                     * (aggregate reset to duration 0 / no optionals, the
                     * "parameters unknown" default a later
                     * host-initiated boost must emit). Lock-held per the
                     * emission discipline; failure logged by the helper,
                     * never failing the push. */
                    (void)d->GenerateBoostStartedEvent(d->m_boost.duration, d->m_boost.one_shot,
                                                       d->m_boost.emergency, d->m_boost.setpoint,
                                                       d->m_boost.target_pct, d->m_boost.reheat);
                    d->m_boost = HearthWhmDelegate::BoostParams{};
                } else {
                    /* Active -> Inactive: BoostEnded. The parameter cache
                     * is NOT touched here (only emission consumes it,
                     * only an allow writes it). */
                    (void)d->GenerateBoostEndedEvent();
                }
            }
            attr_id = Attributes::BoostState::Id;
            break;
        }
        case MT_WHM_F_TANK_VOLUME:
            d->m_tank_volume = (uint16_t)values[i];
            attr_id = Attributes::TankVolume::Id;
            break;
        case MT_WHM_F_EST_HEAT_REQ:
            d->m_est_heat_req = values[i];
            attr_id = Attributes::EstimatedHeatRequired::Id;
            break;
        default: /* MT_WHM_F_TANK_PERCENT, pass 1 admits nothing else */
            d->m_tank_percent = (uint8_t)values[i];
            attr_id = Attributes::TankPercentage::Id;
            break;
        }
        MatterReportingAttributeChangeCallback(ep, Id, attr_id);
    }
    return MT_ATTR_OK;
}

/*
 * DE397 live read for the six WHM values, dispatched from
 * mt_matter_attr_read() under its StackLock: the delegate cache is the
 * served truth (the five narrow shadows are seeds, EstimatedHeatRequired
 * has no slot at all). None of the six is nullable; EstimatedHeatRequired
 * is the one signed answer (energy_mwh, the INT64S alias family).
 */
static int mt_whm_attr_read_live(uint16_t ep, uint32_t attr, int64_t *out, bool *is_unsigned)
{
    namespace WHM = chip::app::Clusters::WaterHeaterManagement;
    HearthWhmDelegate *d = whm_for(ep);
    if (d == nullptr) {
        /* Cannot happen once the boot rebuild has run; defensive. */
        if (is_unsigned) {
            *is_unsigned = false;
        }
        return MT_ATTR_ERR_FAILED;
    }
    bool u = true;
    int64_t v;
    switch (attr) {
    case WHM::Attributes::HeaterTypes::Id:    v = d->m_heater_types; break;
    case WHM::Attributes::HeatDemand::Id:     v = d->m_heat_demand; break;
    case WHM::Attributes::TankVolume::Id:     v = d->m_tank_volume; break;
    case WHM::Attributes::TankPercentage::Id: v = d->m_tank_percent; break;
    case WHM::Attributes::BoostState::Id:     v = chip::to_underlying(d->m_boost_state); break;
    case WHM::Attributes::EstimatedHeatRequired::Id:
        v = d->m_est_heat_req;
        u = false;
        break;
    default:
        /* Unreachable: only the six k_instance_served rows route here. */
        if (is_unsigned) {
            *is_unsigned = false;
        }
        return MT_ATTR_ERR_FAILED;
    }
    if (is_unsigned) {
        *is_unsigned = u;
    }
    *out = v;
    return MT_ATTR_OK;
}

/*
 * ===================================================================
 * The cluster-object heap: sizing, and the compile-time floor
 * ===================================================================
 *
 * Placed at the end of the file because every constant below is a sizeof()
 * of a class declared above it. The endpoint block heap's sizing table
 * (mt_devtypes_zephyr.cpp, beside K_HEAP_DEFINE(hearth_ep_heap)) is the
 * model this follows: a table, an explicit worst case, and a static_assert
 * so the table cannot go stale without the build noticing.
 *
 * COST PER BLOCK. One 8-aligned allocation per (endpoint, family) pair,
 * charged kObjCostOf(payload) = roundup(payload, 8) + 8. The payload is the
 * delegate followed by the raw Instance storage at the Instance's own
 * alignment (obj_pair_bytes()), or a lone object where the family has only
 * one:
 *
 *   family                  delegate  Instance  payload   heap cost
 *   OperationalState              16       240      256         264
 *   RvcOperationalState           12       248      264         272
 *   DeviceEnergyManagement       240        40      280         288
 *   MeterIdentification            -       304      304         312
 *   ElectricalPowerMeasurement   120        28      148         160
 *   WaterHeaterManagement         48        40       88          96
 *   ModeBase                      16        64       80          88
 *   MicrowaveOvenControl          12        64       76          88
 *   Chime                         12        40       56          64
 *   PowerTopology                  8        28       36          48
 *   water valve                    8         -        8          16
 *
 * (Sizes measured with nm on the 6c31f09 build, the MicrowaveOvenControl row
 * read out of the compiler on the batch-8 build; the constants below are
 * sizeof()s, so the table is documentation and the arithmetic is not.)
 *
 * COST PER DEVICE TYPE is the sum over the families its cluster list draws
 * on, from mt_devtype_create()'s claim block. Fix round I1: this table is a
 * SECOND table and it drifted from the first. It was rebuilt mechanically,
 * by running mt_devtype_create()'s ten claim predicates over every declared
 * cluster list reached from s_registry (both variants where max_variant is
 * 1), which is the only way to keep it honest; four rows were wrong before.
 *
 *   obj  device types (registry id, variant)          families claimed
 *   584  battery storage v0        0x0018 v0          DEM + ModeBase + EPM + PowerTopology
 *   440  microwave oven            0x0079             OperationalState + ModeBase + MWOC
 *   448  robotic vacuum cleaner    0x0074             RvcOpState + 2 x ModeBase
 *   392  water heater v0           0x050F v0          WHM + ModeBase + EPM + PowerTopology
 *   376  device energy mgmt        0x050D v0 and v1   DEM + ModeBase
 *   312  electrical UTILITY meter  0x0511             MeterIdentification
 *   352  cabinet, Heater           0x0071 under 0x007B  OperationalState + ModeBase
 *   264  laundry washer, dishwasher, laundry dryer
 *                                  0x0073 0x0075 0x007C   OperationalState
 *   208  electrical sensor         0x0510 v0 and v1   EPM + PowerTopology
 *        battery storage v1        0x0018 v1
 *        heat pump                 0x0309
 *        solar power               0x0017 v0 and v1
 *   184  water heater v1           0x050F v1          WHM + ModeBase
 *   160  electrical meter          0x0514 v0 and v1   EPM
 *    64  chime                     0x0146             Chime
 *    88  refrigerator              0x0070             ModeBase
 *        cabinet, Cooler           0x0071 under 0x0070 ModeBase
 *    16  water valve               0x0042             valve delegate
 *     0  the other thirty-three catalogue rows, the unparented cabinet,
 *        the cook surface, the cooktop, the oven and the extractor hood
 *        among them
 *
 * Mode select (0x0027) is deliberately in the zero row: it takes the ONE
 * process-global SupportedModesManager, not a per-endpoint object.
 *
 * What the four corrected rows were: electrical sensor v1 was listed at 160
 * (it is 208: electricalSensorPowerOnlyClusters is PowerTopology + EPM +
 * Descriptor); the 312 row was labelled "electrical meter" when
 * MeterIdentification appears in exactly one cluster list, utilityMeterClusters,
 * whose registry row is 0x0511; and battery storage v1, the heat pump and
 * solar power were all folded into the zero row when each draws 208.
 *
 * THE WORST COMPOSITION, and why this heap can never be the binding wall.
 * The draw is maximised subject to the three walls that already exist:
 * sixteen endpoints (kServiceableEndpoints), 8,112 usable bytes of endpoint
 * block heap, and the per-family caps (MT_MEAS_MAX 8 for each of EPM and
 * PowerTopology, MT_DEM_MAX 4, MT_WHM_MAX 4, MT_METER_MAX 2,
 * kModeBasePoolSlots 20). The FIVE 16-deep pools (OperationalState,
 * RvcOperationalState, Chime, valve and, since batch 8, MicrowaveOvenControl)
 * can never bind: each endpoint draws at most one of each and there are at
 * most sixteen endpoints.
 *
 * Fix round C1: this is now an EXHAUSTIVE maximisation over every
 * admissible multiset of the table's rows, not a greedy fill. The greedy
 * fill this comment used to carry sorted by object bytes per endpoint and
 * then filled, which reached for water heaters and never revisited the RVC
 * as a filler once the DEM and METER caps saturated. Swapping the four
 * water heaters for four RVCs buys 224 more object bytes and still fits the
 * block heap, so the greedy answer (6,112 B) was 224 B under the truth and
 * 16 B under what the heap then held. The maximum is:
 *
 *   13 x microwave oven      5,720 B   block 6,968   OpState 13/16, MWOC 13/16, MB 13
 *    2 x utility meter          624 B   block   256   METER 2/2
 *    1 x battery storage v0     584 B   block   856   DEM 1/4, EPM 1/8, PTOP 1/8, MB 1
 *   ---------------------------------------------------------------------
 *   16 endpoints             6,928 B   block 8,080 of 8,112 B
 *
 * CATALOGUE BATCH 8 MOVED THIS, and the microwave oven is the whole reason.
 * Three per-endpoint object pairs (OperationalState, MicrowaveOvenMode's
 * ModeBase and MicrowaveOvenControl) on a 536 B block is 0.82 object bytes
 * per block byte, against battery storage's 0.68 and the RVC's 0.52, so a
 * composition of microwaves now dominates the answer where a mix of the
 * heaviest energy types used to. The pre-batch-8 maximum was 6,336 B from
 * 4 battery + 4 RVC + 2 meter + 6 washer, 8,000 B of block; that mix is
 * still admissible, it is simply no longer the largest.
 *
 * Every wall is satisfied: 16 endpoints exactly, the block heap 32 B short
 * of its own wall, METER saturated, ModeBase 14 of 20 and OperationalState
 * 13 of 16. Nearby mixes the same search reports: 2 meter + 14 microwave
 * gives 6,784; 2 meter + 2 battery + 12 microwave wants 8,400 B of block and
 * does not fit; 15 microwave + 1 cooktop gives 6,600. A second battery is
 * what the 32 spare block bytes are 288 short of affording.
 *
 * The search is the same exhaustive maximisation over every admissible
 * multiset the fix round C1 introduced, not a greedy fill, re-run against
 * the full batch-8 catalogue with the seven new device types and their
 * thirteen realised shapes added and with kObjMwoc taken from sizeof rather
 * than estimated. It reproduces the pre-batch-8 6,336 B answer exactly when
 * the batch-8 rows are removed, which is the check that it models the same
 * problem the old comment did.
 *
 * The batch's other new object-drawing shapes do not compete and it is worth
 * saying why, since two of them look close: the heater cabinet draws 352 B
 * on a 536 B block, which is strictly worse than the microwave's 440 B on
 * the same block, and the cooler cabinet and refrigerator draw 88 B on 472
 * and 520 B blocks. Every one of them is dominated, so none appears in any
 * maximising mix.
 *
 * NOTE FOR THE LM20 TIER: 6,928 is a function of the block budget. Raising
 * HEARTH_EP_HEAP_BYTES admits object-heavier compositions, so this heap must
 * be re-derived and raised whenever that one is.
 *
 * HEARTH_OBJ_HEAP_BYTES is 7168 since catalogue batch 8, which leaves 7,088
 * usable. The assertions below pin that against the worst mix and against
 * the two uniform compositions worth naming separately, so a future delegate
 * or Instance that grows fails the build here rather than a bench run later:
 *
 *   worst mixed composition   6,928   fits, 160 B spare
 *   16 x laundry washer       4,224   fits
 *   9 x RVC (the block heap's own limit for that type)   4,032   fits
 *   15 x microwave oven (the block heap's own limit)     6,600   fits
 *
 * The resize itself landed two commits before the microwave, alone and ahead
 * of every consumer, because sizing a heap in the same diff that first draws
 * on it is how a sizing argument stops being checkable. This is the commit
 * that spends it, and the 160 B margin is the same deliberate over-sizing
 * ruling DE413 chose when the figures were 6,528 and 6,336: this heap is
 * sized ABOVE its worst case, unlike the endpoint block heap, because a
 * heap that could bite first would silently move a capacity boundary the
 * README already states.
 *
 * This is a deliberately different trade from the endpoint block heap's.
 * That heap is sized BELOW its own worst case (16 extended colour lights
 * would want 9,600 B) because the RAM is worth more elsewhere and the
 * capacity consequence is documented in the README. This heap is sized
 * ABOVE its worst case, at a cost of 112 B over the tightest fit, because
 * it is NEW: a heap that could bite first would silently move a capacity
 * boundary the README already states, and memory reclaim round A is purely
 * additive on behaviour by construction. Ruling DE413 kept that intent when
 * the number was still 6,112; the number moved, the intent did not.
 */
namespace {

constexpr size_t kObjOpState  = kObjCostOf(
    obj_pair_bytes<HearthOpStateDelegate, chip::app::Clusters::OperationalState::Instance>());
constexpr size_t kObjRvcOpState = kObjCostOf(
    obj_pair_bytes<HearthRvcOpStateDelegate,
                   chip::app::Clusters::RvcOperationalState::Instance>());
constexpr size_t kObjModeBase = kObjCostOf(
    obj_pair_bytes<HearthModeBaseDelegate, chip::app::Clusters::ModeBase::Instance>());
constexpr size_t kObjChime = kObjCostOf(
    obj_pair_bytes<HearthChimeDelegate, chip::app::Clusters::ChimeServer>());
constexpr size_t kObjEpm = kObjCostOf(
    obj_pair_bytes<HearthEpmDelegate,
                   chip::app::Clusters::ElectricalPowerMeasurement::Instance>());
constexpr size_t kObjPtop = kObjCostOf(
    obj_pair_bytes<HearthPtopDelegate, chip::app::Clusters::PowerTopology::Instance>());
constexpr size_t kObjDem = kObjCostOf(
    obj_pair_bytes<HearthDemDelegate,
                   chip::app::Clusters::DeviceEnergyManagement::Instance>());
constexpr size_t kObjWhm = kObjCostOf(
    obj_pair_bytes<HearthWhmDelegate,
                   chip::app::Clusters::WaterHeaterManagement::Instance>());
constexpr size_t kObjMeter =
    kObjCostOf(sizeof(chip::app::Clusters::MeterIdentification::Instance));
constexpr size_t kObjValve = kObjCostOf(sizeof(HearthValveDelegate));
/* Catalogue batch 8. PINNED BY sizeof, not estimated: the batch audit put
 * this pair at "about 88 B" and said outright that the worst-mix search's
 * answer depends on it, so the number below is the compiler's and the
 * static_assert under it is what stops a future SDK bump from moving the
 * worst case quietly. */
constexpr size_t kObjMwoc = kObjCostOf(
    obj_pair_bytes<HearthMwocDelegate,
                   chip::app::Clusters::MicrowaveOvenControl::Instance>());
static_assert(kObjMwoc == 88,
              "the MicrowaveOvenControl pair changed size; re-run the worst-composition search "
              "before touching the assertions below");

/* Per device type, from mt_devtype_create()'s claim block. The five the
 * worst mix uses, plus the two the standalone assertions need; the full
 * corrected table is in the comment above. */
constexpr size_t kObjPerBatteryStorageV0 = kObjDem + kObjModeBase + kObjEpm + kObjPtop;
constexpr size_t kObjPerRvc              = kObjRvcOpState + 2 * kObjModeBase;
constexpr size_t kObjPerWaterHeaterV0    = kObjWhm + kObjModeBase + kObjEpm + kObjPtop;
constexpr size_t kObjPerUtilityMeter     = kObjMeter;
constexpr size_t kObjPerWasherTrio       = kObjOpState;
/* Catalogue batch 8: the catalogue's only THREE-pair device type, and the
 * one that moved the maximum. */
constexpr size_t kObjPerMicrowaveOven    = kObjOpState + kObjModeBase + kObjMwoc;

/* The 16-endpoint mix that maximises the draw, from the exhaustive search
 * in the comment above. METER is saturated, the endpoint count is
 * saturated, and the endpoint block heap is 32 B from its own wall. */
constexpr size_t kObjWorstMixBytes = 13 * kObjPerMicrowaveOven +
                                     2 * kObjPerUtilityMeter +
                                     1 * kObjPerBatteryStorageV0;

/* Not in the worst mix, but its cost feeds the search that found the mix,
 * so it is pinned here: if the water heater grows, the search has to be
 * re-run rather than silently left stale. */
static_assert(kObjPerWaterHeaterV0 == 392,
              "the water heater's object cost moved; re-run the worst-composition search");

/* The endpoint block heap admits nine RVCs (8,112 usable / 864 per block);
 * named here so the RVC row cannot drift out of step with that heap. */
constexpr size_t kObjRvcEndpointLimit = 9;

static_assert(kObjWorstMixBytes <= kObjHeapUsableBytes,
              "the cluster-object heap no longer covers the worst composition the other walls "
              "admit; raise HEARTH_OBJ_HEAP_BYTES or redo the sizing table above");
static_assert(kObjPerWasherTrio * kServiceableEndpoints <= kObjHeapUsableBytes,
              "the cluster-object heap no longer holds sixteen appliance-trio endpoints; raise "
              "HEARTH_OBJ_HEAP_BYTES");
static_assert(kObjPerRvc * kObjRvcEndpointLimit <= kObjHeapUsableBytes,
              "the cluster-object heap no longer holds the nine RVC endpoints the endpoint "
              "block heap admits; raise HEARTH_OBJ_HEAP_BYTES");

/* Pinned so a delegate or Instance that changes size fails the build here
 * and forces the table above to be re-read, rather than silently moving the
 * worst case. */
static_assert(kObjWorstMixBytes == 6928, "the worst-composition arithmetic moved off 6,928 B");

} /* namespace */
