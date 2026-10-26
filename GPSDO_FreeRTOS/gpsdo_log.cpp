#include "gpsdo_log.h"
#include "gpsdo_config.h"

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

    int n = snprintf(line, sizeof(line),
                     "LOG1 ts=- up_ms=%lu lvl=%s sub=%s evt=%s",
                     (unsigned long)millis(),
                     log_level_name(level),
                     subsystem,
                     event);

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

    OUT_SERIAL.println(line);
}

void gpsdo_log_event(gpsdo_log_level_t level,
                     const char *subsystem,
                     const char *event)
{
    gpsdo_log_eventf(level, subsystem, event, nullptr);
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