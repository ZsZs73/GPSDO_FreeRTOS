/* Hardware calibration persistence: CT plant K + LC detector calibration. */
#include "gpsdo_hw_cal.h"
#include "gpsdo_flash_ring.h"
#include "gpsdo_algorithms.h"
#include <math.h>
#include <string.h>

#define HW_CAL_REC_VER   1u
#define HW_CAL_HAVE_K    0x01u
#define HW_CAL_HAVE_LTIC 0x02u

typedef struct {
    uint8_t ver, flags, reserved0[2];
    float k_hz_per_lsb;
    float ltic_ns_per_volt;
    float ltic_zero_offset;
    float ltic_range_ns;
    float ltic_centre_v;
    int8_t ltic_polarity;
    uint8_t reserved1[3];
} hw_cal_record_t;
static_assert(sizeof(hw_cal_record_t) == 28u, "hardware calibration record layout changed");

static double s_k = 0.0;
static bool s_have_k = false;
static bool s_have_ltic = false;

static bool k_ok(double k) {
    return isfinite(k) && k >= HW_CAL_K_MIN_HZ_PER_LSB && k <= HW_CAL_K_MAX_HZ_PER_LSB;
}
static bool ltic_ok(float nsv, float zero, float range, int8_t pol, float centre) {
    return isfinite(nsv) && nsv > 0.0f && nsv <= 1.0e6f &&
           isfinite(zero) && zero >= 0.0f && zero <= 3.3f &&
           isfinite(range) && range > 0.0f && range <= 1.0e9f &&
           (pol == -1 || pol == 1) &&
           isfinite(centre) && centre >= 0.0f && centre <= 3.3f;
}

void hw_cal_clear(void) { s_k = 0.0; s_have_k = false; s_have_ltic = false; }
bool hw_cal_k_valid(void) { return s_have_k && k_ok(s_k); }
bool hw_cal_set_k(double k) { if (!k_ok(k)) return false; s_k = k; s_have_k = true; return true; }
double hw_cal_k_hz_per_lsb(void) { return hw_cal_k_valid() ? s_k : 0.0; }
double hw_cal_lsb_per_hz(void) { return hw_cal_k_valid() ? 1.0 / s_k : 0.0; }

bool hw_cal_ltic_valid(void) {
    return s_have_ltic && ltic_ok(g_ltic.ns_per_volt, g_ltic.zero_offset,
                                  g_ltic.range_ns, g_ltic.polarity, g_ltic.centre_v);
}
bool hw_cal_set_ltic(float nsv, float zero, float range, int8_t pol, float centre) {
    if (!ltic_ok(nsv, zero, range, pol, centre)) return false;
    g_ltic.ns_per_volt = nsv; g_ltic.zero_offset = zero; g_ltic.range_ns = range;
    g_ltic.polarity = pol; g_ltic.centre_v = centre; s_have_ltic = true; return true;
}

bool hw_cal_save(void) {
    hw_cal_record_t r; memset(&r, 0, sizeof(r)); r.ver = HW_CAL_REC_VER;
    if (hw_cal_k_valid()) { r.flags |= HW_CAL_HAVE_K; r.k_hz_per_lsb = (float)s_k; }
    if (hw_cal_ltic_valid()) {
        r.flags |= HW_CAL_HAVE_LTIC;
        r.ltic_ns_per_volt = g_ltic.ns_per_volt; r.ltic_zero_offset = g_ltic.zero_offset;
        r.ltic_range_ns = g_ltic.range_ns; r.ltic_centre_v = g_ltic.centre_v;
        r.ltic_polarity = g_ltic.polarity;
    }
    if (!r.flags) return false;
    return flash_ring_write(REC_HW_CAL, (const uint8_t*)&r, (uint16_t)sizeof(r));
}

hw_cal_status_t hw_cal_load_status(uint8_t *stored_version) {
    if (stored_version) *stored_version = 0u;
    hw_cal_clear();
    hw_cal_record_t r; memset(&r, 0, sizeof(r)); uint16_t n = 0u;
    flash_ring_read_status_t rs = flash_ring_read_newest_ex(
        REC_HW_CAL, (uint8_t*)&r, (uint16_t)sizeof(r), &n);
    if (rs == FLASH_RING_READ_MISSING) return HW_CAL_MISSING;
    if (rs == FLASH_RING_READ_CHECKSUM) return HW_CAL_CHECKSUM;
    if (rs == FLASH_RING_READ_NOT_READY) return HW_CAL_STORAGE;
    if (rs == FLASH_RING_READ_LENGTH) return HW_CAL_SIZE;
    const bool recovered = (rs == FLASH_RING_READ_RECOVERED);
    if (rs != FLASH_RING_READ_OK && !recovered) return HW_CAL_STORAGE;
    if (n != sizeof(r)) return HW_CAL_SIZE;
    if (stored_version) *stored_version = r.ver;
    if (r.ver != HW_CAL_REC_VER) return HW_CAL_VERSION;
    if ((r.flags & ~(HW_CAL_HAVE_K|HW_CAL_HAVE_LTIC)) || !(r.flags & (HW_CAL_HAVE_K|HW_CAL_HAVE_LTIC)))
        return HW_CAL_INVALID;
    if ((r.flags & HW_CAL_HAVE_K) && !hw_cal_set_k((double)r.k_hz_per_lsb)) return HW_CAL_INVALID;
    if ((r.flags & HW_CAL_HAVE_LTIC) &&
        !hw_cal_set_ltic(r.ltic_ns_per_volt, r.ltic_zero_offset, r.ltic_range_ns,
                         r.ltic_polarity, r.ltic_centre_v)) return HW_CAL_INVALID;
    return recovered ? HW_CAL_RECOVERED : HW_CAL_OK;
}
