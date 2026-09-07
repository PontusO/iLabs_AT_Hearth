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
#include "mt_matter.h"
#include "mt_ota.h"
}

using namespace chip;
using namespace chip::DeviceLayer;

namespace {

const char *TAG = "hearth_ota_esp";

class HearthConfigurationManager : public ConfigurationManagerImpl
{
public:
    CHIP_ERROR GetSoftwareVersion(uint32_t &v) override
    {
        auto &sv = hearth::software_version();
        if (sv.have) {
            v = sv.version;
            return CHIP_NO_ERROR;
        }
        return ConfigurationManagerImpl::GetSoftwareVersion(v);
    }
    CHIP_ERROR GetSoftwareVersionString(char *buf, size_t bufSize) override
    {
        auto &sv = hearth::software_version();
        if (sv.have) {
            VerifyOrReturnError(bufSize > strlen(sv.str), CHIP_ERROR_BUFFER_TOO_SMALL);
            strcpy(buf, sv.str);
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
    auto &sv = hearth::software_version();
    sv.have = (mt_ota_swver_stored(&sv.version, sv.str, sizeof(sv.str)) == 0);
    SetConfigurationMgr(&sConfigMgr);
    if (sv.have) {
        HEARTH_LOGI(TAG, "product version %lu \"%s\" in force", (unsigned long)sv.version, sv.str);
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
    sv.have = true;
    sv.version = version;
    strncpy(sv.str, str, sizeof(sv.str) - 1);
    sv.str[sizeof(sv.str) - 1] = '\0';

    /* AT parser task: the reporting engine is CHIP-context-only. */
    PlatformMgr().ScheduleWork([](intptr_t) {
        MatterReportingAttributeChangeCallback(0, app::Clusters::BasicInformation::Id,
                                               app::Clusters::BasicInformation::Attributes::SoftwareVersion::Id);
        MatterReportingAttributeChangeCallback(0, app::Clusters::BasicInformation::Id,
                                               app::Clusters::BasicInformation::Attributes::SoftwareVersionString::Id);
    }, 0);
    return 0;
}
