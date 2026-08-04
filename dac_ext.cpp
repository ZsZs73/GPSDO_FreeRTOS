/*
 * dac_ext.cpp — external SPI DAC. STUB, deliberately non-functional.
 *
 * Part of GPSDO FreeRTOS v1.03
 * See dac_ext.h for why this path exists and what has to be decided first.
 */
#include <Arduino.h>
#include "gpsdo_config.h"
#include "dac_ext.h"

#if defined(GPSDO_DAC_EXT)

#error "GPSDO_DAC_EXT: no DAC part selected yet - dac_ext.cpp is a stub. \
Choose a device, fill in dac_ext_write(), then remove this #error."

bool dac_ext_begin(void)
{
    pinMode(PIN_DAC_SCK,  OUTPUT);
    pinMode(PIN_DAC_MOSI, OUTPUT);
    pinMode(PIN_DAC_CS,   OUTPUT);
    digitalWrite(PIN_DAC_CS,  HIGH);
    digitalWrite(PIN_DAC_SCK, LOW);
    return true;
}

void dac_ext_write(uint32_t code)
{
    /* Shape of the eventual implementation, left here so the timing is obvious
     * rather than rediscovered: assert CS, clock out the word MSB first on the
     * rising edge, release CS. Whether the word is 24 bits with command nibbles
     * or a bare 18 depends on the part.
     *
     * At once per second none of this needs to be fast, which is the whole point
     * of choosing an external DAC over a modulator that has to run continuously:
     * a few microseconds of bit-banging against 6% of the CPU. */
    (void)code;
}

#endif /* GPSDO_DAC_EXT */
