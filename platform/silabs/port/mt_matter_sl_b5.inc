/*
 * mt_matter_sl_b5.inc: a fragment of mt_matter_sl.cpp, NOT a compile unit.
 *
 * Holds bridge code: catalogue batch 5a (nRF mt_matter_zephyr.cpp
 * 3130-3167, the AT+MTSWITCH bridge), included at the end of
 * mt_matter_sl.cpp after mt_matter_sl_b4.inc, so the whole is still one
 * translation unit. Do not add it to hearth.slcp.
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
