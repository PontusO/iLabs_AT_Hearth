/*
 * hearth_ota_sl.cpp - the MG24 arm of firmware over the air: the
 * ConfigurationManager that serves the persisted product version, the live
 * Basic Information update, and the point where Hearth's requestor is wired.
 *
 * The CHIP half (requestor, driver, storage, BDX downloader and the image
 * processor that relays every block to the host) is
 * platform/common/hearth_ota_requestor.cpp, shared with the C6 and the nRF.
 * The SDK contributes matter_ota_requestor only: CHIP's generic requestor
 * sources, with no platform image processor. matter_ota_support (the efr32
 * processor writing a Gecko storage slot) is deliberately absent: the image
 * store is the HOST, which flashes the co-processor back through the
 * uart-xmodem bootloader, and a storage slot would not fit beside this image
 * anyway (the xG24 slot starts at 0x080EA000, inside the application).
 *
 * Nothing else on this platform starts a requestor: BaseApplication and
 * OTAConfig, the sample's wiring, are not compiled. So unlike the C6 there is
 * no race for chip::SetRequestorInstance(), and the wiring sits right after
 * Server::Init(), as on the nRF.
 */
#include <cstring>

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/reporting/reporting.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ConfigurationManager.h>
#include <platform/silabs/ConfigurationManagerImpl.h>

#include "hearth_ota_requestor.h"
#include "hearth_ota_sl.h"

extern "C" {
#include "hearth_log.h"
#include "hearth_port.h"
#include "mt_matter.h"
#include "mt_ota.h"
}

using namespace chip;
using namespace chip::DeviceLayer;

namespace {

const char *TAG = "hearth_ota_sl";

/*
 * Two tasks touch hearth::software_version(): the AT parser task writes it
 * from AT+MTSWVER, and the Matter thread reads it whenever a controller reads
 * Basic Information. The version is one word and would be safe on its own, but
 * the STRING is a 32-byte copy, so a reader landing mid-strncpy would encode a
 * torn value onto the wire. HEARTH_CRIT_OTA covers both sides; it is the OTA
 * module's own section, it is never held across a blocking call here (a
 * compare and a copy of at most 32 bytes), and the reads happen on the Matter
 * thread while the only writer is the AT parser task, so the two can never
 * deadlock on it. Same shape as the C6's and the nRF's, on purpose.
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
 * hearth_matter_init(), before PlatformMgr().InitChipStack(), which runs
 * ConfigurationMgr().Init() on whichever instance is installed by then.
 */
void hearth_swver_install(void)
{
    SetConfigurationMgr(&sConfigMgr);
}

/*
 * hearth_matter_init(), after InitChipStack() and before Server::Init():
 * Basic Information starts answering reads at Server::Init(), and
 * DefaultOTARequestor::Init() (inside hearth_ota_wire()) reads the current
 * software version once and keeps it. hearth_kv_* opens NVM3 lazily on its
 * first call, so there is no lower bound beyond the stack being up.
 */
void hearth_swver_load(void)
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
 * The two Basic Information attributes are not stored values here either:
 * the cluster answers every read of SoftwareVersion and
 * SoftwareVersionString out of ConfigurationMgr()
 * (BasicInformationCluster.cpp's ReadSoftwareVersion), which is the manager
 * installed above. What is left is to tell subscribers the value moved.
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

    /* AT parser task: the reporting engine is CHIP-context-only.
     * Checked, because the failure is silent: a full event queue would drop the
     * report and every subscriber would keep the old version until its next
     * read, with nothing saying why. The stored value is already correct at
     * this point, so this is a reporting loss and not a data loss, which is why
     * it logs rather than failing the command. */
    CHIP_ERROR err = PlatformMgr().ScheduleWork([](intptr_t) {
        MatterReportingAttributeChangeCallback(0, app::Clusters::BasicInformation::Id,
                                               app::Clusters::BasicInformation::Attributes::SoftwareVersion::Id);
        MatterReportingAttributeChangeCallback(0, app::Clusters::BasicInformation::Id,
                                               app::Clusters::BasicInformation::Attributes::SoftwareVersionString::Id);
    }, 0);
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE(TAG, "could not schedule the Basic Information report (%" CHIP_ERROR_FORMAT
                         "); subscribers keep the old version until their next read",
                    err.Format());
    }
    return 0;
}

/*
 * hearth_matter_init(), inside the stack-lock block, after Server::Init()
 * has succeeded and before the event loop starts.
 *
 * WHY THE SECOND CALL IS DEFERRED: DefaultOTARequestorDriver::Init() does not
 * call the image processor's ConfirmCurrentImage() inline, it posts a
 * SystemLayer().ScheduleLambda() that does, and that lambda sets the pending
 * flag ota_requestor_server_ready() acts on. Called as the next statement it
 * would run before the driver's lambda and a freshly applied bundle would
 * never be reported. Posting our own lambda here queues it behind the
 * driver's: both are kChipLambdaEvent posts to the one FreeRTOS event queue,
 * which is FIFO, and the event loop that drains it starts after this
 * returns.
 *
 * Not from platform/chip/mt_chip_events.cpp's kServerReady: Server::Init()
 * can post kServerReady itself, before this wiring exists.
 */
void hearth_ota_wire(void)
{
    hearth::ota_requestor_init();
    CHIP_ERROR err = SystemLayer().ScheduleLambda([] { hearth::ota_requestor_server_ready(); });
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE(TAG, "could not schedule the server-ready hook (%" CHIP_ERROR_FORMAT
                         "); a pending NotifyUpdateApplied is lost for this boot",
                    err.Format());
    }
}
