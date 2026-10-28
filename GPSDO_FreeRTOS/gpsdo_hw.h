#pragma once

#include <stdint.h>

typedef enum {
    GPSDO_HW_UNKNOWN = 0,
    GPSDO_HW_PRESENT,
    GPSDO_HW_ABSENT,
    GPSDO_HW_FAILED
} gpsdo_hw_status_t;

typedef enum {
    GPSDO_MCU_CLOCK_UNKNOWN = 0,
    GPSDO_MCU_CLOCK_CRYSTAL,
    GPSDO_MCU_CLOCK_EXTERNAL
} gpsdo_mcu_clock_input_t;

typedef struct {
    gpsdo_hw_status_t     mcu_clock;
    gpsdo_mcu_clock_input_t mcu_clock_input;
} gpsdo_hw_info_t;

void gpsdo_hw_log_reset_cause(void);
void gpsdo_hw_post_begin(void);

const gpsdo_hw_info_t *gpsdo_hw_info(void);
