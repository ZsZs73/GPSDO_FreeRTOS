/* ======================================================================
 * gpsdo_flash_ring.cpp  —  STM32F411 HAL layer for the live-data + settings ring
 *
 * Part of GPSDO FreeRTOS v1.07
 *
 * This is the ONLY part of the ring buffer that touches real flash. It
 * implements the three primitives the hardware-independent core needs
 * (read / program / erase) against sector 7 of the STM32F411CE, then
 * exposes the small public API declared in gpsdo_flash_ring.h.
 *
 * SAFETY
 * ------
 *  - Always enabled (no runtime toggle). Settings (ES) and live data both
 *    share this ring, distinguished by record type (REC_LIVE / REC_SETTINGS).
 *  - Sector 7 @ 0x08060000 (128 KB). Firmware ends in sector 5 (~0x0803A918);
 *    sectors 0-5 are reserved for code, sector 6 is a growth buffer.
 *    DO NOT move RING_SECTOR without re-checking the map.
 *  - Every write is verified by read-back in the core (fr_write), and every
 *    slot carries a CRC, so a mis-programmed or power-cut write is detected.
 *  - Erase/program run with interrupts kept enabled but are short; the
 *    control loop tolerates the occasional stall because saves are rare
 *    (hysteresis in live_store; ES is user-initiated).
 * ====================================================================== */

#include "gpsdo_flash_ring.h"
#include "gpsdo_flash_ring_core.h"
#include "gpsdo_log.h"

#include "stm32f4xx_hal.h"
#include <string.h>

/* ---- flash geometry (STM32F411CE) ---- */
#define RING_SECTOR       FLASH_SECTOR_7
#define RING_BASE_ADDR    0x08060000UL
#define RING_SECTOR_LEN   0x00020000UL      /* 128 KB */
#define RING_VOLTAGE      FLASH_VOLTAGE_RANGE_3   /* 2.7-3.6 V → word program */

static fr_state_t s_state;
static bool       s_ready = false;

/* A full-ring compaction must survive the sector erase with every persistence
 * domain already held in RAM. Six maximum-sized records cost ~3 KiB. */
typedef struct {
    uint8_t  present;
    uint16_t len;
    uint8_t  data[FR_PAYLOAD];
} ring_cache_t;

static ring_cache_t s_compact[6];

static const uint8_t s_compact_types[6] = {
    REC_LIVE,
    REC_SETTINGS,
    REC_AL13,
    REC_HW_CAL,
    REC_AL11,
    REC_AL12
};

/* ---- primitives handed to the core ---- */

static void hw_read(uint32_t off, uint8_t *dst, uint32_t n)
{
    /* flash is memory-mapped; a plain copy is the fastest correct read */
    memcpy(dst, (const void *)(RING_BASE_ADDR + off), n);
}

static int hw_program(uint32_t off, const uint8_t *src, uint32_t n)
{
    /* core always calls 32-byte aligned; program word-by-word (4 bytes).
     * Requires the destination to already be 0xFF where we set bits, which
     * the ring guarantees (fresh slots are erased). */
    if ((off & 3u) || (n & 3u)) return -1;      /* must be word aligned */

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGSERR);

    int rc = 0;
    for (uint32_t i = 0; i < n; i += 4) {
        uint32_t w;
        memcpy(&w, src + i, 4);                 /* respect alignment of src */
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                              RING_BASE_ADDR + off + i, w) != HAL_OK) {
            rc = -1;
            break;
        }
    }
    HAL_FLASH_Lock();
    return rc;
}

static int hw_erase(void)
{
    FLASH_EraseInitTypeDef ei;
    ei.TypeErase    = FLASH_TYPEERASE_SECTORS;
    ei.Sector       = RING_SECTOR;
    ei.NbSectors    = 1;
    ei.VoltageRange = RING_VOLTAGE;
    ei.Banks        = 0;   /* unused for F411 */

    uint32_t err = 0;
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGSERR);
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&ei, &err);
    HAL_FLASH_Lock();
    return (st == HAL_OK && err == 0xFFFFFFFFU) ? 0 : -1;
}

static const fr_ops_t OPS = {
    hw_read, hw_program, hw_erase, RING_SECTOR_LEN
};

/* ---- public API ---- */

bool flash_ring_begin(void)
{
    int r = fr_begin(&s_state, &OPS);
    s_ready = true;
    return (r == 1);
}

uint32_t flash_ring_base_addr(void) { return RING_BASE_ADDR; }
uint8_t  flash_ring_sector_no(void) { return (uint8_t)RING_SECTOR; }

bool flash_ring_wipe(void)
{
    /* Cold-restart erase: physically erase the ring sector, then re-init so a
     * fresh header is laid down. After this the ring reads as empty — the next
     * boot (or the next settings_recall) finds no valid slot and falls back to
     * compile-time defaults. Used by the CR command. Returns true on success. */
    if (hw_erase() != 0) return false;
    int r = fr_begin(&s_state, &OPS);   /* reformats the just-erased sector */
    s_ready = true;
    return (r == 1);
}

flash_ring_read_status_t flash_ring_read_newest_ex(
    uint8_t type, uint8_t *out, uint16_t maxlen, uint16_t *stored_len)
{
    if (stored_len) *stored_len = 0u;
    if (!s_ready) return FLASH_RING_READ_NOT_READY;

    fr_read_status_t st =
        fr_read_newest_ex(&s_state, type, out, maxlen, stored_len);

    switch (st) {
    case FR_READ_OK:        return FLASH_RING_READ_OK;
    case FR_READ_RECOVERED: return FLASH_RING_READ_RECOVERED;
    case FR_READ_MISSING:  return FLASH_RING_READ_MISSING;
    case FR_READ_CHECKSUM: return FLASH_RING_READ_CHECKSUM;
    case FR_READ_LENGTH:   return FLASH_RING_READ_LENGTH;
    default:               return FLASH_RING_READ_LENGTH;
    }
}

uint16_t flash_ring_read_newest(uint8_t type, uint8_t *out, uint16_t maxlen)
{
    uint16_t stored_len = 0u;
    flash_ring_read_status_t rs =
        flash_ring_read_newest_ex(type, out, maxlen, &stored_len);
    if (rs != FLASH_RING_READ_OK && rs != FLASH_RING_READ_RECOVERED)
        return 0u;
    return (stored_len > maxlen) ? maxlen : stored_len;
}

static bool cache_latest_records(void)
{
    for (unsigned i = 0; i < 6u; i++) {
        ring_cache_t *c = &s_compact[i];
        c->present = 0u;
        c->len = 0u;

        uint16_t stored_len = 0u;
        fr_read_status_t rs = fr_read_newest_ex(
            &s_state,
            s_compact_types[i],
            c->data,
            (uint16_t)sizeof(c->data),
            &stored_len);

        if (rs == FR_READ_MISSING)
            continue;

        /* A full-sector erase is the wrong time to discard evidence of a
         * damaged domain. Abort compaction if that domain cannot be recovered. */
        if (rs != FR_READ_OK && rs != FR_READ_RECOVERED)
            return false;

        if (stored_len > sizeof(c->data))
            return false;

        c->present = 1u;
        c->len = stored_len;
    }

    return true;
}

static bool compact_and_write(uint8_t type, const uint8_t *data, uint16_t len)
{
    if (len > FR_PAYLOAD) return false;

    /* BEFORE erasing anything, capture the newest valid record of every known
     * persistence domain. This is the key protection missing from the old wrap. */
    if (!cache_latest_records())
        return false;

    const uint16_t prev_erase_count = s_state.erase_count;

    /* Accepted trade-off: from this erase until the rebuild finishes there is
     * a small power-loss window. The cached image exists only in RAM. */
    if (fr_begin_fresh_count(&s_state, prev_erase_count) != 0)
        return false;

    /* Rebuild a complete baseline first, including the previous value of the
     * triggering type. If the final new write fails, the old value is still
     * present in the freshly rebuilt ring. */
    for (unsigned i = 0; i < 6u; i++) {
        const ring_cache_t *c = &s_compact[i];
        if (!c->present) continue;

        if (fr_write(&s_state,
                     s_compact_types[i],
                     c->data,
                     c->len) != FR_WRITE_OK)
            return false;
    }

    /* Append the requested new value last so it becomes authoritative. */
    if (fr_write(&s_state, type, data, len) != FR_WRITE_OK)
        return false;

    gpsdo_logf(
        GPSDO_LOG_INFO, "CONFIG",
        "Flash storage recycled for continued saving (erase count %lu).",
        (unsigned long)fr_erase_count(&s_state));
    return true;
}

bool flash_ring_write(uint8_t type, const uint8_t *data, uint16_t len)
{
    if (!s_ready || len > FR_PAYLOAD) return false;

    const int r = fr_write(&s_state, type, data, len);
    if (r == FR_WRITE_OK) return true;
    if (r != FR_WRITE_FULL) return false;

    return compact_and_write(type, data, len);
}

uint32_t flash_ring_erase_count(void) { return s_ready ? fr_erase_count(&s_state) : 0; }
uint16_t flash_ring_slot_count(void)  { return s_ready ? s_state.slot_count : 0; }
uint16_t flash_ring_slots_used(void)  { return s_ready ? s_state.used : 0; }
