/*
 * hearth_matter_init.cpp - Hearth's Matter stack bring-up.
 *
 * This is the load-bearing half of the Silicon Labs sample's
 * SilabsMatterConfig::InitMatter()
 * ($MATTER_EXT_ROOT/third_party/matter_sdk/examples/platform/silabs/
 * MatterConfig.cpp, 2.8.1) without the sample's application layer: no AppTask,
 * no BaseApplication app delegate, no shell, no LCD, no RPC, no tracing, no
 * ICD, no Wi-Fi. The ORDER is the sample's and is not a matter of taste; the
 * line numbers beside each step are MatterConfig.cpp's, so a future SDK bump
 * can be diffed against them.
 *
 * Three deliberate departures from the sample, each argued where it happens:
 *
 *   1. chip::Platform::MemoryInit() is NOT called. The sample calls it only
 *      under SL_WIFI (:235); on a Thread build sl_ot_create_instance() has
 *      already called it (ThreadStackManagerImpl.cpp:184) from
 *      sl_ot_rtos_stack_init(), which sl_main runs in
 *      sl_main_second_stage_init() BEFORE app_init(). A second call is not
 *      idempotent: CHIPMem-Platform.cpp:82-87 abort()s on it.
 *   2. GetPlatform().VerifyIfUpdated() (:320) is dropped. It clears an NVM3
 *      key the Matter OTA image processor writes, and this product has no
 *      Matter OTA (FIRMWARE_UPDATE_SPEC.md): the key is never written, so the
 *      call can only ever be a no-op.
 *   3. The device attestation credentials provider is set where the sample
 *      sets it, in the caller's position (ApplicationStart():213-216, after
 *      InitMatter returns) rather than inside the Server::Init lock, because
 *      that is the order the shipping samples are tested in.
 */

#include "hearth_matter_init.h"

#include <app/DefaultTimerDelegate.h>
#include <app/clusters/network-commissioning/network-commissioning.h>
#include <app/reporting/ReportSchedulerImpl.h>
#include <app/server/Server.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <data-model-providers/codegen/Instance.h>
#include <inet/EndPointStateOpenThread.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/CommissionableDataProvider.h>
#include <platform/DeviceInfoProvider.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <platform/OpenThread/GenericNetworkCommissioningThreadDriver.h>
#include <platform/ThreadStackManager.h>
#include <platform/silabs/efr32/Efr32PsaOperationalKeystore.h>
#include <platform/silabs/platformAbstraction/SilabsPlatform.h>

#include <DeviceInfoProviderImpl.h>
#include <headers/ProvisionManager.h>

#include <cstdarg>
#include <cstdio>

#include "hearth_log.h"

/*
 * Route CHIP's own log output to Hearth's console (USART0 TX on PA00).
 *
 * The alternative is what task 2 measured and the README records: CHIP writes
 * through chip::Logging::Platform::LogV in
 * third_party/matter_sdk/src/platform/silabs/Logging.cpp:227, which calls
 * PrintLog():119 and lands in SEGGER_RTT_WriteNoLock():132 unless
 * SILABS_LOG_OUT_UART is 1 (:126). Setting that macro needs uart.h and the
 * SDK's matter_uart driver on a peripheral this port drives itself with bare
 * emlib, so it is not available; and LogV itself is a plain (non-weak)
 * definition in a component source, so it cannot be overridden by a strong
 * symbol either.
 *
 * chip::Logging::SetLogRedirectCallback() is the supported third way and needs
 * neither. TextOnlyLogging.cpp:157-168 checks the redirect before calling
 * Platform::LogV, so every ChipLogError/Progress/Detail line reaches the
 * callback instead of RTT, with no SDK driver involved. It is compiled into
 * this image already.
 *
 * What it does NOT capture, stated so nobody looks for it on the console:
 * silabsLog()/SILABS_LOG (Logging.cpp:175) and otPlatLog() (:309) call PrintLog
 * directly and still go to RTT. So do CHIP logs emitted before this callback is
 * installed.
 *
 * Set to 0 to put CHIP's logs back on RTT alone. The reason that switch exists:
 * hearth_log_write() blocks the calling task until the last byte is out of the
 * USART (hearth_port_sl.c:521-522), about 87 us per byte at 115200, and the
 * calling task here is usually the CHIP event loop. A round that finds the
 * event loop starved under commissioning traffic turns this off first and
 * measures second.
 */
#ifndef HEARTH_CHIP_LOG_TO_CONSOLE
#define HEARTH_CHIP_LOG_TO_CONSOLE 1
#endif

using namespace chip;
using namespace chip::DeviceLayer;

namespace {

/*
 * The CHIP platform layer's opaque (PSA/SE) operational keystore. Static
 * storage duration because chip::Server holds the pointer for the life of the
 * image; the sample's gOperationalKeystore (MatterConfig.cpp:73) is the same
 * object with the same lifetime.
 */
chip::DeviceLayer::Internal::Efr32PsaOperationalKeystore sOperationalKeystore;
chip::DeviceLayer::DeviceInfoProviderImpl sDeviceInfoProvider;

/*
 * The Network Commissioning cluster instance for endpoint 0 and the OpenThread
 * driver behind it. Without this the root node advertises cluster 0x0031 with
 * nothing serving it and a commissioner cannot hand over a dataset, which is
 * the whole of Thread commissioning. MatterConfig.cpp:147.
 */
app::Clusters::NetworkCommissioning::InstanceAndDriver<NetworkCommissioning::GenericThreadDriver> sThreadNetworkDriver(
    kRootEndpointId);

/*
 * chip::Server keeps the pointer it is handed in endpointNativeParams, so the
 * struct outlives Server::Init(). The sample makes it a local of InitMatter()
 * (MatterConfig.cpp:305) and gets away with it because nothing reads it after
 * init; static storage here removes the question rather than answering it.
 */
chip::Inet::EndPointStateOpenThread::OpenThreadEndpointInitParam sNativeParams;

/* The sample's, MatterConfig.cpp:325-330, with
 * CHIP_CONFIG_SYNCHRONOUS_REPORTS_ENABLED 0 (config/sl_matter_config.h). */
app::DefaultTimerDelegate sTimerDelegate;
app::reporting::ReportSchedulerImpl sReportScheduler(&sTimerDelegate);

/* MatterConfig.cpp:299. Static because Server::Init() stores pointers into it. */
CommonCaseDeviceServerInitParams sInitParams;

void LockOpenThreadTask(void)
{
    ThreadStackMgr().LockThreadStack();
}

void UnlockOpenThreadTask(void)
{
    ThreadStackMgr().UnlockThreadStack();
}

#if HEARTH_CHIP_LOG_TO_CONSOLE
void ChipLogToConsole(const char *module, uint8_t category, const char *msg, va_list args)
{
    /* 160 bytes on whichever task is logging: the CHIP event loop (8 KiB,
     * CHIP_DEVICE_CONFIG_CHIP_TASK_STACK_SIZE), the OpenThread task, or the
     * boot task during init. hearth_log_write() formats into 192 bytes of its
     * own including the level, the tag and the CRLF, so a longer CHIP line
     * would be truncated there anyway. */
    char line[160];

    if (vsnprintf(line, sizeof(line), msg, args) < 0) {
        return;
    }
    hearth_log_write(category == Logging::kLogCategory_Error ? HEARTH_LOG_ERROR : HEARTH_LOG_INFO,
                     "chip", "[%s] %s", module, line);
}
#endif

/*
 * InitOpenThread(), MatterConfig.cpp:164-187, with the non-FTD arms dropped:
 * CHIP_DEVICE_CONFIG_THREAD_FTD is 1 in src/CHIPProjectConfig.h and there is no
 * ICD server in this build, so the sample's other three SetThreadDeviceType
 * branches cannot be taken.
 */
CHIP_ERROR InitOpenThread(void)
{
    ChipLogProgress(DeviceLayer, "Initializing OpenThread stack");
    ReturnErrorOnFailure(ThreadStackMgr().InitThreadStack());
    ReturnErrorOnFailure(ConnectivityMgr().SetThreadDeviceType(ConnectivityManager::kThreadDeviceType_Router));

    sThreadNetworkDriver.Init();

    ChipLogProgress(DeviceLayer, "Starting OpenThread task");
    return ThreadStackMgrImpl().StartThreadTask();
}

} // namespace

CHIP_ERROR hearth_matter_init(const char *ble_name)
{
#if HEARTH_CHIP_LOG_TO_CONSOLE
    /* Before anything else, so the stack's own init lines are visible. */
    Logging::SetLogRedirectCallback(ChipLogToConsole);
#endif

    /* SilabsMatterConfig::AppInit():238. NvmInit() (SilabsConfig::Init() plus
     * the NVM3 key migrations) is what KeyValueStoreManagerImpl and
     * ConfigurationManagerImpl read through, so this is not optional; it also
     * latches the reset cause for GeneralDiagnostics' BootReason and calls
     * silabsInitLog(). The sample's other two AppInit steps are the main task
     * it spawns (Hearth's boot task is that task) and StartScheduler, which
     * sl_main owns. */
    ReturnErrorOnFailure(Silabs::GetPlatform().Init());

    ChipLogProgress(DeviceLayer, "Init CHIP Stack"); /* :273 */
    ReturnErrorOnFailure(PlatformMgr().InitChipStack()); /* :286 */

    /* :288. This REPLACES the advertised name rather than prefixing it:
     * BLEManagerImpl.cpp:251-266 sets kDeviceNameSet, and :466-471 is the
     * "<CHIP_DEVICE_CONFIG_BLE_DEVICE_NAME_PREFIX><discriminator>" fallback
     * that then never runs. See src/CHIPProjectConfig.h. */
    ReturnErrorOnFailure(ConnectivityMgr().SetBLEDeviceName(ble_name));

    /* :291-295. The provisioning storage is three providers at once: the
     * device instance info, the commissionable data (discriminator, spake2p
     * verifier) and, further down, the attestation credentials. */
    Silabs::Provision::Manager &provision = Silabs::Provision::Manager::GetInstance();
    ReturnErrorOnFailure(provision.Init());
    SetDeviceInstanceInfoProvider(&provision.GetStorage());
    SetCommissionableDataProvider(&provision.GetStorage());
    ChipLogProgress(DeviceLayer, "Provision mode %s", provision.IsProvisionRequired() ? "ENABLED" : "disabled");

    ReturnErrorOnFailure(InitOpenThread()); /* :302 */

    /* :305-309. CHIP's Inet layer opens its UDP endpoints on the OpenThread
     * instance and needs both the instance and the lock pair to reach it. */
    sNativeParams.lockCb                = LockOpenThreadTask;
    sNativeParams.unlockCb              = UnlockOpenThreadTask;
    sNativeParams.openThreadInstancePtr = ThreadStackMgrImpl().OTInstance();
    sInitParams.endpointNativeParams    = static_cast<void *>(&sNativeParams);

    CHIP_ERROR err = CHIP_NO_ERROR;
    {
        /* :322 and :389. Matter event handling is stopped while the server's
         * resources are set up. */
        StackLock lock;

        /* :332. Set BEFORE InitializeStaticResourcesBeforeServerInit(), which
         * would otherwise install a second scheduler of its own (Server.h:295,
         * "Injection of report scheduler WILL lead to two schedulers being
         * allocated"). */
        sInitParams.reportScheduler = &sReportScheduler;

        /* :344-345. The opaque PSA key store instead of the default one, which
         * keeps the operational private keys inside the Secure Element. Init()
         * takes no argument on this platform
         * (Efr32PsaOperationalKeystore.h:64). */
        ReturnErrorOnFailure(sOperationalKeystore.Init());
        sInitParams.operationalKeystore = &sOperationalKeystore;

        ReturnErrorOnFailure(sInitParams.InitializeStaticResourcesBeforeServerInit()); /* :349 */

        /* :350, and it must come AFTER the line above: the provider is
         * constructed around the persistent storage delegate that call
         * installs. Server::Init() fails outright on a null dataModelProvider
         * (Server.h:210-213). This is the generated ember/ZAP model in
         * data_model/. */
        sInitParams.dataModelProvider = app::CodegenDataModelProviderInstance(sInitParams.persistentStorageDelegate);

        /* :373-374. No cluster in this data model reads the device info
         * provider (it serves FixedLabel, UserLabel, LocalizationConfiguration
         * and TimeFormatLocalization, none of which is enabled), and
         * Server::Init() tolerates a null one (Server.cpp:218-222). It is set
         * anyway, because the first round to enable a label cluster would
         * otherwise find a null pointer rather than a missing line. */
        sDeviceInfoProvider.SetStorageDelegate(sInitParams.persistentStorageDelegate);
        SetDeviceInfoProvider(&sDeviceInfoProvider);

        err = Server::GetInstance().Init(sInitParams); /* :377 */
    }
    ReturnErrorOnFailure(err); /* :391, reported after the lock is dropped */

    ReturnErrorOnFailure(PlatformMgr().StartEventLoopTask()); /* :394 */

    /* ApplicationStart():213-216: the sample sets this after InitMatter has
     * returned, i.e. with the event loop already running, under the lock. */
    {
        StackLock lock;
        Credentials::SetDeviceAttestationCredentialsProvider(&Silabs::Provision::Manager::GetInstance().GetStorage());
    }

    HEARTH_LOGI("matter", "stack up: server initialised, event loop running");
    return CHIP_NO_ERROR;
}
