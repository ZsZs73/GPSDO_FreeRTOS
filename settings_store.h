/* ======================================================================
 * settings_store.h  —  persistent user settings via the flash ring
 *
 * Part of GPSDO FreeRTOS v1.01
 *
 * Replaces the old STM32duino emulated EEPROM (gpsdo_state.cpp eeprom_*).
 * Settings are stored as a REC_SETTINGS slot in the flash ring (sector 7),
 * alongside the live data (REC_LIVE). This gives them the same atomicity
 * guarantees the ring already provides for live data: per-slot CRC16,
 * sequence-numbered newest-wins, read-back verify, power-loss safety.
 *
 * Layout version (SETTINGS_VER) is the first byte of the payload. A mismatch
 * on recall (e.g. older layout saved by a previous firmware) is treated like
 * a blank ring: compile-time defaults apply, the user re-runs CT/LC/TZ/ES.
 *
 * Selective save (ES <object>): rather than overwrite the entire settings
 * block when the user changes one field, settings_save_partial() reads the
 * last saved block from the ring, updates only the requested group of fields
 * from the live runtime state, and writes a fresh slot. Untouched fields
 * keep their stored values — a deliberate `ES TZ` cannot clobber a hand-tuned
 * PID set the way the old full-state EEPROM write could.
 * ====================================================================== */
#ifndef SETTINGS_STORE_H
#define SETTINGS_STORE_H

#include <stdint.h>
#include <stdbool.h>

/* Bump when the on-flash layout of SettingsBlock_t changes. A mismatch with
 * the stored value triggers a fall-back to defaults on recall. */
#define SETTINGS_VER  4u   /* v4: + LT local-time flag; v3 FA windows; v2 LTIC-Lars */

/* The flat on-flash block. Total size must stay <= FR_PAYLOAD (506 B).
 * Every field is little-endian and tightly packed; the recall path range-
 * checks each one so a corrupt byte can't poison the runtime. */
typedef struct {
    uint8_t  ver;            /* SETTINGS_VER — must match or recall rejects */
    uint16_t pwm;            /* gCtrl.pwm_output                           */
    uint8_t  algo;           /* gCtrl.active_algo                          */
    /* PID params for algos 3-9 (7 algos × 4 floats) */
    float    pid_kp[7];      /* g_pid[3..9].Kp                             */
    float    pid_ki[7];
    float    pid_kd[7];
    float    pid_il[7];
    float    blend_cross;    /* g_blend_crossover                          */
    float    blend_scale;    /* g_blend_scale                              */
    float    nn_max_step;    /* g_nn_max_step                              */
    float    press_off;      /* g_pressure_offset                          */
    float    alt_off;        /* g_altitude_offset                          */
    uint8_t  svin_en;        /* g_svin_enabled                             */
    /* LTIC params (algo 10) */
    float    ltic_nsv;       /* g_ltic.ns_per_volt                         */
    float    ltic_zero;      /* g_ltic.zero_offset                         */
    float    ltic_range;     /* g_ltic.range_ns                            */
    float    ltic_dpll_kp, ltic_dpll_ki, ltic_dpll_kd, ltic_dpll_il;
    float    ltic_lock_kp, ltic_lock_ki, ltic_lock_kd, ltic_lock_il;
    float    ltic_acq_th;    /* g_ltic.acq_threshold_ns                    */
    float    ltic_dpll_th;   /* g_ltic.dpll_lock_thresh                    */
    uint16_t ltic_lock_iv;   /* g_ltic.lock_interval_s                     */
    uint8_t  ltic_state;     /* g_ltic.state                               */
    uint8_t  ltic_submode;   /* g_ltic.submode                             */
    float    ltic_acq_kp, ltic_acq_ki, ltic_acq_kd, ltic_acq_il;
    int8_t   ltic_polarity;  /* g_ltic.polarity                            */
    float    ltic_centre_v;  /* g_ltic.centre_v                            */
    /* LTIC-Lars params (algo 11) — SETTINGS_VER 2+ */
    float    lars_gain;      /* g_lars.gain (0 = auto from CT)             */
    float    lars_damping;   /* g_lars.damping                            */
    uint16_t lars_tc;        /* g_lars.time_const_s                       */
    uint8_t  lars_fdiv;      /* g_lars.filter_div                         */
    uint16_t lars_tic_off;   /* g_lars.tic_offset                         */
    uint16_t lars_lock_lim;  /* g_lars.lock_ns_lim                        */
    uint8_t  lars_lock_fac;  /* g_lars.lock_factor                        */
    int16_t  lars_tcoeff;    /* g_lars.temp_coeff                         */
    uint16_t lars_tref;      /* g_lars.temp_ref                           */
    uint8_t  lars_flags;     /* g_lars.flags                              */
    uint16_t fa_win_dpll;    /* g_freq_damp_win_dpll (FAD)                */
    uint16_t fa_win_lock;    /* g_freq_damp_win_lock (FAL)                */
    uint8_t  lt_local;       /* g_show_local_time (LT): 1 = local, 0 = UTC */
    /* LRN params (fallback only — normally owned by live_store/ring) */
    uint8_t  lrn_en;
    float    lrn_drift;      /* g_lrn_drift                                */
    float    lrn_damp;       /* g_lrn_damp                                 */
    /* enable flags */
    uint8_t  warmup_en;      /* g_warmup_enable                            */
    uint8_t  splash_en;      /* g_splash_enable                            */
    uint8_t  saw_en;         /* g_qerr_enable                              */
    /* timezone */
    uint8_t  tz_mode;        /* g_tz_mode (0=manual 1=auto-EU 2=posix)     */
    int16_t  tz_manual_min;  /* g_tz_manual_min                            */
    char     tz_str[48];     /* g_tz_str (POSIX rule, NUL-terminated)      */
} SettingsBlock_t;

/* Which group of fields a partial save should update. Used by `ES <object>`. */
typedef enum {
    SET_ALL    = 0,   /* full save from runtime (backwards-compatible ES) */
    SET_TZ     = 1,   /* tz_mode, tz_manual_min, tz_str                   */
    SET_PID    = 2,   /* pid_kp/ki/kd/il[3..9], blend, nn_max_step        */
    SET_LTIC   = 3,   /* all ltic_* fields                                */
    SET_FLAGS  = 4,   /* warmup_en, splash_en, saw_en, lrn_en, svin_en    */
    SET_ALGO   = 5,   /* pwm, algo                                        */
    SET_PO     = 6,    /* press_off, alt_off                              */
} settings_partial_t;

/* Boot-time recall. Reads the newest REC_SETTINGS slot from the ring,
 * validates version + per-field range guards, and applies the values to the
 * runtime globals (gCtrl, g_pid, g_ltic, g_tz_*, flags). Safe to call
 * before the scheduler starts (no mutex use). Returns true if a valid
 * stored block was found and applied; false if the ring had no settings
 * slot (compile-time defaults remain in effect). */
bool settings_recall(void);

/* Full save: snapshot every settings field from runtime into a fresh
 * SettingsBlock_t and write it as REC_SETTINGS. Returns true on success. */
bool settings_save(void);

/* Partial save: read the last stored block, update only the requested group
 * of fields from the live runtime, write it back. Fields outside the group
 * keep their stored values. If no prior block exists, the non-requested
 * fields are seeded from the current runtime (so the first partial save
 * behaves like a full save). Returns true on success. */
bool settings_save_partial(settings_partial_t which);

#endif /* SETTINGS_STORE_H */
