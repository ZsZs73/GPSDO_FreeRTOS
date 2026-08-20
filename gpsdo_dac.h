/*
 * gpsdo_dac.h — single point where the control voltage leaves the firmware.
 *
 * Part of GPSDO FreeRTOS v1.05
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
 * gCtrl.pwm_output directly. Rounded, not truncated, so what the displays show
 * is the nearest 16-bit code to what is actually on the pin. */
uint16_t gpsdo_dac_last16(void);

/* ---- THE FINE PATH ---------------------------------------------------------
 *
 * The comment at the top of this file said the plumbing was ready and the loop
 * was not. This is the loop catching up.
 *
 * WHY THE FRACTION LIVES HERE, AND NOWHERE ELSE
 * The control value is written from 21 call sites: the CT and LC sweeps, the
 * acquisition ramps, holdover steering, the SP command, and the loop itself.
 * Twenty of those are deliberately coarse — a sweep that lands on 30720.4
 * instead of 30720 is not a better sweep, it is a sweep whose reference point
 * nobody can state. So the fraction is owned by this layer: every coarse write
 * clears it as a side effect of arriving here, and no caller has to remember
 * to. Keeping it in the control loop instead would mean twenty places that
 * each had to know to reset it, which is exactly the class of bug the top of
 * this file was written to prevent.
 *
 * WHAT IT BUYS. One 16-bit step is about 320 µHz on the measured plant here,
 * which is 3.2e-11 of 10 MHz — coarser than the 4e-12 the loop was measured
 * holding over 10 000 s. It got there by dithering between adjacent codes from
 * one correction to the next, which works but leaves the control voltage
 * hunting. With the fraction kept, a correction smaller than one step is
 * applied instead of being truncated away, and the step becomes 1.25e-13.
 *
 * The truncation it removes was also biased: (int32_t) rounds toward zero, so
 * every correction lost part of itself in the same direction, which reads to
 * the loop as a gain error of up to a sixth at the 6-LSB corrections measured
 * in normal operation.
 *
 * WHAT IT DOES NOT CHANGE. gpsdo_dac_last16() still returns a plain uint16_t,
 * so every display, the telemetry line and the flash ring see exactly what they
 * saw before. The settings block still stores 16 bits; a restore starts with a
 * zero fraction and gives up at most 1.25e-13, which is below anything this
 * hardware can show.
 * -------------------------------------------------------------------------- */

/* Command in 16-bit units WITH the fraction kept. Values outside 1..65535 are
 * clamped to the same band gpsdo_dac_write16() uses, so the fine path can never
 * reach a code the coarse path could not. */
void gpsdo_dac_write16f(double v16);

/* The value actually on the pin, in 16-bit units including the fraction. This
 * is what an algorithm's output stage should add its correction to — using the
 * rounded uint16_t instead would throw the fraction away once per cycle and
 * make the whole exercise pointless. */
double gpsdo_dac_last16f(void);

/* The raw 24-bit code, for reporting. */
uint32_t gpsdo_dac_last24(void);

/* True when the compiled-in output path genuinely resolves more than 16 bits,
 * i.e. the dithered PWM or an external DAC. False for plain analogWrite(), where
 * the fraction is harmless but buys nothing — the loop checks this so it can
 * report honestly rather than claim resolution it does not have. */
bool gpsdo_dac_fine_available(void);

#endif /* GPSDO_DAC_H */
