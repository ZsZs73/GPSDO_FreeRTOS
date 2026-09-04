#pragma once

extern float g_encl_temp;
extern float g_encl_humi;
extern float g_encl_pres;

extern float g_ocxo_volt;
extern float g_ocxo_curr;

void gpsdo_sensor_measurements_begin();
void gpsdo_sensor_measurements_read();

bool gpsdo_sensor_bmp280_available();
