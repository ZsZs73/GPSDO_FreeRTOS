/* =======================================================================
 * TeeSerial — mirror one logical stream onto two physical UARTs.
 * Part of GPSDO FreeRTOS v1.07
 *
 * When GPSDO_BLUETOOTH_PARALLEL is enabled the firmware talks on USB Serial
 * and the Bluetooth UART (Serial2) at the same time: every byte written goes
 * to both, and input is accepted from whichever port a character arrives on.
 * This lets a USB terminal and a Bluetooth terminal both watch the telemetry
 * and both issue CLI commands, without one excluding the other.
 *
 * It derives from Arduino's Stream so it is a drop-in for the existing
 * OUT_SERIAL / CLI_SERIAL / REPORT_SERIAL macros — the call sites (print,
 * println, write, available, read) are unchanged. Writes fan out to both
 * ports; reads poll port A first, then port B, so commands can come from
 * either terminal. Both ports must already be begin()-run by the caller.
 * ======================================================================= */
#ifndef GPSDO_TEE_SERIAL_H
#define GPSDO_TEE_SERIAL_H

#include <Arduino.h>
#include "FreeRTOS.h"
#include "task.h"

class TeeSerial : public Stream
{
public:
    TeeSerial(Stream &a, Stream &b) : _a(a), _b(b) {}

    /* ---- Print/Stream write path: fan out to both ports ---- */

    size_t write(uint8_t c) override
    {
        size_t n = _a.write(c);
        _b.write(c);

        boot_line_guard(c == '\n');
        return n;                 /* report the primary port's count */
    }

    size_t write(const uint8_t *buf, size_t size) override
    {
        size_t n = _a.write(buf, size);

        /*
        * Before the scheduler starts, pace the secondary UART at line
        * boundaries. The ESP serial/TCP bridge can lose bytes when startup
        * diagnostics arrive as one continuous burst even though the STM32 UART
        * itself has transmitted them successfully.
        *
        * Split only the secondary copy so USB retains its normal behaviour.
        */
        if (boot_guard_active()) {
            size_t start = 0;

            for (size_t i = 0; i < size; ++i) {
                if (buf[i] != '\n')
                    continue;

                _b.write(buf + start, i - start + 1);
                _b.flush();
                delay(GPSDO_BT_BOOT_GUARD_MS);
                start = i + 1;
            }

            if (start < size)
                _b.write(buf + start, size - start);
        } else {
            _b.write(buf, size);
        }

        return n;
    }

    /* ---- How much can be written without blocking ----
     *
     * Stream's default returns 0, and a caller that treats 0 as "not now"
     * would silence a tee build completely — which is why this override has
     * to exist the moment anything starts asking. The answer for a tee is the
     * SMALLER of the two: a write goes to both ports, so the one with less
     * room is the one that decides whether the call blocks.
     *
     * A port that reports 0 room but is genuinely absent (an unopened CDC)
     * would otherwise veto output on the port that IS there, so treat "no
     * room at all" on one side as no constraint from that side. Neither
     * HardwareSerial nor USBSerial reports 0 when it is working and idle. */
    int availableForWrite(void) override
    {
        int a = _a.availableForWrite();
        int b = _b.availableForWrite();
        if (a <= 0) return b;
        if (b <= 0) return a;
        return (a < b) ? a : b;
    }

    /* ---- Input path: read from whichever port has data ----
     * Poll A first, then B. available() reports the sum so callers that
     * loop `while (available())` drain both. peek()/read() service A before
     * B, which is fine for line-oriented CLI input. */
    int available() override
    {
        return _a.available() + _b.available();
    }

    int read() override
    {
        int c = _a.read();
        if (c >= 0) return c;
        return _b.read();
    }

    int peek() override
    {
        int c = _a.peek();
        if (c >= 0) return c;
        return _b.peek();
    }

    void flush() override
    {
        _a.flush();
        _b.flush();
    }

private:
    bool boot_guard_active() const
    {
#if defined(GPSDO_BT_BOOT_GUARD_MS) && GPSDO_BT_BOOT_GUARD_MS > 0
        return xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED;
#else
        return false;
#endif
    }

    void boot_line_guard(bool end_of_line)
    {
#if defined(GPSDO_BT_BOOT_GUARD_MS) && GPSDO_BT_BOOT_GUARD_MS > 0
        if (end_of_line && boot_guard_active()) {
            _b.flush();
            delay(GPSDO_BT_BOOT_GUARD_MS);
        }
#else
        (void)end_of_line;
#endif
    }

    Stream &_a;
    Stream &_b;
};

#endif /* GPSDO_TEE_SERIAL_H */
