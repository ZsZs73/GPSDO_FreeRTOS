/* ======================================================================
 * gpsdo_live_store.h  —  learned/runtime persistence over the flash ring
 *
 * REC_LIVE owns runtime state which changes during normal operation and is
 * useful across warm restarts. It does not own user configuration, algorithm
 * selection, or hardware calibration.
 *
 * Current payload (10 bytes, little-endian):
 *   [0..3]  learned drift    float
 *   [4..7]  learned damping  float
 *   [8..9]  last PWM         uint16_t
 *
 * Automatic writes keep the existing wear policy: a 20-minute minimum interval
 * plus material learned-value change. Explicit save requests bypass that time
 * gate and retry after a failed write.
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
 * runtime state). */
void live_store_request_save(void);

#endif /* GPSDO_LIVE_STORE_H */
