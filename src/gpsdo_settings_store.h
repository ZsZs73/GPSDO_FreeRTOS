/* ======================================================================
 * gpsdo_settings_store.h  —  persistent user settings via the flash ring
 *
 * Part of GPSDO FreeRTOS v1.07
 *
 * Replaces the old STM32duino emulated EEPROM (gpsdo_state.cpp eeprom_*).
 * Settings are stored as a REC_SETTINGS slot in the flash ring (sector 7),
 * alongside the live data (REC_LIVE). This gives them the same atomicity
 * guarantees the ring already provides for live data: per-slot CRC16,
 * sequence-numbered newest-wins, read-back verify, power-loss safety.
 *
 * Layout version (SETTINGS_VER) is the first byte of the payload. A mismatch
 * on recall rejects the stored block, leaving the runtime defaults in force.
 * The next save writes a block using the current layout.
 *
 * Selective save (ES <object>): rather than overwrite the entire settings
 * block when the user changes one field, settings_save_partial() reads the
 * last saved block from the ring, updates only the requested group of fields
 * from the live runtime state, and writes a fresh slot. Untouched fields
 * keep their stored values — a deliberate `ES TZ` cannot clobber a hand-tuned
 * PID set the way the old full-state EEPROM write could.
 * ====================================================================== */
#ifndef GPSDO_SETTINGS_STORE_H
#define GPSDO_SETTINGS_STORE_H

#include <stdint.h>
#include <stdbool.h>

/* Fork development settings format.
 * Keep this value stable during development; payload size mismatch rejects
 * stale records after layout changes. Bump when release compatibility matters. */
#define SETTINGS_VER  100u

/* The flat on-flash block. Total size must stay <= FR_PAYLOAD (504 B).
 * Stored using the native STM32 struct layout; SETTINGS_VER and payload size
 * must both match exactly on recall. Individual values are range-checked. */
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
    /* LTIC loop/tuning params (hardware calibration lives in REC_HW_CAL) */
    float    ltic_dpll_kp, ltic_dpll_ki, ltic_dpll_kd, ltic_dpll_il;
    float    ltic_lock_kp, ltic_lock_ki, ltic_lock_kd, ltic_lock_il;
    float    ltic_acq_th;    /* g_ltic.acq_threshold_ns                    */
    float    ltic_dpll_th;   /* g_ltic.dpll_lock_thresh                    */
    uint16_t ltic_lock_iv;   /* g_ltic.lock_interval_s                     */
    uint8_t  ltic_state;     /* g_ltic.state                               */
    uint8_t  ltic_submode;   /* g_ltic.submode                             */
    float    ltic_acq_kp, ltic_acq_ki, ltic_acq_kd, ltic_acq_il;
    /* LTIC-Lars params (algo 11) */
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
    /* LRN enable is user configuration; learned values are owned by REC_LIVE. */
    uint8_t  lrn_en;
    /* enable flags */
    uint8_t  warmup_en;      /* g_warmup_enable                            */
    uint8_t  splash_en;      /* g_splash_enable                            */
    uint8_t  saw_en;         /* g_qerr_enable                              */
    /* timezone */
    uint8_t  tz_mode;        /* g_tz_mode (0=manual 1=auto-EU 2=posix)     */
    int16_t  tz_manual_min;  /* g_tz_manual_min                            */
    char     tz_str[48];     /* g_tz_str (POSIX rule, NUL-terminated)      */
    /* Control-voltage output path selected by the DAC command.
     * Zero means unset so recall can apply the default path. */
    uint8_t  dac_path;       /* 0=unset 1=PWM 2=DITH (g_dac_path+1) */
    /* TFT backlight percentage (BL command). */
    uint8_t  bl_pct;         /* g_tft_bl_pct, 30..100; 0 = unset            */
    /* Algorithm 12 (multi-level accumulator).
     *
     * These are here rather than compiled in because the limits are the one
     * thing that has to change per oscillator: only the 128 s value was ever
     * derived from a specification, and the rest were arbitrary even in the
     * design this comes from. A user tuning against their own OCXO must be able
     * to keep the result. */
    float    a12_gain;       /* g_mlacc_gain, LSB per ns (0 = auto from CT) */
    uint8_t  a12_run_level;  /* g_mlacc_run_level                          */
    uint8_t  a12_thr_src;    /* g_mlacc_thr_src   (MF)                     */
    uint16_t a12_thr_tgt_s;  /* g_mlacc_thr_tgt_s (MFT), 0 = default       */
    int32_t  a12_lim[11];    /* g_mlacc_lim[], phase limits in ns          */
    /* ADC divider scale used for Vctl measurement. */
    uint16_t adc_vdiv_h;     /* g_adc_vdiv ×100 (AV), 0 = 1.00×             */
} SettingsBlock_t;

/* Which group of fields a partial save should update. Used by `ES <object>`. */
typedef enum {
    SET_ALL    = 0,   /* full save from runtime (plain ES)                */
    SET_TZ     = 1,   /* tz_mode, tz_manual_min, tz_str                   */
    SET_PID    = 2,   /* pid_kp/ki/kd/il[3..9], blend, nn_max_step        */
    SET_LTIC   = 3,   /* all ltic_* fields                                */
    SET_FLAGS  = 4,   /* warmup_en, splash_en, saw_en, lrn_en, svin_en,   */
                      /* bl_pct — the display group                        */
    SET_ALGO   = 5,   /* pwm, algo, dac_path, AV scale                    */
    SET_ALGO12 = 7,   /* algo-12 gain, run level, per-level limits         */
    SET_PO     = 6,    /* press_off, alt_off                              */
} settings_partial_t;

typedef enum {
    SETTINGS_LOAD_OK = 0,
    SETTINGS_LOAD_RECOVERED,
    SETTINGS_LOAD_MISSING,
    SETTINGS_LOAD_CHECKSUM,
    SETTINGS_LOAD_VERSION,
    SETTINGS_LOAD_SIZE,
    SETTINGS_LOAD_STORAGE
} settings_load_status_t;

/* Detailed boot-time recall. stored_version receives the record's version byte
 * when one could be read. */
settings_load_status_t settings_recall_status(uint8_t *stored_version);

/* Boolean convenience wrapper. */
bool settings_recall(void);

/* Full save: snapshot every settings field from runtime into a fresh
 * SettingsBlock_t and write it as REC_SETTINGS. Returns true on success. */
bool settings_save(void);

/* Partial save: read the last stored block, update only the requested group
 * of fields from the live runtime, write it back. Fields outside the group
 * keep their stored values. If no valid current-version block exists, the
 * non-requested fields are seeded from the current runtime, so the first
 * partial save behaves like a full save. Returns true on success. */
bool settings_save_partial(settings_partial_t which);

#endif /* GPSDO_SETTINGS_STORE_H */
