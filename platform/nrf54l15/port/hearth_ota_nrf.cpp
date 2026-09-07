/*
 * hearth_ota_nrf.cpp - the nRF54L15 arm of firmware over the air: the two
 * symbols NCS's common Matter code calls under CONFIG_CHIP_OTA_REQUESTOR
 * (defined here so NCS's own DFU wiring is never compiled and never linked),
 * the ConfigurationManager that serves the persisted product version, and the
 * live Basic Information update.
 *
 * Why the two symbols live here rather than in NCS's ota_util.cpp: that file
 * is not in this platform's CMakeLists.txt source list, deliberately. It
 * instantiates chip::DeviceLayer::OTAImageProcessorImpl, which writes a
 * downloaded image into an MCUboot secondary slot with dfu_target and
 * dfu_multi_image. This firmware has a single MCUboot slot and no secondary,
 * because the image store is the HOST: the requestor downloads, the core
 * relay hands every block up over AT, and the host flashes the co-processor
 * back over serial recovery (FIRMWARE_UPDATE_SPEC). So the DFU machinery is
 * neither compiled nor wanted, and the two entry points NCS's common code
 * calls unconditionally under CONFIG_CHIP_OTA_REQUESTOR are supplied here
 * against Hearth's own requestor instead:
 *
 *   matter_event_handler.cpp  kServerReady -> InitBasicOTARequestor()
 *   matter_init.cpp           pre-server    -> OtaConfirmNewImage()
 */
#include <cstring>

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/reporting/reporting.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ConfigurationManager.h>
#include <platform/Zephyr/ConfigurationManagerImpl.h>

#include "hearth_ota_requestor.h"

extern "C" {
#include "hearth_log.h"
#include "hearth_port.h"
#include "mt_matter.h"
#include "mt_ota.h"
}

using namespace chip;
using namespace chip::DeviceLayer;

namespace {

const char *TAG = "hearth_ota_nrf";

/*
 * Two tasks touch hearth::software_version(): the AT parser task writes it
 * from AT+MTSWVER, and the Matter thread reads it whenever a controller reads
 * Basic Information. The version is one word and would be safe on its own, but
 * the STRING is a 32-byte copy, so a reader landing mid-strncpy would encode a
 * torn value onto the wire. HEARTH_CRIT_OTA covers both sides; it is the OTA
 * module's own section, it is never held across a blocking call here (a
 * compare and a copy of at most 32 bytes), and the reads happen on the Matter
 * thread while the only writer is the AT parser task, so the two can never
 * deadlock on it. Same shape as the C6's hearth_ota_esp.cpp, on purpose.
 */
class HearthConfigurationManager : public ConfigurationManagerImpl
{
public:
    CHIP_ERROR GetSoftwareVersion(uint32_t &v) override
    {
        auto &sv = hearth::software_version();
        hearth_crit_enter(HEARTH_CRIT_OTA);
        bool have = sv.have;
        uint32_t version = sv.version;
        hearth_crit_exit(HEARTH_CRIT_OTA);
        if (have) {
            v = version;
            return CHIP_NO_ERROR;
        }
        return ConfigurationManagerImpl::GetSoftwareVersion(v);
    }
    CHIP_ERROR GetSoftwareVersionString(char *buf, size_t bufSize) override
    {
        auto &sv = hearth::software_version();
        char copy[sizeof(sv.str)];
        hearth_crit_enter(HEARTH_CRIT_OTA);
        bool have = sv.have;
        memcpy(copy, sv.str, sizeof(copy));
        hearth_crit_exit(HEARTH_CRIT_OTA);
        if (have) {
            VerifyOrReturnError(bufSize > strlen(copy), CHIP_ERROR_BUFFER_TOO_SMALL);
            strcpy(buf, copy);
            return CHIP_NO_ERROR;
        }
        return ConfigurationManagerImpl::GetSoftwareVersionString(buf, bufSize);
    }
};

HearthConfigurationManager sConfigMgr;

} // namespace

/*
 * main(), first statement: before Nrf::Matter::PrepareServer(), which runs
 * InitChipStack() and with it ConfigurationMgr().Init(). Installing the
 * manager afterwards would leave the default instance initialised and this
 * one not.
 */
void hearth_swver_install()
{
    SetConfigurationMgr(&sConfigMgr);
}

/*
 * main(), between PrepareServer() and StartServer(). Split from the install
 * above because this one reads the key-value store, and unlike the C6 (whose
 * NVS is up after nvs_flash_init()) the settings backend behind hearth_kv_*
 * on this platform is the same Zephyr settings subsystem CHIP initialises in
 * PrepareServer(). It must still land before StartServer(), for two reasons:
 * Basic Information starts answering reads there, and kServerReady (fired
 * from inside StartServer()'s chain) is where the requestor is wired, and
 * DefaultOTARequestor::Init() reads the current software version once and
 * keeps it (spec 5.1).
 */
void hearth_swver_load()
{
    /* Read into locals first: mt_ota_swver_stored() goes to the key-value
     * store, and hearth_port.h forbids a blocking call inside a critical
     * section. */
    uint32_t version = 0;
    char str[sizeof(hearth::SoftwareVersion::str)] = "";
    bool have = (mt_ota_swver_stored(&version, str, sizeof(str)) == 0);

    auto &sv = hearth::software_version();
    hearth_crit_enter(HEARTH_CRIT_OTA);
    sv.version = version;
    memcpy(sv.str, str, sizeof(sv.str));
    sv.have = have;
    hearth_crit_exit(HEARTH_CRIT_OTA);

    if (have) {
        HEARTH_LOGI(TAG, "product version %lu \"%s\" in force", (unsigned long)version, str);
    }
}

/*
 * The two Basic Information attributes are not stored values on this platform
 * either: the generic cluster implementation answers every read of
 * SoftwareVersion and SoftwareVersionString straight out of ConfigurationMgr()
 * (BasicInformationCluster.cpp's ReadSoftwareVersion), which is the manager
 * installed above. Installing it is therefore the whole of the value change,
 * and all that is left is to tell anyone subscribed that the value moved.
 *
 * The requestor keeps its own copy of the current version, read once in
 * DefaultOTARequestor::Init(), so a version declared after boot reaches
 * QueryImage from the next boot (spec 5.1).
 */
extern "C" int mt_matter_swver_set(uint32_t version, const char *str)
{
    auto &sv = hearth::software_version();
    hearth_crit_enter(HEARTH_CRIT_OTA);
    sv.version = version;
    strncpy(sv.str, str, sizeof(sv.str) - 1);
    sv.str[sizeof(sv.str) - 1] = '\0';
    sv.have = true;
    hearth_crit_exit(HEARTH_CRIT_OTA);

    /* AT parser task: the reporting engine is CHIP-context-only. */
    PlatformMgr().ScheduleWork([](intptr_t) {
        MatterReportingAttributeChangeCallback(0, app::Clusters::BasicInformation::Id,
                                               app::Clusters::BasicInformation::Attributes::SoftwareVersion::Id);
        MatterReportingAttributeChangeCallback(0, app::Clusters::BasicInformation::Id,
                                               app::Clusters::BasicInformation::Attributes::SoftwareVersionString::Id);
    }, 0);
    return 0;
}

/* ---- what NCS's matter_init.cpp / matter_event_handler.cpp call ---------- */

namespace Nrf::Matter {

/*
 * DefaultEventHandler's kServerReady case, on the Matter thread. Nothing else
 * on this platform starts a requestor, so this one call is the whole of the
 * wiring; ota_requestor_init() is idempotent and a no-op if an instance
 * somehow already exists. Unlike the C6, where esp-matter's own requestor has
 * to be beaten to chip::SetRequestorInstance() and the wiring therefore
 * happens straight after esp_matter::start(), nothing here competes for it,
 * so the wiring sits where NCS puts it.
 *
 * WHY THE SECOND CALL IS DEFERRED and not simply the next statement. The C6
 * wires the requestor at one point in the boot and calls
 * ota_requestor_server_ready() from a later kServerReady event, so it never
 * had to think about the gap between them. Here they are the same event, and
 * the gap matters: DefaultOTARequestorDriver::Init() does not call the image
 * processor's ConfirmCurrentImage() inline, it posts a
 * SystemLayer().ScheduleLambda() that does
 * (DefaultOTARequestorDriver.cpp's Init), and that lambda is what sets the
 * pending flag ota_requestor_server_ready() acts on. Called as the next
 * statement it would run BEFORE the driver's lambda, see no notification
 * owed, and every first run of a freshly applied bundle would leave the
 * provider none the wiser.
 *
 * Posting our own lambda from here puts it behind the driver's in the same
 * queue: both go through PlatformEventing::ScheduleLambdaBridge, which posts
 * a kChipLambdaEvent to the platform event queue, and Zephyr's queue is a
 * k_msgq, so the order the two were posted in is the order they run in. The
 * driver's is posted first, inside the ota_requestor_init() above.
 */
void InitBasicOTARequestor()
{
    hearth::ota_requestor_init();
    SystemLayer().ScheduleLambda([] { hearth::ota_requestor_server_ready(); });
}

/*
 * Single-slot MCUboot has no tentative image to confirm: there is no swap,
 * no revert, and boot_is_img_confirmed() would be answering about an image
 * this firmware never wrote. The host verified the bundle before it flashed
 * it and confirms by declaring the new version over AT+MTSWVER (spec 7.5),
 * which is the confirmation that actually means something here.
 */
void OtaConfirmNewImage() {}

} // namespace Nrf::Matter
