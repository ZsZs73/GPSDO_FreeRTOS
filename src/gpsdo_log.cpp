#include "gpsdo_log.h"
#include "gpsdo_config.h"
#include "gpsdo_state.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

#define GPSDO_LOG_LINE_MAX 256

static const char *log_level_name(gpsdo_log_level_t level)
{
    switch (level) {
        case GPSDO_LOG_DEBUG: return "DEBUG";
        case GPSDO_LOG_INFO:  return "INFO";
        case GPSDO_LOG_WARN:  return "WARN";
        case GPSDO_LOG_ERROR: return "ERROR";
        case GPSDO_LOG_DATA:  return "DATA";
        default:              return "UNKNOWN";
    }
}

/*
 * UTC anchor for LOG1.
 *
 * GPS refreshes this whenever it has a valid date/time. Between updates the
 * logger advances UTC from millis(), so event timestamps remain useful through
 * position loss and even complete NMEA loss. Before the first valid UTC
 * observation of the boot, LOG1 continues to use ts=-.
 *
 * A tiny sequence lock avoids a torn 64-bit epoch read on the 32-bit MCU
 * without taking an RTOS mutex from the logging path.
 */
static volatile uint32_t s_utc_seq = 0u;
static volatile uint64_t s_utc_epoch_s = 0u;
static volatile uint32_t s_utc_anchor_ms = 0u;
static volatile bool     s_utc_valid = false;

static int64_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
    y -= (m <= 2u);
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);
    const int32_t mp = (int32_t)m + (m > 2u ? -3 : 9);
    const uint32_t doy =
        (153u * (uint32_t)mp + 2u) / 5u + d - 1u;
    const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int32_t *y, uint32_t *m, uint32_t *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe =
        (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int32_t yy = (int32_t)yoe + (int32_t)era * 400;
    const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const uint32_t mp = (5u * doy + 2u) / 153u;
    const uint32_t dd = doy - (153u * mp + 2u) / 5u + 1u;
    const int32_t mm_i = (int32_t)mp + (mp < 10u ? 3 : -9);
    const uint32_t mm = (uint32_t)mm_i;
    yy += (mm <= 2u);

    *y = yy;
    *m = mm;
    *d = dd;
}

void gpsdo_log_set_utc(uint16_t year, uint8_t month, uint8_t day,
                       uint8_t hour, uint8_t minute, uint8_t second)
{
    /* Reject obviously invalid observations rather than poisoning all later
     * event timestamps. GPSDO builds are contemporary, so 2020 is a generous
     * lower sanity bound. */
    if (year < 2020u || month < 1u || month > 12u ||
        day < 1u || day > 31u || hour > 23u ||
        minute > 59u || second > 60u)
        return;

    int64_t days = days_from_civil((int32_t)year, month, day);
    if (days < 0)
        return;

    uint64_t epoch_s =
        (uint64_t)days * 86400ull +
        (uint64_t)hour * 3600ull +
        (uint64_t)minute * 60ull +
        (uint64_t)(second == 60u ? 59u : second);

    ++s_utc_seq;                  /* odd: writer active */
    s_utc_epoch_s = epoch_s;
    s_utc_anchor_ms = millis();
    s_utc_valid = true;
    ++s_utc_seq;                  /* even: coherent snapshot */
}

static bool log_utc_timestamp(char *out, size_t out_len, uint32_t now_ms)
{
    if (!s_utc_valid)
        return false;

    uint32_t seq_a, seq_b, anchor_ms;
    uint64_t epoch_s;

    do {
        seq_a = s_utc_seq;
        if (seq_a & 1u)
            continue;
        epoch_s = s_utc_epoch_s;
        anchor_ms = s_utc_anchor_ms;
        seq_b = s_utc_seq;
    } while (seq_a != seq_b || (seq_b & 1u));

    /* Unsigned subtraction intentionally handles normal millis() wrap. */
    epoch_s += (uint32_t)(now_ms - anchor_ms) / 1000u;

    int64_t days = (int64_t)(epoch_s / 86400ull);
    uint32_t sod = (uint32_t)(epoch_s % 86400ull);
    int32_t year;
    uint32_t month, day;
    civil_from_days(days, &year, &month, &day);

    uint32_t hour = sod / 3600u;
    uint32_t minute = (sod % 3600u) / 60u;
    uint32_t second = sod % 60u;

    int n = snprintf(out, out_len, "%04ld-%02lu-%02luT%02lu:%02lu:%02luZ",
                     (long)year,
                     (unsigned long)month,
                     (unsigned long)day,
                     (unsigned long)hour,
                     (unsigned long)minute,
                     (unsigned long)second);
    return n > 0 && (size_t)n < out_len;
}

static void log_emit(gpsdo_log_level_t level,
                     const char *subsystem,
                     const char *event,
                     const char *fields_fmt,
                     va_list fields)
{
    char line[GPSDO_LOG_LINE_MAX];

    if (subsystem == nullptr)
        subsystem = "-";
    if (event == nullptr)
        event = "-";

    uint32_t now_ms = millis();
    char timestamp[24];
    bool have_utc = log_utc_timestamp(timestamp, sizeof(timestamp), now_ms);

    int n;
    if (have_utc) {
        n = snprintf(line, sizeof(line),
                     "LOG1 ts=%s up_ms=%lu lvl=%s [%s] %s",
                     timestamp,
                     (unsigned long)now_ms,
                     log_level_name(level),
                     subsystem,
                     event);
    } else {
        n = snprintf(line, sizeof(line),
                     "LOG1 ts=- up_ms=%lu lvl=%s [%s] %s",
                     (unsigned long)now_ms,
                     log_level_name(level),
                     subsystem,
                     event);
    }

    if (n < 0)
        return;

    size_t used = (size_t)n;
    if (used >= sizeof(line))
        used = sizeof(line) - 1;

    if (fields_fmt != nullptr &&
        fields_fmt[0] != '\0' &&
        used < sizeof(line) - 1) {

        line[used++] = ' ';
        line[used] = '\0';

        (void)vsnprintf(line + used,
                        sizeof(line) - used,
                        fields_fmt,
                        fields);
    }

    /*
    * During early boot xSerialMutex does not exist yet, so output directly.
    * Once the RTOS objects have been created, serialize log records with the
    * same mutex used by the CLI/report streams.
    */
    if (xSerialMutex != nullptr) {
        if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            OUT_SERIAL.println(line);
            xSemaphoreGive(xSerialMutex);
        }
    } else {
        OUT_SERIAL.println(line);
    }
}

void gpsdo_log_event(gpsdo_log_level_t level,
                     const char *subsystem,
                     const char *event)
{
    gpsdo_log_eventf(level, subsystem, event, nullptr);
}

void gpsdo_logf(gpsdo_log_level_t level,
                const char *subsystem,
                const char *fmt, ...)
{
    char msg[192];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    gpsdo_log_event(level, subsystem, msg);
}

void gpsdo_log_eventf(gpsdo_log_level_t level,
                      const char *subsystem,
                      const char *event,
                      const char *fields_fmt, ...)
{
    va_list fields;

    va_start(fields, fields_fmt);
    log_emit(level, subsystem, event, fields_fmt, fields);
    va_end(fields);
}
