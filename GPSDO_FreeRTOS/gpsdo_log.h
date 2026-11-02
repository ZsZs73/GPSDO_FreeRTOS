#ifndef GPSDO_LOG_H
#define GPSDO_LOG_H

#include <stdint.h>

/*
 * Structured machine-readable logging.
 *
 * Record format:
 *
 *   LOG1 up_ms=1234 lvl=INFO sub=CTRL evt=LOCK_ENTER algo=11 pwm=34822
 *
 * LOG1 is the schema version.  The common fields are emitted by this module;
 * callers supply only the subsystem, event name and optional event-specific
 * key=value fields.
 *
 * Rules:
 *   - subsystem and event names are stable machine-readable tokens
 *   - keys and values must not contain whitespace
 *   - use scaled integers with units in the key where practical:
 *       vctl_mv=1754 phase_ns=-12 period_ms=1000
 *   - do not rely on prose for information needed by software
 *   - do not use %f in formatted fields; log scaled integer values instead
 *
 * This interface is not ISR-safe and must not be called from timing-critical
 * sections such as the LTIC PPS measurement.
 */

typedef enum {
    GPSDO_LOG_DEBUG = 0,
    GPSDO_LOG_INFO,
    GPSDO_LOG_WARN,
    GPSDO_LOG_ERROR,
    GPSDO_LOG_DATA
} gpsdo_log_level_t;

/* Event with no additional fields. */
void gpsdo_log_event(gpsdo_log_level_t level,
                     const char *subsystem,
                     const char *event);

/*
 * Event with additional printf-style key=value fields.
 *
 * Example:
 *
 *   gpsdo_log_eventf(GPSDO_LOG_INFO, "CTRL", "LOCK_ENTER",
 *                    "algo=%u pwm=%u vctl_mv=%ld",
 *                    algo, pwm, vctl_mv);
 */
/* Human-facing log message. The LOG1/timestamp/level/subsystem envelope stays
 * structured; fmt is normal operator-readable text. */
/* Supply a trusted UTC observation to the logger.
 * Once set, LOG1 timestamps free-run from millis() between GPS updates and
 * remain available if GPS position or the NMEA stream is later lost. */
void gpsdo_log_set_utc(uint16_t year, uint8_t month, uint8_t day,
                       uint8_t hour, uint8_t minute, uint8_t second);

void gpsdo_logf(gpsdo_log_level_t level,
                const char *subsystem,
                const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

void gpsdo_log_eventf(gpsdo_log_level_t level,
                      const char *subsystem,
                      const char *event,
                      const char *fields_fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

#endif /* GPSDO_LOG_H */
