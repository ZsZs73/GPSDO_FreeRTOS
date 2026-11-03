#pragma once
#include <stdint.h>

/*
 * Startup configuration loading and operator-facing CONFIG diagnostics.
 *
 * Common settings and live state are restored at boot. Algorithm-private
 * configuration is loaded only when that algorithm is selected.
 */
void gpsdo_config_load(void);
void gpsdo_algo_config_load(uint8_t algo);
bool gpsdo_algo_config_save(uint8_t algo);
bool gpsdo_algo_save_active(uint8_t algo);
bool gpsdo_algo_selection_is_saved(uint8_t algo);
void gpsdo_config_log_active(void);
