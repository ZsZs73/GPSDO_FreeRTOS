/*
 * gpsdo_config_load.cpp
 *
 * Common boot configuration + concise, human-readable diagnostics.
 * Algorithm-private configuration is deliberately loaded on demand.
 */

#include "gpsdo_config_load.h"

#include "gpsdo_config.h"
#include "gpsdo_state.h"
#include "gpsdo_flash_ring.h"
#include "gpsdo_live_store.h"
#include "gpsdo_settings_store.h"
#include "gpsdo_algorithms.h"
#include "gpsdo_log.h"

#include <stdint.h>

/* Defined in gpsdo_cli.cpp. */
extern int16_t g_time_offset_min;


static bool    s_saved_algo_valid = false;
static uint8_t s_saved_algo       = 0u;
static void log_settings_status(settings_load_status_t st, uint8_t version)
{
    switch (st) {
    case SETTINGS_LOAD_OK:
        gpsdo_log_event(GPSDO_LOG_INFO, "CONFIG", "Settings loaded from flash.");
        break;

    case SETTINGS_LOAD_RECOVERED:
        gpsdo_log_event(GPSDO_LOG_WARN, "CONFIG",
            "Latest saved settings in flash are corrupt; recovered previous valid settings.");
        break;

    case SETTINGS_LOAD_MISSING:
        gpsdo_log_event(
            GPSDO_LOG_INFO, "CONFIG",
            "No saved settings found; using built-in defaults.");
        break;

    case SETTINGS_LOAD_CHECKSUM:
        gpsdo_log_event(
            GPSDO_LOG_WARN, "CONFIG",
            "Saved settings in flash are corrupt; using built-in defaults.");
        break;

    case SETTINGS_LOAD_VERSION:
        gpsdo_logf(
            GPSDO_LOG_WARN, "CONFIG",
            "Saved settings version %u is incompatible with this firmware; "
            "using built-in defaults.",
            (unsigned)version);
        break;

    case SETTINGS_LOAD_SIZE:
        gpsdo_log_event(
            GPSDO_LOG_WARN, "CONFIG",
            "Saved settings record has an invalid size; using built-in defaults.");
        break;

    case SETTINGS_LOAD_STORAGE:
    default:
        gpsdo_log_event(
            GPSDO_LOG_WARN, "CONFIG",
            "Settings could not be loaded because flash storage is not ready; "
            "using built-in defaults.");
        break;
    }
}

static void log_live_status(live_load_status_t st)
{
    switch (st) {
    case LIVE_LOAD_OK:
        gpsdo_log_event(GPSDO_LOG_INFO, "CONFIG", "Live state loaded from flash.");
        break;

    case LIVE_LOAD_RECOVERED:
        gpsdo_log_event(GPSDO_LOG_WARN, "CONFIG",
            "Latest saved live state in flash is corrupt; recovered previous valid live state.");
        break;

    case LIVE_LOAD_MISSING:
        /* Normal on a new board. Nothing useful to say. */
        break;

    case LIVE_LOAD_CHECKSUM:
        gpsdo_log_event(
            GPSDO_LOG_WARN, "CONFIG",
            "Saved live state in flash is corrupt; starting with default live state.");
        break;

    case LIVE_LOAD_SIZE:
        gpsdo_log_event(
            GPSDO_LOG_WARN, "CONFIG",
            "Saved live-state record has an invalid size; "
            "starting with default live state.");
        break;

    case LIVE_LOAD_STORAGE:
    default:
        gpsdo_log_event(
            GPSDO_LOG_WARN, "CONFIG",
            "Live state could not be loaded because flash storage is not ready; "
            "starting with default live state.");
        break;
    }
}

void gpsdo_algo_config_load(uint8_t algo)
{
#ifdef GPSDO_LTIC
    if (algo != 13u) return;

    uint8_t version = 0u;
    kf_config_status_t st = kf_store_load_status(&version);

    switch (st) {
    case KF_CONFIG_OK:
        gpsdo_log_event(
            GPSDO_LOG_INFO, "CONFIG",
            "ALG13 configuration loaded from flash.");
        break;

    case KF_CONFIG_RECOVERED:
        gpsdo_log_event(
            GPSDO_LOG_WARN, "CONFIG",
            "Latest saved ALG13 configuration in flash is corrupt; recovered previous valid configuration.");
        break;

    case KF_CONFIG_MISSING:
        gpsdo_log_event(
            GPSDO_LOG_INFO, "CONFIG",
            "No saved ALG13 configuration found; using built-in defaults.");
        break;

    case KF_CONFIG_CHECKSUM:
        gpsdo_log_event(
            GPSDO_LOG_WARN, "CONFIG",
            "Saved ALG13 configuration in flash is corrupt; "
            "using built-in defaults.");
        break;

    case KF_CONFIG_VERSION:
        gpsdo_logf(
            GPSDO_LOG_WARN, "CONFIG",
            "Saved ALG13 configuration version %u is incompatible; "
            "using built-in defaults.",
            (unsigned)version);
        break;

    case KF_CONFIG_SIZE:
        gpsdo_log_event(
            GPSDO_LOG_WARN, "CONFIG",
            "Saved ALG13 configuration in flash is corrupt; "
            "using built-in defaults.");
        break;

    case KF_CONFIG_INVALID_KR:
        gpsdo_log_event(
            GPSDO_LOG_WARN, "CONFIG",
            "ALG13 measurement-noise setting (KR) is invalid; "
            "using built-in defaults.");
        break;

    case KF_CONFIG_INVALID_KQ:
        gpsdo_log_event(
            GPSDO_LOG_WARN, "CONFIG",
            "ALG13 process-noise setting (KQ) is invalid; "
            "using built-in defaults.");
        break;

    case KF_CONFIG_INVALID_KT:
        gpsdo_log_event(
            GPSDO_LOG_WARN, "CONFIG",
            "ALG13 estimator horizon (KT) is invalid; "
            "using built-in defaults.");
        break;

    case KF_CONFIG_INVALID_KC:
        gpsdo_log_event(
            GPSDO_LOG_WARN, "CONFIG",
            "ALG13 controller horizon (KC) is invalid; "
            "using built-in defaults.");
        break;

    case KF_CONFIG_STORAGE:
    default:
        gpsdo_log_event(
            GPSDO_LOG_WARN, "CONFIG",
            "ALG13 configuration could not be read from flash; "
            "using built-in defaults.");
        break;
    }
#else
    (void)algo;
#endif
}


bool gpsdo_algo_config_save(uint8_t algo)
{
    /*
     * Private algorithm persistence is dispatched here, symmetrically with
     * gpsdo_algo_config_load(). Algorithms whose tuning already lives in the
     * common settings record, or which own no private configuration, need no
     * action here.
     *
     * Add future private-record algorithms as new switch cases.
     */
    switch (algo) {
#ifdef GPSDO_LTIC
    case 13u:
        return kf_store_save();
#endif
    default:
        return true;
    }
}

bool gpsdo_algo_save_active(uint8_t algo)
{
    if (!settings_save_partial(SET_ALGO))
        return false;

    /*
     * REC_SETTINGS now contains the selected algorithm, regardless of whether
     * a later private-record write succeeds.
     */
    s_saved_algo = algo;
    s_saved_algo_valid = true;

    return gpsdo_algo_config_save(algo);
}

bool gpsdo_algo_selection_is_saved(uint8_t algo)
{
    return s_saved_algo_valid && s_saved_algo == algo;
}

void gpsdo_config_load(void)
{
    /* Common compile-time defaults. Valid saved state overwrites these. */
    gCtrl.pwm_output  = DEFAULT_PWM_OUTPUT;
    gCtrl.active_algo = 0;
    g_time_offset_min = 0;

    (void)flash_ring_begin();

    uint8_t settings_version = 0u;
    settings_load_status_t settings =
        settings_recall_status(&settings_version);

    g_persist_valid =
        (settings == SETTINGS_LOAD_OK ||
         settings == SETTINGS_LOAD_RECOVERED);
    s_saved_algo_valid = (settings == SETTINGS_LOAD_OK ||
         settings == SETTINGS_LOAD_RECOVERED);
    if (s_saved_algo_valid)
        s_saved_algo = gCtrl.active_algo;
    log_settings_status(settings, settings_version);

    live_load_status_t live = live_store_begin_status();
    log_live_status(live);

    /* The settings record has now established the selected algorithm. */
    gpsdo_algo_config_load(gCtrl.active_algo);
}

void gpsdo_config_log_active(void)
{
    /* Called after SPAN remapping, so PWM is the value actually applied. */
    gpsdo_logf(
        GPSDO_LOG_INFO,
        "CONFIG",
        "Active: PWM %u, algorithm %u, time offset %d min.",
        (unsigned)gCtrl.pwm_output,
        (unsigned)gCtrl.active_algo,
        (int)g_time_offset_min
    );
}
