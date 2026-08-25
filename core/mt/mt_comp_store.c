/*
 * mt_comp_store.c - read and write the endpoint composition in NVS.
 */

#include <string.h>

#include "hearth_log.h"
#include "hearth_port.h"

#include "mt_comp_store.h"

static const char *TAG = "mt_comp_store";

#define MT_COMP_NVS_NAMESPACE "mt_ep"
#define MT_COMP_NVS_KEY       "comp"

int mt_comp_store_load(mt_composition_t *out)
{
    if (!out) {
        return -1;
    }

    uint8_t blob[MT_COMP_BLOB_MAX];
    size_t len = sizeof(blob);
    int r = hearth_kv_get_blob(MT_COMP_NVS_NAMESPACE, MT_COMP_NVS_KEY, blob, &len);
    if (r == 1) {
        return 1; /* namespace never written: factory fresh */
    }
    if (r != 0) {
        HEARTH_LOGE(TAG, "hearth_kv_get_blob failed");
        return -1;
    }

    if (mt_comp_decode(blob, len, out) != 0) {
        HEARTH_LOGE(TAG, "stored composition is corrupt (%u bytes)", (unsigned)len);
        return -1;
    }

    HEARTH_LOGI(TAG, "loaded composition: %u endpoint(s)", out->count);
    return 0;
}

int mt_comp_store_save(const mt_composition_t *comp)
{
    if (!comp) {
        return -1;
    }

    uint8_t blob[MT_COMP_BLOB_MAX];
    int n = mt_comp_encode(comp, blob, sizeof(blob));
    if (n < 0) {
        return -1;
    }

    if (hearth_kv_set_blob(MT_COMP_NVS_NAMESPACE, MT_COMP_NVS_KEY, blob, (size_t)n) != 0) {
        HEARTH_LOGE(TAG, "saving composition failed");
        return -1;
    }

    HEARTH_LOGI(TAG, "saved composition: %u endpoint(s)", comp->count);
    return 0;
}

int mt_comp_store_erase(void)
{
    int r = hearth_kv_delete(MT_COMP_NVS_NAMESPACE, MT_COMP_NVS_KEY);
    if (r == 1) {
        return 0; /* nothing stored: already unconfigured */
    }
    if (r != 0) {
        HEARTH_LOGE(TAG, "erasing composition failed");
        return -1;
    }

    HEARTH_LOGI(TAG, "composition erased, device is unconfigured");
    return 0;
}
