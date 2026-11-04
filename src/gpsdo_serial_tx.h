#ifndef GPSDO_SERIAL_TX_H
#define GPSDO_SERIAL_TX_H

#include <Arduino.h>
#include <Stream.h>

/*
 * One logical console stream with an explicit TX backend.
 *
 * Before the scheduler starts, writes are synchronous so boot diagnostics
 * still work. After gpsdo_serial_start(), runtime writes go through a bounded
 * FreeRTOS queue. Producers wait for queue space instead of silently dropping
 * output.
 */
class GpsdoSerial : public Stream
{
public:
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buf, size_t size) override;

    int available(void) override;
    int read(void) override;
    int peek(void) override;
    void flush(void) override;
    int availableForWrite(void) override;
};

extern GpsdoSerial g_serial;

bool gpsdo_serial_start(void);
bool gpsdo_serial_ready(void);

#endif
