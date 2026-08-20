/*
 * gpsdo_health.cpp — correction statistics. See the header for what these
 * numbers mean and, more importantly, what they cannot mean.
 *
 * Part of GPSDO FreeRTOS v1.05
 *
 * Exponentially weighted rather than buffered: three time constants cost three
 * multiply-adds per second and no memory, where a ring buffer covering an hour
 * would be several kilobytes to answer the same question no better.
 */
#include <Arduino.h>
#include <math.h>
#include "gpsdo_config.h"
#include "GPSDO_algorithms.h"
#include "gpsdo_state.h"
#include "gpsdo_health.h"

/* Weights, in CORRECTIONS rather than seconds. A decade apart, so the four
 * windows cover four orders of magnitude without overlapping usefully.
 *
 * What they span in wall-clock time depends on the algorithm, which is exactly
 * why they are not labelled in minutes:
 *
 *              algorithm 11 (1 s)   algorithm 10, LIV 60
 *     100            1.7 min              1.7 h
 *     1 000          17 min               17 h
 *     10 000         2.8 h                7 days
 *     100 000        28 h                 70 days
 *
 * These are exponential weights, so N is a time constant and not a hard window:
 * about 63% of the weight falls inside N corrections and 95% inside 3N. The
 * oldest data fades rather than dropping out. That costs four multiply-adds per
 * correction and no memory, where a ring buffer holding 100 000 samples would be
 * most of the RAM budget to answer the same question no better. */
#define A_100   (1.0 / 100.0)
#define A_1K    (1.0 / 1000.0)
#define A_10K   (1.0 / 10000.0)
#define A_100K  (1.0 / 100000.0)

/* True while the loop is disciplining normally: locked, and no calibration
 * sweeping the DAC underneath us. Anything else writing to the DAC is issuing a
 * command, not correcting an error, and must not enter the statistics.
 *
 * Algorithms 0-9 have no lock concept, so they are excluded entirely rather than
 * counted with a meaning nobody could interpret; CS says so instead of quietly
 * reporting a number about nothing. */
static bool counting_now(void)
{
    /* setup() writes the DAC three times — the initial 127, the recalled PWM and
     * the default — before xEventGroupCreate() has run, so this is reached with
     * xSysEvents still NULL. Passing NULL to xEventGroupGetBits() trips
     * configASSERT and the processor stops: the board went dead before the LED
     * ever blinked, with nothing on the console to say why.
     *
     * Those early writes are commands anyway, not corrections, so returning
     * false is both safe and correct. */
    if (xSysEvents == NULL) return false;

    EventBits_t ev = xEventGroupGetBits(xSysEvents);
    if (ev & (EVT_NEED_CALIBRATION | EVT_NEED_LTIC_CAL)) return false;

    switch (gCtrl.active_algo) {
        case 10: return (g_ltic.state == LTIC_LOCK);
        case 11: return g_lars_locked;
        default: break;             /* algorithms 0-9: no lock state to gate on */
    }
    return false;
}

static uint16_t s_prev;
static bool     s_have_prev;
static double   s_ms100, s_ms1k, s_ms10k, s_ms100k;   /* mean squares */
static double   s_bias;
static uint16_t s_peak;
static uint32_t s_n;
static uint32_t s_gated;   /* writes ignored because the loop was not locked */
/* Interval between corrections, smoothed. Measured rather than assumed: it is
 * the only way to state the windows in seconds without hard-coding an
 * assumption about the algorithm that would be wrong half the time. */
static uint32_t s_last_ms;
static double   s_dt;

void health_reset(void)
{
    s_have_prev = false;
    s_ms100 = s_ms1k = s_ms10k = s_ms100k = 0.0;
    s_bias = 0.0;
    s_peak = 0;
    s_n = 0;
    s_gated = 0;
    s_last_ms = 0;
    s_dt = 0.0;
}

void health_note_output(uint16_t pwm)
{
    if (!counting_now()) {
        /* Drop the reference too. Resuming across a gap would fabricate a single
         * enormous "correction" equal to everything that happened while we were
         * not looking. */
        s_have_prev = false;
        s_last_ms   = 0;   /* forget the timestamp too: the first interval after
                            * a gap would otherwise be the length of the gap */
        s_gated++;
        return;
    }
    uint32_t now = millis();
    if (s_last_ms != 0) {
        double dt = (double)(now - s_last_ms) / 1000.0;
        /* Ignore absurd gaps: a resumed session after the gate was shut would
         * otherwise report an interval of hours. */
        if (dt > 0.0 && dt < 600.0)
            s_dt = (s_dt == 0.0) ? dt : (s_dt + 0.05 * (dt - s_dt));
    }
    s_last_ms = now;

    if (!s_have_prev) { s_prev = pwm; s_have_prev = true; return; }

    double d = (double)pwm - (double)s_prev;
    s_prev = pwm;

    /* A correction of zero is still a correction: it says the loop saw nothing
     * worth acting on, which is exactly the information wanted. Skipping them
     * would flatter the statistics. */
    double sq = d * d;
    s_ms100  += A_100  * (sq - s_ms100);
    s_ms1k   += A_1K   * (sq - s_ms1k);
    s_ms10k  += A_10K  * (sq - s_ms10k);
    s_ms100k += A_100K * (sq - s_ms100k);
    s_bias   += A_100K * (d  - s_bias);

    double ad = fabs(d);
    if (ad > (double)s_peak && ad < 65535.0) s_peak = (uint16_t)ad;
    if (s_n < 0xFFFFFFFFu) s_n++;
}

void health_get(health_stats_t *out)
{
    /* CT stores the slope as g_pid[7].Kp = 0.40/K, so K is recovered from it.
     * Without CT there is no way to turn LSB into hertz, and the caller is told
     * so rather than being handed a number derived from a guess. */
    double kp = (double)g_pid[7].Kp;
    out->have_k       = (kp > 100.0);
    out->k_hz_per_lsb = out->have_k ? (0.40 / kp) : 0.0;
    out->updates = s_n;
    out->gated   = s_gated;
    out->counting = counting_now();
    out->rms_100  = sqrt(s_ms100);
    out->rms_1k   = sqrt(s_ms1k);
    out->rms_10k  = sqrt(s_ms10k);
    out->rms_100k = sqrt(s_ms100k);
    out->bias_100k = s_bias;
    out->secs_per_corr = s_dt;
    out->peak    = s_peak;
}
