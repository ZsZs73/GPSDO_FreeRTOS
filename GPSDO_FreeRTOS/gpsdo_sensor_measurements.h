#pragma once

extern float g_encl_temp;
extern float g_encl_humi;
extern float g_encl_pres;

extern float g_ocxo_temp;
extern float g_ocxo_volt;
extern float g_ocxo_curr;

void gpsdo_sensor_measurements_begin();
void gpsdo_sensor_measurements_read();

bool gpsdo_sensor_aht_available();
bool gpsdo_sensor_bmp280_available();
bool gpsdo_sensor_bme280_available();
bool gpsdo_sensor_ina219_available();
bool gpsdo_sensor_lm75_available();
