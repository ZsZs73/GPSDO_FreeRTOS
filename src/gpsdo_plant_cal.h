#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Shared OCXO plant calibration.
 *
 * CT measures K = oscillator frequency change per 16-bit control LSB.
 * This is a hardware/plant property, not an algorithm coefficient.
 */
#define PLANT_K_MIN_HZ_PER_LSB  1.0e-5
#define PLANT_K_MAX_HZ_PER_LSB  2.0e-3

typedef enum {
    PLANT_CAL_OK = 0,
    PLANT_CAL_RECOVERED,
    PLANT_CAL_MISSING,
    PLANT_CAL_CHECKSUM,
    PLANT_CAL_VERSION,
    PLANT_CAL_SIZE,
    PLANT_CAL_STORAGE,
    PLANT_CAL_INVALID_K
} plant_cal_status_t;

void   plant_cal_clear(void);
bool   plant_cal_valid(void);
bool   plant_cal_set_k(double k_hz_per_lsb);
double plant_cal_k_hz_per_lsb(void);
double plant_cal_lsb_per_hz(void);

bool plant_cal_save(void);
plant_cal_status_t plant_cal_load_status(uint8_t *stored_version);
