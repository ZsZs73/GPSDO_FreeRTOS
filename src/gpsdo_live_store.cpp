/* ======================================================================
 * gpsdo_live_store.cpp  —  live-data persistence over the flash ring buffer
 * Part of GPSDO FreeRTOS v1.07
 * See gpsdo_live_store.h for the design and payload layout.
 * ====================================================================== */

#include "gpsdo_live_store.h"
#include "gpsdo_flash_ring.h"
#include "gpsdo_state.h"
#include "gpsdo_algorithms.h"
#include <string.h>
#include <math.h>

/* hysteresis thresholds (see header) */
#define LS_DRIFT_LSB   8.0f
#define LS_DAMP_DELTA  0.03f
#define LS_MIN_MS      (20u * 60u * 1000u)   /* 20 minutes */

#define LS_PAYLOAD     10u
/* last-saved values, to measure change since the previous write */
static float    s_last_drift = 0.0f;
static float    s_last_damp  = 1.0f;
static uint32_t s_last_ms    = 0;
static bool     s_have_last  = false;
static bool     s_force_save = false;

/* ---- little-endian pack/unpack helpers ---- */

static void ls_put_f(uint8_t *p, float v)  { memcpy(p, &v, 4); }
static void ls_put_u16(uint8_t *p, uint16_t v){ p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }
static float    ls_get_f(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }
static uint16_t ls_get_u16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1] << 8)); }

static void ls_pack(uint8_t *buf)
{
    float drift = g_lrn_drift;
    float damp  = g_lrn_damp;
    uint16_t pwm = DEFAULT_PWM_OUTPUT;

    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        pwm = gCtrl.pwm_output;
    } else if (xSemaphoreTake(xCtrlMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        pwm = gCtrl.pwm_output;
        xSemaphoreGive(xCtrlMutex);
    }

    ls_put_f  (buf + 0, drift);
    ls_put_f  (buf + 4, damp);
    ls_put_u16(buf + 8, pwm);
}

static void ls_apply(const uint8_t *buf)
{
    g_lrn_drift = ls_get_f(buf + 0);
    g_lrn_damp  = ls_get_f(buf + 4);

    if (g_lrn_damp < LRN_DAMP_LO) g_lrn_damp = LRN_DAMP_LO;
    if (g_lrn_damp > LRN_DAMP_HI) g_lrn_damp = LRN_DAMP_HI;

    uint16_t pwm = ls_get_u16(buf + 8);
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        gCtrl.pwm_output = pwm;
    } else if (xSemaphoreTake(xCtrlMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        gCtrl.pwm_output = pwm;
        xSemaphoreGive(xCtrlMutex);
    }

    s_last_drift = g_lrn_drift;
    s_last_damp  = g_lrn_damp;
    s_have_last  = true;
}

live_load_status_t live_store_begin_status(void)
{
    uint8_t buf[LS_PAYLOAD];
    uint16_t n = 0u;
    flash_ring_read_status_t rs =
        flash_ring_read_newest_ex(REC_LIVE, buf, sizeof(buf), &n);

    if (rs == FLASH_RING_READ_MISSING)   return LIVE_LOAD_MISSING;
    if (rs == FLASH_RING_READ_CHECKSUM)  return LIVE_LOAD_CHECKSUM;
    if (rs == FLASH_RING_READ_NOT_READY) return LIVE_LOAD_STORAGE;
    if (rs == FLASH_RING_READ_LENGTH)    return LIVE_LOAD_SIZE;

    const bool recovered = (rs == FLASH_RING_READ_RECOVERED);
    if (rs != FLASH_RING_READ_OK && !recovered)
        return LIVE_LOAD_STORAGE;
    if (n != LS_PAYLOAD) return LIVE_LOAD_SIZE;

    ls_apply(buf);
    return recovered ? LIVE_LOAD_RECOVERED : LIVE_LOAD_OK;
}

bool live_store_begin(void)
{
    live_load_status_t st = live_store_begin_status();
    return st == LIVE_LOAD_OK || st == LIVE_LOAD_RECOVERED;
}

static bool ls_should_save(uint32_t now_ms)
{
    if (s_force_save) return true;
    if (!s_have_last) {
        /* first save allowed once we have any learned value away from theory */
        return (fabsf(g_lrn_drift) > LS_DRIFT_LSB) ||
               (fabsf(g_lrn_damp - 1.0f) > LS_DAMP_DELTA);
    }
    /* time gate first (cheap), then magnitude gate */
    if ((uint32_t)(now_ms - s_last_ms) < LS_MIN_MS) return false;
    if (fabsf(g_lrn_drift - s_last_drift) > LS_DRIFT_LSB) return true;
    if (fabsf(g_lrn_damp  - s_last_damp ) > LS_DAMP_DELTA) return true;
    return false;
}

void live_store_tick(uint32_t now_ms)
{
    if (flash_ring_slot_count() == 0) return;   /* ring disabled → no-op */
    if (!ls_should_save(now_ms)) return;

    uint8_t buf[LS_PAYLOAD];
    ls_pack(buf);
    if (flash_ring_write(REC_LIVE, buf, LS_PAYLOAD)) {
        s_last_drift = g_lrn_drift;
        s_last_damp  = g_lrn_damp;
        s_last_ms    = now_ms;
        s_have_last  = true;
        s_force_save = false;
    }
    /* Failed forced writes keep s_force_save set so the next tick retries. */
}

void live_store_request_save(void)
{
    s_force_save = true;
}
