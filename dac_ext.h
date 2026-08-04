/*
 * dac_ext.h — external SPI DAC on the control-voltage output. STUB.
 *
 * Part of GPSDO FreeRTOS v1.03
 *
 * NOT IMPLEMENTED. The interface is here so the decision can be made without
 * touching anything else; the body is empty and GPSDO_DAC_EXT is refused at
 * compile time until a part is chosen.
 *
 * WHY THIS EXISTS RATHER THAN THE SIGMA-DELTA PATH
 * ------------------------------------------------
 * The sigma-delta DAC was measured and does not deliver what it promised. Its
 * command is 24 bits, but the resolution reaching the oscillator is set by how
 * long the analogue filter averages, not by the command width:
 *
 *     averaging      effective bits
 *     4 096 bits          13.6
 *     262 144 bits        19.0
 *     16 777 216 bits     24.0     (43 seconds at 390 kHz)
 *
 * A filter fast enough not to delay the loop — 0.5 Hz, 0.3 s — averages about
 * 120 000 bits and yields roughly 18 bits. Useful, but two bits over the 16-bit
 * PWM rather than the eight the command width suggests, bought with 6% of the
 * CPU, a precision reference, a CMOS gate and a fourth-order active filter.
 *
 * An external SPI DAC reaches the same 18 bits with none of that: no filter
 * delay, no averaging, no CPU load beyond a few microseconds once per second,
 * and a reference designed for the job. See doc/README for the full comparison.
 *
 * NO HARDWARE SPI IS NEEDED OR AVAILABLE
 * --------------------------------------
 * SPI1 belongs to the TFT and every SPI2 pin on this package is already taken
 * (PB10, PB13, PB15). That does not matter: the DAC is written once per second,
 * so bit-banging 24 bits costs on the order of a microsecond. Free GPIO on this
 * package at the time of writing: PB0, PB2, PB4, PB6, PB7, PB14, PA4, PA6.
 *
 * WHAT STILL HAS TO BE DECIDED
 * ----------------------------
 *   - the part, and with it the word length and format
 *   - the reference, and whether it is internal to the DAC
 *   - the output span against the OCXO's EFC range
 *
 * Candidates considered: AD5680 (18-bit, external reference), AD5683R (16-bit
 * with internal reference and output buffer), AD5761R (16-bit, configurable
 * span including 0-5 V). AD5680 with a REF5045 gives 18 bits over 4.5 V — about
 * 17 uV per step, near 9e-12 fractional on a 5.3 Hz/V oscillator, against
 * 2.7e-11 for the PWM in use today.
 */
#ifndef DAC_EXT_H
#define DAC_EXT_H

#include <stdint.h>

/* Pins. Provisional, and chosen to avoid two traps:
 *
 *   - PB6 and PB7 look free but are NOT. Wire.begin() is called without
 *     arguments, so I2C1 takes its default pins, which on this variant are
 *     exactly those two. Putting the DAC there would break the sensors, the
 *     HT16K33 clock display and anything else on the bus.
 *   - Everything below is plain GPIO with no alternate function this firmware
 *     uses, verified against gpsdo_config.h rather than assumed.
 *
 * Change freely — nothing in the firmware depends on these three. Also free at
 * present, if a chosen part needs LDAC, RESET or a busy input: PA4, PA6, PA8,
 * PA9, PA10, PB2, PC14, PC15. */
#ifndef PIN_DAC_SCK
#define PIN_DAC_SCK   PB0
#endif
#ifndef PIN_DAC_MOSI
#define PIN_DAC_MOSI  PB2
#endif
#ifndef PIN_DAC_CS
#define PIN_DAC_CS    PB4
#endif

/* Command width the loop will be offered once a part is chosen. Left at 18 as
 * the working assumption; gpsdo_dac.cpp scales into it. */
#define DAC_EXT_BITS  18u
#define DAC_EXT_MAX   ((1u << DAC_EXT_BITS) - 1u)

/* Configure the pins and put the DAC into a known state. Returns false if it
 * did not respond, where the part allows that to be detected. */
bool dac_ext_begin(void);

/* Write a code, 0..DAC_EXT_MAX. Expected to be called about once per second. */
void dac_ext_write(uint32_t code);

#endif /* DAC_EXT_H */
