/* hearth_kvid.c - FNV-1a over "ns/key", folded into the Hearth key range. */
#include <stdint.h>
#include "hearth_kvid.h"

uint32_t hearth_kv_id(const char *ns, const char *key)
{
    uint32_t h = 2166136261u;
    for (const char *p = ns; *p; p++)  { h ^= (uint8_t)*p; h *= 16777619u; }
    h ^= '/'; h *= 16777619u;
    for (const char *p = key; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    return HEARTH_KV_BASE + (h % HEARTH_KV_SPAN);
}
