#include "gpsdo_sensor_measurements.h"
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

#ifdef GPSDO_LM75
#include <Wire.h>
static uint8_t s_lm75_addr = GPSDO_LM75_ADDRESS;
static bool s_lm75_ok = false;
#endif

#ifdef GPSDO_LM75
static bool lm75_read_temperature(float *out_temp)
{
    Wire.beginTransmission(s_lm75_addr);
    Wire.write((uint8_t)0x00);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }

    if (Wire.requestFrom(s_lm75_addr, (uint8_t)2) != 2) {
        return false;
    }

    int16_t raw = ((int16_t)Wire.read() << 8) | Wire.read();
    *out_temp = raw / 256.0f;
    return true;
}
#endif

void gpsdo_sensor_measurements_begin()
{
#ifdef GPSDO_AHT10
    s_aht_ok = s_aht.begin();
    OUT_SERIAL.println(s_aht_ok ? "HW: AHT10/AHT20 sensor    OK  (I2C 0x38)"
                                : "HW: AHT10/AHT20 sensor    not found");
#endif

#ifdef GPSDO_BMP280_I2C
    s_bmp_ok = s_bmp.begin(0x77, 0x58);
    if (s_bmp_ok) {
        s_bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                          Adafruit_BMP280::SAMPLING_X2,
                          Adafruit_BMP280::SAMPLING_X16,
                          Adafruit_BMP280::FILTER_X16,
                          Adafruit_BMP280::STANDBY_MS_500);
        OUT_SERIAL.println("HW: BMP280 sensor         OK  (I2C 0x77)");
    } else {
        OUT_SERIAL.println("HW: BMP280 sensor         not found");
    }
#endif

#ifdef GPSDO_BME280_I2C
    s_bme_ok = s_bme.begin(0x76);
    OUT_SERIAL.println(s_bme_ok ? "HW: BME280 sensor         OK  (I2C 0x76)"
                                : "HW: BME280 sensor         not found");
#endif

#ifdef GPSDO_INA219
    s_ina_ok = s_ina.begin();
    if (s_ina_ok) {
        s_ina.setCalibration_32V_1A();
        OUT_SERIAL.println("HW: INA219 sensor         OK  (I2C 0x40)");
    } else {
        OUT_SERIAL.println("HW: INA219 sensor         not found");
    }
#endif

#ifdef GPSDO_LM75
    Wire.beginTransmission(s_lm75_addr);
    s_lm75_ok = (Wire.endTransmission() == 0);

    if (s_lm75_ok) {
        OUT_SERIAL.print("HW: LM75-family sensor    OK  (I2C 0x");
        OUT_SERIAL.print(s_lm75_addr, HEX);
        OUT_SERIAL.println(")");
    } else {
        OUT_SERIAL.println("HW: LM75-family sensor    not found");
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

#ifdef GPSDO_LM75
    if (s_lm75_ok) {
        float temp;
        if (lm75_read_temperature(&temp)) {
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

bool gpsdo_sensor_lm75_available()
{
#ifdef GPSDO_LM75
    return s_lm75_ok;
#else
    return false;
#endif
}
