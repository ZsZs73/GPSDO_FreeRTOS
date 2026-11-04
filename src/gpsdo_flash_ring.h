/* ======================================================================
 * gpsdo_flash_ring.h  —  wear-levelled persistence ring
 *
 * Part of GPSDO FreeRTOS v1.07
 *
 * Purpose
 * -------
 * Stores all persistence domains in sector 7 as fixed-size CRC-protected slots.
 * Normal saves append into erased slots. When the sector becomes full, the
 * newest valid record of every known domain is first cached in RAM, the sector
 * is erased once, the cached baseline is rebuilt and verified, then the new
 * triggering record is appended last.
 *
 *   REC_SETTINGS   user/global settings + boot algo + LRN enable
 *   REC_HW_CAL     CT K + LC detector calibration
 *   REC_LIVE       learned drift + learned damping + last PWM
 *   REC_AL11/12/13 per-algorithm configuration
 *
 * Slot layout (512 bytes, word-aligned, fmt_ver=2):
 *   [0..1]     seq    uint16  sequence number (wraps; newest = highest, mod)
 *   [2]        type   uint8   record type (REC_LIVE / REC_SETTINGS)
 *   [3]        rsv    uint8   reserved, zero
 *   [4..5]     len    uint16  payload length in bytes (<= FR_PAYLOAD)
 *   [4..509]   data   506 bytes payload
 *   [510..511] crc16  CRC-16/CCITT over bytes [0..509]
 *
 * A half-written slot (power loss mid-write) fails CRC and is skipped, so the
 * previous good slot is always recoverable.
 *
 * Sector 7 @ 0x08060000 (128 KB) on STM32F411CE. Firmware ends in sector 5
 * (~0x0803A918); sectors 0-5 are code, sector 6 is a growth buffer, sector 7
 * is the ring. There are 255 usable 512-byte slots after the 16-byte header.
 * Compaction preserves every persistence domain, but because sector 7 is one
 * physical erase unit there is an accepted brief power-loss window while a
 * full ring is being erased and rebuilt from its RAM cache.
 * ====================================================================== */
#ifndef GPSDO_FLASH_RING_H
#define GPSDO_FLASH_RING_H

#include <stdint.h>
#include <stdbool.h>

/* Slot size — also defined in gpsdo_flash_ring_core.h, mirrored here for callers
 * that want to size their payload buffers without including the core. */
#define FR_PAYLOAD    504u   /* usable data bytes per slot (6-B slot hdr)*/
#define FR_SLOT_SIZE  512u   /* total slot size (payload + header)        */

/* Record type IDs — see gpsdo_flash_ring_core.h for the canonical definition. */
#define REC_LIVE      0u
#define REC_SETTINGS  1u
/* Algorithm 13 (Kalman) parameters. Its own record rather than a field in the
 * settings block: that block has no padding left, and growing it would force a
 * SETTINGS_VER bump that throws away everyone's PID, LC and timezone for three
 * numbers. A new type costs nothing and an older build simply never reads it. */
#define REC_AL13    2u
#define REC_AL11    4u
#define REC_AL12    5u
/* Shared CT/OCXO plant calibration (K = Hz per 16-bit LSB). */
#define REC_HW_CAL  3u

/* Initialise: validate header, scan slots. Call once at boot before the
 * scheduler starts. Returns true if at least one valid slot exists. */
bool     flash_ring_begin(void);

typedef enum {
    FLASH_RING_READ_OK = 0,
    FLASH_RING_READ_RECOVERED,
    FLASH_RING_READ_MISSING,
    FLASH_RING_READ_CHECKSUM,
    FLASH_RING_READ_LENGTH,
    FLASH_RING_READ_NOT_READY
} flash_ring_read_status_t;

/* Detailed read for user-facing diagnostics. stored_len is the length recorded
 * in flash; at most maxlen bytes are copied to out. */
flash_ring_read_status_t flash_ring_read_newest_ex(
    uint8_t type, uint8_t *out, uint16_t maxlen, uint16_t *stored_len);

/* Compatibility wrapper: returns copied payload length, or 0 on failure. */
uint16_t flash_ring_read_newest(uint8_t type, uint8_t *out, uint16_t maxlen);

/* Append a new payload as the next ring slot. When full, cache the newest
 * valid records of all known types, erase/rebuild sector 7, then append this
 * payload last. Returns true only if the requested value is verified in flash. */
bool     flash_ring_write(uint8_t type, const uint8_t *data, uint16_t len);

/* Number of sector erase cycles so far (wear indicator, shown by EW). */
uint32_t flash_ring_erase_count(void);

/* Total slots and how many are currently used (for EW diagnostics). */
uint16_t flash_ring_slot_count(void);
uint16_t flash_ring_slots_used(void);

/* Physical location of the ring, for diagnostics. Exposed as accessors rather
 * than left to each caller's own constants: the EW command used to print a
 * hardcoded "sector 6, 0x08040000" while the ring had always been in sector 7,
 * so the one place an operator looks was the one place that lied. */
uint32_t flash_ring_base_addr(void);
uint8_t  flash_ring_sector_no(void);

/* Erase and reformat the ring sector (cold-restart wipe). After this the ring
 * reads empty and the next recall falls back to defaults. Returns true on OK. */


bool     flash_ring_wipe(void);

#endif /* GPSDO_FLASH_RING_H */
