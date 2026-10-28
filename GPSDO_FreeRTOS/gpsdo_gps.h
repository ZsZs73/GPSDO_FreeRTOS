#ifndef GPSDO_GPS_H
#define GPSDO_GPS_H

#include <stdint.h>

typedef struct {
    bool     detected;          /* valid receiver traffic seen           */
    bool     nmea_seen;         /* at least one valid NMEA sentence       */
    bool     ubx_seen;          /* valid UBX response received            */
    uint32_t baud;              /* detected receiver baud; 0 if unknown   */

    char sw_version[31];        /* MON-VER swVersion                      */
    char hw_version[11];        /* MON-VER hwVersion                      */
    char model[31];             /* MON-VER MOD= value, if reported        */
    char fw_version[31];        /* MON-VER FWVER= value, if reported      */
    char prot_version[16];      /* MON-VER PROTVER= value, if reported    */
} gpsdo_gnss_info_t;

void gpsdo_gps_init(void);
const gpsdo_gnss_info_t *gpsdo_gps_info(void);

#endif