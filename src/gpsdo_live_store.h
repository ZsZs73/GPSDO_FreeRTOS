/* ======================================================================
 * gpsdo_live_store.h  —  live-data persistence (LRN + LC + PWM) over the ring
 *
 * Part of GPSDO FreeRTOS v1.07
 *
 * Sits between the volatile "live" values (learned drift/damping, LC
 * calibration, last PWM) and the wear-levelled flash ring buffer. It packs
 * them into one 27-byte slot and applies hysteresis so the flash is written
 * only when a value has genuinely settled on a new level:
 *
 *   - drift changed by > 8 LSB  since last save, OR
 *   - damp  changed by > 0.03   since last save,
 *   AND at least 20 min have elapsed since the last save.
 *   - LC calibration change requests an immediate save (rare, user-driven).
 *
 * Payload layout (27 bytes):
 *   [0..3]   lrn_drift    float
 *   [4..7]   lrn_damp     float
 *   [8..11]  ns_per_volt  float
 *   [12..15] zero_offset  float
 *   [16..19] range_ns     float
 *   [20..21] pwm          uint16
 *   [22..25] centre_v     float
 *   [26]     algo         uint8
 * ====================================================================== */
#ifndef GPSDO_LIVE_STORE_H
#define GPSDO_LIVE_STORE_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    LIVE_LOAD_OK = 0,
    LIVE_LOAD_RECOVERED,
    LIVE_LOAD_MISSING,
    LIVE_LOAD_CHECKSUM,
    LIVE_LOAD_SIZE,
    LIVE_LOAD_STORAGE
} live_load_status_t;

/* Detailed load for startup diagnostics. */
live_load_status_t live_store_begin_status(void);

/* Compatibility wrapper. */
bool live_store_begin(void);

/* Periodic tick from the control task (once per second is plenty). Applies
 * hysteresis and writes a new ring slot when the criteria are met. now_ms is
 * a millisecond timebase (millis()). */
void live_store_tick(uint32_t now_ms);

/* Request an immediate save on the next tick (used after a successful LC
 * calibration). */
void live_store_request_save(void);

#endif /* GPSDO_LIVE_STORE_H */
