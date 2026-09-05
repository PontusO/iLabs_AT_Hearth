/* hearth_kvid.h - (ns, key) to NVM3 object id. Pure C, host-tested. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define HEARTH_KV_BASE 0x0A000u   /* Hearth-owned NVM3 key range; CHIP's keys live elsewhere */
#define HEARTH_KV_SPAN 0x1000u
uint32_t hearth_kv_id(const char *ns, const char *key);
#ifdef __cplusplus
}
#endif
