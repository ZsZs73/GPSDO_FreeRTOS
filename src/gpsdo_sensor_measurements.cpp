#include "gpsdo_sensor_measurements.h"
#include "gpsdo_log.h"
#include "gpsdo_hw.h"
#include "gpsdo_config.h"

extern float g_pressure_offset;

float g_encl_temp = 0.0f;
float g_encl_humi = 0.0f;
float g_encl_pres = 0.0f;

float g_ocxo_temp = 0.0f;
float g_ocxo_volt = 0.0f;
float g_ocxo_curr = 0.0f;

#ifdef GPSDO_AHT10
#include <Adafruit_AHTX0.h>
static Adafruit_AHTX0 s_aht;
static bool s_aht_ok = false;
#endif

#ifdef GPSDO_BMP280_I2C
#include <Adafruit_BMP280.h>
static Adafruit_BMP280 s_bmp;
static bool s_bmp_ok = false;
#endif

#ifdef GPSDO_BME280_I2C
#include <Adafruit_BME280.h>
static Adafruit_BME280 s_bme;
static bool s_bme_ok = false;
#endif

#ifdef GPSDO_INA219
#include <Adafruit_INA219.h>
static Adafruit_INA219 s_ina;
static bool s_ina_ok = false;
#endif

#ifdef GPSDO_OCXO_TEMP
#include <Wire.h>
static bool s_ocxo_temp_ok = false;
#endif

#ifdef GPSDO_OCXO_TEMP
static bool ocxo_temp_read_temperature(float *out_temp)
{
    if (!out_temp)
        return false;

    const gpsdo_ocxo_temp_info_t *info = gpsdo_hw_ocxo_temp_info();
    if (!info ||
        info->type == GPSDO_OCXO_TEMP_NONE ||
        info->type == GPSDO_OCXO_TEMP_UNKNOWN)
        return false;

    Wire.beginTransmission(info->address);
    Wire.write((uint8_t)0x00);   /* temperature register */
    if (Wire.endTransmission(false) != 0)
        return false;

    if (Wire.requestFrom(info->address, (uint8_t)2) != 2)
        return false;

    const uint16_t u =
        ((uint16_t)Wire.read() << 8) | (uint16_t)Wire.read();
    const int16_t raw = (int16_t)u;

    switch (info->type) {
    case GPSDO_OCXO_TEMP_PCT2075:
    case GPSDO_OCXO_TEMP_TMP1075:
    case GPSDO_OCXO_TEMP_SE95:
        /*
         * These three LM75-compatible parts store their signed result
         * left-aligned in the 16-bit register. Reading both bytes and dividing
         * the signed word by 256 preserves the native 11/12/13-bit steps.
         */
        *out_temp = (float)raw / 256.0f;
        return true;

    case GPSDO_OCXO_TEMP_TMP116:
        /* Full-width signed 16-bit result, 1 LSB = 1/128 C. */
        *out_temp = (float)raw / 128.0f;
        return true;

    default:
        return false;
    }
}
#endif

void gpsdo_sensor_measurements_begin()
{
#ifdef GPSDO_AHT10
    s_aht_ok = false;
    if (gpsdo_hw_is_present(GPSDO_HW_AHT)) {
        s_aht_ok = s_aht.begin();
        gpsdo_hw_set_initialized(GPSDO_HW_AHT, s_aht_ok);
        if (!s_aht_ok) {
            gpsdo_log_event(
                GPSDO_LOG_WARN, "SENSOR",
                "AHT10/AHT20 was detected but initialization failed; enclosure temperature and humidity unavailable.");
        }
    }
#endif

#ifdef GPSDO_BMP280_I2C
    s_bmp_ok = false;
    if (gpsdo_hw_is_present(GPSDO_HW_BMP280)) {
        s_bmp_ok = s_bmp.begin(0x77, 0x58);
        if (s_bmp_ok) {
            s_bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                              Adafruit_BMP280::SAMPLING_X2,
                              Adafruit_BMP280::SAMPLING_X16,
                              Adafruit_BMP280::FILTER_X16,
                              Adafruit_BMP280::STANDBY_MS_500);
        }
        gpsdo_hw_set_initialized(GPSDO_HW_BMP280, s_bmp_ok);
        if (!s_bmp_ok) {
            gpsdo_log_event(
                GPSDO_LOG_WARN, "SENSOR",
                "BMP280 was detected but initialization failed; enclosure temperature and pressure unavailable.");
        }
    }
#endif

#ifdef GPSDO_BME280_I2C
    s_bme_ok = false;
    if (gpsdo_hw_is_present(GPSDO_HW_BME280)) {
        s_bme_ok = s_bme.begin(0x76);
        gpsdo_hw_set_initialized(GPSDO_HW_BME280, s_bme_ok);
        if (!s_bme_ok) {
            gpsdo_log_event(
                GPSDO_LOG_WARN, "SENSOR",
                "BME280 was detected but initialization failed; enclosure measurements unavailable.");
        }
    }
#endif

#ifdef GPSDO_INA219
    s_ina_ok = false;
    if (gpsdo_hw_is_present(GPSDO_HW_INA219)) {
        s_ina_ok = s_ina.begin();
        if (s_ina_ok)
            s_ina.setCalibration_32V_1A();

        gpsdo_hw_set_initialized(GPSDO_HW_INA219, s_ina_ok);
        if (!s_ina_ok) {
            gpsdo_log_event(
                GPSDO_LOG_WARN, "SENSOR",
                "INA219 was detected but initialization failed; OCXO voltage and current measurements unavailable.");
        }
    }
#endif

#ifdef GPSDO_OCXO_TEMP
    /* No separate driver object: POST identified the concrete sensor type. */
    {
        const gpsdo_ocxo_temp_info_t *info = gpsdo_hw_ocxo_temp_info();
        s_ocxo_temp_ok =
            gpsdo_hw_is_present(GPSDO_HW_OCXO_TEMP) &&
            info &&
            info->type != GPSDO_OCXO_TEMP_NONE &&
            info->type != GPSDO_OCXO_TEMP_UNKNOWN;

        gpsdo_hw_set_initialized(GPSDO_HW_OCXO_TEMP, s_ocxo_temp_ok);
    }
#endif
}

void gpsdo_sensor_measurements_read()
{
#ifdef GPSDO_AHT10
    if (s_aht_ok) {
        sensors_event_t hum, tmp;
        s_aht.getEvent(&hum, &tmp);
        g_encl_temp = tmp.temperature;
        g_encl_humi = hum.relative_humidity;
    }
#endif

#ifdef GPSDO_BMP280_I2C
    if (s_bmp_ok) {
#ifndef GPSDO_AHT10
        g_encl_temp = s_bmp.readTemperature();
#else
        s_bmp.readTemperature();
#endif
        float raw_pres = s_bmp.readPressure();
        g_encl_pres = (raw_pres + g_pressure_offset) / 100.0f;
    }
#endif

#ifdef GPSDO_BME280_I2C
    if (s_bme_ok) {
        g_encl_temp = s_bme.readTemperature();
        g_encl_humi = s_bme.readHumidity();
        g_encl_pres = (s_bme.readPressure() + g_pressure_offset) / 100.0f;
    }
#endif

#ifdef GPSDO_INA219
    if (s_ina_ok) {
        g_ocxo_volt = s_ina.getBusVoltage_V();
        g_ocxo_curr = s_ina.getCurrent_mA();
    }
#endif

#ifdef GPSDO_OCXO_TEMP
    if (s_ocxo_temp_ok) {
        float temp;
        if (ocxo_temp_read_temperature(&temp)) {
            g_ocxo_temp = temp;
        }
    }
#endif
}

bool gpsdo_sensor_aht_available()
{
#ifdef GPSDO_AHT10
    return s_aht_ok;
#else
    return false;
#endif
}

bool gpsdo_sensor_bmp280_available()
{
#ifdef GPSDO_BMP280_I2C
    return s_bmp_ok;
#else
    return false;
#endif
}

bool gpsdo_sensor_bme280_available()
{
#ifdef GPSDO_BME280_I2C
    return s_bme_ok;
#else
    return false;
#endif
}

bool gpsdo_sensor_ina219_available()
{
#ifdef GPSDO_INA219
    return s_ina_ok;
#else
    return false;
#endif
}

bool gpsdo_sensor_ocxo_temp_available()
{
#ifdef GPSDO_OCXO_TEMP
    return s_ocxo_temp_ok;
#else
    return false;
#endif
}
