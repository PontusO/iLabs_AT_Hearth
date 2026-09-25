/*
 * mt_devtypes_sl_tables_b3.inc: a fragment of mt_devtypes_sl.cpp, NOT a compile unit.
 *
 * Holds device tables: catalogue batch 3 (nRF 1248-1484).
 * mt_devtypes_sl.cpp #includes it inside its anonymous namespace, in
 * order with the other fragments, so the whole is still one translation
 * unit: file-local names stay file-local and the generated code is the
 * same as when this text lived in the .cpp. Split out 2026-09-25 so each
 * file fits a small model's context (graph T549, DE550). Do not add it
 * to hearth.slcp.
 */

/* ---- door lock (0x000A) (nRF 1248-1357) ------------------------
 *
 * Catalogue batch 3 audit, DoorLock (0x0101). The first cluster on this
 * platform whose commands need an application VERDICT, so the audit asked a
 * fourth question beyond the usual three: where does the app's answer
 * attach, and what does the SDK do with it.
 *
 *   Code-driven? No. door-lock-server is the only door-lock directory in
 *   the tree, and DoorLock is absent from the CodeDrivenClusters list
 *   (src/app/common/templates/config-data.yaml:144-168). Confirmed
 *   empirically: regenerating hearth.zap with the cluster on ep240 left
 *   zap-generated/CodeDrivenInitShutdown.cpp byte-identical.
 *
 *   AAI? Yes, wildcard-endpoint (DoorLockServer itself derives from
 *   AttributeAccessInterface, door-lock-server.h:99, constructed with
 *   Optional<EndpointId>::Missing() at :102 and registered from
 *   MatterDoorLockPluginServerInitCallback, door-lock-server.cpp:4312).
 *   Harmless for this attribute set: Read() (:4421) has a case for
 *   nothing but the nine Aliro attributes and then `default: break;
 *   return CHIP_NO_ERROR`, i.e. falls through to the attribute store. None
 *   of the Aliro attributes is declared below, so every read of this
 *   endpoint reaches the arena. There is no Write() override.
 *
 *   ServerInit? YES, and this one is REQUIRED, not merely worth running.
 *   The catch is the name. door-lock-cluster.xml:44 declares
 *   <server tick="false" init="false">, so ZAP puts NO init function in the
 *   cluster's function array and emberAfDoorLockClusterServerInitCallback
 *   (with "Server") is declared but never called. The hook that IS called
 *   is emberAfDoorLockClusterInitCallback (no "Server"), dispatched from
 *   emberAfClusterInitCallback() inside initializeEndpoint()
 *   (attribute-storage.cpp:480). Unlike the LevelControl/ColorControl
 *   inits below, that path DOES run for a dynamic endpoint: it is reached
 *   through emberAfEndpointEnableDisable(), which emberAfSetDynamicEndpoint
 *   calls itself (:393), and the isEnabled bit is set BEFORE
 *   initializeEndpoint runs (:991-996), so the endpoint index resolves.
 *   So this cluster does NOT join the B388 call site: the strong override
 *   of emberAfDoorLockClusterInitCallback below is what calls
 *   DoorLockServer::InitEndpoint(), and ember drives it. That is also what
 *   the nRF lock sample does (nrf/samples/matter/lock/src/
 *   zcl_callbacks.cpp:97-99), though it uses the deprecated InitServer
 *   alias and discards the error; see the override for why this port does
 *   neither.
 *
 *   The delegate surface: NOTHING is link-mandatory. All thirty
 *   emberAfPluginDoorLock* application hooks carry weak defaults in
 *   door-lock-server-callback.cpp (the first at :46), which
 *   app_config_dependent_sources.cmake:19 compiles unconditionally
 *   alongside the server. Feature gating is at RUNTIME, off the FeatureMap
 *   attribute (GetFeatures(), door-lock-server.cpp:1471), not by #ifdef, so
 *   a lock with FeatureMap 0 never reaches the user/credential/schedule
 *   hooks at all. This firmware therefore overrides exactly two of them,
 *   emberAfPluginDoorLockOnDoorLockCommand and ...OnDoorUnlockCommand
 *   (mt_matter_sl.cpp), and leaves the other twenty-eight to their weak
 *   stubs. Same two the C6 defines.
 *
 * FeatureMap 0 is conformant. Every feature in DoorLock's feature list is
 * optionalConform, and USR is mandatory only under (PIN|RID|FPG|FACE),
 * which none of them is here; the device type agrees
 * (matter-devices.xml:2003-2025). ZAP's own default FeatureMap of 0x0001
 * (door-lock-cluster.xml:49) is a seed value in that file, not a
 * requirement.
 *
 * AutoRelockTime is OPTIONAL and is declared anyway, deliberately. This is
 * bug B129 from the C6, and it reproduces verbatim in this tree: the 7-arg
 * SetLockState() ends an UNLOCK with
 * VerifyOrReturnError(GetAutoRelockTime(endpointId, autoRelockTime), false)
 * (door-lock-server.cpp:206). With the attribute absent that read fails and
 * the function returns false for an unlock that actually happened and
 * actually emitted its LockOperation event, which turns every host
 * AT+MTLOCK unlock into a bare ERROR with the state changed underneath.
 * Seeded 0, which disables auto-relock. If a controller writes it non-zero
 * the server relocks at expiry through its own timer
 * (DoorLockOnAutoRelockCallback, :4342) and the host observes the LockState
 * change as a +MTATTR URC, the same path as any controller write.
 *
 * LockDoor and UnlockDoor are the two mandatory commands and the only two
 * declared. Both are mustUseTimedInvoke in door-lock-cluster.xml's command
 * list. UnlockWithTimeout and UnboltDoor are not declared, so their
 * handlers are unreachable and the extra hooks they would need stay
 * unwritten. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(doorLockAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(DoorLock::Attributes::LockState::Id, ENUM8, 1,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(DoorLock::Attributes::LockType::Id, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DoorLock::Attributes::ActuatorEnabled::Id, BOOLEAN, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DoorLock::Attributes::AutoRelockTime::Id, INT32U, 4,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(DoorLock::Attributes::OperatingMode::Id, ENUM8, 1,
                              ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(DoorLock::Attributes::SupportedOperatingModes::Id, BITMAP16, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(DoorLock::Attributes::FeatureMap::Id, BITMAP32, 4, 0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kDoorLockIncoming[] = { DoorLock::Commands::LockDoor::Id,
                                            DoorLock::Commands::UnlockDoor::Id,
                                            kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(doorLockClusters)
DECLARE_DYNAMIC_CLUSTER(DoorLock::Id, doorLockAttrs, ZAP_CLUSTER_MASK(SERVER), kDoorLockIncoming,
                        nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(doorLockEndpoint, doorLockClusters);

constexpr EmberAfDeviceType kDoorLockTypes[] = { { 0x000A, 3 } };

/* ---- water valve (0x0042) (nRF 1358-1484) ---------------------
 *
 * Catalogue batch 3 audit, ValveConfigurationAndControl (0x0081). The other
 * verdict type, and the one that needs a DELEGATE OBJECT rather than free
 * ember callbacks.
 *
 *   Code-driven? No, despite appearances. The directory's files were
 *   renamed valve-configuration-and-control-server.cpp to -cluster.cpp in
 *   2025 and its BUILD.gn is now an empty group(), which makes it LOOK
 *   like a port to the ServerCluster path. It is not: there is no
 *   CodegenIntegration.cpp, no ServerClusterInterface subclass, and the
 *   cluster is absent from CodeDrivenClusters (config-data.yaml:144-168).
 *   Confirmed empirically the same way as the door lock:
 *   CodeDrivenInitShutdown.cpp came back byte-identical.
 *
 *   AAI? Yes, wildcard-endpoint, and for exactly ONE attribute.
 *   ValveConfigAndControlAttrAccess (cluster.cpp:131, constructed with
 *   Optional<EndpointId>::Missing() at :134) answers RemainingDuration from
 *   a private shadow array gRemainingDuration[] (:61-67) and falls through
 *   the default: for everything else without encoding, which the provider
 *   reads as "not handled" (CodegenDataModelProvider_Read.cpp:112 tries the
 *   AAI first, :59 decides what counts as handled). No Write() override. So
 *   OpenDuration, DefaultOpenDuration, CurrentState and TargetState are
 *   plain ember external storage against the arena; RemainingDuration is
 *   the BooleanState-shaped split, documented in the seed comment below and
 *   in the platform README rather than bridged: it is a firmware-managed
 *   countdown with no host write path, so there is nothing for a bridge to
 *   push.
 *
 *   ServerInit? No per-endpoint init at all. The only init is
 *   MatterValveConfigurationAndControlPluginServerInitCallback()
 *   (cluster.cpp:528), which takes no endpoint, runs once from
 *   MATTER_PLUGINS_INIT, and does nothing but register that AAI. This
 *   cluster does NOT join the B388 call site.
 *
 *   The delegate: chip::app::Clusters::ValveConfigurationAndControl::
 *   Delegate (delegate.h:34), three pure virtuals (:40-42), registered per
 *   endpoint with the free function SetDefaultDelegate(EndpointId,
 *   Delegate*) (cluster.h:40, impl cluster.cpp:262). Catalogue batch 3 adds
 *   this port's mt_devtype_create() claim and SetDefaultDelegate() call, and
 *   with them the ordering rule this forces and why it differs from the
 *   C6's.
 *
 * FeatureMap 0. TS (bit 0) is left clear DELIBERATELY and not merely by
 * omission: with TS set but no Time Synchronization cluster server on the
 * image, SetValveLevel() returns CHIP_ERROR_NOT_IMPLEMENTED (cluster.cpp:
 * 336) and every Open answers Status::Failure. LVL (bit 1) is optional and
 * not taken, which is why CurrentLevel and TargetLevel are not declared
 * (AT_MT_SPEC.md 3.19 documents the consequence for AT+MTVALVE's <level>,
 * and it is the same consequence the C6 has for the same reason).
 * AutoCloseTime is TS-gated and absent for the same reason.
 *
 * ValveFault (0x0009) is optional and is NOT declared. It is the cluster's
 * only escape hatch for failing a command on the wire: both handlers check
 * it first and answer AddClusterSpecificFailure(kFailureDueToFault)
 * (cluster.cpp:445 for Open, :511 for Close) before the delegate is
 * consulted at all. Declaring it would not help. The +MTCMD verdict arrives
 * INSIDE the delegate call, which is past that check, so a deny still could
 * not fail the in-flight command; the attribute would only let a host
 * pre-arm a fault for the NEXT command, which is not what the verdict frame
 * is for and is not a surface AT_MT_SPEC.md 3.19 describes. Left out, and
 * the "verdict cannot fail the valve command" property stays exactly what
 * the spec already documents.
 *
 * ---- the auto-close re-entry (fix round, I1) --------------------------
 *
 * One consequence of declaring DefaultOpenDuration writable, which the XML
 * makes it and the mandatory set requires: a TIMED open is reachable on
 * this build, and the server closes the valve itself when the countdown
 * expires. That path re-enters the delegate.
 *
 * onValveConfigurationAndControlTick() (cluster.cpp:214) decrements
 * RemainingDuration and re-arms a 1 s SystemLayer timer through
 * startRemainingDurationTick() (:235, :248); on the terminal tick it calls
 * CloseValve() instead (:250-252), and CloseValve() calls
 * delegate->HandleCloseValve() (:303). So a single controller Open with a
 * duration produces a SECOND, UNSOLICITED +MTCMD for cluster 129 command 1
 * that no controller asked for, raised from timer context.
 *
 * That forward's verdict is MEANINGLESS, and worse than the ordinary valve
 * case. CloseValve() has already set TargetState kClosed (:283), set
 * CurrentState kTransitioning (:285), nulled OpenDuration (:287) and
 * RemainingDuration (:296), cancelled the tick timer (:298) and emitted
 * ValveStateChanged (:300) BEFORE the delegate is called at all. A deny
 * cannot undo any of it, so the fabric-visible state changes either way.
 * It also blocks the CHIP event loop for up to the mailbox's full 1000 ms
 * from a timer callback.
 *
 * Not fixed here, and no in-scope fix exists: the forward happens because
 * HandleCloseValve() is the only hook the SDK offers and it cannot tell an
 * invoke from an auto-close, and core owns the blocking semantics. The C6
 * has exactly the same behaviour for exactly the same reason. Documented
 * in the platform README so a host author knows a 129/1 forward may be
 * server-initiated; the AT_MT_SPEC amendment and the bench case are the
 * controller's. */
HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_BEGIN(valveAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ValveConfigurationAndControl::Attributes::OpenDuration::Id, ELAPSED_S, 4,
                          ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ValveConfigurationAndControl::Attributes::DefaultOpenDuration::Id,
                              ELAPSED_S, 4,
                              ZAP_ATTRIBUTE_MASK(NULLABLE) | ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ValveConfigurationAndControl::Attributes::RemainingDuration::Id,
                              ELAPSED_S, 4, ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ValveConfigurationAndControl::Attributes::CurrentState::Id, ENUM8, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ValveConfigurationAndControl::Attributes::TargetState::Id, ENUM8, 1,
                              ZAP_ATTRIBUTE_MASK(NULLABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(ValveConfigurationAndControl::Attributes::FeatureMap::Id, BITMAP32, 4,
                              0),
    HEARTH_DECLARE_CONST_ATTRIBUTE_LIST_END();

constexpr CommandId kWaterValveIncoming[] = { ValveConfigurationAndControl::Commands::Open::Id,
                                              ValveConfigurationAndControl::Commands::Close::Id,
                                              kInvalidCommandId };

HEARTH_DECLARE_CONST_CLUSTER_LIST_BEGIN(waterValveClusters)
DECLARE_DYNAMIC_CLUSTER(ValveConfigurationAndControl::Id, valveAttrs, ZAP_CLUSTER_MASK(SERVER),
                        kWaterValveIncoming, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr),
    HEARTH_DECLARE_CONST_CLUSTER_LIST_END;

HEARTH_DECLARE_CONST_ENDPOINT(waterValveEndpoint, waterValveClusters);

constexpr EmberAfDeviceType kWaterValveTypes[] = { { 0x0042, 1 } };

