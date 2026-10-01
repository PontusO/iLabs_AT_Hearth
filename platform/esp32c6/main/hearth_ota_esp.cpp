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
 * B665: the requestor is Hearth's, on every C6 image. esp-matter would install
 * its own from its kDnssdInitialized handler (esp_matter_ota_requestor_start),
 * and on the combined image (platform DNS-SD with WiFi compiled in:
 * mdns_init() calls its init callback synchronously) that event is posted
 * during Server::Init(), before esp_matter::start() returns: Hearth's wiring
 * in app_main then won or
 * lost a race for the stack lock (lost 14 of 14 uncommissioned boots and 3 of
 * 10 commissioned ones on the combined image, bench 2026-10-01). The SDK patch
 * sdk-patches/esp-matter/0002-hearth-app-owned-ota-requestor.patch makes
 * esp-matter's start a no-op when this answers 1, so the wiring in app_main
 * is the only one there is. A strong definition, so it overrides the patch's
 * weak declaration on every image.
 */
extern "C" int mt_app_owns_ota_requestor(void)
{
    return 1;
}

namespace {

/*
 * How long after the network comes up before the first-run NotifyUpdateApplied
 * goes out. The notification needs a CASE session to the provider, so the
 * provider's address has to resolve and a route has to exist: on WiFi that
 * follows the IPv6 address within a second or two; on Thread the OMR address
 * (SLAAC) and the SRP registration follow the attach, the same ten seconds the
 * MG24 port settles for (hearth_ota_sl.cpp).
 */
constexpr System::Clock::Seconds32 kNotifySettleWifi{ 5 };
constexpr System::Clock::Seconds32 kNotifySettleThread{ 10 };

void NotifyAfterSettle(System::Layer *, void *)
{
    hearth::ota_requestor_server_ready();
}

void ArmNotify(System::Clock::Seconds32 settle)
{
    SystemLayer().CancelTimer(NotifyAfterSettle, nullptr);
    CHIP_ERROR err = SystemLayer().StartTimer(settle, NotifyAfterSettle, nullptr);
    if (err != CHIP_NO_ERROR) {
        HEARTH_LOGE(TAG, "could not arm the first-run notification timer (%" CHIP_ERROR_FORMAT
                         "); a pending NotifyUpdateApplied waits for the next network event",
                    err.Format());
    }
}

} // namespace

/*
 * Matter thread, from app_event_cb(). The first-run NotifyUpdateApplied waits
 * for the network, not for kServerReady: on the WiFi image the two coincide
 * (minimal mDNS posts kDnssdInitialized, and with it kServerReady, only once an
 * address exists), but on the combined image kServerReady arrives about a
 * second before the WiFi address and long before a Thread attach, and
 * a notification sent then fails and is never retried (the shared code clears
 * the applying state before it sends). The requestor is wired well before the
 * earliest of these timers fires, so the driver's ConfirmCurrentImage() lambda
 * has set the pending flag by then. ota_requestor_server_ready() is one-shot:
 * later addresses and attaches are no-ops once it has run.
 */
void hearth_ota_network_event(const ChipDeviceEvent *event)
{
    switch (event->Type) {
    case DeviceEventType::kInterfaceIpAddressChanged:
        if (event->InterfaceIpAddressChanged.Type == InterfaceIpChangeType::kIpV6_Assigned) {
            ArmNotify(kNotifySettleWifi);
        }
        break;
    case DeviceEventType::kThreadConnectivityChange:
        if (event->ThreadConnectivityChange.Result == kConnectivity_Established) {
            ArmNotify(kNotifySettleThread);
        } else if (event->ThreadConnectivityChange.Result == kConnectivity_Lost) {
            SystemLayer().CancelTimer(NotifyAfterSettle, nullptr);
        }
        break;
    default:
        break;
    }
}
