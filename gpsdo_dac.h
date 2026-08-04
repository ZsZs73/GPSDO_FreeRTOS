/*
 * gpsdo_dac.h — single point where the control voltage leaves the firmware.
 *
 * Part of GPSDO FreeRTOS v1.03
 *
 * Before this existed, analogWrite(PIN_VCTL_PWM, ...) appeared in 23 places
 * across three files. Adding a second output path by editing all of them would
 * have been an invitation to miss one, and a missed call site is the worst kind
 * of bug here: the loop would steer correctly almost all the time and jump
 * whenever the stale path was taken.
 *
 * TWO RESOLUTIONS, DELIBERATELY
 * -----------------------------
 * gpsdo_dac_write16() takes the 16-bit value the control loop has always
 * computed. Under the PWM path it is passed straight through; under the
 * sigma-delta path it is shifted up into the 24-bit command.
 *
 * That shift means the sigma-delta DAC, wired this way, delivers **no extra
 * resolution** — the loop is still only asking for 65536 distinct levels, so the
 * DAC's 16.7 million are stepped 256 at a time. What it does deliver immediately
 * is a much shorter analogue time constant, because a shaped one-bit stream at
 * 390 kHz needs far less filtering than a 16-bit PWM, and filter delay inside a
 * control loop is not free.
 *
 * Actually using 24 bits needs the loop's own arithmetic widened: pwm_output is
 * uint16_t and the PI terms round into it. gpsdo_dac_write24() exists so that
 * work can be done later without touching this layer again — the plumbing is
 * ready, the loop is not. Doing it before the hardware path is proven would mean
 * debugging two things at once.
 */
#ifndef GPSDO_DAC_H
#define GPSDO_DAC_H

#include <stdint.h>

/* Bring up whichever output path is compiled in. Call once from setup(), before
 * the control task starts. Returns false only if the sigma-delta path failed to
 * start; the PWM path cannot fail. */
bool gpsdo_dac_begin(void);

/* Command in the loop's native 16-bit units. This is what every existing call
 * site uses, and its behaviour under the PWM path is byte-for-byte what
 * analogWrite() did before. */
void gpsdo_dac_write16(uint16_t v);

/* Command in the sigma-delta DAC's native 24-bit units, 0..16777215. Under the
 * PWM path the low 8 bits are discarded, so a caller gains nothing but loses
 * nothing either. */
void gpsdo_dac_write24(uint32_t v);

/* Last value written, in 16-bit units, for telemetry that used to read
 * gCtrl.pwm_output directly. */
uint16_t gpsdo_dac_last16(void);

#endif /* GPSDO_DAC_H */
