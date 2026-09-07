/*
 * hearth_ota_esp.cpp - the ESP32-C6 arm of firmware over the air: the
 * ConfigurationManager that serves the persisted product version, the
 * boot-time load, and the live Basic Information notification.
 */
#include <cstring>

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/reporting/reporting.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ConfigurationManager.h>
#include <platform/ESP32/ConfigurationManagerImpl.h>

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

const char *TAG = "hearth_ota_esp";

/*
 * Two tasks touch hearth::software_version(): the AT parser task writes it
 * from AT+MTSWVER, and the Matter thread reads it whenever a controller reads
 * Basic Information. The version is one word and would be safe on its own, but
 * the STRING is a 32-byte copy, so a reader landing mid-strncpy would encode a
 * torn value onto the wire. HEARTH_CRIT_OTA covers both sides; it is the OTA
 * module's own section, it is never held across a blocking call here (a
 * compare and a copy of at most 32 bytes), and the reads happen on the Matter
 * thread while the only writer is the AT parser task, so the two can never
 * deadlock on it.
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

/* Before any Matter initialisation (app_main, after nvs_flash_init). */
void hearth_swver_install()
{
    /* Read into locals first: mt_ota_swver_stored() goes to the key-value
     * store, and hearth_port.h forbids a blocking call inside a critical
     * section. Nothing else is running yet at this point in app_main, so the
     * section is belt rather than braces, but the shape stays the same as
     * mt_matter_swver_set()'s so the two cannot drift apart. */
    uint32_t version = 0;
    char str[sizeof(hearth::SoftwareVersion::str)] = "";
    bool have = (mt_ota_swver_stored(&version, str, sizeof(str)) == 0);

    auto &sv = hearth::software_version();
    hearth_crit_enter(HEARTH_CRIT_OTA);
    sv.version = version;
    memcpy(sv.str, str, sizeof(sv.str));
    sv.have = have;
    hearth_crit_exit(HEARTH_CRIT_OTA);

    SetConfigurationMgr(&sConfigMgr);
    if (have) {
        HEARTH_LOGI(TAG, "product version %lu \"%s\" in force", (unsigned long)version, str);
    }
}

/*
 * The two Basic Information attributes are NOT stored values on this platform,
 * so there is nothing to write: esp-matter creates both with
 * ATTRIBUTE_FLAG_MANAGED_INTERNALLY (esp_matter_cluster.cpp's
 * basic_information::create), which makes esp_matter::attribute::update()
 * answer ESP_ERR_NOT_SUPPORTED outright, and CHIP's BasicInformationCluster
 * answers every read of SoftwareVersion / SoftwareVersionString straight out
 * of ConfigurationMgr() (BasicInformationCluster.cpp's ReadSoftwareVersion).
 * Installing HearthConfigurationManager above is therefore the whole of the
 * value change; all that is left is to tell anyone subscribed that the value
 * moved. Verified on the bench: chip-tool reads the host-declared version back
 * with no attribute write anywhere in this file.
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
