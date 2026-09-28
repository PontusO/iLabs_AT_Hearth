/*
 * hearth_ota_sl.h - the MG24 arm of firmware over the air, as
 * hearth_matter_init.cpp calls it. The CHIP half is shared
 * (platform/common/hearth_ota_requestor.cpp); this port supplies the
 * version seam and the wiring point.
 */
#pragma once

/* Before PlatformMgr().InitChipStack(): installs the ConfigurationManager
 * that answers the host-declared product version. */
void hearth_swver_install(void);

/* After InitChipStack(), before Server::Init(): loads the stored product
 * version into force. */
void hearth_swver_load(void);

/* Under the stack lock, after a successful Server::Init(): wires Hearth's
 * requestor and registers the handler that sends a pending first-run
 * notification once Thread has attached (hearth_ota_sl.cpp). */
void hearth_ota_wire(void);
