/*
 * gpsdo_health.h — self-assessment from the loop's own corrections.
 *
 * Part of GPSDO FreeRTOS v1.03
 *
 * WHY THIS EXISTS
 * ---------------
 * Algorithm 11 was validated against a rubidium standard on someone else's
 * bench. Almost nobody who builds this will have one, and without it they have
 * the author's word and a green rectangle on the display. That is a poor
 * position to leave a builder in.
 *
 * The idea is Alan's (MIS42N on EEVblog), who pointed out that a disciplined
 * oscillator can assess itself: the correction the loop applies is the error it
 * just observed, so the size of those corrections says whether the discipline is
 * working. GPS is the reference and there is nothing better to compare frequency
 * against. His own design relies on this, which is why it needs no secondary
 * standard — a real advantage over the approach taken here.
 *
 * The firmware already computes every one of these numbers and throws them away.
 *
 * WHAT IT DOES AND DOES NOT TELL YOU
 * ----------------------------------
 * It measures whether the LOOP IS SETTLED, not whether the OUTPUT IS GOOD. Those
 * are the same thing only while the phase detector is trustworthy.
 *
 * If the detector is noisy, the loop makes corrections chasing that noise. The
 * corrections grow, and this reports them faithfully — but the oscillator was
 * fine and the loop has just made it worse. That is precisely the failure a GPSDO
 * is prone to: perfectly locked by every indicator, and worse at short averaging
 * times than if left alone. Nothing measured from inside the loop can see it.
 *
 * So read a small figure as "the loop is not fighting anything", which is
 * necessary but not sufficient. A large or growing figure is the useful signal:
 * something is wrong, even if this cannot say what.
 */
#ifndef GPSDO_HEALTH_H
#define GPSDO_HEALTH_H

#include <stdint.h>
#include <stdbool.h>

/* Called from the DAC layer on every write, so it sees every correction whatever
 * algorithm produced it and needs no hook in the control code. */
void health_note_output(uint16_t pwm);

/* Reset the statistics — after CT, LC or an algorithm change, where the step
 * that follows is a deliberate jump rather than a correction. */
void health_reset(void);

typedef struct {
    bool     have_k;        /* false until CT has measured the VCO slope   */
    double   k_hz_per_lsb;  /* 0 if unknown                                */
    bool     counting;      /* gate open right now?                        */
    uint32_t updates;       /* corrections counted since reset             */
    uint32_t gated;         /* writes ignored: not locked, or calibrating  */
    /* RMS correction over the last N corrections, in DAC counts. Counted in
     * CORRECTIONS, not seconds, because the correction rate depends on the
     * algorithm: algorithm 11 steers once a second, algorithm 10 once per LIV.
     * Labelling these in minutes would have meant one thing under algorithm 11
     * and sixty times that under algorithm 10 with LIV 60 — the same number
     * describing two different spans. Corrections are what the loop actually
     * did, so that is what is counted. */
    double   rms_100;       /* last ~100 corrections                       */
    double   rms_1k;        /* ~1 000                                      */
    double   rms_10k;       /* ~10 000                                     */
    double   rms_100k;      /* ~100 000 — over a day at one per second     */
    double   bias_100k;     /* mean correction: non-zero means steady drift */
    /* Measured interval between corrections, seconds. Lets the caller turn the
     * correction counts above into wall-clock time without knowing which
     * algorithm is running or what LIV is set to — algorithm 11 steers once a
     * second, algorithm 10 once per LIV, and the answer differs by that factor.
     * Zero until at least two corrections have been seen. */
    double   secs_per_corr;
    uint16_t peak;          /* largest single correction since reset, LSB   */
} health_stats_t;

void health_get(health_stats_t *out);

#endif /* GPSDO_HEALTH_H */
