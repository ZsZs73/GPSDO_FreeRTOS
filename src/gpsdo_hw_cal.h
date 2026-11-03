#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Board-specific calibration shared by all algorithms. */
#define HW_CAL_K_MIN_HZ_PER_LSB  1.0e-5
#define HW_CAL_K_MAX_HZ_PER_LSB  2.0e-3

typedef enum {
    HW_CAL_OK = 0,
    HW_CAL_RECOVERED,
    HW_CAL_MISSING,
    HW_CAL_CHECKSUM,
    HW_CAL_VERSION,
    HW_CAL_SIZE,
    HW_CAL_STORAGE,
    HW_CAL_INVALID
} hw_cal_status_t;

void   hw_cal_clear(void);
bool   hw_cal_k_valid(void);
bool   hw_cal_set_k(double k_hz_per_lsb);
double hw_cal_k_hz_per_lsb(void);
double hw_cal_lsb_per_hz(void);

bool hw_cal_ltic_valid(void);
bool hw_cal_set_ltic(float ns_per_volt, float zero_offset, float range_ns,
                     int8_t polarity, float centre_v);

bool hw_cal_save(void);
hw_cal_status_t hw_cal_load_status(uint8_t *stored_version);
