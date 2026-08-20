/**
 * GPSDO_algorithms.cpp — Control loop algorithm implementations
 *
 * Part of GPSDO FreeRTOS v1.05
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
#include "gpsdo_dac.h"
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
    /* Instantaneous count error this second, TIM2 ticks from BASE_FREQ.
     * Algorithm 12 accumulates these one at a time; every other algorithm uses
     * the averages above. int16_t: FREQ_LOWER/UPPER admit ±500 Hz and the old
     * int8_t wrapped past ±127, feeding garbage to any gate that read it. */
    int16_t  instant_offset;
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
        s->instant_offset = gFreq.instant_offset;
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

/* ---- the fine control value, published for the control task ----------------
 *
 * adjustVctlPWM() returns uint16_t and that signature is load-bearing: the
 * control task, the displays, the telemetry line and the flash ring all speak
 * 16 bits, and widening the return would touch every one of them. So the exact
 * value rides alongside instead.
 *
 * g_vctl_fine_valid is cleared at the top of every adjustVctlPWM() call and set
 * only by an algorithm that actually computed a fractional target this cycle.
 * A stale fraction is therefore impossible: if the algorithm held (returned pwm
 * unchanged), or if it is one that does not do fractional arithmetic, the
 * control task falls back to the plain 16-bit write it has always done. */
double g_vctl_fine       = 0.0;
bool   g_vctl_fine_valid = false;

/* The value an output stage should add its correction to.
 *
 * NOT the uint16_t pwm the caller passed in — that is gCtrl.pwm_output, which
 * is the rounded view. Adding to it would discard the fraction once per cycle
 * and leave the whole fine path buying nothing.
 *
 * The DAC layer's value is used only when it agrees with what the caller thinks
 * the PWM is, to within one step. If anything else has moved the output since
 * the last loop cycle — a CT sweep, an SP from the CLI, a holdover step — they
 * will disagree, and then the caller's value is the truth and the fraction is
 * correctly abandoned. */
static double fine_base(uint16_t pwm)
{
    if (!gpsdo_dac_fine_available()) return (double)pwm;
    double f = gpsdo_dac_last16f();
    double d = f - (double)pwm;
    if (d < -1.0 || d > 1.0) return (double)pwm;
    return f;
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

    /* Keep the fraction. (int32_t)u truncated toward zero, so a correction of
     * 6.7 LSB was applied as 6 and the missing 0.7 was thrown away — in the
     * same direction every time, which the loop cannot distinguish from a gain
     * error. Accumulating it instead is what makes a run of sub-LSB corrections
     * add up to a real one. */
    double target = fine_base(pwm) + u;
    g_vctl_fine       = target;
    g_vctl_fine_valid = true;

    return clamp_pwm((int32_t)(target + (target < 0.0 ? -0.5 : 0.5)));
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
#endif /* GPSDO_LTIC */

/* ======================================================================
 * ALGORITHM 12 — Multi-level accumulator
 *
 * After Alan Cashin's (MIS42N on EEVblog) Budget GPSDO, transcribed from the
 * PIC16F1455 assembly he publishes on SourceForge. The structure is his; this is
 * a port rather than a reinterpretation, so that a misbehaviour is traceable to
 * the transcription rather than to a rewrite of the idea.
 *
 * WHAT PROBLEM IT SOLVES
 * ----------------------
 * Every other loop here has one time constant, and that constant is a compromise
 * nobody wins. Measured on Dan Wiering's bench against a rubidium reference:
 *
 *     tau          LTC 60              LTC 240
 *     10-400 s     worse               up to 1.44x better
 *     800-2000 s   up to 1.58x better  worse
 *
 * You pick one. Short tracks the oscillator and lets GPS noise in; long rejects
 * the noise and is slow to catch real drift.
 *
 * This does not pick. Readings accumulate into a hierarchy of levels, level n
 * covering 2^(n+1) seconds, and a correction is applied at the LOWEST level whose
 * error exceeds that level's limit. A large error acts within two seconds; a small
 * one waits for a longer average before anything is done. The error chooses its
 * own averaging time.
 *
 * HOW THE LEVELS WORK — no ring buffers, no loop over timescales
 * -------------------------------------------------------------
 * The level comes out of the bit pattern of the seconds counter: inspect from the
 * least significant bit upward, stop at the first zero. Level n then receives a
 * value once every 2^n seconds exactly, for one 16-bit variable per level. Eleven
 * levels — 2 s to 2048 s — cost 22 bytes.
 *
 * At each level two stored values, A (older) and B (newer):
 *
 *     slope = B - A                  frequency error over the level's span
 *     phase = (A + B) + 2*(B - A)    error extrapolated to end of period
 *
 * Both are tested against per-level limits. Exceed either and a correction is
 * applied and collection restarts from level 0. Pass both and A+B is promoted to
 * the next level, where the same test runs over twice the span.
 *
 * TWO DETAILS WORTH KEEPING
 * -------------------------
 * The frequency test is SKIPPED when slope and phase have opposite signs: the
 * loop is already correcting the phase and testing the frequency would add a
 * redundant nudge. One XOR on the sign bit.
 *
 * Each reading enters as 2*x+1, not x. Alan found that assigning zero to a
 * reading inside the target window let the phase wander; forcing every reading to
 * carry a significant value pinned it, and converges the mean of early and late
 * arrivals on zero rather than on the middle of a quantisation bin. The same
 * principle as dither in a converter: a dead zone in a control loop is worse than
 * noise, because the loop pushes, sees nothing, pushes harder, and then jumps.
 *
 * NOT AN LTIC ALGORITHM
 * ---------------------
 * This works on the raw TIM2 count, so it needs no phase detector and no picDIV.
 * That is the point: Alan's design reaches parts in 10^11 from a PIC and a NEO-6M
 * for under twenty dollars precisely because it never needs the hardware that
 * algorithms 10 and 11 depend on. So this is the algorithm for a board that has
 * none — and a fair comparison for one that does.
 *
 * STATUS: UNTUNED. The limits below are scaled from Alan's, whose counter
 * resolves 25 ns where TIM2 here resolves 100 ns, and whose receiver had no
 * sawtooth correction where this firmware applies qErr. They are a starting point
 * and are expected to need adjusting against measurement.
 * ====================================================================== */

/* Levels 0..MLACC_LEVELS-1 span 2..2048 seconds. Past that an OCXO's own drift
 * dominates whatever the averaging recovers — Alan's limit, and his reasoning
 * carries over. */
/* Per-level limits: {phase, slope}, in TIM2 ticks.
 *
 * Alan's table, scaled. His counter resolves 25 ns against 100 ns here, so his
 * tick counts are divided by four; but his receiver had no sawtooth correction
 * and this one does, so the phase limits are not loosened further to compensate
 * for jitter that is no longer present. The result is a table that should be in
 * the right region and is certainly not yet right.
 *
 * The pattern matters more than the numbers: limits grow with level, because a
 * longer average tolerates a larger absolute error before it is worth acting on,
 * while the frequency limit tightens in relative terms. */
/* Shortest time a correction may be spread over, in seconds.
 *
 * Alan corrects over the measurement span and calls the choice arbitrary — "it
 * could be shorter or longer". Taken literally it is far too aggressive at the
 * low levels: a 1012 ns error seen at level 0 asks to be nulled in two seconds,
 * which needs 5 Hz, which is 15685 DAC counts on this oscillator. It clamped,
 * the phase could not be cleared, and the oscillator was thrown far enough that
 * the arming gate never opened — so the detector stayed railed and the loop
 * could not recover. Measured: 14000 counts of PWM swing.
 *
 * A GPSDO wants a large error corrected gently. The floor makes a big excursion
 * take a minute rather than two seconds, still far quicker than the oscillator
 * drifts. */
/* ---- MEASURED per-level limits (MF 3) -------------------------------------
 *
 * The formula this replaces is
 *
 *     thr[L] = 8 * sigma * sqrt(2^L) * sqrt(10)
 *
 * and every term in it is an assumption. sqrt(2^L) says the phase is WHITE, so
 * that averaging 2^L samples reduces the test by 2^(L/2). Measured on two
 * boards of this design — same PCB, same OCXO, different rooms — the exponent
 * is 0.95 and 1.03, not 0.50. Averaging buys almost nothing, because the phase
 * that matters here is a slow wander (autocorrelation 0.96 at 60 s, 0.64 at
 * 300 s) and not sample-to-sample noise.
 *
 * The error compounds: at level 0 the formula understates the real spread by
 * about 5x, at level 10 by over 100x, so the table falls 32x across the
 * hierarchy where the phase itself falls by 1.3x. On a board holding 5 ns of
 * phase the crossing still lands at level 4-6 and the loop behaves; on the same
 * board in a noisier room, holding 26 ns, it lands at level 0-1 — below
 * MLACC_FREQ_MIN_LEVEL, where the frequency term is gated off and the
 * correction is a bare slew. That is the hunting mode this file's own
 * correction comment warns about.
 *
 * So the exponent is measured instead of assumed. Each level keeps the mean
 * square of its own test statistic; a least-squares fit of log2(sd) against
 * level gives the amplitude and the exponent together, and the table is built
 * from the fit. Fitting ACROSS levels rather than trusting each level alone is
 * what makes it usable early: level 8 is evaluated once every 512 s and would
 * need half a day to have a variance of its own, but it does not need one — the
 * low levels populate in minutes and the fit extrapolates.
 *
 * WHAT KEEPS IT FROM CHASING ITSELF. Two failure modes are on record in this
 * file, and the design has to survive both.
 *
 *   Downward: the loop suppresses the phase, the spread falls, the threshold
 *   falls, the loop acts on less. It stops on its own, and on a floor that is
 *   MEASURED rather than declared: once the phase is down to the per-sample
 *   noise, consecutive readings decorrelate and sd(3b-a) can fall no further
 *   than 2*sigma*sqrt(10). The old code wrote that floor as a constant 5 ns,
 *   which is a property of THIS detector and this receiver's sawtooth, not of
 *   the arithmetic.
 *
 *   Upward: an uncorrected frequency error ramps the phase, the ramp inflates
 *   the spread, the threshold grows with the error it exists to catch and the
 *   loop freezes. That is what the exponent clamp below is for, and it is why
 *   the clamp is [0.5, 1.0] rather than a wider band picked for comfort: 0.5 is
 *   white noise, the most that averaging can ever remove, and 1.0 is a spread
 *   flat in nanoseconds. A fit above 1.0 is not a noisier board — it is a ramp,
 *   and accepting it would be that second failure exactly.
 *
 * The adaptation time constant is the full depth of the hierarchy, so every
 * level averages over the same wall-clock span rather than the same number of
 * evaluations. Nothing here is tuned to a board: the only quantity a user sets
 * is how often a correction may be triggered by noise alone (MFT), and the
 * per-level multiplier follows from it, which is what the hand-picked 8 in the
 * formula was standing in for. */
#define MLACC_FIT_MIN_N     24u        /* evaluations before a level may be fitted */
#define MLACC_FIT_TAU_S     ((double)(1u << (MLACC_LEVELS + 1)))  /* 4096 s */
#define MLACC_ALPHA_WHITE   0.5        /* what white noise gives                */
#define MLACC_ALPHA_FLAT    1.0        /* spread flat in ns; above this is a ramp */

uint8_t  g_mlacc_thr_src   = MLACC_THR_FOLLOW;
uint16_t g_mlacc_thr_tgt_s = 0;        /* 0 = MLACC_THR_TGT_DEFAULT */

static double   s_mla_ms_test[MLACC_LEVELS];   /* EMA of test^2, per level */
static uint16_t s_mla_test_n[MLACC_LEVELS];
static mlacc_fit_t s_mla_fit;

#define MLACC_MIN_HORIZON  64

/* Lowest hierarchy level at which the frequency term is trusted. Below this the
 * pair-slope is dominated by its own noise — see the sign/gate block in
 * multi_level_accum() for the arithmetic and the measurement. The TIM2 trim
 * below is not bound by this: it reads the counter, not the slope. */
#define MLACC_FREQ_MIN_LEVEL 3

/* Gate for the TIM2 frequency trim, in Hz. avg100 resolves 0.01 Hz, so 0.03 Hz
 * is three counts of the measurement. Below it the counter has nothing to say
 * and the pair-slope term owns the fine work as before. */
#define MLACC_TIM2_TRIM_GATE 0.03

/* Per-level phase limits, in accumulator units (1 ns per LSB of phase).
 *
 * These are Alan's own table, multiplied by 25 — his counter stepped 25 ns where
 * the LTIC detector steps about 1, and the accumulator arithmetic is otherwise
 * identical, so his numbers carry across by simple scaling.
 *
 * The first attempt scaled them down instead, on the reasoning that a finer
 * detector should permit tighter thresholds. That was wrong, and the measurement
 * says so. What sets the floor is not the detector's resolution but the GPS
 * signal's own wander, which is the same for both of us — Alan's note about the
 * one derived value makes the point exactly: the specification allowed 64 ns at
 * 64 s, but "a cheap GPS module with a poor signal can often wander +/-100 ns and
 * generate a false error", so he used 125 ns at 128 s instead.
 *
 * Halving his numbers put the level-0 threshold at 225 ns against a phase that
 * swings +/-400 ns, so corrections fired on noise: 177 of them in 720 seconds,
 * with the loop never climbing past level 3. Restored, level 0 sits at 462 ns and
 * noise alone no longer reaches it.
 *
 * The phase threshold each entry represents is limit / 2^(level+2), since the
 * accumulator holds 2^(level+1) samples of (2*phase + 1):
 *
 *     level 0    462 ns     level 4    191 ns     level 8    108 ns
 *     level 1    400 ns     level 5    164 ns     level 9    103 ns
 *     level 2    331 ns     level 6    126 ns  <- the derived one
 *     level 3    264 ns     level 7    117 ns     level 10    63 ns
 *
 * Runtime rather than const: only the 128 s value was ever derived from a
 * specification and Alan calls the rest arbitrary, so every board will want to
 * adjust them. Editable with MLP, listed by ML, saved with ES ALGO12. */
int32_t g_mlacc_lim[MLACC_LEVELS] = {
    /* level  0,    2 s */    1850,
    /* level  1,    4 s */    3200,
    /* level  2,    8 s */    5300,
    /* level  3,   16 s */    8450,
    /* level  4,   32 s */   12200,
    /* level  5,   64 s */   21050,
    /* level  6,  128 s */   32350,   /* 126 ns — the one derived value */
    /* level  7,  256 s */   59700,
    /* level  8,  512 s */  110300,
    /* level  9, 1024 s */  210200,
    /* level 10, 2048 s */  259200,
};

/* Static run state. Not user-tunable — that lives in g_mlacc_* above. */
static int32_t  s_mla_val[MLACC_LEVELS];   /* stored value per level          */
static uint32_t s_mla_count;               /* seconds since last correction   */
static uint8_t  s_mla_last_level;          /* level that last acted           */
static uint32_t s_mla_corrections;
/* Seconds since the loop last LOST the phase, not since it last acted. A
 * correction or a zero-crossing is the loop working, not the loop struggling —
 * coupling the LOCK lamp to s_mla_count showed "ACQ" for 40% of a healthy
 * run (measured, 17.08 log: corrections every ~4 min, 64 s of count needed).
 * Reset only where control was genuinely absent: WAIT/SYNC/FLL/NOPH. */
static uint32_t s_mla_quiet;
/* Scale from phase to DAC counts, published so the zero-crossing test can use it
 * before the correction block computes it. */
static double   s_mla_lsb_ns;
static bool     lsb_per_ns_ready;
static double   s_mla_ms_phase;     /* mean square phase, for the noise estimate */
static uint32_t s_mla_ms_n;
/* Zero-crossing state. See the block in multi_level_accum() for why this is
 * not optional. */
static bool     s_mla_returning;    /* a limit correction is still settling   */
static uint32_t s_mla_wait;         /* samples since the limit correction     */
static int32_t  s_mla_ph_sign;      /* sign of the phase when the limit fired */
static int32_t  s_mla_slew_lsb;     /* deliberate slew, removed at the crossing */
static uint32_t s_mla_zc_hits;      /* zero-crossing corrections applied      */
static uint32_t s_mla_armed;        /* picDIV re-arms, for telemetry */
/* Seconds to ignore the detector after a picDIV re-arm: the divider lands the
 * phase at a quantised offset (~±3 µs on this build), and that jump must not
 * enter the accumulator or the noise estimate. */
static uint32_t s_mla_post_arm;
/* Was the PREVIOUS second a usable, contiguous phase reading? The noise
 * estimate below works on consecutive differences, so a difference taken
 * ACROSS a gap (NOPH, SYNC, a re-arm) is not a measurement of noise — it is
 * the gap. Tracking this structurally is what lets the outlier gate stop
 * being self-referential; see the estimator block. */
static bool     s_mla_prev_valid;
static int32_t  s_mla_last_slope;
static int32_t  s_mla_last_phase;

/* Scale from phase error to DAC counts. Zero means derive it from the CT
 * calibration, which is what algorithm 11 does and what most users want. */
float   g_mlacc_gain = 0.0f;
/* Force a correction once this level is reached, whatever the limits say —
 * otherwise a slow drift under every limit would never be acted on. */
uint8_t g_mlacc_run_level = 9;
/* Zero-crossing correction on/off. On by default because Alan calls it essential,
 * but switchable: it has been the source of two separate faults here and a tester
 * needs to be able to take it out of the picture without recompiling. */

/* Published for telemetry: the last count error seen, and the state of the
 * hierarchy. The tuner plots the offset, so it has to leave the algorithm. */
/* int16_t, not int8_t: this now carries phase in nanoseconds, and the detector
 * band alone spans several hundred. The first version held the count error in
 * whole hertz, where int8_t was ample — the type had to grow with the meaning. */
int16_t g_last_offset = 0;

/* Outside the GPSDO_LTIC guard, like mlacc_get_stats() below it and like every
 * g_mlacc_* global: the accessors and the settings fields exist on any build so
 * that the CLI and the flash ring do not need a guard of their own. Only the
 * loop itself is conditional. */
void mlacc_get_fit(mlacc_fit_t *out)
{
    if (out) *out = s_mla_fit;
}

void mlacc_get_stats(mlacc_stats_t *out)
{
    out->last_action  = s_mla_last_level;
    out->corrections  = s_mla_corrections;
    out->arms         = s_mla_armed;
    out->sigma_ns     = (int32_t)sqrt(s_mla_ms_phase);
    out->zero_cross   = s_mla_zc_hits;
    out->seconds      = s_mla_count;
    out->last_slope   = s_mla_last_slope;
    out->last_phase   = s_mla_last_phase;
}

static void mlacc_reset(void)
{
    /* Clears the accumulator hierarchy only. The zero-crossing flag deliberately
     * survives: it is set BY a correction and has to outlive the reset that
     * correction performs, or the crossing it is waiting for never gets noticed. */
    for (int i = 0; i < MLACC_LEVELS; i++) s_mla_val[i] = 0;
    s_mla_count = 0;
}

#ifdef GPSDO_LTIC
/* Algorithm 12 requires the phase detector. It once had a fallback that
 * integrated the TIM2 count error on boards without one; that fallback
 * accumulated a random walk of quantisation noise rather than phase and
 * destroyed the lock, so it is gone and the whole function is guarded instead.
 * LA 12 refuses at the CLI when this is not defined. */

/* Standard normal quantile by bisection on erf(). Called eleven times every
 * 300 s, so the cost does not matter and a closed-form approximation would only
 * add a second thing to be wrong about. */
static double mlacc_probit(double p)
{
    if (p <= 0.5) return 0.0;
    double lo = 0.0, hi = 8.0;
    for (int i = 0; i < 60; i++) {
        double m = 0.5 * (lo + hi);
        if (0.5 * (1.0 + erf(m / sqrt(2.0))) < p) lo = m; else hi = m;
    }
    return 0.5 * (lo + hi);
}

/* How many sigma at this level, from the interval the user is willing to see
 * between corrections that noise alone triggered.
 *
 * This is the term the formula's hard-coded 8 was doing by hand. Its comment
 * says as much — "the hierarchy tests level 0 twice as often as level 1 and
 * eight times as often as level 3, so a threshold that is merely unlikely per
 * test still fires constantly at the bottom" — and then picks a single number
 * for every level anyway. The test rate IS the level, so the multiplier can be
 * derived rather than chosen: level L is evaluated every 2^(L+1) seconds, so
 * allowing one false fire per tgt seconds fixes the tail probability. */
static double mlacc_level_q(int L, double tgt_s)
{
    double p = (double)(1u << (L + 1)) / tgt_s;
    if (p > 0.5) p = 0.5;              /* at the top the hierarchy is MR's job */
    return mlacc_probit(1.0 - 0.5 * p);
}

/* Which source is actually driving the table. MLACC_THR_FOLLOW keeps the old
 * welding of limits to gain, so an installation that never touches MF sees the
 * behaviour it has always had. */
static uint8_t mlacc_thr_source(void)
{
    if (g_mlacc_thr_src != MLACC_THR_FOLLOW) return g_mlacc_thr_src;
    return (g_mlacc_gain <= 0.0f) ? MLACC_THR_SIGMA : MLACC_THR_STORED;
}

/* The white-noise table, unchanged in arithmetic and moved out of the control
 * path so MF can select it independently of the gain. */
static void mlacc_build_sigma(void)
{
            double sigma = sqrt(s_mla_ms_phase);
            /* Floor: this detector plus the sawtooth cannot honestly resolve
             * below a few ns, and a sigma under that only means the estimator
             * has been starved, not that the board got quiet. */
            if (sigma < 5.0) sigma = 5.0;
            if (sigma > 1.0 && sigma < 20000.0) {
                /* The threshold applies to the TEST EXPRESSION, not to the
                 * average phase — a distinction that cost a full round of
                 * measurement to notice.
                 *
                 * The test is |(a+b) + 2*(b-a)| = |3b - a|, where a and b are
                 * each sums of 2^L samples. So sd(a) = sd(b) = sigma*sqrt(2^L),
                 * and sd(3b - a) = sigma*sqrt(2^L)*sqrt(10). Setting the
                 * threshold from sigma/sqrt(N) instead — the standard error of
                 * the mean — makes it 4.5x too low at level 0 and worse above,
                 * so noise crossed it constantly and the hierarchy reset before
                 * it could climb. Simulated: level 0 firing 1040 times in 4000 s
                 * with levels 3 and above never reached.
                 *
                 * Five sigma on the correct quantity fires on noise about once in
                 * 3.5 million tests. Real drift accumulates in both terms and
                 * crosses far sooner, because it does not cancel the way noise
                 * does. */
                for (int L = 0; L < MLACC_LEVELS; L++) {
                    double sd_test = sigma * sqrt((double)(1u << L)) * sqrt(10.0);
                    /* Eight sigma, not the usual three or four.
                     *
                     * The hierarchy tests level 0 twice as often as level 1 and
                     * eight times as often as level 3, so a threshold that is
                     * merely "unlikely" per test still fires constantly at the
                     * bottom. Six sigma settled the loop — phase to 10 ns RMS
                     * about a zero mean over seven hours — but was still applying
                     * a correction every 50 s once the noise had fallen to 9 ns,
                     * which is the loop reacting to its own noise floor rather
                     * than to drift. Eight quiets that without loosening the
                     * response to anything real: drift accumulates in both terms
                     * of the test and crosses far sooner than noise, which
                     * largely cancels.
                     *
                     * Adjustable per level with MLP if a board wants otherwise. */
                    int32_t units  = (int32_t)(8.0 * sd_test);
                    if (units < 100) units = 100;
                    g_mlacc_lim[L] = units;
                }
            }
    s_mla_fit.valid = false;
}

/* The measured table. Fits log2(sd of the level-L test) against L and builds
 * the whole table from the fit — amplitude and exponent both measured, and the
 * per-level multiplier derived from MFT rather than chosen. See the block above
 * MLACC_MIN_HORIZON for why every constant the formula version carries is gone
 * and what stops the estimate chasing its own tail. */
static void mlacc_build_measured(void)
{
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    int n = 0;
    for (int L = 0; L < MLACC_LEVELS; L++) {
        s_mla_fit.tests[L] = s_mla_test_n[L];
        if (s_mla_test_n[L] < MLACC_FIT_MIN_N) continue;
        if (s_mla_ms_test[L] <= 0.0) continue;
        double y = log(sqrt(s_mla_ms_test[L])) / log(2.0);
        sx += L; sy += y; sxx += (double)L * L; sxy += (double)L * y; n++;
    }
    s_mla_fit.levels_used = (uint8_t)n;
    if (n < MLACC_FIT_MIN_LVL) { s_mla_fit.valid = false; return; }

    double den = (double)n * sxx - sx * sx;
    if (den <= 0.0) { s_mla_fit.valid = false; return; }
    double slope = ((double)n * sxy - sx * sy) / den;
    double inter = (sy - slope * sx) / (double)n;

    /* The clamp is physics, not taste. Below 0.5 would mean averaging removes
     * more than white noise allows; above 1.0 the spread is growing faster than
     * flat-in-ns, which is a phase RAMP rather than a noisier board — and
     * letting a ramp raise the threshold is the failure this file already
     * recorded once, where sigma climbed 165 -> 746 ns and the loop froze. */
    if (slope < MLACC_ALPHA_WHITE) slope = MLACC_ALPHA_WHITE;
    if (slope > MLACC_ALPHA_FLAT)  slope = MLACC_ALPHA_FLAT;

    double tgt = (g_mlacc_thr_tgt_s > 0u) ? (double)g_mlacc_thr_tgt_s
                                          : (double)MLACC_THR_TGT_DEFAULT;
    for (int L = 0; L < MLACC_LEVELS; L++) {
        double sd = pow(2.0, inter + slope * (double)L);
        double u  = mlacc_level_q(L, tgt) * sd;
        /* Same band MLP accepts, so a measured table and a hand-set one are
         * always the same kind of number and ML can print them side by side. */
        if (u < 1.0)      u = 1.0;
        if (u > 500000.0) u = 500000.0;
        g_mlacc_lim[L] = (int32_t)u;
    }
    s_mla_fit.exponent       = (float)slope;
    s_mla_fit.intercept_log2 = (float)inter;
    s_mla_fit.valid          = true;
}

uint16_t multi_level_accum(uint16_t pwm, uint32_t ppscount)
{
    FreqSnapshot_t s;
    take_freq_snapshot(&s);
    if (!s.have10) { s_mla_quiet = 0; set_trend("WAIT"); return pwm; }

    /* INPUT: phase in nanoseconds, from the LTIC detector.
     *
     * The first port fed instant_offset — the TIM2 count error in whole hertz —
     * and it was blind, for the reason above. Alan accumulates PHASE, and that is
     * the difference: phase integrates, so an error of 1e-11 that is invisible in
     * a one-second frequency count becomes 25 ns of phase in 2500 seconds. He
     * asked why I had said 100 ns when a TIC resolves 1 ns; he was right, I had
     * quoted the counter's resolution rather than the detector's. The detector is
     * 25x finer than the design this comes from. */
    bool    ph_valid = false;
    double  ph       = ltic_phase_error_ns(&ph_valid, ppscount);
    bool    have_phase = (ph_valid && g_ltic.ns_per_volt > 1.0f);
    int32_t phase_ns   = have_phase
                       ? (int32_t)(ph + (ph < 0 ? -0.5 : 0.5))
                       : 0;

    /* TIM2 frequency estimate [Hz]. The 100 s average when it exists (0.01 Hz
     * resolution), otherwise a ~50 s EMA of the 1-second count error. This is
     * the one measurement Alan never had — his PIC had no counter gated by
     * PPS — and it is what breaks the deadlock his design could only escape
     * by restarting: while the phase detector is blind, the frequency still
     * reads true. */
    double  f_meas;
    {
        static double f_ema = 0.0;
        f_ema += ((double)s.instant_offset - f_ema) * 0.02;
        f_meas = s.have100 ? (s.avg100 - 10000000.0) : f_ema;
    }

    /* The picDIV has to be armed, or the detector never gives a valid reading.
     *
     * Algorithms 10 and 11 both arm it: algo 10 on entering ACQ, algo 11 through a
     * hold-off bridge once the frequency has settled but the phase is still
     * railed. Algorithm 12 did neither, and the consequence was quiet rather than
     * loud — with the ramp parked against a rail nothing works and nothing says so.
     *
     * The gate is |f| < 0.5 Hz, read from the TIM2 estimate above. The earlier
     * +/-10 Hz gate re-armed 87 times in one hour on the 14.08 log: each arm
     * lands the phase at a divider-quantised offset (~±3 µs, mostly outside
     * the ±1 µs band) while the uncorrected frequency keeps the phase ramping,
     * so the detector rails again within seconds and the next arm rolls the
     * same dice. Arming is only worth disturbing the divider once the
     * frequency is close enough for the phase to STAY wherever it lands; until
     * then the FLL below brings it there. */
    {
        static uint32_t railed_cnt  = 0;
        static uint32_t arm_holdoff = 0;
        bool freq_close = (f_meas > -0.5 && f_meas < 0.5);

        if (arm_holdoff > 0) {
            arm_holdoff--;
            railed_cnt = 0;
        } else if (!have_phase && freq_close) {
            if (railed_cnt < 0xFFFFFFFFu) railed_cnt++;
            if (railed_cnt >= 5u) {
                ltic_arm_picdiv();
                railed_cnt     = 0;
                arm_holdoff    = 60u;   /* was 15: let the phase settle before another try */
                s_mla_post_arm = 5u;    /* keep the ±3 µs landing jump out of the accumulator */
                s_mla_armed++;
            }
        } else {
            railed_cnt = 0;
        }
    }

    /* Just re-armed: the divider output has jumped to a quantised offset and
     * the first few readings are that jump, not the oscillator. Let it settle. */
    if (s_mla_post_arm > 0) {
        s_mla_post_arm--;
        s_mla_quiet = 0;
        set_trend("SYNC");
        s_mla_count++;
        s_mla_prev_valid = false;   /* the next reading is not contiguous */
        return pwm;
    }

    if (!have_phase) {
        /* Detector fitted but not reading — railed, saturated, or not yet armed.
         *
         * Alan's answer to this state was a full restart (RTquickAct: control
         * voltage to zero, re-anchor, begin again). We can do better, because
         * TIM2 measures the frequency directly: a gentle FLL step walks f
         * toward zero, the phase ramp that was carrying it out of the band
         * stops, and the detector recovers on its own — no dice roll needed.
         *
         * This is what the last 19 minutes of the 14.08 log were missing:
         * f = +0.26 Hz (measured, printed every second as f100), the phase
         * sweeping the band every ~20 s, PWM frozen at 41486, trend cycling
         * NOPH/false-LOCK. Gate at 0.3 Hz so the fine loop owns anything
         * smaller; step damped 0.1 and clamped so a mis-scaled CT constant
         * cannot run away — the correction is proportional to the same
         * lsb_per_hz it uses, so it converges at any scale, just slower. */
        /* ONE COMPUTED STEP, THEN WAIT FOR THE AVERAGE TO REFRESH.
         *
         * This was a bang-bang loop and could not have been anything else: the
         * step was -f*lsb_per_hz*0.10 clamped to +/-64, which saturates at
         * |f| = 64/(2500*0.1) = 0.256 Hz, while the gate below it only opened
         * at 0.3 Hz. The proportional part could never act at all. Driven once
         * a second from a 100 s average — about 50 s of lag — that gives
         * 64 LSB/s * 50 s = 3200 LSB of travel before the measurement even
         * begins to respond, which at the measured 319.5 uHz/LSB is 1.0 Hz of
         * overshoot.
         *
         * Both numbers are in the logs. 14.08 21:10: 462 of 655 consecutive
         * steps exactly +/-64, PWM sweeping 12845 LSB, f100 swinging -1.29 to
         * +1.55 Hz. 15.08 00:14, t=108..300: PWM 41289 -> 45897 -> 39241 in one
         * cycle, f100 -5.79 -> -0.59 -> +1.26.
         *
         * So: apply the whole computed correction ONCE, then hold off for as
         * long as the average it was computed from needs to refresh. Damped 0.5
         * against a gain that CT knows only to about 25% (it derives 2500 LSB/Hz
         * where the log measures 3130), which leaves the per-step loop gain near
         * 0.4 — convergent for any gain error within a factor of two.
         *
         * TWO SPEEDS, because one window cannot do both jobs. While the error is
         * large the 10 s average is used: ~5 s of lag, so a step every 10 s, and
         * a cold start walks in briskly. Once it is small the 100 s average
         * takes over, because it is the only one that resolves 0.01 Hz. The
         * hold-off always matches the window in use, so no step is ever applied
         * twice on the strength of a single measurement.
         *
         * GATE at 0.05 Hz, not 0.3. Above 0.147 Hz the phase crosses the whole
         * +/-940 ns detector band inside one 64 s horizon, so the old gate left
         * a dead zone from 0.147 to 0.3 Hz in which the phase loop could not get
         * a long enough look and the FLL considered its work done. The 15.08
         * log ended parked in it, at f = +0.27 Hz.
         *
         * Simulated on a model calibrated against that log, four seeds: over two
         * hours this holds |f| at 0.001 Hz with 83% of seconds giving a usable
         * phase reading and the hierarchy averaging level 3.1, against 0.200 Hz,
         * 36% and level 0.3 for the loop as it stands. */
        static uint32_t fll_holdoff = 0;
        double lsb_per_hz = (g_pid[7].Kp > 100.0) ? ((double)g_pid[7].Kp / 0.40) : 0.0;
        if (fll_holdoff > 0u) {
            fll_holdoff--;
        } else if (lsb_per_hz > 0.0 && ppscount > 60u &&
                   (f_meas > 0.05 || f_meas < -0.05)) {
            double f_use = f_meas;
            /* Far out: steer from the short window and step again sooner. */
            if (s.have10) {
                double f_fast = s.avg10 - 10000000.0;
                if (f_fast > 1.0 || f_fast < -1.0) {
                    f_use = f_fast;
                    fll_holdoff = 10u;
                } else {
                    fll_holdoff = 100u;
                }
            } else {
                fll_holdoff = 100u;
            }
            /* SIGN FROM THE BOARD, NOT FROM THIS LINE.
             *
             * This read -f_use, which is right only because LPOL is -1 here.
             * The TIM2 frequency branch takes +polarity, the convention algo 11
             * established from a cold-start runaway on this wiring (see the
             * comment at its freq_lsb): TIM2 and the LTIC detector have
             * opposite orientation, so the frequency branch and the phase
             * branch carry opposite signs. With polarity == -1 this evaluates
             * to exactly what it did before, so nothing changes on this board;
             * on an LPOL +1 board the old line was positive feedback. */
            double pol_f = (g_ltic.polarity == -1) ? -1.0 : 1.0;
            int32_t d = (int32_t)(pol_f * f_use * lsb_per_hz * 0.5);
            s_mla_quiet = 0;
            set_trend("FLL ");
            return clamp_pwm((int32_t)pwm + d);
        }
        s_mla_quiet = 0;
        set_trend("NOPH");
        s_mla_count++;
        s_mla_prev_valid = false;   /* the next reading is not contiguous */
        return pwm;
    }
    g_last_offset = (int16_t)phase_ns;

    /* ZERO-CROSSING TEST.
     *
     * Alan calls this essential and the reason is worth stating: after a limit
     * correction changes the frequency, the phase keeps moving in the direction
     * it was already going. It sweeps through zero and out the other side, and
     * usually fails the limit again — so the loop corrects, overshoots, corrects
     * back, and settles slowly if at all.
     *
     * The moment the phase crosses zero is special. At that instant the phase
     * error is nil, but the frequency error that carried it there is still
     * present. Cancel the frequency error exactly then and the oscillator is left
     * with both the right frequency AND no phase error — a clean state, rather
     * than one the loop has to iterate towards.
     *
     * Measured against Alan's own logs: his loop corrects every 506 seconds where
     * mine corrects every 130. Most of that difference is this test.
     *
     * The flag is set when a limit correction fires, and cleared here on the first
     * sample whose phase has taken the sign the correction was pushing towards —
     * which is the crossing. */
    /* Give up waiting after a while. If the phase does not cross within a few
     * minutes, the correction did not overshoot and there is nothing to cancel —
     * holding the flag open would let a much later, unrelated crossing trigger a
     * correction based on a stale slope. 300 s: a slew at the 64 s minimum
     * horizon nulls its phase in ~64 s, so 5 horizons is generous. */
    if (s_mla_returning && ++s_mla_wait > 300u) {
        s_mla_returning = false;
    }
    if (s_mla_returning && phase_ns != 0) {
        /* The phase has arrived: it has taken the opposite sign to the error that
         * triggered the correction. Remove the deliberate slew and the oscillator
         * is left at the right frequency AND no phase error — which is the point,
         * and why Alan calls this essential rather than an optimisation. */
        if ((phase_ns > 0) != (s_mla_ph_sign > 0)) {
            s_mla_returning = false;
            if (s_mla_slew_lsb != 0) {
                int32_t dz = -s_mla_slew_lsb;
                s_mla_slew_lsb = 0;
                s_mla_zc_hits++;
                mlacc_reset();
                set_trend("ZC  ");
                return clamp_pwm((int32_t)pwm + dz);
            }
        }
    }

    /* MEASURE THE PHASE NOISE, AND SET THE THRESHOLDS FROM IT.
     *
     * The limits were first taken from Alan's design and scaled by the ratio of
     * counter steps. That was the wrong quantity to scale by. What a threshold
     * has to clear is not the detector's resolution but the NOISE on the
     * measurement — detector, sawtooth and the GPS signal's own wander together
     * — and that differs between builds for reasons a step size does not capture.
     *
     * Measured here: mean phase -1 ns, standard deviation 462 ns. The oscillator
     * was correctly tuned; all of that spread was noise. The level-0 threshold
     * sat at 462 ns, so 41% of samples crossed it, each firing a correction of a
     * couple of hundred DAC counts. 620 corrections in 1685 seconds, the loop
     * resetting its hierarchy every 2.7 s and never reaching level 2 — the
     * arithmetic was right and the threshold was meaningless.
     *
     * So it is measured instead. An exponential estimate of the mean-square phase
     * gives sigma; averaging N samples divides it by sqrt(N); and a threshold of
     * four sigma on that mean fires on noise about once in 16000 tests, which at
     * level 0 is once in nine hours. Any real drift crosses it far sooner.
     *
     * MG > 0 keeps the stored table instead, for anyone who would rather set them
     * by hand. */
    /* The white-noise estimate runs whatever MG says. It used to live inside
     * `if (g_mlacc_gain <= 0.0f)` together with the table build, which had two
     * costs: MF could not offer the formula table with a hand-set gain, and
     * `sig=` in telemetry read a flat zero on every board running MG > 0 — not
     * because the board was quiet but because nothing was measuring. */
    {
        /* Noise from FIRST DIFFERENCES, not deviations about a tracked mean.
         *
         * Two estimators failed here before. Mean-square-of-phase counted a
         * standing offset as noise (phase parked at -985 ns, 45 ns of real
         * noise, "sigma" read 986). Deviations about a slow tracked mean fixed
         * that but not the worse case: the phase RAMP that an uncorrected
         * frequency error produces swept the estimate upward with it — over
         * the 14.08 log sigma climbed 165 -> 746 ns while every threshold
         * scaled with it, until no threshold was reachable inside the ±1 µs
         * detector band and the loop froze outright. A threshold that grows
         * with the error it is meant to catch can never catch it.
         *
         * Consecutive differences kill a linear ramp exactly (dp of a ramp is
         * constant, and its contribution is (drift*dt)² which at GPSDO rates
         * is nano-noise: 50 ns/s of drift gives dp = 50, vs sigma 165). They
         * halve into the per-sample variance (E[dp²] = 2σ²), and do not care
         * where the mean sits. A re-arm's ±3 µs landing is one huge
         * difference — rejected by the outlier gate below and by the 5 s
         * post-arm skip anyway. */
        /* THE OUTLIER GATE MUST NOT BE ABLE TO SUPPRESS ITS OWN INPUT.
         *
         * The previous gate was dp_lim = 5*sigma, read from the estimate it was
         * feeding. That is a one-way ratchet: once sigma is small, every
         * difference big enough to raise it is rejected as an outlier, so it can
         * only ever fall further. Measured on 14.08 21:10 — sig read exactly
         * 2 ns for all 1020 samples of the run, and with the limits derived from
         * it the whole hierarchy pinned at the 100-unit floor: 79 of 80
         * corrections fired at level 0, one at level 1. A multi-level
         * accumulator that never leaves level 0 is not one.
         *
         * Two changes. The gate is now ABSOLUTE (300 ns — well above any real
         * per-second phase step, well below a re-arm's ±3 µs landing), so it
         * cannot move with the estimate. And the genuine outliers it was really
         * there to catch — differences taken across a NOPH/SYNC/re-arm gap — are
         * excluded structurally by s_mla_prev_valid instead of statistically. */
        static int32_t prev_ph = 0;
        if (s_mla_prev_valid) {
            double dp = (double)(phase_ns - prev_ph);
            if (dp < 0.0) dp = -dp;
            if (dp < 300.0) {
                s_mla_ms_phase += 0.002 * (0.5 * dp * dp - s_mla_ms_phase);
            }
        }
        prev_ph = phase_ns;
        s_mla_prev_valid = true;
        if (++s_mla_ms_n >= 300u) {          /* enough samples to mean something */
            s_mla_ms_n = 0;                  /* re-measure every 300 s */
            if (mlacc_thr_source() == MLACC_THR_MEASURED) mlacc_build_measured();
            else if (mlacc_thr_source() == MLACC_THR_SIGMA) mlacc_build_sigma();
        }
    }

    int32_t carry = 2 * phase_ns + 1;

    uint32_t work = s_mla_count;
    int level = 0;
    int32_t applied = 0;

    for (; level < MLACC_LEVELS; level++) {
        int32_t a = s_mla_val[level];
        s_mla_val[level] = carry;          /* the new value becomes the stored one */

        if ((work & 1u) == 0u) {
            /* First value at this level — nothing to compare against yet. */
            if (work == 0u) break;         /* and nothing above holds anything */
            break;
        }

        /* Two values at this level: A is the one just displaced, B the new one. */
        int32_t b     = carry;
        int32_t slope = b - a;
        int32_t phase = (a + b) + 2 * slope;

        int32_t pabs = phase < 0 ? -phase : phase;
        int32_t sabs = slope < 0 ? -slope : slope;

        /* MEASURE THIS LEVEL'S OWN SPREAD, before deciding anything with it.
         *
         * Updated on every evaluation, fired or not: an estimate that only saw
         * the tests it let through would be censored by its own threshold,
         * which is the shape of the dp_lim ratchet recorded above. Skipped
         * while a slew is in flight, because during those seconds the test is
         * measuring the loop's own deliberate action and not the board.
         *
         * The rate is the same wall-clock time constant at every level rather
         * than the same number of samples — level 0 is evaluated 1024x more
         * often than level 10, and an EMA in samples would give them adaptation
         * spans four hours apart. The span is the depth of the hierarchy
         * itself, so it is not a number anyone had to pick. */
        /* Not while acquiring either. Same reason as s_mla_returning: during
         * pull-in the phase is the loop travelling, not the board's noise. The
         * 20.08 15:08 run measured what it costs — 102 ns rms over the first
         * 300 s against 5.7 ns once settled, which is 321x in the mean SQUARE
         * the estimator accumulates, and with a 4096 s span the pull-in would
         * still be setting the thresholds an hour after boot.
         *
         * s_mla_quiet is the right gate rather than a timer: it counts seconds
         * since control was genuinely absent (WAIT/SYNC/FLL/NOPH) and is NOT
         * reset by corrections, so it says "the loop has the phase" and not
         * "nothing has happened lately". It is the same test the LOCK lamp
         * uses, so the estimator measures exactly the regime the operator is
         * being shown. A board that never settles never fills the estimate,
         * the table stays at whatever is stored, and ML says the fit is not
         * ready — which is the honest outcome, not a silent bad table. */
        if (!s_mla_returning && s_mla_quiet > 16u) {
            double a_ema = (double)(1u << (level + 1)) / MLACC_FIT_TAU_S;
            if (a_ema > 0.25) a_ema = 0.25;
            if (s_mla_test_n[level] < 0xFFFFu) s_mla_test_n[level]++;
            /* WARM-UP. The weight is the larger of the EMA rate and 1/n, so the
             * first samples form a plain running mean and the EMA only takes
             * over once it would average over more history than exists.
             *
             * Seeding the EMA from a single squared test instead — which is
             * what the first version did — costs 1/a_ema samples to forget, and
             * at level 0 that is 2048 evaluations. Caught on the 20.08 home
             * record: eighteen minutes in, every level was still carrying its
             * own first sample, the fit read 0.46 where the same data measured
             * offline gives 0.92, and the clamp turned that into the white-noise
             * exponent — the estimator would have reported exactly the
             * assumption it exists to replace, and looked healthy doing it. */
            double w = 1.0 / (double)s_mla_test_n[level];
            if (w < a_ema) w = a_ema;
            double t2 = (double)phase * (double)phase;
            s_mla_ms_test[level] += w * (t2 - s_mla_ms_test[level]);
        }

        /* MR: force a correction once this level is reached, whatever the limits
         * say. Without it a drift slow enough to stay under every limit would be
         * accumulated forever and never acted on — Alan's SUrunLev serves the
         * same purpose in the original. Suppressed while a slew is in flight,
         * like every limit correction (see the act gate below). */
        if (!s_mla_returning && level >= (int)g_mlacc_run_level) {
            s_mla_last_level = (uint8_t)level;
            s_mla_last_slope = slope;
            s_mla_last_phase = phase;
            applied = phase;
            break;
        }

        /* Phase test only. Alan dropped the frequency test after using it for a
         * while: "It was an experiment. The phase test can be slower to see a
         * deviation, but what we want is a stable system where the tests always
         * pass. So the frequency test is unnecessary." Keeping it would add a
         * second path to a correction that the phase test reaches anyway.
         *
         * Suppressed while a correction's deliberate slew is still walking the
         * phase home (s_mla_returning): stacking a second slew on top of the
         * first is what turned the 14.08 pull-in into a +3800 LSB overshoot
         * and the following minutes into a 4000-LSB limit cycle at one
         * correction every two seconds. The zero-crossing test clears the
         * flag when the phase arrives; the timeout abandons it if it never
         * does. Accumulation continues — the state after the crossing is
         * fresh, not stale. */
        (void)sabs;
        bool act = (!s_mla_returning) && (pabs >= g_mlacc_lim[level]);

        if (act) {
            s_mla_last_level = (uint8_t)level;
            s_mla_last_slope = slope;
            s_mla_last_phase = phase;
            applied = phase;
            break;
        }

        /* Passed both: promote the sum and test again over twice the span. */
        carry = a + b;
        work >>= 1;
    }

    s_mla_count++;

    if (applied == 0) {
        /* LOCK means: the phase has been readable since the last real
         * anomaly (arm, railed detector, no data) AND the TIM2 frequency is
         * home. Corrections and zero-crossings do NOT reset s_mla_quiet —
         * this algorithm corrects on a schedule as a form of self-test, so
         * a correction is evidence of health, not of acquisition. The old
         * test (s_mla_count > 64) read the accumulator's bookkeeping
         * instead of the loop's state of control and lit ACQ for 40% of a
         * settled run. The frequency gate is two-tier: the 100 s average
         * resolves 0.01 Hz, so when it exists 0.05 Hz is a real bound — the
         * 16.08 band-edge cycle sat at 0.12 Hz and would have shown ACQ
         * under it, as it should. Before have100 exists the EMA only
         * resolves whole hertz; keep the loose 0.5 Hz bound there. */
        bool freq_good = s.have100 ? (f_meas < 0.05 && f_meas > -0.05)
                                   : (f_meas < 0.5  && f_meas > -0.5);
        s_mla_quiet++;
        set_trend((s_mla_quiet > 16u && freq_good) ? "LOCK" : "ACQ ");
        return pwm;
    }

    /* Convert the accumulated phase into DAC counts.
     *
     * Two things were wrong here and both showed up in the same log.
     *
     * UNITS. `applied` is the sum of (2*phase_ns + 1) over `span` samples, so
     * dividing by two and by the count gives the average phase error in
     * NANOSECONDS. The first version then multiplied that by LSB-per-HERTZ, as
     * though nanoseconds and hertz were the same quantity. Nulling P ns over T
     * seconds needs a fractional offset of P/(T*1e9), which at 10 MHz is
     * P/(100*T) Hz — so the correction was 100*T times too large, from 200x at
     * level 0 to 102400x at level 9. Every correction slammed into the +/-2000
     * clamp, which is exactly what the log showed.
     *
     * POLARITY. Algorithm 11 applies -polarity to its phase term; this did not
     * apply it at all. On a board with LPOL -1 the correction therefore went the
     * wrong way — positive feedback into a loop that was already over-correcting
     * by four orders of magnitude. */
    /* MG: a hand-set gain overrides the CT-derived one, in LSB per ns directly
     * rather than per hertz — the units a user tuning against a scope actually
     * has. Zero means derive it, which is the default and what most want. */
    double lsb_per_ns = s_mla_lsb_ns;
    if (g_mlacc_gain > 0.0f) {
        lsb_per_ns = (double)g_mlacc_gain;
    } else {
        double lsb_per_hz = (g_pid[7].Kp > 100.0) ? ((double)g_pid[7].Kp / 0.40) : 0.0;
        if (lsb_per_hz <= 0.0) {
            set_trend("NoCT");
            return pwm;
        }
        /* Hz per ns of phase corrected over one second is 1/100 at 10 MHz, so the
         * per-second gain in LSB per ns is lsb_per_hz/100. The span divides it
         * again below, where the correction horizon is applied. */
        lsb_per_ns = lsb_per_hz / 100.0;
    }
    /* Publish for the zero-crossing test, which runs earlier in the next pass and
     * has no other way to know the scale. */
    s_mla_lsb_ns     = lsb_per_ns;
    lsb_per_ns_ready = (lsb_per_ns > 0.0);

    /* TWO terms, applied together. This is what was missing, and the reason both
     * zero-crossing attempts made things worse instead of better.
     *
     * State is phase p and frequency error f, with dp/dt = f. A limit correction
     * has two separate jobs:
     *
     *     cancel the measured frequency error       df = -f
     *     impose a deliberate slew to bring p back  df = -p/T
     *
     * which together leave f = -p/T exactly. Apply only the second — all this did
     * — and the oscillator runs at f_old - p/T instead, so the phase neither
     * returns cleanly nor stays put when it arrives. The loop hunts, which is what
     * every log showed.
     *
     * With both terms the phase walks to zero along a KNOWN slope. That slope is
     * deliberate, so it has to be removed when the phase arrives — which is what
     * the zero-crossing test does. The three pieces are one mechanism; any one of
     * them alone is worse than none, which is exactly what the measurements said.
     *
     * Simulated over 40000 s, 15 ns phase noise, drifting oscillator, four seeds:
     *     phase only          33.6 ns RMS
     *     phase + frequency   33.0 ns
     *     all three           22.4 ns   */
    double polarity = (g_ltic.polarity == -1) ? -1.0 : 1.0;
    double span     = (double)(1u << (s_mla_last_level + 1));
    if (span < (double)MLACC_MIN_HORIZON) span = (double)MLACC_MIN_HORIZON;

    double p_ns  = (double)applied / 2.0 / (double)(1u << (s_mla_last_level + 1));
    double f_nss = (double)s_mla_last_slope
                 / (double)(1u << (2u * s_mla_last_level + 1u));

    double slew_lsb = -polarity * (p_ns / span) * lsb_per_ns;

    /* The frequency term, restored. Alan's cvPWM applies BOTH corrections
     * (250321-O.asm 1293-1297: "add in the frequency correction"; total =
     * phase_corr + freq_corr), and f_nss below is exactly his slope estimate
     * — the pair's phase drift in ns/s, already divided by his 2^(2L+1).
     *
     * It was removed here after noise measurements, but those were made with
     * a poisoned sigma: the estimator of the day counted an uncorrected
     * frequency error's phase ramp as noise (sigma read 300+ ns), so the term
     * amplified exactly the malfunction it was meant to cure. With the
     * first-difference estimator above, the slope of a 2^(L+1)-second pair is
     * a measurement again — a 50 ns/s drift stands out by construction, not
     * luck. And without this term no correction ever cancels the phase ramp:
     * the 14.08 log ended with f = +0.26 Hz, the phase sweeping the detector
     * band every ~20 s, and the loop frozen for the last 19 minutes.
     *
     * Damped 0.5 rather than full cancellation: one pair over 2^(L+1) s is a
     * single measurement, and half-strength converges in two corrections
     * where full strength stakes everything on one. This is Alan's own k, the
     * experimental divisor he mentions and never publishes. */
    double f_ns   = f_nss;
    if (f_ns >   5000.0) f_ns =   5000.0;   /* 0.5 µs/s is not a measurement, it is a fault */
    if (f_ns <  -5000.0) f_ns =  -5000.0;

    /* SIGN: -polarity, the same factor the phase term uses.
     *
     * It was +polarity, copied from algorithm 11's frequency branch. But algo
     * 11's frequency branch reads TIM2, and this firmware's own algo-11 comment
     * records the hardware finding that TIM2 and the LTIC detector have
     * OPPOSITE orientation on this wiring. f_nss is not a TIM2 reading: it is
     * the slope of the SAME accumulator values a and b that produce p_ns, from
     * the SAME detector. A quantity and its own time derivative, measured by one
     * sensor, cannot need opposite feedback signs. Alan's cvPWM agrees — it
     * pushes phase and slope through one conversion and ADDs them
     * (250321O.asm, correctit: "add in the frequency correction" -> CALL addm).
     *
     * With the plant now MEASURED rather than assumed — +319.5 uHz/LSB, from
     * regressing the 100 s mean of PWM against the printed 100 s frequency
     * average over the 14.08 21:10 log, correlation 0.999 at zero lag — the old
     * sign works out to d(phase_rate) = +0.4*f_ns. That is positive feedback.
     *
     * LEVEL GATE: the slope's own noise is sd(f_nss) = sigma * 2^((1-3L)/2), so
     * at level 0 it is 1.41*sigma of pure noise, scaled by 12.5 LSB per ns/s
     * against the phase term's 0.39 LSB per ns. That is a 32:1 noise-to-signal
     * advantage for the wrong quantity, and the log shows exactly what it buys:
     * 46% of corrections slammed into the ±470 clamp, one of them with the
     * phase reading 0 ns and the correction at full scale. By level 3 the same
     * estimate is averaged over 16 s pairs and is a measurement again.
     *
     * Simulated, 20000 s, four seeds, locked start: with the sign fixed and the
     * gate at L>=3 the loop holds 32 ns RMS with the hierarchy averaging level
     * 3.7 and 100% of samples inside the detector band; without the gate,
     * 4190 ns and level 2.1. */
    double freq_lsb = 0.0;
    /* TIM2 TRIM — the frequency term from the COUNTER, at any level.
     *
     * The 16.08 log is why this exists. A phase disturbance pushed the loop to
     * the detector edge at 14:30; every NOPH re-armed the divider ~1.8 us from
     * zero; the pair test fired immediately at level 0-1 (big phase, fresh
     * hierarchy — the corrections and the zero-crossings had reset it twice
     * over), where the slope term above is gated off. So every correction was
     * pure slew, the crossing dutifully removed it, and the PWM returned to a
     * baseline that TIM2 said was 0.12 Hz wrong — for hours, at "LOCK", while
     * the phase rode the band edge every 4:45 min. The three-part mechanism
     * (slew + frequency + cancel-at-crossing) only closes if the frequency
     * part actually fires; pinning the hierarchy at level 0-1 amputated it.
     *
     * The counter is the one measurement Alan never had, and here it sees the
     * error at twelve times its resolution. It REPLACES the slope term while
     * it is open: both estimate the same quantity, and a 100 s gated count
     * beats one pair of accumulator sums at any level the gate admits. The
     * sign carries no LTIC polarity — TIM2 and the detector sit with opposite
     * orientation on this wiring (the algo-11 finding), and this is the same
     * -f*lsb*0.5 the NOPH FLL step uses, whose convergence the 16.08 15:00
     * storm already demonstrated (-290/-224/-187 LSB steps walking f100 home).
     * Damped 0.5, so a mis-scaled CT cannot make it runaway. */
    bool tim2_trim = (s.have100 &&
                      (f_meas > MLACC_TIM2_TRIM_GATE || f_meas < -MLACC_TIM2_TRIM_GATE));
    if (tim2_trim) {
        /* 1 Hz at 10 MHz is 100 ns of phase per second, and lsb_per_ns*100 is
         * lsb_per_hz — stated that way so a hand-set MG gain divides through
         * the same as the CT-derived one. */
        /* +polarity, like every other TIM2-derived term — see the FLL branch
         * above for why the frequency and phase branches differ in sign. With
         * LPOL -1 this is identical to the -f_meas it replaces. */
        freq_lsb = polarity * f_meas * (100.0 * lsb_per_ns) * 0.5;
    } else if (s_mla_last_level >= MLACC_FREQ_MIN_LEVEL) {
        freq_lsb = -polarity * f_ns * lsb_per_ns * 0.5;
    }
    /* Kept as a double to the end. The measured corrections in normal operation
     * have a median size of 6 LSB, so truncating here discarded up to a sixth
     * of each one, always toward zero. */
    double dq = slew_lsb + freq_lsb;

    {   /* Clamp against the detector band: a correction is only useful while the
         * phase stays measurable. 1% of the band per second crosses it in 100 s,
         * comfortably slower than the 64 s minimum horizon. */
        double lim_lsb = 500.0;
        if (g_ltic.range_ns > 100.0f && lsb_per_ns > 0.0) {
            double l = (double)g_ltic.range_ns * 0.01 * lsb_per_ns;
            if (l > 10.0 && l < lim_lsb) lim_lsb = l;
        }
        if (dq >  lim_lsb) dq =  lim_lsb;
        if (dq < -lim_lsb) dq = -lim_lsb;
    }
    int32_t d = (int32_t)dq;

    /* Remember the DELIBERATE part only. The crossing removes exactly this and
     * nothing else — no second measurement, which is where the earlier attempts
     * went wrong by cancelling a freshly measured slope unrelated to the slew
     * actually imposed. */
    s_mla_slew_lsb  = (int32_t)slew_lsb;
    s_mla_returning = true;
    /* RESET THE WAIT COUNTER. Without this line s_mla_wait is free-running: it
     * appeared exactly twice in this file, at its declaration and at the ++ in
     * the give-up test, and never went back to zero. So once it passed 300 —
     * about five minutes into any run — the give-up test fired on the SAME
     * second the flag was raised, and from then on `returning` was dead. That
     * disables both things it gates: the zero-crossing correction, and the
     * suppression of new corrections while a slew is still walking the phase
     * home.
     *
     * Measured on 14.08 22:30, the run that found it: zc = 7 in 76 minutes (all
     * of them inside the first five minutes), and 1072 of 1174 corrections
     * exactly 2 seconds apart — which is the bare level-0 cadence with nothing
     * holding it back (count resets to 0, even second skips, odd second fires).
     * The hierarchy therefore never left level 0: 1133 corrections at level 0,
     * 30 at level 1, 12 at level 2, none above.
     *
     * This is also why the simulation mispredicted: it reset the counter,
     * because that is what the code was clearly meant to do. It modelled the
     * intent, not the source. */
    s_mla_wait      = 0;
    s_mla_ph_sign   = (p_ns > 0.0) ? +1 : -1;


    s_mla_corrections++;
    mlacc_reset();
    set_trend("CORR");
    {
        double target = fine_base(pwm) + dq;
        g_vctl_fine       = target;
        g_vctl_fine_valid = true;
    }
    return clamp_pwm((int32_t)pwm + d);
#endif /* GPSDO_LTIC */
}

uint16_t adjustVctlPWM(uint16_t prev_pwm, uint32_t ppscount, uint8_t algo_no)
{
    /* Invalidate first, so a fractional target can only ever come from THIS
     * cycle. An algorithm that holds, or one that does no fractional
     * arithmetic, simply never sets it and the control task writes 16 bits as
     * it always did. This is the one line that makes the fine path safe to add
     * to some algorithms and not others. */
    g_vctl_fine_valid = false;

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
        /* 12 belongs inside the guard with 10 and 11: it works on the detector's
         * phase and there is no fallback, so without GPSDO_LTIC there is nothing
         * for it to do. LA 12 refuses at the CLI for the same reason. */
        case 12: return multi_level_accum (prev_pwm, ppscount);
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
