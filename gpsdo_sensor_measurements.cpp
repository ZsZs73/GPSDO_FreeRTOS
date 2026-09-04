#include "gpsdo_sensor_measurements.h"
#include "gpsdo_config.h"

extern float g_pressure_offset;

float g_encl_temp = 0.0f;
float g_encl_humi = 0.0f;
float g_encl_pres = 0.0f;

float g_ocxo_volt = 0.0f;
float g_ocxo_curr = 0.0f;

#ifdef GPSDO_BME280_I2C
#include <Adafruit_BME280.h>
static Adafruit_BME280 s_bme;
static bool s_bme_ok = false;
#endif

void gpsdo_sensor_measurements_begin()
{
#ifdef GPSDO_BME280_I2C
    s_bme_ok = s_bme.begin(0x76);
    OUT_SERIAL.println(s_bme_ok ? "HW: BME280 sensor         OK  (I2C 0x76)"
                                : "HW: BME280 sensor         not found");
#endif
}

void gpsdo_sensor_measurements_read()
{
#ifdef GPSDO_BME280_I2C
    if (s_bme_ok) {
        g_encl_temp = s_bme.readTemperature();
        g_encl_humi = s_bme.readHumidity();
        g_encl_pres = (s_bme.readPressure() + g_pressure_offset) / 100.0f;
    }
#endif
}
