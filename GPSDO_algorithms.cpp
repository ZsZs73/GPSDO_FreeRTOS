/**
 * GPSDO_algorithms.cpp — Control loop algorithm implementations
 *
 * Part of GPSDO FreeRTOS v1.03
 * Author:   J. M. Niewiński
 * GitHub:   https://github.com/jmnlabs/GPSDO_FreeRTOS
 * Based on: GPSDO v0.06c by André Balsa
 * AI:       Claude (Anthropic)
 *
 *
 * Ten algorithms (0-9) selectable at runtime via CLI command LA.
 * Algorithms 0-2 are from the original v0.06c codebase.
 * Algorithms 3-9 are new implementations:
 *   3: FLL PID (manual coefficients)
 *   4: PLL PI  (manual, no derivative)
 *   5: PLL PID (manual)
 *   6: FLL PID (genetically optimised coefficients)
 *   7: PLL PID (genetically optimised coefficients)
 *   8: Hybrid FLL+PLL with sigmoid blend
 *   9: Experimental single-layer neural network
 *
 * All PID coefficients are stored in the global g_pid[] array and
 * can be modified at runtime via CLI (KP/KI/KD/IL commands).
 */

#include "GPSDO_algorithms.h"
#include "gpsdo_config.h"
#include "gpsdo_state.h"
#include "ubx_timtp.h"
#include <string.h>
#include <math.h>

/* -----------------------------------------------------------------------
 * Internal helper: snapshot frequency data from FreqData_t under mutex.
 * Fills a local copy so algorithms don't need to hold the mutex.
 * ----------------------------------------------------------------------- */
typedef struct {
    double   avg10, avg100, avg1000, avg10000, avg20000;
    bool     have10, have100, have1000, have10000, have20000;
    int16_t  cumul10, cumul100, cumul1000, cumul10000, cumul20000;
} FreqSnapshot_t;

static void take_freq_snapshot(FreqSnapshot_t *s)
{
    memset(s, 0, sizeof(*s));
    if (xSemaphoreTake(xFreqMutex, pdMS_TO_TICKS(15)) == pdTRUE) {
        s->avg10    = gFreq.avg10;     s->have10    = gFreq.full10;
        s->avg100   = gFreq.avg100;    s->have100   = gFreq.full100;
        s->avg1000  = gFreq.avg1000;   s->have1000  = gFreq.full1000;
        s->avg10000 = gFreq.avg10000;  s->have10000 = gFreq.full10000;
        s->avg20000 = gFreq.avg20000;  s->have20000 = gFreq.full20000;
        s->cumul10     = gFreq.cumul10;
        s->cumul100    = gFreq.cumul100;
        s->cumul1000   = gFreq.cumul1000;
        s->cumul10000  = gFreq.cumul10000;
        s->cumul20000  = gFreq.cumul20000;
        xSemaphoreGive(xFreqMutex);
    }
}

/* Clamp PWM to valid range and guard against uint16 wrap */
static uint16_t clamp_pwm(int32_t v)
{
    if (v < 1)     return 1;
    if (v > 65535) return 65535;
    return (uint16_t)v;
}

/* -----------------------------------------------------------------------
 * apply_correction — common output stage for the disciplining algorithms.
 *
 *   1. Dead-zone (lock hold): if the frequency error is within LOCK_HZ AND
 *      the accumulated phase is within LOCK_PHASE (Hz·s ≈ time offset in
 *      units of cycles), the loop is locked — suppress the step so the PWM
 *      stops twitching and the OCXO free-runs on its own short-term
 *      stability.  *locked is set true so the caller can show "hit".
 *      Without the phase test the PLL would nudge ±2-5 LSB every period
 *      forever, because the phase term never resolves to exactly zero.
 *
 *   2. Slew-rate limit: clamp |u| to max_step so a slow overnight phase
 *      drift is corrected over several periods, not one big PWM jump.
 *
 *   3. Clamp + write.
 * ---------------------------------------------------------------------- */
/* ===================== self-learning loop aid (LRN) =====================
 * Two SLOW, passive learners shared by algo 7 and LTIC. Neither injects any
 * excitation — they only observe the phase the loop already produces, so they
 * cannot destabilise it. Both are gated to run ONLY when the loop is locked,
 * update at most once per LRN_TICK seconds, and are clamped to narrow bands
 * around the theoretical values.
 *
 *  (1) DRIFT feed-forward: the OCXO ages/thermally drifts, so the phase ramps
 *      steadily between corrections (Dan's overnight trace: a ~9000 s ±80 ns
 *      sawtooth, an 8E-12/day drift the integrator kept chasing). We estimate
 *      the mean phase slope over a long window and add a constant PWM term
 *      that cancels it — the loop stops chasing a moving target and the phase
 *      goes flat.  d_ppm accumulates in PWM LSB.
 *  (2) DAMPING adaption: an ADEV bump at the loop time constant (Dan: ~1.4e-11
 *      at 80-100 s) means the proportional gain is a touch high — the phase
 *      overshoots zero after each correction. We watch zero-crossings of the
 *      phase error: overshoot (crossing with growing amplitude) → nudge the
 *      damping factor down; sluggish (no crossing, slow approach) → up.
 *
 * Persistence: learned drift term and damping factor are saved by ES and
 * recalled at boot; LRN 0 freezes learning, LRN R resets to theory. */
bool     g_lrn_enable   = true;          /* LRN 0/1 (EEPROM byte 222)        */
float    g_lrn_drift    = 0.0f;          /* learned feed-forward, PWM LSB     */
float    g_lrn_damp     = 1.0f;          /* damping multiplier, 0.5..1.5      */

#define LRN_TICK        30               /* seconds between learner updates    */
#define LRN_DRIFT_MAX   400.0f           /* |feed-forward| clamp, LSB          */
/* LRN_DAMP_LO / LRN_DAMP_HI live in GPSDO_algorithms.h (shared with
 * live_store for clamping restored values). The floor was raised 0.30 → 0.45
 * when a too-hard floor was found to starve the loop of correction authority
 * on a wide HC74 detector (phase ran 11→−425 ns in 51 s at damp 0.30 and
 * dropped LOCK→DPLL→ACQ). 0.45 still damps a limit cycle but keeps enough gain
 * to track a real OCXO drift. */

/* Live telemetry (printed by the report task when learning is active). */
float    g_lrn_slope_ns_s = 0.0f;        /* observed mean phase slope, ns/s    */
uint16_t g_lrn_osc_period = 0;           /* observed limit-cycle period, s     */
float    g_lrn_osc_amp_ns = 0.0f;        /* observed limit-cycle amplitude, ns */

/* Feed one locked-loop phase sample (ns) to the learners. dt = seconds since
 * the previous call. Returns the drift feed-forward to add to PWM (already
 * damped and clamped). Safe to call every PPS; internally rate-limited. */
static float lrn_update_ef(double e_freq, double phase_ns, double dt, bool locked)
{
    static double ef_win = 0.0;    static double win_t = 0.0;
    static double last_cross_t = 0.0, run_t = 0.0;
    static double last_ext = 0.0;  static int8_t  last_sign = 0;
    static bool   armed = false;
    static uint8_t ff_boot = 0;    /* fast feed-forward windows after (re)lock */

    if (!g_lrn_enable || !locked) { armed = false; return g_lrn_drift; }

    run_t += dt;
    /* ---- (1) drift feed-forward: slow integral of the FREQUENCY error ----
     * The feed-forward exists to cancel the OCXO's systematic drift so the
     * main loop's integrator does not have to chase it. We accumulate e_freq
     * over LRN_TICK windows and nudge the feed-forward to drive that mean
     * toward zero. Because e_freq already reflects the feed-forward we are
     * applying, this is inherently closed-loop: once the drift is absorbed
     * the mean e_freq is ~0 and the feed-forward stops moving. A deadband
     * (0.2 mHz) and a small per-tick step prevent wind-up and hunting. */
    ef_win += e_freq; win_t += dt;
    /* Feed-forward window and step: normally slow (30 s, 25 % of residual) to
     * stay quiet in steady state. But right after lock the drift is unknown
     * and the phase can run away before a 30 s window even closes — on air the
     * phase reached −425 ns (past the ACQ threshold) in 51 s while the learner
     * was still gathering its first window, so lock was lost before the
     * feed-forward ever moved. BOOTSTRAP: for the first few windows after lock,
     * use a short 8 s window and take a larger 60 % step, so the drift is
     * absorbed within ~10–20 s; then relax to the slow, quiet regime. */
    double tick = (ff_boot > 0) ? 8.0  : (double)LRN_TICK;
    double gain = (ff_boot > 0) ? 0.60 : 0.25;
    if (win_t >= tick) {
        double mean_ef = ef_win / win_t;          /* Hz, residual after ff  */
        g_lrn_slope_ns_s = (float)(mean_ef * 1.0e9 / (double)BASE_FREQ); /* ns/s */
        if (fabs(mean_ef) > 2.0e-4) {             /* 0.2 mHz deadband       */
            double lsbhz = (g_pid[7].Kp > 100.0) ? ((double)g_pid[7].Kp / 0.40) : 3000.0;
            /* u = -(…): positive e_freq (freq high) must LOWER pwm, so the
             * feed-forward carries -e_freq. Move a fraction of the residual. */
            double corr = -mean_ef * lsbhz;
            g_lrn_drift += (float)(gain * corr);
            if (g_lrn_drift >  LRN_DRIFT_MAX) g_lrn_drift =  LRN_DRIFT_MAX;
            if (g_lrn_drift < -LRN_DRIFT_MAX) g_lrn_drift = -LRN_DRIFT_MAX;
        }
        if (ff_boot > 0) ff_boot--;
        ef_win = 0.0; win_t = 0.0;
    }

    /* ---- (2) damping adaption from the phase limit cycle ----
     * Amplitude threshold is SCALED to the detector range: a fixed 5 ns limit
     * treats a wide-detector HC74 (~1650 ns span, ADC-noise floor ~50 ns) as a
     * perpetual oscillator and floors damping at LRN_DAMP_LO forever — exactly
     * what was seen on air (damp stuck at 0.5, drift hunting). Threshold =
     * 3 % of the measured range (clamped 5..150 ns) is the ADC + qErr noise
     * floor on a wide detector and a real oscillation on a narrow one. */
    float amp_thr = (g_ltic.range_ns > 1.0f) ? 0.03f * g_ltic.range_ns : 5.0f;
    if (amp_thr < 5.0f)   amp_thr = 5.0f;
    if (amp_thr > 150.0f) amp_thr = 150.0f;
    if (armed) {
        int8_t sgn = (phase_ns > amp_thr * 0.2) ? 1 : (phase_ns < -amp_thr * 0.2) ? -1 : 0;
        if (sgn != 0 && last_sign != 0 && sgn != last_sign) {
            double period = run_t - last_cross_t;
            if (period > 4.0) {
                g_lrn_osc_period = (uint16_t)(2.0 * period);
                g_lrn_osc_amp_ns = (float)fabs(last_ext);
                float amp = fabs(last_ext);
                if (amp > amp_thr) {
                    /* overshoot grows → damp more. Step grows with how far
                     * over the noise floor the cycle swings, but is capped so a
                     * single noisy crossing cannot collapse damping. */
                    float step = 0.01f + 0.0005f * (amp - amp_thr);
                    if (step > 0.10f) step = 0.10f;
                    g_lrn_damp -= step;
                } else {
                    /* cycle is inside the noise floor → relax damping back up
                     * toward unity, gently, so a quiet loop isn't over-damped. */
                    g_lrn_damp += 0.01f;
                }
                if (g_lrn_damp < LRN_DAMP_LO) g_lrn_damp = LRN_DAMP_LO;
                if (g_lrn_damp > LRN_DAMP_HI) g_lrn_damp = LRN_DAMP_HI;
                last_cross_t = run_t; last_ext = 0.0;
            }
        }
        if (fabs(phase_ns) > fabs(last_ext)) last_ext = phase_ns;
        if (sgn != 0) last_sign = sgn;
    } else {
        armed = true; last_cross_t = run_t; last_ext = 0.0; last_sign = 0;
        ff_boot = 3;    /* three fast 8 s windows to absorb drift after lock */
    }
    return g_lrn_drift;
}

/* Convenience wrapper: apply learning to any classic algorithm (3-8) in one
 * line. Pass the loop's correction u, its accumulated phase (Hz·s), the
 * frequency error (Hz) and the update period Ts. Returns the adjusted u
 * (damped + drift feed-forward). Locked = frequency on target. Algorithms
 * without a phase accumulator can pass 0 for phase_hz_s (drift learner then
 * simply sees no slope and stays neutral). */
static double lrn_apply(double u, double phase_hz_s, double e_freq, double Ts)
{
    bool lk = (fabs(e_freq) < 0.02);
    /* feed-forward is driven by the FREQUENCY error, not the (clamped)
     * phase accumulator: when the feed-forward has absorbed the OCXO drift,
     * e_freq → 0 on its own, which is the closed-loop signal that stops the
     * feed-forward growing. phase_hz_s is passed only for the damping/limit-
     * cycle observer (amplitude/period). */
    float ff = lrn_update_ef(e_freq, phase_hz_s, Ts, lk);
    return u * (double)g_lrn_damp + (double)ff;
}

static uint16_t apply_correction(uint16_t pwm, double u,
                                 double e_freq, double phase_acc,
                                 double max_step, bool *locked)
{
    const double LOCK_HZ    = 0.0010;   /* 1 mHz  ≈ 1e-10 frac. freq.     */
    const double LOCK_PHASE = 5.0;      /* 5 Hz·s ≈ 500 ns accumulated    */

    bool in_lock = (e_freq   > -LOCK_HZ    && e_freq   < LOCK_HZ) &&
                   (phase_acc > -LOCK_PHASE && phase_acc < LOCK_PHASE);
    if (locked) *locked = in_lock;

    if (in_lock) return pwm;            /* hold — no PWM motion in lock */

    /* Slew-rate limit */
    if (u >  max_step) u =  max_step;
    if (u < -max_step) u = -max_step;

    return clamp_pwm((int32_t)pwm + (int32_t)u);
}

/* Write trendstr — caller already holds xCtrlMutex */
static void set_trend(const char *s)
{
    strncpy(gCtrl.trendstr, s, 4);
    gCtrl.trendstr[4] = '\0';
}

/* ======================================================================
 * RUNTIME-TUNABLE PARAMETERS
 *
 * These are STARTING values only. The OCXO type does not need to be known
 * at compile time: the CT (Calibrate & Tune) command measures the actual
 * plant gain K and recomputes every coefficient for the fitted oscillator
 * (PLL Kp = 0.40/K on frequency; FLL Kp = 0.35/K, Ki = Kp/300, Kd = Kp*73;
 * NN max step = 0.05/K). Run CT once, then ES to persist.
 *
 * The defaults below assume a typical 10 MHz OCXO with K ~ 0.4 mHz/LSB on
 * a 0..3.3 V PWM DAC (16-bit, 1 LSB = 50.35 uV). They are deliberately
 * mid-range so the loop is stable on any common unit before CT is run.
 * All values can also be set at runtime via CLI (KP/KI/KD/IL/BC/BS/NS).
 *
 * Coefficient meaning:
 *   PLL (4,5,7): Kp on frequency error, Kd/Ki gentle terms on phase
 *   FLL (3,6):   classic frequency-domain PID
 *   [8] hybrid reads [6]+[7]; only its I_LIMIT is used here
 *   [9] NN uses fixed weights; only I_LIMIT (normalisation) applies
 * ====================================================================== */
PidParams_t g_pid[10] = {
    /* [0] */  { 0.0,    0.0,      0.0,       0.0     },
    /* [1] */  { 0.0,    0.0,      0.0,       0.0     },
    /* [2] */  { 0.0,    0.0,      0.0,       0.0     },
    /* [3]  FLL PID manual                   */
               { 70.0,   0.70,     175.0,     9000.0  },
    /* [4]  PLL: Kp=freq gain, Kd=phase prop, Ki=phase integral */
               { 1000.0, 0.020,    2.0,       7000.0  },
    /* [5]  PLL: Kp=freq, Kd=phase, Ki=phase integral */
               { 1000.0, 0.020,    2.0,       10000.0 },
    /* [6]  FLL PID genetic (freq-domain)     */
               { 205.0,  0.264,    14950.0,   13000.0 },
    /* [7]  PLL: Kp=freq, Kd=phase, Ki=phase integral */
               { 1000.0, 0.020,    2.0,       10000.0 },
    /* [8]  hybrid (uses [6]+[7]) IL only     */
               { 0.0,    0.0,      0.0,       13000.0 },
    /* [9]  NN  I_LIMIT = normalisation bound */
               { 0.0,    0.0,      0.0,       450.0   },
};
double g_blend_crossover = 0.024;   /* Hz — sigmoid centre */
double g_blend_scale     = 0.012;   /* Hz — sigmoid width  */
double g_nn_max_step     = 175.0;   /* LSB — max PWM delta  */

/* Algorithm 10 (LTIC three-stage PLL) defaults. Calibration fields start at 0
 * (uncalibrated) — the loop must not run until they are set on real hardware.
 * The PID/threshold defaults are plausible starting points to be tuned once
 * the TIC is characterised; they are NOT validated (no hardware yet). */
LticParams_t g_ltic = {
    /* ns_per_volt */ 0.0f,    /* 0 = uncalibrated */
    /* zero_offset */ 0.0f,
    /* range_ns    */ 0.0f,    /* 0 = unknown; loop must refuse to run */
    /* acq   */ { 1.20, 0.02, 0.0, 8000.0 },   /* coarse, frequency-led  */
    /* dpll  */ { 0.40, 0.02, 2.0, 5000.0 },   /* wide-band, fast settle */
    /* lock  */ { 0.05, 0.001, 0.0, 2000.0 },  /* narrow-band, slow      */
    /* acq_threshold_ns */ 100.0f,
    /* dpll_lock_thresh */ 5.0e-10f,
    /* lock_interval_s  */ 300u,
    /* state   */ LTIC_ACQ,
    /* submode */ 0u,
    /* polarity*/ 0,          /* 0 = auto-detect PWM→phase sign */
    /* centre_v*/ 0.0f,       /* 0 = use detected range middle  */
};

/* Algorithm 11 (LTIC-Lars) parameters. gain 0 = auto from CT calibration; a
 * non-zero gain set with LG overrides the auto value with a manual scale. */
LarsParams_t g_lars = {
    /* gain         */ 0.0f,    /* 0 = auto from CT calibration; LG sets manual */
    /* damping      */ 3.0f,    /* Lars' default                                */
    /* time_const_s */ 60u,     /* ~ limit-cycle period / 6                      */
    /* filter_div   */ 2u,      /* pre-filter = time_const / 2                   */
    /* tic_offset   */ 2620u,   /* ~2.11 V at 3.3 V / 4096 (mid-band from log)   */
    /* lock_ns_lim  */ 100u,    /* Lars' default phase window                    */
    /* lock_factor  */ 5u,      /* Lars' default                                */
    /* temp_coeff   */ 0,       /* off                                          */
    /* temp_ref     */ 0u,      /* set from BMP at enable time                   */
    /* flags        */ 0u,      /* temp comp disabled                           */
    /* reserved     */ { 0, 0, 0, 0, 0, 0 },
};

/* Algo 11 live telemetry (see header). Published by ltic_lars_pi each cycle. */
float g_lars_scale      = 0.0f;
float g_lars_phase_filt = 0.0f;
bool  g_lars_locked     = false;
bool  g_lars_gain_auto  = true;

/* ======================================================================
 * ALGORITHM 10 — LTIC three-stage PLL (ACQ → DPLL → LOCK)
 *
 * Disciplines the OCXO using the hardware TIC phase voltage (PA1), which
 * resolves phase far finer than the TIM2 cycle counter. A hybrid design:
 * the COARSE stages lean on the TIM2 frequency error (robust, no wrap), and
 * the fine stages lean on the LTIC phase (high resolution). State machine:
 *
 *   ACQ   frequency-led pull-in (TIM2). Get the OCXO close to 10 MHz so the
 *         phase ramps slowly and the detector can be caught. picDIV is armed
 *         on entry. Exit when |phase| is inside acq_threshold_ns for a few
 *         cycles.
 *   DPLL  both terms: Kp·e_freq (TIM2, fast) + phase PI (LTIC). Centre the
 *         phase quickly. Exit to LOCK when |phase| is small AND drift is low.
 *   LOCK  phase-led (LTIC), slow updates every lock_interval_s, narrow band.
 *         Exit back to DPLL if |phase| exceeds a hysteresis band persistently.
 *
 * Phase is taken from g_ltic_voltage. If calibrated (ns_per_volt != 0) the
 * loop works in nanoseconds against zero_offset; if not, it falls back to a
 * volts-based error around mid-rail and a nominal scale (a warning is printed
 * once). State persists in g_ltic.state so a warm reboot (RB) resumes where it
 * left off instead of restarting cold from ACQ.
 *
 * This is our own design; it borrows the staged structure and the bumpless
 * idea from the time-nuts discussion but works in calibrated units with the
 * project's existing snapshot/clamp/trend helpers.
 * ====================================================================== */
#ifdef GPSDO_LTIC

/* Convert the latest TIC voltage to a signed phase error.
 *   calibrated:   error_ns = (V - zero_offset) * ns_per_volt
 *   uncalibrated: error is measured against zero_offset if LC has at least set
 *                 it (mid of the swept range), else against a coarse mid-rail.
 * The key point learned on real hardware: the detector's working range may be
 * a narrow band well away from mid-ADC (e.g. 0..0.45 V), so we must NOT assume
 * 1.65 V is the centre — we use the calibrated zero_offset. Returns phase
 * error; *valid is false if the reading is railed (outside the detector
 * window), which the loop treats specially. */
/* Damping-term frequency error, windowed per the FA command. Reads the average
 * the caller passes in (10/100/1000), falling back to the smooth default
 * (e_freq, i.e. avg100) whenever the requested window is not full yet. With the
 * default 100 it returns bit-for-bit e_freq: the whole point is that a window of
 * 100 changes nothing.
 *
 * Called once per state — DPLL and LOCK pass their own windows (FAD / FAL) — so
 * the frequency term can be damped differently in acquisition and steady state.
 * Escape detection, self-learning, the state machine and the phase PI all keep
 * the smooth avg100 via e_freq, where a noisier average would cost more than it
 * gains. */
static double damp_e_freq(const FreqSnapshot_t *s, double e_freq_default,
                          uint16_t win)
{
    switch (win) {
        case 10:   return s->have10   ? (s->avg10   - (double)BASE_FREQ) : e_freq_default;
        case 1000: return s->have1000 ? (s->avg1000 - (double)BASE_FREQ) : e_freq_default;
        case 100:
        default:   return e_freq_default;   /* identical to the historical path */
    }
}

static double ltic_phase_error_ns(bool *valid, uint32_t ppscount)
{
    float v = g_ltic_voltage;
    *valid = (v > 0.02f && v < 3.28f);          /* not railed low/high */

    /* SATURATION GUARD for the Kaashoek RC detector. The voltage-to-phase
     * mapping is only linear over the swept band measured by LC (span =
     * range_ns / ns_per_volt, centred on zero_offset). Outside it the cap is
     * plateaued: V no longer tracks phase, so a "phase_ns" computed from a
     * near-rail V is garbage that the loop then integrates — the mechanism of
     * the observed ~370 s limit cycle (saturate → false phase → integrator
     * overshoot → re-saturate). Mark such samples invalid so the loop holds
     * instead of chasing a phantom. Allow ~10 % margin past the band. */
    if (*valid && g_ltic.range_ns > 1.0f && g_ltic.ns_per_volt > 1.0f) {
        double span_v = (double)g_ltic.range_ns / (double)g_ltic.ns_per_volt;
        double lo = (double)g_ltic.zero_offset - 0.55 * span_v;
        double hi = (double)g_ltic.zero_offset + 0.55 * span_v;
        if ((double)v < lo || (double)v > hi) *valid = false;
    }

    /* Centre: use the calibrated zero_offset when we have one (LC sets it to
     * the mid-point of the actual swept band). Fall back to it even when
     * ns_per_volt is 0, since LC may set zero_offset/range without a trusted
     * slope. Only if nothing is known do we use a coarse 0.22 V guess that
     * matches the narrow low-band detectors seen in practice. */
    double centre = (g_ltic.zero_offset > 0.001f) ? (double)g_ltic.zero_offset : 0.22;
    double slope  = (g_ltic.ns_per_volt != 0.0f)  ? (double)g_ltic.ns_per_volt : 100.0;
    double phase  = ((double)v - centre) * slope;

    /* Sawtooth correction: the receiver's 1PPS lands on an internal clock
     * edge, off true GPS time by a known qErr (UBX-TIM-TP). Subtracting it
     * removes the receiver granularity sawtooth (LEA-6T: ~±10 ns) and leaves
     * the OCXO's own phase error. Zero when SAW is off or no fresh qErr.
     *
     * Pairing by ppscount (v0.95 audit fix): the correction is taken ONLY if
     * the latest TIM-TP genuinely describes THIS pulse — accounting for the
     * mode bit (next-pulse vs this-pulse). If TIM-TP hasn't arrived yet for
     * this PPS (vGpsTask lagging) or arrived for a different one, the call
     * returns 0 and the sample is treated as uncorrected, rather than
     * subtracting a stale qErr off by one PPS. */
    phase -= (double)ubx_timtp_correction_for_pps(ppscount);
    return phase;
}

/* Request a picDIV arm (non-blocking; control task sequences the pulse). */
static void ltic_arm_picdiv(void)
{
    xEventGroupSetBits(xSysEvents, EVT_ARM_PICDIV);
}

/* ---- LTIC auto-tuning ----------------------------------------------------
 * Derive every loop coefficient from the two MEASURED hardware constants:
 *   K   (Hz per PWM LSB, from CT — recovered via g_pid[7].Kp = 0.40/K)
 *   nsv (ns per volt) and range_ns (from LC)
 * so no per-board hand tuning (AQP/DPP/...) is ever required. Called after a
 * successful LC and on entry to algorithm 10. Design rules:
 *   freq loop : cancel ~50-60%% of Δf per step  → Kp = 0.5·(1/K)
 *   phase loop: pull phase to zero with τ≈20 s  → Kd = (1/K)/(100·20) LSB/ns
 *               integral 10× slower; LOCK 4× gentler than DPLL
 *   ACQ threshold: quarter of the measured detector range (clamped 20..200).
 */
void ltic_autotune(void)
{
    double lsb_per_hz = (g_pid[7].Kp > 100.0) ? (g_pid[7].Kp / 0.40) : 3000.0;
    /* Phase gain for DPLL. The original τ=20 s (lsb_per_hz/2000) crawled: with
     * a narrow detector (ns/V small, e.g. 34 on an LVC74 clocked at 10 MHz)
     * an 18 ns error produced ~28 LSB per 2 s step, i.e. ~1 ns/s of phase
     * pull — the DPLL→LOCK gate (|phase| ≤ 0.4×acq_threshold) took many
     * minutes. /800 gives ~2.5× the pull and still leaves the loop well
     * damped, because the phase term is integrated over a 2 s period. */
    double kd_dpll = lsb_per_hz / 800.0;

    g_ltic.acq.Kp  = 0.5 * lsb_per_hz;   /* LSB per Hz of e_freq            */
    g_ltic.acq.Ki  = 0.02;               /* centre pull (V-domain, capped)  */
    g_ltic.acq.Kd  = 0.0;
    g_ltic.acq.I_LIMIT = 8000.0;

    g_ltic.dpll.Kp = 0.5 * lsb_per_hz;   /* freq feed                       */
    g_ltic.dpll.Kd = kd_dpll;            /* phase proportional, LSB per ns  */
    g_ltic.dpll.Ki = kd_dpll / 6.0;      /* phase integral per 2 s step     */
    g_ltic.dpll.I_LIMIT = 5000.0;

    /* LOCK stays deliberately slow: it is the narrow-band state and its gains
     * are referenced to the ORIGINAL conservative constant, not to the faster
     * DPLL one, so speeding up acquisition does not raise the locked noise. */
    double kd_lock = lsb_per_hz / 2000.0;
    g_ltic.lock.Kp = 0.0;                /* TIM2 noise floor — phase only   */
    g_ltic.lock.Kd = kd_lock / 4.0;
    g_ltic.lock.Ki = kd_lock / 40.0;
    g_ltic.lock.I_LIMIT = 2000.0;

    if (g_ltic.range_ns > 1.0f) {
        float th = g_ltic.range_ns / 4.0f;
        if (th < 20.0f)  th = 20.0f;
        if (th > 200.0f) th = 200.0f;
        g_ltic.acq_threshold_ns = (uint16_t)th;
    }
    OUT_SERIAL.print("LTIC autotune: lsb/Hz=");   OUT_SERIAL.print(lsb_per_hz, 0);
    OUT_SERIAL.print("  dpll Kd=");               OUT_SERIAL.print(g_ltic.dpll.Kd, 3);
    OUT_SERIAL.print(" Ki=");                     OUT_SERIAL.print(g_ltic.dpll.Ki, 4);
    OUT_SERIAL.print("  acq_thresh=");            OUT_SERIAL.print(g_ltic.acq_threshold_ns);
    OUT_SERIAL.println(" ns");
}

uint16_t ltic_three_stage(uint16_t pwm, uint32_t ppscount)
{
    /* ---- persistent loop memory ---- */
    static double   integ        = 0.0;     /* integral term (PWM units)     */
    static double   last_phase   = 0.0;     /* for drift estimate            */
    static uint32_t stable_cnt   = 0;       /* consecutive in-band cycles    */
    static uint32_t exit_cnt     = 0;       /* consecutive out-of-band (LOCK)*/
    static uint32_t last_lock_pps= 0;       /* LOCK cadence timer            */
    static uint32_t last_pps     = 0xFFFFFFFF;
    static bool     warned_uncal = false;
    static bool     seeded       = false;
    static uint8_t  prev_state   = 0xFF;

    /* Seed integral from the incoming PWM once, so the first correction is
     * bumpless (no jump from a cold integrator). */
    if (!seeded) { integ = (double)pwm; seeded = true; }

    /* Flush detector on PPS-counter reset */
    if (ppscount < last_pps) { stable_cnt = 0; exit_cnt = 0; }
    last_pps = ppscount;

    /* One-time uncalibrated warning */
    if (g_ltic.ns_per_volt == 0.0f && !warned_uncal) {
        OUT_SERIAL.println("LTIC: running UNCALIBRATED (run LC). Using nominal V-based phase.");
        warned_uncal = true;
    }

    /* Resume state from EEPROM-backed g_ltic.state on first run / state change.
     * On entering ACQ, arm the picDIV. */
    uint8_t state = g_ltic.state;
    if (state > LTIC_LOCK) state = LTIC_ACQ;
    /* BOOT SANITY: a persisted LOCK/DPLL is only trustworthy if the present
     * phase is both VALID and already CLOSE to zero_offset. After a power cycle
     * the OCXO has thermally drifted and the picDIV edge can be anywhere, so
     * the detector may start railed OR merely far off centre. Seen on air: a
     * warm boot resumed LOCK with Vphase ≈2.09 V while zero_offset was 1.85 V
     * (~300 mV ≈ hundreds of ns off) — technically "on the ramp" so the old
     * valid-only check passed, but far too coarse for LOCK; DPLL then dropped
     * it to ACQ a minute later and the full ~6 min pull-in ran anyway. On the
     * FIRST call of this boot (prev_state == 0xFF), demote a persisted
     * LOCK/DPLL to ACQ unless the phase is valid AND within the ACQ window of
     * zero_offset — i.e. genuinely where a lock belongs. This costs nothing on
     * a clean warm boot (already centred → stays LOCK) and skips the wasted
     * LOCK→DPLL→ACQ bounce when it isn't. */
    if (prev_state == 0xFF && state != LTIC_ACQ) {
        bool boot_valid = false;
        double boot_ph = ltic_phase_error_ns(&boot_valid, ppscount);
        bool near_centre = boot_valid &&
                           fabs(boot_ph) <= (double)g_ltic.acq_threshold_ns;
        if (!near_centre) {
            state = LTIC_ACQ;
            g_ltic.state = LTIC_ACQ;
        }
    }
    if (state != prev_state) {
        if (state == LTIC_ACQ) { ltic_autotune(); ltic_arm_picdiv(); }
        prev_state = state;
    }

    /* Update period depends on state. DPLL corrects every 2 s (not 10): on a
     * narrow detector the phase sweeps its whole range in ~10-15 s of residual
     * drift, so a 10 s interval let it wander between corrections and the lock
     * never closed. LOCK uses lock_interval_s but capped so it can still track
     * a narrow detector; if that is set very large (legacy) we clamp to 5 s. */
    uint32_t lock_iv = g_ltic.lock_interval_s;
    /* Clamp to the nearest bound rather than snapping to a default: an
     * out-of-range value used to jump to 5 s, so asking for a SLOWER loop
     * silently gave you the fastest one. The field is uint16 and 600 fits. */
    if (lock_iv < 1u)        lock_iv = 1u;
    else if (lock_iv > 600u) lock_iv = 600u;
    uint32_t period = (state == LTIC_LOCK) ? lock_iv
                    : (state == LTIC_DPLL) ? 2u : 5u;
    if ((ppscount % period) != 0) return pwm;

    /* ---- read both sensors ---- */
    FreqSnapshot_t s;
    take_freq_snapshot(&s);
    /* e_freq from avg100 (0.01 Hz), not avg10 (0.1 Hz). avg10's coarse
     * quantisation, times Kp (~1550 LSB/Hz), produced ±150 LSB PWM jumps — a
     * ~10 s limit cycle in ACQ that stopped the phase settling under the lock
     * threshold (GML-5.2 analysis). avg100 is 10× finer and settles cleanly.
     * Fall back to avg10 only until the first 100 s window has filled. */
    double e_freq = s.have100 ? (s.avg100 - (double)BASE_FREQ)
                  : s.have10  ? (s.avg10  - (double)BASE_FREQ) : 0.0;

    /* Damping-term windows, per state (FAD / FAL). With both at the default 100
     * these are bit-for-bit e_freq, so behaviour is unchanged unless the operator
     * shortens a window to chase a limit cycle. */
    double e_freq_damp_dpll = damp_e_freq(&s, e_freq, g_freq_damp_win_dpll);
    double e_freq_damp_lock = damp_e_freq(&s, e_freq, g_freq_damp_win_lock);

    bool ph_valid = false;
    double phase_ns = ltic_phase_error_ns(&ph_valid, ppscount);

    /* drift = change in phase per second. Reject wrap-induced spikes: if the
     * phase appears to jump more than half the detector range in one step, it
     * wrapped rather than really moved that far, so we skip the drift update
     * this cycle (a false huge slope would otherwise wreck the ACQ drive and
     * the slope-gated transitions). */
    double drift = 0.0;
    double half_range = (g_ltic.range_ns > 1.0f) ? (double)g_ltic.range_ns * 0.5 : 150.0;
    double dphase = phase_ns - last_phase;
    if (fabs(dphase) < half_range) {
        drift = dphase / (double)period;
    }   /* else: wrap — leave drift at 0 for this cycle */
    last_phase = phase_ns;

    /* ---- pick the active PID set for this state ---- */
    PidParams_t *pid = (state == LTIC_ACQ)  ? &g_ltic.acq
                     : (state == LTIC_DPLL) ? &g_ltic.dpll
                     :                        &g_ltic.lock;

    /* ---- compute correction ----
     * ACQ : drive the phase toward the CENTRE of the detector range (not a rail
     *       and not zero_offset, which can sit near the floor). The PWM→phase
     *       sign is board-dependent and set once by the user via LPOL (a
     *       single-cycle auto-probe proved unreliable on a narrow, drifting
     *       detector — the phase's own drift swamped the probe and the sign
     *       came out wrong), so ACQ holds until g_ltic.polarity is set.
     * DPLL: frequency Kp + phase PI (same polarity).
     * LOCK: phase-led PI, frequency term dropped (TIM2 quantisation noise). */

    /* Centre target: explicit centre_v if set, else the middle of the detected
     * range (range_ns/ns_per_volt gives the span in volts), else a safe 0.16 V
     * that keeps us off both rails on a narrow low-band detector. */
    double centre_v;
    if (g_ltic.centre_v > 0.001f) {
        centre_v = (double)g_ltic.centre_v;          /* explicit LCV override  */
    } else if (g_ltic.zero_offset > 0.001f) {
        /* zero_offset IS the middle of the observed band since v0.66/v0.74 —
         * the old "+ span/2" here dated from when it was the band's floor and
         * made ACQ park the phase half a range away from the point the
         * ACQ→DPLL threshold is measured against (a permanent stalemate:
         * loop holds at its "centre", threshold never satisfied). One point
         * of truth now: the pull target equals the threshold's zero. */
        centre_v = (double)g_ltic.zero_offset;
    } else {
        centre_v = 0.16;
    }

    double u = 0.0;
    if (state == LTIC_ACQ) {
        float vraw = g_ltic_voltage;
        double err_v = (double)vraw - centre_v;    /* how far from centre, in volts */

        /* --- polarity handling ---
         * A single-cycle probe cannot separate the PWM effect from the phase's
         * own drift on a narrow, drifting detector (the drift dominates dV and
         * the sign comes out wrong), so we do NOT auto-probe. If polarity is
         * unset, hold and ask the user to set it once with LPOL (then ES). This
         * is reliable; the probe was not. */
        int8_t pol = g_ltic.polarity;
        static uint32_t warn_ms = 0;

        if (pol == 0) {
            u = 0.0;                         /* hold — do not guess the sign */
            uint32_t now = millis();
            if (now - warn_ms > 10000u) {    /* remind every 10 s */
                OUT_SERIAL.println("LTIC ACQ: polarity unset — run 'LPOL -1' (or +1) then 'ES'. Holding.");
                warn_ms = now;
            }
        } else {
            /* Two-part ACQ drive:
             *  drift_term  — dominant: nulls the phase slope (frequency offset)
             *                so the phase stops sweeping/wrapping.
             *  centre_term — once the drift is small, actively walk the phase
             *                to mid-range. A weak pull is not enough: to move
             *                the phase from a parked position we must inject a
             *                deliberate, bounded frequency offset (PWM step)
             *                proportional to how far off-centre we are, then the
             *                drift term arrests it again near the middle. */
            /* ACQ drives the TIM2-measured frequency error to zero — NOT the
             * voltage-derived drift. The stepped detector read goes flat at a
             * band edge (seen on air: phase parked at 0.336 V while a real
             * −0.3 Hz offset persisted), which blinds a V-derived slope; TIM2
             * sees the offset regardless. Gains come from ltic_autotune()
             * (Kp = LSB per Hz, derived from measured K).
             * SIGN: K is positive on every board (+PWM → +f), so the frequency
             * path takes NO board polarity; pol applies only to the PHASE
             * (Vphase) path below. Routing e_freq through pol=-1 was inverting
             * a correct frequency correction. */
            double freq_term   = pid->Kp * e_freq;          /* no pol here */
            double centre_term = 0.0;
            if (fabs(drift) < 4.0) {                          /* settled enough to steer */
                /* err_v>0 → phase above centre → drive it down. The step must
                 * be small enough that the phase CRAWLS toward centre without
                 * shooting through a sensitive detector window and wrapping to
                 * the far rail (the old 6000×/±400 LSB pairing was tuned for a
                 * less sensitive HC74). Tunable live with ACG. */
                centre_term = (double)g_ltic_acq_centre_gain * err_v;
                double cap = (double)g_ltic_acq_centre_cap;
                if (centre_term >  cap) centre_term =  cap;
                if (centre_term < -cap) centre_term = -cap;
            }
            u = -freq_term - (double)pol * centre_term;
            if (u >  1500.0) u =  1500.0;
            if (u < -1500.0) u = -1500.0;
        }
    } else {
        /* DPLL / LOCK: phase PI in ns. Apply the SAME board polarity as ACQ.
         * If polarity is still unknown (0), do NOT guess a sign and risk
         * running the phase onto a rail — hold PWM and let the machine drop
         * back to ACQ, which will probe and set g_ltic.polarity.
         *
         * SATURATION HANDLING: if the phase read is INVALID (detector
         * saturated — see the guard in ltic_phase_error_ns), the
         * voltage-derived phase is garbage, so we freeze the PHASE path
         * (proportional + integral) but KEEP the FREQUENCY path (TIM2 sees
         * the true offset regardless of detector saturation). That is how the
         * loop recovers: TIM2 pulls the OCXO back into the detector window,
         * phase becomes valid again, and the PI resumes. Without this, a
         * saturated read fed a false 1000 ns phase to the integrator and
         * drove the ~370 s limit cycle (saturate → slam → overshoot →
         * re-saturate). */
        int8_t pol = g_ltic.polarity;
        if (pol == 0) {
            u = 0.0;                    /* hold; polarity not established yet */
        } else if (!ph_valid) {
            /* frequency-only recovery: TIM2 pull, no phase integral wind-up */
            double freq_term = (state == LTIC_DPLL) ? (pid->Kp * e_freq_damp_dpll)
                           : (state == LTIC_LOCK)  ? (pid->Kp * e_freq_damp_lock * 0.3) : 0.0;
            u = -freq_term;
        } else {
            /* LOCK gentleness (deadband + soft knee): inside the deadband the
             * phase error is treated as zero (ADC noise floor — do not chase
             * it) and the integrator holds; outside, the error ramps from
             * zero (soft knee). Sized from the same measured constants as
             * autotune. Computed FIRST — both the integrator and the phase
             * term below use p_eff. */
            double p_eff = phase_ns;
            if (state == LTIC_LOCK) {
                double db = (g_ltic.range_ns > 1.0f) ? (double)g_ltic.range_ns / 40.0 : 8.0;
                if (db < 6.0) db = 6.0;
                if (fabs(p_eff) <= db) p_eff = 0.0;
                else                   p_eff -= (p_eff > 0.0) ? db : -db;
            }
            if (!(state == LTIC_LOCK && p_eff == 0.0))     /* deadband: hold integ */
                integ += -(double)pol * (pid->Ki * p_eff);
            if (integ > 65300.0) integ = 65300.0;
            if (integ < 200.0)   integ = 200.0;
            /* Frequency path: NO pol (K positive on every board); autotuned
             * Kp is already LSB-per-Hz. Phase path keeps pol.
             *
             * LOCK now carries a GENTLE frequency term (0.1×Kp), not zero. With
             * freq_term=0 the only defence against a real OCXO drift was the
             * slow drift feed-forward, which on warm hardware lagged ~60× too
             * slow — the phase walked 11→−425 ns in 51 s and lock dropped
             * (LOCK→DPLL→ACQ). A light 0.1×Kp cancels the live frequency error
             * every update without injecting the TIM2 quantisation noise a full
             * DPLL-strength term would (e_freq is the smooth avg100 now). The
             * feed-forward still absorbs the systematic part; this catches the
             * rest immediately. Credit: analysis by GML-5.2. */
            double freq_term  = (state == LTIC_DPLL) ? (pid->Kp * e_freq_damp_dpll)
                              : (state == LTIC_LOCK) ? (pid->Kp * e_freq_damp_lock * 0.1)
                              : 0.0;
            double phase_term = (double)pol * pid->Kd * p_eff;
            u = integ - (double)pwm - freq_term - phase_term;
            /* self-learning: damp the correction, add drift feed-forward when
             * locked. Driven by e_freq (closed-loop); phase_ns feeds the
             * limit-cycle/damping observer. */
            {
                bool  lk = (state == LTIC_LOCK);
                float ff = lrn_update_ef(e_freq, phase_ns, (double)period, lk);
                u *= (double)g_lrn_damp;
                u += (double)ff;
            }
            if (state == LTIC_LOCK) {
                /* hard cap per step: ≈4 mHz regardless of unit (from measured K) */
                double lsbhz = (g_pid[7].Kp > 100.0) ? (g_pid[7].Kp / 0.40) : 3000.0;
                double cap = lsbhz * 0.004;
                if (cap < 3.0) cap = 3.0;
                if (u >  cap) u =  cap;
                if (u < -cap) u = -cap;
            }
        }
    }

    /* slew-rate limit from I_LIMIT (reuse field as max step here) */
    double max_step = pid->I_LIMIT > 0 ? pid->I_LIMIT : 5000.0;
    if (u >  max_step) u =  max_step;
    if (u < -max_step) u = -max_step;

    /* Runaway guard — reviewed after a real 3 Hz escape reached PWM 63500:
     *  (1) PRIMARY criterion is the measured frequency error from TIM2, not a
     *      PWM-LSB excursion: a hardcoded LSB threshold silently assumes the
     *      OCXO's Hz/LSB sensitivity (a false, per-unit assumption), while
     *      e_freq is hardware truth. Limit: |e_freq| > 0.5 Hz with the phase
     *      railed → freeze.
     *  (2) The baseline must NOT re-anchor on every healthy sample: during a
     *      runaway the phase periodically wraps (briefly un-railed), and the
     *      old guard re-baselined each time — it chased the escape and never
     *      tripped. Re-baseline only when the loop is genuinely healthy
     *      (un-railed AND |e_freq| < 0.25 Hz).
     *  (3) Freezing the step is not enough: the DPLL/LOCK integrator kept
     *      winding up and would slam PWM on recovery — re-seed it while
     *      frozen. */
    static uint16_t start_pwm = 0;
    static bool     start_set = false;
    static bool     runaway_warned = false;
    static double   prev_abs_ef = 0.0;   /* |e_freq| at the previous railed cycle */
    static uint8_t  no_improve  = 0;     /* consecutive railed cycles with no gain */
    if (!start_set) { start_pwm = pwm; start_set = true; }

    /* Rail threshold from the LC calibration where one exists: the detector band
     * is zero_offset ± half the swept span, so a fixed 3.28 V only happens to be
     * right for one board. */
    float rail_hi = 3.28f;
    if (g_ltic.range_ns > 1.0f && g_ltic.ns_per_volt > 1.0f) {
        float span_v = g_ltic.range_ns / g_ltic.ns_per_volt;
        rail_hi = g_ltic.zero_offset + 0.55f * span_v;   /* high edge of the band */
    }
    bool railed_now = (g_ltic_voltage <= 0.02f || g_ltic_voltage >= rail_hi);
    int32_t pwm_excursion = (int32_t)pwm - (int32_t)start_pwm;

    /* A railed detector together with a large |e_freq| is NOT by itself a
     * runaway — it is the normal state of a cold or far-off OCXO at the start of
     * acquisition: the phase sweeps the whole range in seconds, so the cap sits
     * against a stop while TIM2 still reports the true offset. Freezing there
     * removes the only path back, because the frequency term is exactly what
     * pulls the OCXO into the detector window.
     *
     * What actually distinguishes a runaway is that the correction does not
     * work: with the polarity wrong the loop drives the wrong way and |e_freq|
     * fails to shrink however long it runs. So track improvement across railed
     * cycles and only freeze once the error has stalled for several of them. A
     * healthy acquisition improves every cycle and never trips this; a genuine
     * runaway trips after ~5 corrections. Without this both guards fired during
     * perfectly healthy pull-ins — one observed run moved 3855 LSB while railed
     * and was frozen mid-recovery. */
    double abs_ef = fabs(e_freq);
    if (railed_now) {
        if (prev_abs_ef > 0.0 && abs_ef > prev_abs_ef * 0.98) {
            if (no_improve < 255u) no_improve++;   /* stalled this cycle */
        } else {
            no_improve = 0;                        /* still converging */
        }
        prev_abs_ef = abs_ef;
    } else {
        no_improve  = 0;
        prev_abs_ef = 0.0;
    }
    bool stalled      = (no_improve >= LTIC_RUNAWAY_STALL);
    bool freq_escape  = railed_now && abs_ef > 0.5 && stalled;
    bool lsb_backstop = railed_now && stalled &&
                        (pwm_excursion > 2000 || pwm_excursion < -2000);
    if (freq_escape || lsb_backstop) {
        if (!runaway_warned) {
            OUT_SERIAL.print("LTIC: runaway (");
            OUT_SERIAL.print(e_freq, 2);
            OUT_SERIAL.println(" Hz, phase railed, not converging) — freezing; check LPOL / re-centre.");
            runaway_warned = true;
        }
        u = 0.0;                        /* freeze; do not chase further */
        integ = (double)pwm;            /* and stop the integrator winding up */
    } else if (!railed_now && fabs(e_freq) < 0.25) {
        runaway_warned = false;         /* genuinely healthy: recovered */
        no_improve     = 0;
        start_pwm = pwm;                /* re-baseline ONLY here */
    }

    /* ---- ACQ re-arm retry -------------------------------------------------
     * The entry arm above fires once, on the TRANSITION into ACQ. If that sync
     * does not land the phase inside the detector window — the flip-flop's
     * ambiguous point, or the OCXO still far enough off that the phase sweeps
     * straight back out — ACQ gets no second attempt: the state does not
     * change, so the transition never repeats, the detector reads "ovf"
     * indefinitely and only a manual AP recovers it. Reported from the field
     * after a reboot (Dan Wiering).
     *
     * Mirrors algorithm 11's phase-capture bridge: once the frequency has
     * settled but the phase is still railed, re-arm periodically. Gated on the
     * frequency because arming while the OCXO is still far off is pointless —
     * the phase would race out of the window again immediately — and held off
     * afterwards so a failed capture retries about every 20 s rather than every
     * cycle. */
    static uint32_t acq_railed_cnt = 0;
    static uint32_t acq_rearm_hold = 0;
    if (state == LTIC_ACQ) {
        if (acq_rearm_hold > 0) {
            acq_rearm_hold--;
            acq_railed_cnt = 0;
        } else if (railed_now && fabs(e_freq) <= 0.05) {
            if (acq_railed_cnt < 0xFFFFFFFFu) acq_railed_cnt++;
            if (acq_railed_cnt >= 5u) {
                ltic_arm_picdiv();
                acq_railed_cnt = 0;
                acq_rearm_hold = 15u;
            }
        } else {
            acq_railed_cnt = 0;
        }
    } else {
        acq_railed_cnt = 0;
        acq_rearm_hold = 0;
    }

    uint16_t out = clamp_pwm((int32_t)pwm + (int32_t)u);

    /* ---- state transitions ---- */
    char trend[5] = "ACQ ";
    if (state == LTIC_ACQ) {
        strcpy(trend, "ACQ ");
        /* ACQ → DPLL: phase inside the ACQ window AND its slope (drift) inside
         * a WIDE window. Slope matters because the phase can sweep through
         * centre with a large slope (frequency still far off) — catching it
         * there would lock onto the wrong frequency. Slope = dPhase/dt, and
         * frequency is the first derivative of phase, so a small slope means
         * the frequency is already close to 10 MHz. Wide slope gate here,
         * tightened at the next transition. (Insight from Dan / time-nuts.) */
        /* Gate on TIM2 (|Δf|), not on the V-derived slope: the stepped
         * detector read produces phantom 50-100 ns/s spikes at each step,
         * which kept the loop parked in ACQ for hundreds of cycles. TIM2 is
         * immune to the stepping. Position (phase_ns) still comes from V. */
        if (ph_valid && fabs(phase_ns) <= (double)g_ltic.acq_threshold_ns
                     && fabs(e_freq)   <= 0.05) {
            if (++stable_cnt >= 3) { state = LTIC_DPLL; stable_cnt = 0; integ = (double)out; }
        } else if (stable_cnt > 0) stable_cnt--;
    } else if (state == LTIC_DPLL) {
        strcpy(trend, "DPLL");
        /* DPLL → LOCK: tight phase AND tight slope, sustained. The gate was
         * 0.4×threshold held for 6 cycles (12 s); with a narrow detector that
         * demanded sub-11 ns centring before LOCK would ever engage. 0.5× for
         * 4 cycles (8 s) reaches LOCK markedly sooner while still requiring a
         * genuinely settled phase — LOCK's own hysteresis (1.5×) and its
         * 3-cycle exit counter still protect against a premature promotion. */
        bool tight = ph_valid && fabs(phase_ns) <= (double)g_ltic.acq_threshold_ns * 0.5
                              && fabs(e_freq)   <= 0.03;
        if (tight) { if (++stable_cnt >= 4) { state = LTIC_LOCK; stable_cnt = 0; last_lock_pps = ppscount; } }
        else if (stable_cnt > 0) stable_cnt--;   /* a stepped read sets back, not to zero */
        bool broken = !ph_valid || fabs(phase_ns) > (double)g_ltic.acq_threshold_ns * 3.0
                               || fabs(e_freq)   > 0.30;
        if (broken) {
            if (++exit_cnt >= 3) { state = LTIC_ACQ; exit_cnt = 0; ltic_arm_picdiv(); }
        } else if (exit_cnt > 0) exit_cnt--;
    } else { /* LOCK */
        strcpy(trend, "LOCK");
        /* Leave LOCK if the phase leaves a hysteresis band OR the slope grows
         * (frequency drifting away) — both indicate the lock is degrading. */
        double hyst = (double)g_ltic.acq_threshold_ns * 1.5;
        if (!ph_valid || fabs(phase_ns) > hyst || fabs(e_freq) > 0.10) {
            if (++exit_cnt >= 3) { state = LTIC_DPLL; exit_cnt = 0; stable_cnt = 0; }
        } else if (exit_cnt > 0) exit_cnt--;
    }

    /* persist state (cheap; EEPROM only written on ES) */
    g_ltic.state = state;
    prev_state = state;

    set_trend(trend);
    return out;
}
#endif /* GPSDO_LTIC */

/* ======================================================================
 * ALGORITHM DISPATCHER
 * ====================================================================== */
uint16_t ltic_lars_pi(uint16_t pwm, uint32_t ppscount)
{
    /* ppscount is unused: unlike algo 10, this loop updates every second with
     * no period gate — the adaptive filter provides the smoothing instead. */
    (void)ppscount;
    /* Persistent loop state. */
    static float    s_integ        = 0.0f;   /* dacValue: integral accumulator */
    static float    s_i_remain      = 0.0f;   /* Lars' I_term_remain            */
    static float    s_phase_filt    = 0.0f;   /* filtered phase [ns]            */
    static uint32_t s_lock_cnt      = 0;      /* Lars' lockPPScounter           */
    static bool     s_locked        = false;
    static bool     s_init          = false;
    static uint16_t s_tc_old        = 0;      /* detect timeConst change        */

    /* Refuse to run without TIC calibration — phase has no scale otherwise. */
    if (g_ltic.ns_per_volt == 0.0f || g_ltic.range_ns == 0.0f) {
        set_trend(0);
        return pwm;
    }

    /* One-time seed: start the integrator at the current PWM so we take over
     * smoothly from whatever value the OCXO was already sitting at. */
    if (!s_init) {
        s_integ      = (float)pwm;
        s_phase_filt = 0.0f;
        s_i_remain   = 0.0f;
        s_lock_cnt   = 0;
        s_locked     = false;
        s_tc_old     = g_lars.time_const_s;
        s_init       = true;
    }

    /* ---- inputs, evaluated EVERY second (no period gate) ---- */
    FreqSnapshot_t s;
    take_freq_snapshot(&s);
    double e_freq = s.have100 ? (s.avg100 - (double)BASE_FREQ)
                  : s.have10  ? (s.avg10  - (double)BASE_FREQ) : 0.0;

    bool   ph_valid = false;
    double phase_ns = ltic_phase_error_ns(&ph_valid, ppscount);   /* 0 = on target       */
    bool   railed   = !ph_valid;

    /* ---- adaptive filter/time constants (Lars 261-263) ----
     * filterConst = timeConst / filterDiv, forced to 1 while unlocked so the
     * loop reacts fast during acquisition and smooths only once settled. */
    uint16_t time_const = g_lars.time_const_s;
    if (time_const < 1u) time_const = 1u;
    uint32_t filt = (uint32_t)time_const / (g_lars.filter_div ? g_lars.filter_div : 1u);
    if (filt < 1u) filt = 1u;
    if (!s_locked) filt = 1u;

    /* If timeConst changed under us, rescale the integrator so the output does
     * not jump (Lars 266-268: dacValue scales with timeConst). Our integrator
     * is in PWM units directly, so no rescale is needed here — but reset the
     * remainder to avoid a stale fractional carry. */
    if (time_const != s_tc_old) { s_i_remain = 0.0f; s_tc_old = time_const; }

    /* ---- phase pre-filter (Lars 285), outlier-gated ----
     * Exponential smoother with variable constant. Skip the update on a railed
     * reading (no valid phase) so a rail excursion cannot poison the filter. */
    if (!railed) {
        s_phase_filt += ((float)phase_ns - s_phase_filt) / (float)filt;
    }

    /* ---- frequency-led assist when the phase is out of range ----
     * railed → the phase detector is blind, so steer by TIM2 frequency alone,
     * exactly the situation that used to freeze algo 10. e_freq is in Hz;
     * multiply by gain-scaled term to pull PWM back toward the detector window.
     * The sign convention matches the phase path (positive error → reduce PWM),
     * with polarity honoured. */
    double polarity = (g_ltic.polarity == -1) ? -1.0 : 1.0;

    /* Effective frequency-branch scale. g_lars.gain == 0 means "auto": derive it
     * from the CT calibration, exactly as algo 10 does. CT measures K (Hz per PWM
     * LSB) and stores it as g_pid[7].Kp = 0.40/K, so lsb_per_hz = g_pid[7].Kp/0.40
     * is LSB of PWM per Hz of frequency error — which is precisely what the
     * frequency branch needs to convert e_freq into a PWM correction, with no
     * hand-tuning and no board-specific constant. If the user has set a non-zero
     * gain with LG, that takes priority and is used with the fixed 6553.6 scale
     * as before. A light 0.5 factor on the auto path keeps the proportional term
     * from being too hot on boards with a large lsb_per_hz. */
    float freq_scale;      /* LSB per Hz  — used by the acquisition branch */
    float phase_gain;      /* LSB per ns  — used by the phase branch        */
    if (g_lars.gain > 0.0f) {
        freq_scale  = g_lars.gain * 6553.6f;             /* manual: LG value */
        phase_gain  = g_lars.gain;
        g_lars_gain_auto = false;
    } else {
        double lsb_per_hz = (g_pid[7].Kp > 100.0)
                          ? ((double)g_pid[7].Kp / 0.40)  /* from CT          */
                          : 3000.0;                       /* fallback if no CT */
        freq_scale = (float)(0.5 * lsb_per_hz);

        /* The two branches need DIFFERENT units and both must be derived, or the
         * loop half-works: an earlier version fed the auto value to the frequency
         * branch only, leaving the phase branch multiplying by g_lars.gain — which
         * is zero in auto mode. Acquisition then pulled in, the picDIV bridge
         * delivered the phase to the window, and the phase PI did nothing at all.
         *
         * Phase gain is LSB per ns. Nulling a phase error P over the loop time
         * constant needs a fractional frequency offset P/(tau*1e9), which at
         * 10 MHz is P/(100*tau) Hz, so lsb_per_hz/(100*tau) LSB per ns. Half of
         * that is used: the loop also has an integral term, so a proportional
         * term sized to null the error inside one time constant on its own is too
         * hot. The result lands within 15% of the hand-tuned 0.3 that produced a
         * clean lock on the bench, and unlike a fixed number it follows both the
         * measured VCO slope and whatever LTC is set to. */
        phase_gain = (float)(0.5 * lsb_per_hz / (100.0 * (double)time_const));
        g_lars_gain_auto = true;
    }
    g_lars_scale = freq_scale;

    float p_term, i_term;
    if (railed) {
        /* Frequency pull-in as a proper PI, with the PROPORTIONAL term doing the
         * work and the integrator contributing only a small trim. An earlier
         * version fed the full frequency correction into the integrator; that
         * integrator then carried momentum — it reached e_freq=0 with a pile of
         * accumulated LSB, sailed past, and the loop oscillated ±2 Hz around the
         * target with a ~150 s period, never settling into the phase window
         * (seen on air). The fix mirrors Lars' own structure (strong P, integral
         * = a fraction of P): P responds to the current e_freq and self-brakes as
         * the frequency comes down, while a light integral removes any residual
         * static offset without storing momentum.
         *
         * SIGN — from hardware, not assumed. A cold start showed the OCXO +8.4 Hz
         * fast with PWM driven UP to 65535, making it faster: a runaway. So the
         * frequency error needs the OPPOSITE sign from the phase branch (phase and
         * frequency have opposite orientation on this wiring): +polarity*e_freq
         * here vs -polarity below. Positive e_freq (fast) then lowers PWM. */
        float freq_lsb = (float)(polarity) * (float)e_freq * freq_scale;

        /* P dominates and self-brakes; I is a small fraction (Lars: I=P/damping,
         * and here further reduced so acquisition cannot wind up momentum). */
        p_term = freq_lsb;
        i_term = freq_lsb / g_lars.damping / 10.0f + s_i_remain;

        /* STEP CLAMP on the integral only. P is allowed its full self-limiting
         * value (it shrinks with e_freq on its own); the integral is what could
         * accumulate an overshoot, so cap its per-cycle contribution. The real
         * VCO slope (Hz/LSB) is unknown and folded into gain, so a bounded walk
         * is what keeps acquisition from leaping past the lock point. */
        const float ACQ_MAX_ISTEP = 50.0f;
        if (i_term >  ACQ_MAX_ISTEP) i_term =  ACQ_MAX_ISTEP;
        if (i_term < -ACQ_MAX_ISTEP) i_term = -ACQ_MAX_ISTEP;

        /* Also bound the total P excursion so a huge cold-start e_freq cannot
         * slam PWM across the whole span in one cycle; large enough to move
         * briskly, small enough not to overshoot a nearby lock point. */
        const float ACQ_MAX_P = 2000.0f;
        if (p_term >  ACQ_MAX_P) p_term =  ACQ_MAX_P;
        if (p_term < -ACQ_MAX_P) p_term = -ACQ_MAX_P;

        /* ANTI-WINDUP / runaway stop — safety net independent of sign. If PWM is
         * at a rail and the integral would push further in, freeze it, so the
         * loop can never sit pinned at 0 or 65535. */
        bool at_hi = (s_integ >= 65534.0f);
        bool at_lo = (s_integ <= 1.0f);
        if ((at_hi && i_term > 0.0f) || (at_lo && i_term < 0.0f)) {
            i_term     = 0.0f;
            s_i_remain = 0.0f;
        }
    } else {
        /* Lars' phase PI (292-293). ltic_phase_error_ns() already returns the
         * phase relative to the calibrated band centre (zero_offset), so 0 IS
         * the target and we drive the filtered phase to zero directly. gain is
         * DAC bits per ns. NOTE: g_lars.tic_offset is stored and available but
         * intentionally NOT summed in here — phase_ns is already centred on the
         * LC-measured zero, and adding tic_offset as a second reference would
         * fight it. tic_offset is reserved for an explicit target-shift feature
         * (like algo 10's centre_v) once we decide how the two should compose. */
        float phase_err = s_phase_filt;                 /* already relative to 0 */
        p_term  = (float)(-polarity) * phase_err * phase_gain;
        i_term  = p_term / g_lars.damping / (float)time_const + s_i_remain;
    }

    /* ---- integrate with remainder preserved (Lars 293-296) ---- */
    long  i_whole = (long)i_term;
    s_i_remain    = i_term - (float)i_whole;
    s_integ      += (float)i_whole;

    /* ---- optional temperature feed-forward (Lars 306) ----
     * dacValue += (tempRef - tempFiltered) * tempCoeff. Off unless enabled. */
    float temp_ff = 0.0f;
    if (g_lars.flags & LARS_FLAG_TEMP_COMP) {
        extern float g_bmp_temp;                         /* board temperature °C */
        /* Map temperature to ADC-like counts the same way tempRef is stored:
         * tempRef is in ADC counts, so we approximate the current temp in the
         * same units. A dedicated temp-ADC channel can replace this later. */
        float temp_now = g_bmp_temp * 100.0f;            /* 0.01 °C resolution */
        temp_ff = ((float)g_lars.temp_ref - temp_now)
                * (float)g_lars.temp_coeff / 10000.0f;
    }

    /* ---- output = integrator + proportional + temp (Lars 297, 306) ---- */
    float out_f = s_integ + p_term + temp_ff;

    /* clamp to valid PWM range */
    if (out_f < 0.0f)     out_f = 0.0f;
    if (out_f > 65535.0f) out_f = 65535.0f;
    /* keep the integrator itself bounded so it cannot wind up past the rail */
    if (s_integ < 0.0f)     s_integ = 0.0f;
    if (s_integ > 65535.0f) s_integ = 65535.0f;

    uint16_t out = (uint16_t)(out_f + 0.5f);

    /* ---- lock detection (Lars 230-242): phase in window AND frequency in
     * window, held for lock_factor * timeConst seconds ---- */
    double phase_win_ns = (double)g_lars.lock_ns_lim;
    bool   in_phase = ph_valid && fabs(s_phase_filt) <= phase_win_ns;
    bool   in_freq  = fabs(e_freq) <= 0.02;             /* ~20 ppb, Lars' gate */
    if (in_phase && in_freq) {
        if (s_lock_cnt < 0xFFFFFFFFu) s_lock_cnt++;
    } else {
        s_lock_cnt = 0;
    }
    s_locked = (s_lock_cnt > (uint32_t)time_const * g_lars.lock_factor);

    /* publish live state for the Learn telemetry line */
    g_lars_locked     = s_locked;
    g_lars_phase_filt = s_phase_filt;

    /* ---- phase-capture bridge -------------------------------------------
     * The frequency branch can drive e_freq to ~0 while the phase is still
     * railed far outside the detector window (seen on air: e_freq ±0.03 Hz but
     * dph stuck at ~1926 ns, trend ACQ forever). At zero frequency error the
     * phase no longer moves, so it can never drift into range on its own — the
     * loop is disciplined in frequency but stranded in phase. Algo 10 avoids
     * this because entering ACQ re-arms the picDIV, which resyncs the divider to
     * the 1PPS edge and snaps the phase to ~0; algo 11 never armed it at all.
     * So: once the frequency is genuinely settled but the phase is still railed,
     * re-arm the picDIV ONCE to bring the phase into the window, then let the
     * phase branch take over. Guarded by a hold-off counter so it fires at most
     * once per stranding, not every cycle. */
    static uint32_t s_freqok_railed = 0;   /* cycles: freq settled yet railed  */
    static uint32_t s_rearm_holdoff = 0;   /* cool-down after a re-arm         */
    if (s_rearm_holdoff > 0) {
        s_rearm_holdoff--;
        s_freqok_railed = 0;
    } else if (railed && fabs(e_freq) <= 0.05) {
        /* frequency good, phase blind: count how long we have been stranded */
        if (s_freqok_railed < 0xFFFFFFFFu) s_freqok_railed++;
        /* wait a few seconds of steady frequency before disturbing the divider,
         * so a brief frequency zero-crossing during acquisition does not trigger
         * a needless re-arm */
        if (s_freqok_railed >= 5u) {
            ltic_arm_picdiv();
            s_freqok_railed = 0;
            s_rearm_holdoff = 15u;   /* let the phase settle before trying again */
        }
    } else {
        s_freqok_railed = 0;
    }

    /* Trend string for telemetry/displays/tuner (4 chars). Uses the same
     * ACQ/PLL/LOCK vocabulary as algo 10 so the displays read consistently;
     * algo 11 says PLL where algo 10 says DPLL, which also keeps the two
     * distinguishable in logs. */
    set_trend(s_locked ? "LOCK" : (railed ? "ACQ " : "PLL "));

    return out;
}


uint16_t adjustVctlPWM(uint16_t prev_pwm, uint32_t ppscount, uint8_t algo_no)
{
    switch (algo_no) {
        case 0:  return primitive_ctl_loop(prev_pwm, ppscount);
        case 1:  return forced_drift_Vctl (prev_pwm, ppscount);
        case 2:  return random_walk_Vctl  (prev_pwm, ppscount);
        case 3:  return fll_pid_manual    (prev_pwm, ppscount);
        case 4:  return pll_pi_manual     (prev_pwm, ppscount);
        case 5:  return pll_pid_manual    (prev_pwm, ppscount);
        case 6:  return fll_pid_genetic   (prev_pwm, ppscount);
        case 7:  return pll_pid_genetic   (prev_pwm, ppscount);
        case 8:  return hybrid_fll_pll    (prev_pwm, ppscount);
        case 9:  return nn_mlp_ctl_loop   (prev_pwm, ppscount);
#ifdef GPSDO_LTIC
        case 10: return ltic_three_stage  (prev_pwm, ppscount);
        case 11: return ltic_lars_pi      (prev_pwm, ppscount);
#endif
        default: return primitive_ctl_loop(prev_pwm, ppscount);
    }
}

/* ======================================================================
 * ALGORITHM 0 — Primitive stepped controller (original, unchanged)
 * ====================================================================== */
uint16_t primitive_ctl_loop(uint16_t pwm, uint32_t ppscount)
{
    const uint32_t PERIOD = 429;
    if ((ppscount % PERIOD) != 0) return pwm;

    FreqSnapshot_t s;
    take_freq_snapshot(&s);

    int32_t  new_pwm = pwm;
    /* trendstr format: exactly 4 chars, no leading space, right-padded.
     * Displayed as-is on OLED row 7, serial, and LCD line 3 indicator.
     * LCD shows only first 3 chars in the indicator field (cols 17-19).
     * Meanings: hit=locked, vf=very-far, uf=ultra-fine, c=coarse, f=fine */
    char     trend[5] = "___ ";

    if (s.have1000 && s.avg1000 >= 9999999.990 && s.avg1000 <= 10000000.010) {
        if      (s.avg1000 >= 10000000.005) { new_pwm = pwm - 5; strcpy(trend, "vf- "); }
        else if (s.avg1000 >= 10000000.001) { new_pwm = pwm - 1; strcpy(trend, "uf- "); }
        else if (s.avg1000 <= 9999999.995)  { new_pwm = pwm + 5; strcpy(trend, "vf+ "); }
        else if (s.avg1000 <= 9999999.999)  { new_pwm = pwm + 1; strcpy(trend, "uf+ "); }
        else                                {                     strcpy(trend, "hit "); }
    } else if (s.have100) {
        if      (s.avg100 >= 10000000.10) { new_pwm = pwm - 100; strcpy(trend, "c-  "); }
        else if (s.avg100 >= 10000000.01) { new_pwm = pwm -  10; strcpy(trend, "f-  "); }
        else if (s.avg100 <= 9999999.90)  { new_pwm = pwm + 100; strcpy(trend, "c+  "); }
        else if (s.avg100 <= 9999999.99)  { new_pwm = pwm +  10; strcpy(trend, "f+  "); }
    }

    set_trend(trend);
    return clamp_pwm(new_pwm);
}

/* ======================================================================
 * ALGORITHM 1 — Forced drift (original)
 * ====================================================================== */
uint16_t forced_drift_Vctl(uint16_t pwm, uint32_t ppscount)
{
    if ((ppscount % 1000) == 0) return clamp_pwm((int32_t)pwm + 1);
    return pwm;
}

/* ======================================================================
 * ALGORITHM 2 — Random walk (original)
 * ====================================================================== */
uint16_t random_walk_Vctl(uint16_t pwm, uint32_t ppscount)
{
    if ((ppscount % 5) == 0) {
        int32_t delta = (int32_t)(random(3)) - 1;
        return clamp_pwm((int32_t)pwm + delta);
    }
    return pwm;
}

/* ======================================================================
 * ALGORITHM 3 — FLL PID, manually tuned
 *
 * Loop type:   Frequency-Locked Loop
 * Error input: avg_100s frequency error (refreshes every 100 s once full)
 * Update rate: every 100 s (ppscount % 100 == 0)
 *
 * PID state (static — persists between calls, reset on flush):
 *   integral_e:  running sum of error × Ts
 *   prev_e:      previous error for derivative
 *
 * Tuning rationale:
 *   OCXO sensitivity ≈ 5 µHz/LSB → Kp=80 gives ~16 mHz/Hz correction
 *   Ki=0.8 provides ~1 Hz·s⁻¹ steady-state correction rate
 *   Kd=200 damps step disturbances (temperature ramp)
 *   Anti-windup: integral clamped to ±10000 LSB
 * ====================================================================== */
uint16_t fll_pid_manual(uint16_t pwm, uint32_t ppscount)
{
    /* State — zeroed at startup, reset via flush */
    static double  integral_e = 0.0;
    static double  prev_e     = 0.0;
    static bool    prev_valid = false;
    static uint32_t last_flush_pps = 0;

    const uint32_t PERIOD = 100;     /* update every 100 s */
    const double   Kp     = g_pid[3].Kp;
    const double   Ki     = g_pid[3].Ki;
    const double   Kd     = g_pid[3].Kd;
    const double   Ts     = (double)PERIOD;    /* 100 s effective sample */
    const double   I_LIMIT = g_pid[3].I_LIMIT; /* anti-windup clamp [LSB] */

    /* Detect ring buffer flush (ppscount resets to 0) */
    if (ppscount < last_flush_pps) {
        integral_e  = 0.0;
        prev_e      = 0.0;
        prev_valid  = false;
    }
    last_flush_pps = ppscount;

    if ((ppscount % PERIOD) != 0) return pwm;

    FreqSnapshot_t s;
    take_freq_snapshot(&s);
    if (!s.have100) return pwm;

    /* Frequency error: positive = too fast = must decrease PWM */
    double e = s.avg100 - (double)BASE_FREQ;

    /* Integral with anti-windup */
    integral_e += e * Ts;
    if (integral_e >  I_LIMIT) integral_e =  I_LIMIT;
    if (integral_e < -I_LIMIT) integral_e = -I_LIMIT;

    /* Derivative (first call: no derivative action) */
    double derivative = prev_valid ? (e - prev_e) / Ts : 0.0;
    prev_e     = e;
    prev_valid = true;

    /* PID output: negate because positive error → decrease PWM */
    double u = -(Kp * e + Ki * integral_e + Kd * derivative);
    u = lrn_apply(u, integral_e, e, Ts);   /* self-learning (LRN) */

    char trend[5];
    bool e_small = (e > -0.0010 && e < 0.0010);
    if (e_small && u > -1.0 && u < 1.0) {
        strcpy(trend, "hit ");
        set_trend(trend);
        return pwm;
    }
    if      (u > 10.0)  strcpy(trend, "p+  ");
    else if (u > 0.0)   strcpy(trend, "f+  ");
    else if (u < -10.0) strcpy(trend, "p-  ");
    else                strcpy(trend, "f-  ");
    set_trend(trend);

    return clamp_pwm((int32_t)pwm + (int32_t)u);
}

/* ======================================================================
 * ALGORITHM 4 — PLL PI + frequency damping, manually tuned
 *               Inspired by Lars Walenius' GPSDO design
 *
 * Loop type:   true Phase-Locked Loop.
 * Phase error: locally accumulated — every 10 s the loop adds
 *              (avg10 − 10 MHz)·10 s, which equals the EXACT sum of the
 *              last ten 1-second cycle-count offsets (integer cycles).
 *              Unlike a rolling-window average this responds to PWM
 *              corrections with only a 10-second lag.
 * Damping:     Kd × frequency error (avg100 when available, else avg10).
 *              A pure PI on phase with an integrating plant is marginally
 *              unstable — the frequency term provides the damping that a
 *              derivative-of-phase would, without differencing noise.
 * Update rate: every 10 s
 *
 * Tuning:        Kp on phase, gentle damping from frequency error
 *                 (well-damped discrete loop)
 * ====================================================================== */
uint16_t pll_pi_manual(uint16_t pwm, uint32_t ppscount)
{
    static double  phase_acc    = 0.0;   /* accumulated phase [Hz·s = cycles] */
    static uint32_t last_flush_pps = 0;

    const uint32_t PERIOD  = 10;
    const double   Kp      = g_pid[4].Kp;
    const double   Ki      = g_pid[4].Ki;
    const double   Kd      = g_pid[4].Kd;
    const double   Ts      = (double)PERIOD;
    const double   I_LIMIT = g_pid[4].I_LIMIT;

    if (ppscount < last_flush_pps) { phase_acc = 0.0; }
    last_flush_pps = ppscount;

    if ((ppscount % PERIOD) != 0) return pwm;

    FreqSnapshot_t s;
    take_freq_snapshot(&s);
    if (!s.have10) return pwm;

    /* True phase accumulation for the gentle integral term */
    double e_freq = s.have100 ? (s.avg100 - (double)BASE_FREQ)
                              : (s.avg10  - (double)BASE_FREQ);
    phase_acc += (s.avg10 - (double)BASE_FREQ) * Ts;
    if (phase_acc >  I_LIMIT) phase_acc =  I_LIMIT;
    if (phase_acc < -I_LIMIT) phase_acc = -I_LIMIT;

    /* PI on frequency (fast, no overshoot) + soft phase integral.
     * Kd here is the gentle phase-proportional gain (named Kd so the
     * CLI/EEPROM layout is shared with algos 5/7). */
    double u = -(Kp * e_freq + Kd * phase_acc + Ki * phase_acc * Ts);
    u = lrn_apply(u, phase_acc, e_freq, Ts);   /* self-learning (LRN) */

    bool locked = false;
    uint16_t out = apply_correction(pwm, u, e_freq, phase_acc, 12.0, &locked);

    char trend[5];
    if      (locked)   strcpy(trend, "hit ");
    else if (u >  5.0) strcpy(trend, "p+  ");
    else if (u >  0.0) strcpy(trend, "f+  ");
    else if (u < -5.0) strcpy(trend, "p-  ");
    else               strcpy(trend, "f-  ");
    set_trend(trend);

    return out;
}

/* ======================================================================
 * ALGORITHM 5 — PLL PID, manually tuned
 *
 * Loop type:   Phase-Locked Loop with derivative damping
 * Error input: Phase error from cumul1000 (shorter window = faster response
 *              at cost of more GPS noise); derivative from phase rate.
 * Update rate: every 10 s
 *
 * Compared to algo 4:
 *   - Uses 1000s window (faster pull-in)
 *   - Adds derivative term to damp overshoot when correcting large error
 *   - Higher Kp for faster settling
 *
 * Tuning:
 *   Kp=40, Ki=0.01, Kd=800
 *   Kd provides ~20× derivative boost relative to proportional — strongly
 *   damps temperature-induced ramps (slope: ~0.01 Hz/min typical)
 * ====================================================================== */
uint16_t pll_pid_manual(uint16_t pwm, uint32_t ppscount)
{
    static double  phase_acc    = 0.0;   /* accumulated phase [Hz·s] */
    static uint32_t last_flush_pps = 0;

    const uint32_t PERIOD  = 10;
    const double   Kp      = g_pid[5].Kp;    /* on FREQUENCY error */
    const double   Ki      = g_pid[5].Ki;    /* on PHASE (gentle)  */
    const double   Kd      = g_pid[5].Kd;    /* phase proportional */
    const double   Ts      = (double)PERIOD;
    const double   I_LIMIT = g_pid[5].I_LIMIT;

    if (ppscount < last_flush_pps) { phase_acc = 0.0; }
    last_flush_pps = ppscount;

    if ((ppscount % PERIOD) != 0) return pwm;

    FreqSnapshot_t s;
    take_freq_snapshot(&s);
    if (!s.have10) return pwm;

    /* Same two-timescale scheme as algo 7 (see its header). */
    double e_freq = s.have100 ? (s.avg100 - (double)BASE_FREQ)
                              : (s.avg10  - (double)BASE_FREQ);
    phase_acc += (s.avg10 - (double)BASE_FREQ) * Ts;
    if (phase_acc >  I_LIMIT) phase_acc =  I_LIMIT;
    if (phase_acc < -I_LIMIT) phase_acc = -I_LIMIT;

    double u = -(Kp * e_freq + Kd * phase_acc + Ki * phase_acc * Ts);
    u = lrn_apply(u, phase_acc, e_freq, Ts);   /* self-learning (LRN) */

    bool locked = false;
    uint16_t out = apply_correction(pwm, u, e_freq, phase_acc, 12.0, &locked);

    char trend[5];
    if      (locked)    strcpy(trend, "hit ");
    else if (u >  10.0) strcpy(trend, "p+  ");
    else if (u >   0.0) strcpy(trend, "f+  ");
    else if (u < -10.0) strcpy(trend, "p-  ");
    else                strcpy(trend, "f-  ");
    set_trend(trend);

    return out;
}

/* ======================================================================
 * ALGORITHM 6 — FLL PID, GA-optimised coefficients
 *
 * Same structure as algo 3 (FLL PID) but with coefficients derived via
 * genetic algorithm / ITAE minimisation:
 *
 *   Step-test result:  Ku=400 LSB/Hz,  Tu=800 s
 *   ITAE rules (PID):  Kp = 0.585·Ku = 234
 *                      Ti = Tu/1.03   = 777 s  → Ki = Kp/Ti = 0.301
 *                      Td = 0.091·Tu  =  73 s  → Kd = Kp·Td = 17082
 *
 * Additionally: derivative is filtered (N=10 filter) to avoid noise
 * amplification — this is the key improvement over algo 3.
 *
 *   Filtered derivative:
 *     d_f(k) = N/(N + Ts/Td) * d_f(k-1) + Kd*N/(N + Ts/Td) * (e-e_prev)
 *   Here simplified as exponential smoothing with α = Ts/(Td + Ts).
 * ====================================================================== */
uint16_t fll_pid_genetic(uint16_t pwm, uint32_t ppscount)
{
    static double  integral_e  = 0.0;
    static double  prev_e      = 0.0;
    static double  d_filtered  = 0.0;   /* low-pass filtered derivative */
    static bool    prev_valid  = false;
    static uint32_t last_flush_pps = 0;

    const uint32_t PERIOD  = 100;
    const double   Kp      = g_pid[6].Kp;
    const double   Ki      = g_pid[6].Ki;
    const double   Kd      = g_pid[6].Kd;
    const double   Ts      = (double)PERIOD;
    const double   Td      = (Kp > 0.0) ? (Kd / Kp) : 73.0; /* derived */
    /* Derivative filter: α = Ts/(Ts+Td) → weight on raw vs filtered */
    const double   ALPHA   = Ts / (Ts + Td);
    const double   I_LIMIT = g_pid[6].I_LIMIT;

    if (ppscount < last_flush_pps) {
        integral_e = 0.0; prev_e = 0.0; d_filtered = 0.0; prev_valid = false;
    }
    last_flush_pps = ppscount;

    if ((ppscount % PERIOD) != 0) return pwm;

    FreqSnapshot_t s;
    take_freq_snapshot(&s);
    if (!s.have100) return pwm;

    double e = s.avg100 - (double)BASE_FREQ;

    integral_e += e * Ts;
    if (integral_e >  I_LIMIT) integral_e =  I_LIMIT;
    if (integral_e < -I_LIMIT) integral_e = -I_LIMIT;

    /* Filtered derivative (first call: initialise with raw) */
    double raw_d = prev_valid ? (e - prev_e) / Ts : 0.0;
    d_filtered   = ALPHA * raw_d + (1.0 - ALPHA) * d_filtered;
    prev_e       = e;
    prev_valid   = true;

    double u = -(Kp * e + Ki * integral_e + Kd * d_filtered);
    u = lrn_apply(u, integral_e, e, Ts);   /* self-learning (LRN) */

    /* FLL lock: frequency within 1 mHz. No phase term, so freq-only test.
     * Hold PWM when locked and the step is sub-LSB to stop noise dither. */
    char trend[5];
    bool e_small = (e > -0.0010 && e < 0.0010);
    if (e_small && u > -1.0 && u < 1.0) {
        strcpy(trend, "hit ");
        set_trend(trend);
        return pwm;
    }
    strcpy(trend, u >= 0.0 ? "f+  " : "f-  ");
    set_trend(trend);

    return clamp_pwm((int32_t)pwm + (int32_t)u);
}

/* ======================================================================
 * ALGORITHM 7 — PLL PID, GA-optimised coefficients
 *
 * PLL variant: phase error from 1000s cumulative sum.
 *
 * GA / ITAE derivation (PLL phase domain):
 *   System gain in phase domain: G_phase = G_freq / s
 *   Effective Ku for phase loop ≈ 120 LSB / (Hz·s), Tu ≈ 400 s
 *   ITAE (PID):  Kp = 0.585·120 = 70.2  → 70
 *                Ti = 400/1.03  = 388 s  → Ki = 70/388 = 0.181
 *                Td = 0.091·400 = 36.4 s → Kd = 70·36.4 = 2548
 *
 * Derivative filtering: same ALPHA scheme as algo 6.
 * Update rate: every 10 s (tighter loop than FLL alg 6).
 * ====================================================================== */
uint16_t pll_pid_genetic(uint16_t pwm, uint32_t ppscount)
{
    static double  phase_acc    = 0.0;   /* accumulated phase [Hz·s] */
    static uint32_t last_flush_pps = 0;

    const uint32_t PERIOD  = 10;
    const double   Kp      = g_pid[7].Kp;    /* acts on FREQUENCY error  */
    const double   Ki      = g_pid[7].Ki;    /* acts on PHASE (gentle)   */
    const double   Kd      = g_pid[7].Kd;    /* phase proportional, soft */
    const double   Ts      = (double)PERIOD;
    const double   I_LIMIT = g_pid[7].I_LIMIT;

    if (ppscount < last_flush_pps) { phase_acc = 0.0; }
    last_flush_pps = ppscount;

    if ((ppscount % PERIOD) != 0) return pwm;

    FreqSnapshot_t s;
    take_freq_snapshot(&s);
    if (!s.have10) return pwm;

    /* Two-timescale control:
     *   - dominant term Kp·e_freq pulls the FREQUENCY to target fast and
     *     without overshoot (Kp ≈ 0.5/K → half-step deadbeat).
     *   - gentle Kd·phase_acc + Ki·∫phase remove the slow phase drift
     *     with small steps, so steady-state PWM motion stays tiny.
     * The frequency error uses the smoothest window available.         */
    double e_freq = s.have100 ? (s.avg100 - (double)BASE_FREQ)
                              : (s.avg10  - (double)BASE_FREQ);
    phase_acc += (s.avg10 - (double)BASE_FREQ) * Ts;
    if (phase_acc >  I_LIMIT) phase_acc =  I_LIMIT;
    if (phase_acc < -I_LIMIT) phase_acc = -I_LIMIT;

    double u = -(Kp * e_freq + Kd * phase_acc + Ki * phase_acc * Ts);
    u = lrn_apply(u, phase_acc, e_freq, Ts);   /* self-learning (LRN) */

    bool locked = false;
    uint16_t out = apply_correction(pwm, u, e_freq, phase_acc, 12.0, &locked);

    char trend[5];
    if      (locked)    strcpy(trend, "hit ");
    else                strcpy(trend, u >= 0.0 ? "f+  " : "f-  ");
    set_trend(trend);

    return out;
}

/* ======================================================================
 * ALGORITHM 8 — Hybrid FLL + PLL PID
 *
 * Motivation:
 *   - FLL: fast pull-in from large frequency errors (startup, temperature
 *     step), insensitive to GPS phase noise, but poor steady-state jitter
 *   - PLL: low steady-state phase noise, but slow pull-in from large errors
 *   - Hybrid: blend the two proportionally to error magnitude
 *
 * Blending scheme (sigmoid):
 *   w_fll(e) = sigmoid(|e_hz| / BLEND_SCALE)   range 0-1
 *   w_pll    = 1 - w_fll
 *   u = w_fll * u_fll + w_pll * u_pll
 *
 *   where BLEND_SCALE = 0.05 Hz:
 *     |e| > 0.20 Hz → FLL weight > 98%  (effectively pure FLL)
 *     |e| < 0.005 Hz → PLL weight > 90% (effectively pure PLL)
 *     Transition zone 0.005-0.20 Hz: gradual blend
 *
 * FLL branch: algo 6 coefficients (GA-optimised), 100s window
 * PLL branch: algo 7 coefficients (GA-optimised), 1000s window
 * Update:     every 10 s (driven by PLL branch; FLL state updated in sync)
 *
 * Both branches maintain independent integrators; the blending weight
 * is applied to the total PID output, not to individual terms, to avoid
 * integrator wind-up in the dormant branch.
 * ====================================================================== */

/* Sigmoid: 1 / (1 + exp(-x)) — returns 0.5 at x=0 */
static inline double sigmoid(double x)
{
    return 1.0 / (1.0 + exp(-x));
}

uint16_t hybrid_fll_pll(uint16_t pwm, uint32_t ppscount)
{
    /* FLL state */
    static double fll_integral = 0.0;
    static double fll_prev_e   = 0.0;
    static double fll_d_filt   = 0.0;
    static bool   fll_prev_ok  = false;

    /* PLL state */
    static double pll_phase    = 0.0;   /* accumulated phase [Hz·s] */


    static uint32_t last_flush_pps = 0;

    if (ppscount < last_flush_pps) {
        fll_integral = 0.0; fll_prev_e  = 0.0; fll_d_filt = 0.0; fll_prev_ok = false;
        pll_phase = 0.0;
    }
    last_flush_pps = ppscount;

    /* Both branches update at 10 s period */
    const uint32_t PERIOD = 10;
    if ((ppscount % PERIOD) != 0) return pwm;

    FreqSnapshot_t s;
    take_freq_snapshot(&s);
    if (!s.have100) return pwm;     /* need at least 100s of data */

    const double Ts       = (double)PERIOD;
    const double I_LIMIT  = g_pid[8].I_LIMIT;

    /* ---- FLL branch (reads tunable algo 6 coefficients) ---- */
    const double FKp    = g_pid[6].Kp;
    const double FKi    = g_pid[6].Ki;
    const double FKd    = g_pid[6].Kd;
    const double FTd    = (FKp > 0.0) ? (FKd / FKp) : 73.0;
    const double F_ALPHA = Ts / (Ts + FTd);

    double e_hz = s.avg100 - (double)BASE_FREQ;
    fll_integral += e_hz * Ts;
    if (fll_integral >  I_LIMIT) fll_integral =  I_LIMIT;
    if (fll_integral < -I_LIMIT) fll_integral = -I_LIMIT;
    double fraw_d = fll_prev_ok ? (e_hz - fll_prev_e) / Ts : 0.0;
    fll_d_filt    = F_ALPHA * fraw_d + (1.0 - F_ALPHA) * fll_d_filt;
    fll_prev_e    = e_hz;
    fll_prev_ok   = true;
    double u_fll  = -(FKp * e_hz + FKi * fll_integral + FKd * fll_d_filt);

    /* ---- PLL branch (reads tunable algo 7 coefficients) ----
     * Two-timescale: Kp on frequency error (fast, no overshoot),
     * Kd+Ki gently on accumulated phase.  Same scheme as algo 7.       */
    double u_pll = 0.0;
    {
        const double PKp = g_pid[7].Kp;
        const double PKi = g_pid[7].Ki;
        const double PKd = g_pid[7].Kd;

        pll_phase += e_hz * Ts;
        if (pll_phase >  I_LIMIT) pll_phase =  I_LIMIT;
        if (pll_phase < -I_LIMIT) pll_phase = -I_LIMIT;
        u_pll = -(PKp * e_hz + PKd * pll_phase + PKi * pll_phase * Ts);
    }

    /*
     * Blending:
     *   x = |e_hz| / 0.05  → sigmoid gives w_fll
     *   At e=0.20 Hz: x=4 → w_fll=0.982 (almost pure FLL)
     *   At e=0.005 Hz: x=0.1 → w_fll=0.525 → w_pll=0.475 (near equal)
     *   We shift sigmoid: w_fll = sigmoid((|e|-0.02)/0.01)
     *   so crossover at |e|=0.02 Hz (fast enough for PLL stability)
     */
    double abs_e    = (e_hz < 0) ? -e_hz : e_hz;
    double w_fll    = sigmoid((abs_e - g_blend_crossover) / g_blend_scale);
    double w_pll    = 1.0 - w_fll;
    double u        = w_fll * u_fll + w_pll * u_pll;
    u = lrn_apply(u, pll_phase, e_hz, Ts);   /* self-learning (LRN) */

    /* Slew-limited, lock-aware. Wider cap (40 LSB) than the pure PLLs so
     * the FLL branch can still capture a large startup error quickly.
     * Lock test uses the PLL phase accumulator (pll_phase). */
    bool locked = false;
    uint16_t out = apply_correction(pwm, u, e_hz, pll_phase, 40.0, &locked);

    char trend[5];
    if      (locked)      strcpy(trend, "hit ");
    else if (w_fll > 0.8) strcpy(trend, "FLL ");
    else if (w_pll > 0.8) strcpy(trend, "PLL ");
    else                  strcpy(trend, "HYB ");
    set_trend(trend);

    return out;
}

/* ======================================================================
 * ALGORITHM 9 — Neural network MLP with ONLINE THERMAL LEARNING
 *
 * Architecture: 5 inputs → 8 hidden neurons (tanh) → 1 output (tanh)
 *
 * Inputs (normalised to ±1.0):
 *   x[0] = e         / E_SCALE     (frequency error,  E_SCALE=0.5 Hz)
 *   x[1] = integral  / I_SCALE     (integral of error, I_SCALE=500 Hz·s)
 *   x[2] = derivative/ D_SCALE     (rate of change,   D_SCALE=0.05 Hz/s)
 *   x[3] = ΔT        / T_SCALE     (temp deviation from 1 h baseline, 2 °C)
 *   x[4] = dT/dt     / DT_SCALE    (temp rate, 0.005 °C/s)
 *
 * Output (normalised):
 *   delta_PWM = y_norm × MAX_STEP  (MAX_STEP from CT)
 *
 * The PID channels (h0-h2) are analytically constructed as before: a
 * diagonal, bias-free, odd-symmetric network implementing a smooth
 * saturating PID (equilibrium exactly at zero error by design).
 *
 * NEW — THERMAL REGRESSOR + HOLDOVER STEERING. Closed-loop simulation
 * showed that learning thermal weights from the instantaneous error is
 * futile: the loop hides the correlation (the integral tracks slow drift,
 * and what remains is counter-quantisation dither). The signal with real
 * SNR is the CORRECTION the loop was forced to apply: overnight logs show
 * PWM tracking room temperature by tens of LSB. So the network learns the
 * oscillator tempco by an EMA covariance REGRESSION of PWM against
 * temperature (horizon ~4 h, only while disciplined):
 *
 *     tempco [LSB/°C] = cov(T, PWM) / var(T),  clamped ±60
 *
 * (validated in simulation: measured −26.8 vs true −26.7 LSB/°C).
 * Payoff: during HOLDOVER the loop normally freezes PWM and thermal drift
 * runs unchecked (3 °C × typical VCXO tempco ≈ tens of mHz ≈ µs of phase
 * per hour). With the learned tempco the firmware keeps steering PWM from
 * temperature alone — holdover becomes thermally compensated. Temperature
 * comes from the BMP280 (AHT fallback); no sensor → everything neutral.
 * The thermal inputs x[3]/x[4] feed inert hidden channels (W2=0) kept for
 * future offline training. Tempco is reported in the Learn: line.
 * ====================================================================== */

/* tanh approximation — saves linking libm on some toolchains, but since
   we already include math.h for exp() in algo 8, use real tanh here. */
static inline double nn_tanh(double x) { return tanh(x); }

/* Network dimensions */
#define NN_IN   5
#define NN_H    8
#define NN_OUT  1

/* Learned oscillator tempco [LSB/°C] — from the PWM↔temperature regressor
 * below; 0 until enough variance is seen. Exposed for telemetry. */
float g_nn_tempco = 0.0f;

/* ACQ centring drive (algo 10), tunable live with `ACG <gain> [cap]`.
 * gain: LSB of PWM per volt of centring error. cap: max step per update.
 * Defaults are ~4x gentler than the old HC74-era 6000x/±400 pairing, which
 * wrapped a sensitive LVC74 detector rail-to-rail instead of centring it. */
float g_ltic_acq_centre_gain = 2500.0f;
float g_ltic_acq_centre_cap  = 150.0f;

/* Module-level thermal state, shared by the PPS-tick observer (learning)
 * and the holdover steering path (which runs even with no PPS). */
static double s_th_fast = 0.0, s_th_slow = 0.0, s_th_fast_prev = 0.0;
static bool   s_th_valid = false;
static double s_th_mT = 0.0, s_th_mP = 0.0;      /* EMA means (T, PWM)   */
static double s_th_cov = 0.0, s_th_var = 0.0;    /* EMA cov / var        */
static double s_ho_tprev = 0.0;                  /* holdover: last T     */
static bool   s_ho_track = false;
static double s_ho_frac  = 0.0;                  /* fractional LSB accum */
static double s_ho_total = 0.0;                  /* total excursion clamp*/

/* Read the board temperature with a plausibility window (BMP → AHT). */
static bool nn_read_temp(double *out)
{
    float tc = g_bmp_temp;
    if (!(tc > -40.0f && tc < 85.0f)) tc = g_aht_temp;
    if (!(tc > -40.0f && tc < 85.0f)) return false;
    *out = (double)tc;
    return true;
}

/* Advance the thermal EMAs by dt seconds (called from both paths). */
static void nn_thermal_track(double dt)
{
    double tc;
    if (!nn_read_temp(&tc)) return;
    if (!s_th_valid) {
        s_th_fast = s_th_slow = s_th_fast_prev = tc;
        s_th_valid = true;
        return;
    }
    s_th_fast_prev = s_th_fast;
    s_th_fast += (dt / 120.0)  * (tc - s_th_fast);   /* ~2 min  */
    s_th_slow += (dt / 3600.0) * (tc - s_th_slow);   /* ~1 hour */
}

static const double W1[NN_H][NN_IN] = {
    /*  e,       integral,  derivative, dT,   dT/dt */
    {  1.5,      0.0,       0.0,        0.0,  0.0 },   /* h0: P channel      */
    {  0.0,      1.0,       0.0,        0.0,  0.0 },   /* h1: I channel      */
    {  0.0,      0.0,       1.2,        0.0,  0.0 },   /* h2: D channel      */
    {  0.0,      0.0,       0.0,        1.0,  0.0 },   /* h3: thermal level  */
    {  0.0,      0.0,       0.0,        0.0,  1.0 },   /* h4: thermal rate   */
    {  0.0,      0.0,       0.0,        0.0,  0.0 },   /* h5: unused         */
    {  0.0,      0.0,       0.0,        0.0,  0.0 },   /* h6: unused         */
    {  0.0,      0.0,       0.0,        0.0,  0.0 }    /* h7: unused         */
};

static const double b1[NN_H] = {
    0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0
};

static const double W2[NN_OUT][NN_H] = {
    { 0.9, 0.5, 0.6, 0.0, 0.0, 0.0, 0.0, 0.0 }
};

static const double b2[NN_OUT] = { 0.0 };

uint16_t nn_mlp_ctl_loop(uint16_t pwm, uint32_t ppscount)
{
    static double  integral_e  = 0.0;
    static double  prev_e      = 0.0;
    static bool    prev_valid  = false;
    static uint32_t last_flush_pps = 0;

    /* Normalisation scales */
    const double E_SCALE   = 0.5;     /* Hz — input normalisation */
    const double I_SCALE   = 500.0;   /* Hz·s */
    const double D_SCALE   = 0.05;    /* Hz/s */
    const double T_SCALE   = 2.0;     /* °C  — ΔT from baseline   */
    const double DT_SCALE  = 0.005;   /* °C/s — thermal rate      */
    const double MAX_STEP  = g_nn_max_step;  /* LSB — output de-normalisation */
    const double I_LIMIT   = g_pid[9].I_LIMIT; /* anti-windup = normalisation bound */
    const double REG_ALPHA = 10.0 / 14400.0;  /* regressor EMA horizon ~4 h */
    const double TCO_MAX   = 60.0;            /* |tempco| clamp [LSB/°C]    */

    const uint32_t PERIOD = 10;

    if (ppscount < last_flush_pps) {
        integral_e = 0.0; prev_e = 0.0; prev_valid = false;
        s_th_valid = false;   /* re-seed thermal EMAs after a flush */
    }
    last_flush_pps = ppscount;

    if ((ppscount % PERIOD) != 0) return pwm;

    FreqSnapshot_t s;
    take_freq_snapshot(&s);
    if (!s.have10) return pwm;

    /* Compute PID-style features */
    double e          = s.avg10 - (double)BASE_FREQ;
    double Ts         = (double)PERIOD;
    integral_e       += e * Ts;
    if (integral_e >  I_LIMIT) integral_e =  I_LIMIT;
    if (integral_e < -I_LIMIT) integral_e = -I_LIMIT;
    double derivative  = prev_valid ? (e - prev_e) / Ts : 0.0;
    prev_e     = e;
    prev_valid = true;

    /* Thermal tracking + TEMPCO REGRESSOR (the learning). Statistics are
     * only accumulated here — i.e. while the loop is disciplined and this
     * algorithm is issuing corrections — so warm-up, calibration and
     * holdover excursions never poison the regression. */
    double dT = 0.0, dTdt = 0.0;
    nn_thermal_track(Ts);
    s_ho_track = false;                 /* discipline active → reset HO ref */
    if (s_th_valid) {
        dT   = s_th_fast - s_th_slow;
        dTdt = (s_th_fast - s_th_fast_prev) / Ts;
        /* EMA covariance regression of PWM against temperature */
        s_th_mT  += REG_ALPHA * (s_th_fast   - s_th_mT);
        s_th_mP  += REG_ALPHA * ((double)pwm - s_th_mP);
        s_th_cov += REG_ALPHA * ((s_th_fast - s_th_mT) * ((double)pwm - s_th_mP) - s_th_cov);
        s_th_var += REG_ALPHA * ((s_th_fast - s_th_mT) * (s_th_fast - s_th_mT) - s_th_var);
        if (s_th_var > 1.0e-4) {        /* need real temperature variance  */
            double sl = s_th_cov / s_th_var;
            if (sl >  TCO_MAX) sl =  TCO_MAX;
            if (sl < -TCO_MAX) sl = -TCO_MAX;
            g_nn_tempco = (float)sl;
        }
    }

    /* Normalise inputs */
    double x[NN_IN];
    x[0] = e          / E_SCALE;
    x[1] = integral_e / I_SCALE;
    x[2] = derivative / D_SCALE;
    x[3] = dT         / T_SCALE;
    x[4] = dTdt       / DT_SCALE;
    /* Soft-clamp inputs to ±3 (avoids saturation in hidden layer) */
    for (int i = 0; i < NN_IN; i++) {
        if (x[i] >  3.0) x[i] =  3.0;
        if (x[i] < -3.0) x[i] = -3.0;
    }

    /* Forward pass: hidden layer */
    double h[NN_H];
    for (int j = 0; j < NN_H; j++) {
        double z = b1[j];
        for (int i = 0; i < NN_IN; i++) z += W1[j][i] * x[i];
        h[j] = nn_tanh(z);
    }

    /* Forward pass: output layer. The thermal hidden channels h3/h4 are
     * inert (W2 = 0) — kept so a future offline-trained W2 can use them.
     * The learned thermal action lives in nn_thermal_holdover_step(). */
    double y = b2[0];
    for (int j = 0; j < NN_H; j++) y += W2[0][j] * h[j];
    y = nn_tanh(y);  /* output in (-1, 1) */

    /* De-normalise: delta_PWM in LSB */
    /* Sign convention: network output y>0 means "lower frequency needed"
       = decrease PWM */
    double delta_pwm = -y * MAX_STEP;
    /* Self-learning (LRN): e is passed as the FREQUENCY error, so the drift
     * feed-forward learner is fully active on this algorithm (same signal as
     * algo 7). phase_hz_s=0 because the NN keeps no phase accumulator — the
     * damping OBSERVER is therefore blind here (no zero-crossings to watch),
     * but a previously learned damping value still trims the step. */
    delta_pwm = lrn_apply(delta_pwm, 0.0, e, (double)PERIOD);

    char trend[5];
    strcpy(trend, delta_pwm >= 0.0 ? "NN+ " : "NN- ");
    set_trend(trend);

    return clamp_pwm((int32_t)pwm + (int32_t)delta_pwm);
}

/* ======================================================================
 * nn_thermal_holdover_step — thermally steer PWM during HOLDOVER (algo 9)
 *
 * Called from vControlTask on its 5 Hz path (NOT gated on PPS — with the
 * antenna gone there is no PPS at all). Internally rate-limited to ~1 Hz.
 * Uses the tempco learned while disciplined:  Δpwm = tempco · ΔT, with a
 * fractional accumulator (steps are usually << 1 LSB) and a total
 * excursion clamp of ±250 LSB as a safety net. Returns the integer PWM
 * delta to apply now (usually 0), or 0 when idle/no data.
 * ====================================================================== */
int16_t nn_thermal_holdover_step(void)
{
    static uint32_t last_ms = 0;
    uint32_t now = millis();
    if ((uint32_t)(now - last_ms) < 1000u) return 0;
    double dt = (last_ms == 0) ? 1.0 : (double)(now - last_ms) / 1000.0;
    last_ms = now;

    if (g_nn_tempco == 0.0f) return 0;        /* nothing learned yet     */
    nn_thermal_track(dt);
    if (!s_th_valid) return 0;

    if (!s_ho_track) {                        /* first HO tick: set ref  */
        s_ho_tprev = s_th_fast;
        s_ho_track = true;
        s_ho_frac  = 0.0;
        s_ho_total = 0.0;
        return 0;
    }

    double step = (double)g_nn_tempco * (s_th_fast - s_ho_tprev);
    s_ho_tprev = s_th_fast;

    /* total excursion clamp (protects against a bad tempco estimate)    */
    if (s_ho_total + step >  250.0) step =  250.0 - s_ho_total;
    if (s_ho_total + step < -250.0) step = -250.0 - s_ho_total;

    s_ho_frac  += step;
    s_ho_total += step;
    int16_t out = (int16_t)s_ho_frac;         /* emit whole LSBs only    */
    s_ho_frac  -= (double)out;
    return out;
}
