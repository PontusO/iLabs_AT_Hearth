/*
 * hearth_ota_requestor.cpp - see the header. Everything here runs on the
 * Matter thread except the three mt_matter_ota_* shims, which arrive on the
 * AT parser task and hop across with PlatformMgr().ScheduleWork().
 *
 * Memory rule (spec 8.4): the statics below are the whole idle cost; the
 * block buffer is a hearth_stage_alloc() block that exists only between
 * PrepareDownload and the end of the attempt (Apply, Abort, or the requestor
 * returning to idle). It outlives Finalize on purpose: the last block is
 * still in it when the transfer closes.
 */
#include "hearth_ota_requestor.h"

#include <cstdio>
#include <cstring>

#include <app/clusters/ota-requestor/BDXDownloader.h>
#include <app/clusters/ota-requestor/DefaultOTARequestor.h>
#include <app/clusters/ota-requestor/DefaultOTARequestorDriver.h>
#include <app/clusters/ota-requestor/DefaultOTARequestorStorage.h>
#include <app/clusters/ota-requestor/OTARequestorInterface.h>
#include <app/server/Server.h>
#include <lib/core/OTAImageHeader.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/OTAImageProcessor.h>
#include <system/SystemClock.h>

extern "C" {
#include "hearth_log.h"
#include "hearth_port.h"
#include "mt_matter.h"
#include "mt_ota.h"
}

using namespace chip;
using namespace chip::DeviceLayer;

namespace {

const char *TAG = "hearth_ota";

constexpr uint32_t kAckTimeoutMs = 5000;   /* spec 6: no acknowledgement in 5 s aborts */
constexpr uint16_t kBlockSize    = 1024;   /* BDX block; fits one stage block with room */

/*
 * True once a state line carrying a CAUSE has already gone out for the
 * current update attempt (an abort, a cancellation, a discontinuation).
 * This CHIP revision has no OTARequestorDriver::HandleError; the one hook
 * that sees every failure is HandleIdleStateEnter(), which also sees the
 * ordinary returns to idle, so without this latch a host would get both the
 * specific line and a generic one for the same event. Cleared when the
 * requestor next leaves idle.
 */
bool sCauseReported;

/* A first run of a freshly applied bundle has been confirmed and the provider
 * still has to be told. Set by ConfirmCurrentImage(), acted on when the port
 * says the server is ready. */
bool sNotifyPending;

/* Defined below the statics; the driver needs it and is declared first. */
void release_block_buffer();

/* ---- image processor ---------------------------------------------------- */

class HearthOtaImageProcessor : public OTAImageProcessorInterface
{
public:
    void SetDownloader(BDXDownloader *d) { mDownloader = d; }

    CHIP_ERROR PrepareDownload() override
    {
        VerifyOrReturnError(mDownloader != nullptr, CHIP_ERROR_INCORRECT_STATE);
        if (mBuf == nullptr) {
            mBuf = static_cast<uint8_t *>(hearth_stage_alloc(kBlockSize));
        }
        VerifyOrReturnError(mBuf != nullptr, CHIP_ERROR_NO_MEMORY);
        mParams.downloadedBytes = 0;
        mParams.totalFileBytes  = 0;
        mSeq        = 0;
        mPendingLen = 0;
        mLastPercent = 0;
        mFinalized = false;
        sCauseReported = false;
        mHeaderParser.Init();
        /* Not synchronously: BeginPrepareDownload() is still on the stack. */
        return SystemLayer().ScheduleLambda([this] {
            mt_ota_on_state(MT_OTA_DOWNLOADING, "0");
            mDownloader->OnPreparedForDownload(CHIP_NO_ERROR);
        });
    }

    CHIP_ERROR ProcessBlock(ByteSpan &block) override
    {
        VerifyOrReturnError(mDownloader != nullptr && mBuf != nullptr, CHIP_ERROR_INCORRECT_STATE);
        VerifyOrReturnError(block.size() <= kBlockSize, CHIP_ERROR_BUFFER_TOO_SMALL);

        /* The whole file, header included, goes to the host: the host's bundle
         * parser skips the Matter header itself and can check its digest. The
         * header is parsed here only for the total size, on a copy of the span
         * so the original is not advanced. */
        if (mHeaderParser.IsInitialized()) {
            ByteSpan probe = block;
            OTAImageHeader header;
            CHIP_ERROR err = mHeaderParser.AccumulateAndDecode(probe, header);
            if (err == CHIP_NO_ERROR) {
                mParams.totalFileBytes = header.mPayloadSize + (block.size() - probe.size());
                mHeaderParser.Clear();
            } else if (err != CHIP_ERROR_BUFFER_TOO_SMALL) {
                return err;
            }
        }

        memcpy(mBuf, block.data(), block.size());
        mPendingLen = block.size();
        if (mt_ota_on_block(mSeq, mBuf, mPendingLen) != 0) {
            HEARTH_LOGW(TAG, "relay refused block %lu (mode 0 or busy)", (unsigned long)mSeq);
            return CHIP_ERROR_INCORRECT_STATE;
        }
        return SystemLayer().StartTimer(System::Clock::Milliseconds32(kAckTimeoutMs), OnAckTimeout, this);
    }

    /*
     * The downloader calls this straight after ProcessBlock() for the block
     * carrying the end-of-file marker (BDXDownloader.cpp's own TODO says as
     * much), so the LAST block is still pending and unacknowledged here.
     * +MTOTA:DOWNLOADED is therefore NOT raised here but from BlockAcked(),
     * once that last block is in the host's hands: the whole point of the
     * line is "you now hold the bundle, judge it", and the bench showed the
     * eager version both lying about that and being immediately overwritten
     * by the last block's own progress line, so AT+MTOTASTAGED answered
     * +MTERR:12. The acknowledgement timer is deliberately left running: a
     * host that goes quiet on the last block must abort like any other.
     */
    CHIP_ERROR Finalize() override
    {
        mFinalized = true;
        if (mPendingLen == 0) {
            mt_ota_on_state(MT_OTA_DOWNLOADED, nullptr);
        }
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR Apply() override
    {
        Release();
        mt_ota_on_apply();
        return CHIP_NO_ERROR;
    }

    /*
     * Called more than once for one aborted transfer, and the bench proved it:
     * BDXDownloader::EndDownload() calls Abort(), then polls the transfer,
     * and the poll's error event runs CleanupOnError(), which goes idle FIRST
     * and calls Abort() again after. Freeing is idempotent, but a second
     * +MTOTA:ERROR,abort AFTER the closing +MTOTA:IDLE is a lie about the
     * state, so the line is under the same latch as every other cause.
     */
    CHIP_ERROR Abort() override
    {
        SystemLayer().CancelTimer(OnAckTimeout, this);
        Release();
        if (!sCauseReported) {
            sCauseReported = true;
            mt_ota_on_state(MT_OTA_ERROR, "abort");
        }
        return CHIP_NO_ERROR;
    }

    /* The host already verified the bundle and the running product version
     * equals the requestor's target by construction (AT+MTSWVER at boot), so
     * a first run is confirmed unconditionally. */
    bool IsFirstImageRun() override
    {
        OTARequestorInterface *req = GetRequestorInstance();
        VerifyOrReturnError(req != nullptr, false);
        uint32_t current;
        VerifyOrReturnError(ConfigurationMgr().GetSoftwareVersion(current) == CHIP_NO_ERROR, false);
        return req->GetCurrentUpdateState() == OTARequestorInterface::OTAUpdateStateEnum::kApplying &&
               req->GetTargetVersion() == current;
    }

    /*
     * The driver calls this exactly once, on a first run of a freshly applied
     * bundle. There is nothing to confirm on this port (the host flashed the
     * image and would not have booted it otherwise), but two things do follow,
     * and neither can be done here: the provider has to be told, and it cannot
     * be told before there is a network, since the requestor is wired before
     * the radio is up. See ota_requestor_server_ready().
     */
    CHIP_ERROR ConfirmCurrentImage() override
    {
        sNotifyPending = true;
        return CHIP_NO_ERROR;
    }

    /*
     * Free the block buffer outside the Abort() path. It is needed because a
     * transfer that RAN TO COMPLETION leaves the downloader in kComplete, not
     * kInProgress, and BDXDownloader::EndDownload() only reaches Abort() from
     * kInProgress: a host that answers AT+MTOTASTAGED=0 (or AT+MTOTA=0) after
     * +MTOTA:DOWNLOADED would otherwise strand the buffer for the life of the
     * boot. The driver calls this whenever the requestor returns to idle.
     */
    void ReleaseBuffer() { Release(); }

    /* Matter thread, from mt_matter_ota_block_acked(). */
    void BlockAcked(uint32_t seq)
    {
        if (mDownloader == nullptr || seq != mSeq) {
            return;
        }
        SystemLayer().CancelTimer(OnAckTimeout, this);
        mParams.downloadedBytes += mPendingLen;
        mPendingLen = 0;
        mSeq++;
        if (mFinalized) {
            /* That was the end-of-file block: the host holds the whole bundle
             * now, and a progress line after DOWNLOADED would only walk the
             * state backwards. */
            mt_ota_on_state(MT_OTA_DOWNLOADED, nullptr);
        } else if (mParams.totalFileBytes > 0) {
            uint8_t pct = static_cast<uint8_t>((mParams.downloadedBytes * 100) / mParams.totalFileBytes);
            if (pct >= mLastPercent + 5 || pct == 100) {
                mLastPercent = pct;
                char d[8];
                snprintf(d, sizeof(d), "%u", pct);
                mt_ota_on_state(MT_OTA_DOWNLOADING, d);
            }
        }
        /* The end-of-file block is acknowledged after Finalize() has already
         * closed the transfer; asking for more data then is not an error, it
         * is simply nothing left to ask for. */
        if (mDownloader->GetState() == OTADownloader::State::kInProgress) {
            mDownloader->FetchNextData();
        }
    }

private:
    /*
     * The whole update attempt is cancelled, not merely the BDX transfer:
     * BDXDownloader::EndDownload() ends the transfer with reason kSuccess, so
     * DefaultOTARequestor never records an error and would sit in kDownloading
     * until its six-hour watchdog. CancelImageUpdate() ends the transfer (which
     * still reaches Abort() below, so the host still gets ERROR,abort) and then
     * resets the requestor to idle, which is what AT+MTOTA? must report.
     */
    static void OnAckTimeout(System::Layer *, void *ctx)
    {
        auto *self = static_cast<HearthOtaImageProcessor *>(ctx);
        HEARTH_LOGW(TAG, "host did not acknowledge block %lu in %lu ms", (unsigned long)self->mSeq,
                    (unsigned long)kAckTimeoutMs);
        mt_ota_block_timeout();
        OTARequestorInterface *req = GetRequestorInstance();
        if (req != nullptr) {
            req->CancelImageUpdate();
        } else if (self->mDownloader != nullptr) {
            self->mDownloader->EndDownload(CHIP_ERROR_TIMEOUT);
        }
    }

    /*
     * mt_ota_block_timeout() first, always: it is the core's only
     * URC-free "forget the pending block" call, and the block it is asked to
     * forget points into mBuf. Freeing first would leave AT+MTOTAGET able to
     * read freed memory for as long as it takes the host to notice the state
     * line.
     */
    void Release()
    {
        SystemLayer().CancelTimer(OnAckTimeout, this);
        mt_ota_block_timeout();
        if (mBuf != nullptr) {
            hearth_stage_free(mBuf);
            mBuf = nullptr;
        }
        mPendingLen = 0;
    }

    BDXDownloader *mDownloader = nullptr;
    OTAImageHeaderParser mHeaderParser;
    uint8_t *mBuf = nullptr;
    uint32_t mSeq = 0;
    size_t mPendingLen = 0;
    uint8_t mLastPercent = 0;
    bool mFinalized = false;
};

/* ---- driver: relays state, holds the apply for the host's verdict ------- */

class HearthOtaDriver : public DefaultOTARequestorDriver
{
public:
    void UpdateAvailable(const UpdateDescription &update, System::Clock::Seconds32 delay) override
    {
        char d[16];
        snprintf(d, sizeof(d), "%lu", (unsigned long)update.softwareVersion);
        mt_ota_on_state(MT_OTA_AVAILABLE, d);
        DefaultOTARequestorDriver::UpdateAvailable(update, delay);
    }
    /* Spec 7.1: the download completing does not apply; the host answers
     * AT+MTOTASTAGED first, which calls ApplyUpdate() or CancelImageUpdate(). */
    void UpdateDownloaded() override {}
    void UpdateSuspended(System::Clock::Seconds32 delay) override
    {
        char d[16];
        snprintf(d, sizeof(d), "%lu", (unsigned long)delay.count());
        mt_ota_on_state(MT_OTA_DEFERRED, d);
        DefaultOTARequestorDriver::UpdateSuspended(delay);
    }
    void UpdateDiscontinued() override
    {
        sCauseReported = true;
        mt_ota_on_state(MT_OTA_DISCONTINUED, nullptr);
        DefaultOTARequestorDriver::UpdateDiscontinued();
    }
    void UpdateCancelled() override
    {
        /* CancelImageUpdate() ends the BDX transfer first, so an in-flight
         * download has already said ERROR,abort by the time this runs; only
         * a cancellation with nothing downloading needs its own line. */
        if (!sCauseReported) {
            sCauseReported = true;
            mt_ota_on_state(MT_OTA_ERROR, "cancelled");
        }
        DefaultOTARequestorDriver::UpdateCancelled();
    }
    void HandleIdleStateExit() override
    {
        sCauseReported = false;
        DefaultOTARequestorDriver::HandleIdleStateExit();
    }
    /*
     * The one hook this CHIP revision gives an application for "the update
     * attempt is over", successfully or not: DefaultOTARequestor routes every
     * RecordErrorUpdateState() through RecordNewUpdateState(kIdle) and thence
     * here, with the CHIP_ERROR mapped down to a reason. A cause line already
     * sent (abort, cancel, discontinue) suppresses the generic one, and the
     * closing IDLE always goes out so AT+MTOTA? reads IDLE again. The latch is
     * cleared when the NEXT attempt leaves idle, never here: the SDK can still
     * call back into the processor after this point.
     */
    void HandleIdleStateEnter(IdleStateReason reason) override
    {
        if (!sCauseReported && reason != IdleStateReason::kIdle) {
            sCauseReported = true;
            mt_ota_on_state(MT_OTA_ERROR,
                            reason == IdleStateReason::kInvalidSession ? "session" : "failed");
        }
        mt_ota_on_state(MT_OTA_IDLE, nullptr);
        release_block_buffer();
        DefaultOTARequestorDriver::HandleIdleStateEnter(reason);
    }
    /* Mode 0: announcements and the periodic timer are ignored. */
    void ProcessAnnounceOTAProviders(
        const ProviderLocationType &loc,
        app::Clusters::OtaSoftwareUpdateRequestor::OTAAnnouncementReason reason) override
    {
        if (!hearth::ota_mode_enabled()) {
            return;
        }
        DefaultOTARequestorDriver::ProcessAnnounceOTAProviders(loc, reason);
    }
    void SendQueryImage() override
    {
        if (!hearth::ota_mode_enabled()) {
            return;
        }
        mt_ota_on_state(MT_OTA_QUERYING, nullptr);
        DefaultOTARequestorDriver::SendQueryImage();
    }
};

DefaultOTARequestor sRequestor;
DefaultOTARequestorStorage sStorage;
HearthOtaDriver sDriver;
BDXDownloader sDownloader;
HearthOtaImageProcessor sProcessor;
bool sModeEnabled;
hearth::SoftwareVersion sSoftwareVersion;

void release_block_buffer() { sProcessor.ReleaseBuffer(); }

} // namespace

namespace hearth {

void ota_requestor_init()
{
    VerifyOrReturn(GetRequestorInstance() == nullptr);
    sProcessor.SetDownloader(&sDownloader);
    sDownloader.SetImageProcessorDelegate(&sProcessor);
    sStorage.Init(Server::GetInstance().GetPersistentStorage());
    sRequestor.Init(Server::GetInstance(), sStorage, sDriver, sDownloader);
    SetRequestorInstance(&sRequestor);
    sDriver.SetMaxDownloadBlockSize(kBlockSize);
    /* The driver's own Init() would send NotifyUpdateApplied straight away on
     * a first run, which on this port is a second and a half into the boot,
     * with no network and no possible CASE session to the provider: the bench
     * showed the command failing and the requestor dropping to idle with the
     * provider none the wiser. Turned off here and sent from
     * ota_requestor_server_ready() instead. */
    sDriver.SetSendNotifyUpdateApplied(false);
    sDriver.Init(&sRequestor, &sProcessor);
    HEARTH_LOGI(TAG, "OTA requestor wired, mode %d", sModeEnabled ? 1 : 0);
}

void ota_requestor_server_ready()
{
    VerifyOrReturn(sNotifyPending);
    sNotifyPending = false;
    /*
     * Clear the persisted "applying" state first. Nothing else does: this port
     * has no OTA partition, so it has none of the one-shot
     * ESP_OTA_IMG_PENDING_VERIFY bookkeeping the stock ESP32 image processor
     * leans on, and DefaultOTARequestor only rewrites the stored state from
     * Reset() and ApplyUpdate(). Left alone, every later boot would look like
     * a first run and re-send the notification.
     */
    sStorage.StoreCurrentUpdateState(OTARequestorInterface::OTAUpdateStateEnum::kIdle);
    HEARTH_LOGI(TAG, "first run of the applied bundle confirmed, notifying the provider");
    sRequestor.NotifyUpdateApplied();
}

bool ota_mode_enabled() { return sModeEnabled; }

SoftwareVersion &software_version() { return sSoftwareVersion; }

} // namespace hearth

/* ---- the C shims, AT parser task -> Matter thread ----------------------
 *
 * These take no ChipStackLock, unlike the rest of the mt_matter_* bridge.
 * PlatformMgr().ScheduleWork() is the sanctioned way into the CHIP context
 * from another task and takes the lock itself; taking it here as well would
 * only widen the window in which the AT parser task holds the Matter stack.
 * The one thing read outside it is GetRequestorInstance(), a plain pointer
 * written once at boot.
 */

extern "C" int mt_matter_ota_set_mode(int mode)
{
    if (GetRequestorInstance() == nullptr) {
        return -1;
    }
    if (mode == 0) {
        sModeEnabled = false;
        PlatformMgr().ScheduleWork([](intptr_t) {
            if (sRequestor.GetCurrentUpdateState() != OTARequestorInterface::OTAUpdateStateEnum::kIdle) {
                sRequestor.CancelImageUpdate();
            }
        }, 0);
        return 0;
    }
    if (mode == 1) {
        sModeEnabled = true;
        return 0;
    }
    PlatformMgr().ScheduleWork([](intptr_t) {
        mt_ota_on_state(MT_OTA_QUERYING, nullptr);
        /* kUndefinedFabricIndex spelled out: the default argument lives on
         * OTARequestorInterface's declaration, and DefaultOTARequestor's
         * override restates the parameter without it, so a call through the
         * concrete type does not inherit it. */
        if (sRequestor.TriggerImmediateQuery(kUndefinedFabricIndex) != CHIP_NO_ERROR) {
            mt_ota_on_state(MT_OTA_ERROR, "noprovider");
        }
    }, 0);
    return 0;
}

extern "C" int mt_matter_ota_block_acked(uint32_t seq)
{
    if (GetRequestorInstance() == nullptr) {
        return -1;
    }
    PlatformMgr().ScheduleWork([](intptr_t s) { sProcessor.BlockAcked(static_cast<uint32_t>(s)); },
                               static_cast<intptr_t>(seq));
    return 0;
}

extern "C" int mt_matter_ota_staged(int ok, int reason)
{
    if (GetRequestorInstance() == nullptr) {
        return -1;
    }
    if (ok) {
        PlatformMgr().ScheduleWork([](intptr_t) { sRequestor.ApplyUpdate(); }, 0);
    } else {
        HEARTH_LOGW(TAG, "host refused the bundle, reason %d", reason);
        PlatformMgr().ScheduleWork([](intptr_t) { sRequestor.CancelImageUpdate(); }, 0);
    }
    return 0;
}
