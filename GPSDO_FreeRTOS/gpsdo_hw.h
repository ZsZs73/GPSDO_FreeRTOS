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

/* ---- Boot-time hardware inventory -------------------------------------
 *
 * enabled      firmware was built to use the device
 * detectable   POST has a non-destructive presence test
 * present      boot-time discovery positively found the device
 * initialized  owning driver/subsystem subsequently started it
 *
 * Do not infer `present` from a compile-time switch. Hardware such as LTIC
 * is intentionally excluded until a real presence test exists.
 * ---------------------------------------------------------------------- */
typedef enum {
    GPSDO_HW_AHT = 0,
    GPSDO_HW_BMP280,
    GPSDO_HW_BME280,
    GPSDO_HW_INA219,
    GPSDO_HW_OCXO_TEMP,
    GPSDO_HW_OLED,
    GPSDO_HW_DEVICE_COUNT
} gpsdo_hw_device_t;

/* ---- OCXO temperature sensor role --------------------------------------
 *
 * Exactly one sensor may occupy the OCXO-temperature role. The enclosure
 * sensor is independent (BME280).
 * ---------------------------------------------------------------------- */
typedef enum {
    GPSDO_OCXO_TEMP_NONE = 0,
    GPSDO_OCXO_TEMP_PCT2075,
    GPSDO_OCXO_TEMP_TMP1075,
    GPSDO_OCXO_TEMP_SE95,
    GPSDO_OCXO_TEMP_TMP116,
    GPSDO_OCXO_TEMP_UNKNOWN
} gpsdo_ocxo_temp_type_t;

typedef struct {
    gpsdo_ocxo_temp_type_t type;
    uint8_t address;
    uint8_t resolution_bits;
    float resolution_c;
} gpsdo_ocxo_temp_info_t;

const gpsdo_ocxo_temp_info_t *gpsdo_hw_ocxo_temp_info(void);
const char *gpsdo_hw_ocxo_temp_name(gpsdo_ocxo_temp_type_t type);

typedef struct {
    bool    enabled;
    bool    detectable;
    bool    present;
    bool    initialized;
    uint8_t address;
} gpsdo_hw_device_state_t;

/* Call after Wire.begin() and before vTaskStartScheduler(). */
void gpsdo_hw_discover(void);

const gpsdo_hw_device_state_t *gpsdo_hw_device_state(gpsdo_hw_device_t device);
bool gpsdo_hw_is_present(gpsdo_hw_device_t device);
void gpsdo_hw_set_initialized(gpsdo_hw_device_t device, bool initialized);
