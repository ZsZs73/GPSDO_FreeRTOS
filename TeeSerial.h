/* =======================================================================
 * TeeSerial — mirror one logical stream onto two physical UARTs.
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
#ifndef TEE_SERIAL_H
#define TEE_SERIAL_H

#include <Arduino.h>

class TeeSerial : public Stream
{
public:
    TeeSerial(Stream &a, Stream &b) : _a(a), _b(b) {}

    /* ---- Print/Stream write path: fan out to both ports ---- */
    size_t write(uint8_t c) override
    {
        size_t n = _a.write(c);
        _b.write(c);
        return n;                 /* report the primary port's count */
    }

    size_t write(const uint8_t *buf, size_t size) override
    {
        size_t n = _a.write(buf, size);
        _b.write(buf, size);
        return n;
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
    Stream &_a;
    Stream &_b;
};

#endif /* TEE_SERIAL_H */
