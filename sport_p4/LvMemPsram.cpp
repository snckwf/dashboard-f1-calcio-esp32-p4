/* ============================================================================
 *  LvMemPsram - allocatore di LVGL in PSRAM
 *
 *  Con LV_STDLIB_CLIB LVGL usa malloc(): sotto i 4 KB il core ESP32 alloca in
 *  RAM interna, e le ~1000 etichette di questo progetto si mangiavano quasi
 *  tutta la RAM interna (59 KB liberi), lasciando senza memoria gli handshake
 *  TLS. Qui ogni allocazione di LVGL va in PSRAM (32 MB).
 *  Attivo con LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM in lv_conf.h.
 * ========================================================================== */
#include <lvgl.h>
#include <string.h>
#include "esp_heap_caps.h"

static constexpr uint32_t CAPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

extern "C" {

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    (void)mem;
    (void)bytes;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, CAPS);
    if (p == NULL) p = malloc(size);
    return p;
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *q = heap_caps_realloc(p, new_size, CAPS);
    if (q == NULL && new_size > 0) q = realloc(p, new_size);
    return q;
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    if (mon_p != NULL) memset(mon_p, 0, sizeof(*mon_p));
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

}  /* extern "C" */
