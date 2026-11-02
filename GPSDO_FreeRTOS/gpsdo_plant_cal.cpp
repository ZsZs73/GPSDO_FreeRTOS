/*
 * gpsdo_plant_cal.cpp
 *
 * Persistence and runtime ownership for CT's measured OCXO plant slope.
 */

#include "gpsdo_plant_cal.h"
#include "gpsdo_flash_ring.h"

#include <math.h>
#include <string.h>

#define PLANT_CAL_REC_VER  1u

typedef struct {
    uint8_t ver;
    uint8_t reserved[3];
    float   k_hz_per_lsb;
} plant_cal_record_t;

static_assert(sizeof(plant_cal_record_t) == 8u,
              "plant calibration record layout changed");

static double s_k_hz_per_lsb = 0.0;

static bool k_valid(double k)
{
    return isfinite(k) &&
           k >= PLANT_K_MIN_HZ_PER_LSB &&
           k <= PLANT_K_MAX_HZ_PER_LSB;
}

void plant_cal_clear(void)
{
    s_k_hz_per_lsb = 0.0;
}

bool plant_cal_valid(void)
{
    return k_valid(s_k_hz_per_lsb);
}

bool plant_cal_set_k(double k_hz_per_lsb)
{
    if (!k_valid(k_hz_per_lsb))
        return false;

    s_k_hz_per_lsb = k_hz_per_lsb;
    return true;
}

double plant_cal_k_hz_per_lsb(void)
{
    return plant_cal_valid() ? s_k_hz_per_lsb : 0.0;
}

double plant_cal_lsb_per_hz(void)
{
    return plant_cal_valid() ? (1.0 / s_k_hz_per_lsb) : 0.0;
}

bool plant_cal_save(void)
{
    if (!plant_cal_valid())
        return false;

    plant_cal_record_t r;
    memset(&r, 0, sizeof(r));
    r.ver = PLANT_CAL_REC_VER;
    r.k_hz_per_lsb = (float)s_k_hz_per_lsb;

    return flash_ring_write(
        REC_PLANT,
        (const uint8_t *)&r,
        (uint16_t)sizeof(r)
    );
}

plant_cal_status_t plant_cal_load_status(uint8_t *stored_version)
{
    if (stored_version)
        *stored_version = 0u;

    plant_cal_clear();

    plant_cal_record_t r;
    memset(&r, 0, sizeof(r));

    uint16_t n = 0u;
    flash_ring_read_status_t rs =
        flash_ring_read_newest_ex(
            REC_PLANT,
            (uint8_t *)&r,
            (uint16_t)sizeof(r),
            &n
        );

    if (rs == FLASH_RING_READ_MISSING)   return PLANT_CAL_MISSING;
    if (rs == FLASH_RING_READ_CHECKSUM)  return PLANT_CAL_CHECKSUM;
    if (rs == FLASH_RING_READ_NOT_READY) return PLANT_CAL_STORAGE;
    if (rs == FLASH_RING_READ_LENGTH)    return PLANT_CAL_SIZE;

    const bool recovered = (rs == FLASH_RING_READ_RECOVERED);
    if (rs != FLASH_RING_READ_OK && !recovered)
        return PLANT_CAL_STORAGE;

    if (n != sizeof(r))
        return PLANT_CAL_SIZE;

    if (stored_version)
        *stored_version = r.ver;

    if (r.ver != PLANT_CAL_REC_VER)
        return PLANT_CAL_VERSION;

    if (!plant_cal_set_k((double)r.k_hz_per_lsb))
        return PLANT_CAL_INVALID_K;

    return recovered ? PLANT_CAL_RECOVERED : PLANT_CAL_OK;
}
