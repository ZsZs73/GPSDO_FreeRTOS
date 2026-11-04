/* ======================================================================
 * gpsdo_flash_ring_core.c  —  hardware-independent ring-buffer logic
 * Part of GPSDO FreeRTOS v1.07
 * See gpsdo_flash_ring_core.h for the design and rationale.
 * ====================================================================== */
#include "gpsdo_flash_ring_core.h"
#include <string.h>

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) */
uint16_t fr_crc16(const uint8_t *p, uint32_t n)
{
    uint16_t c = 0xFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++)
            c = (c & 0x8000u) ? (uint16_t)((c << 1) ^ 0x1021u)
                              : (uint16_t)(c << 1);
    }
    return c;
}

/* ---- header helpers ---- */

static void fr_build_header(uint8_t *hdr, uint16_t erase_count)
{
    memset(hdr, 0, FR_HDR_SIZE);
    memcpy(hdr, FR_MAGIC_STR, 8);
    hdr[8]  = (uint8_t)FR_FMT_VER;
    hdr[12] = (uint8_t)(erase_count & 0xFFu);
    hdr[13] = (uint8_t)(erase_count >> 8);
    uint16_t crc = fr_crc16(hdr, 14);
    hdr[14] = (uint8_t)(crc & 0xFFu);
    hdr[15] = (uint8_t)(crc >> 8);
}

static int fr_header_valid(const uint8_t *hdr)
{
    if (memcmp(hdr, FR_MAGIC_STR, 8) != 0) return 0;
    if (hdr[8] != (uint8_t)FR_FMT_VER)     return 0;
    uint16_t crc = fr_crc16(hdr, 14);
    return (hdr[14] == (uint8_t)(crc & 0xFFu) &&
            hdr[15] == (uint8_t)(crc >> 8));
}

/* ---- slot helpers ---- */

static int fr_slot_blank(const uint8_t *s)
{
    for (int i = 0; i < (int)FR_SLOT_SIZE; i++)
        if (s[i] != 0xFFu) return 0;
    return 1;
}

static int fr_slot_valid(const uint8_t *s)
{
    if (fr_slot_blank(s)) return 0;
    uint16_t crc = fr_crc16(s, FR_SLOT_SIZE - 2);
    return (s[FR_SLOT_SIZE - 2] == (uint8_t)(crc & 0xFFu) &&
            s[FR_SLOT_SIZE - 1] == (uint8_t)(crc >> 8));
}

static uint16_t fr_slot_seq (const uint8_t *s) { return (uint16_t)(s[0] | (s[1] << 8)); }
static uint8_t  fr_slot_type(const uint8_t *s) { return s[2]; }
static uint16_t fr_slot_len (const uint8_t *s) { return (uint16_t)(s[4] | (s[5] << 8)); }

/* newer-than test using modular sequence distance (handles wrap) */
static int fr_seq_newer(uint16_t a, uint16_t b)
{
    return (uint16_t)(a - b) < 0x8000u;
}

static uint32_t fr_slot_off(const fr_state_t *st, uint16_t idx)
{
    (void)st;
    return FR_HDR_SIZE + (uint32_t)idx * FR_SLOT_SIZE;
}

/* Erase sector and lay down a fresh header with the given erase_count. */
static int fr_fresh(fr_state_t *st, uint16_t erase_count)
{
    if (st->ops->erase() != 0) return -1;
    uint8_t hdr[FR_HDR_SIZE];
    fr_build_header(hdr, erase_count);
    if (st->ops->program(0, hdr, FR_HDR_SIZE) != 0) return -1;
    st->used = 0; st->next_idx = 0; st->cur_seq = 0;
    st->have_data = 0;
    st->erase_count = erase_count;
    return 0;
}

int fr_begin(fr_state_t *st, const fr_ops_t *ops)
{
    st->ops = ops;
    st->slot_count = (uint16_t)((ops->sector_len - FR_HDR_SIZE) / FR_SLOT_SIZE);
    st->used = 0; st->next_idx = 0; st->cur_seq = 0;
    st->have_data = 0; st->erase_count = 0;

    uint8_t hdr[FR_HDR_SIZE];
    ops->read(0, hdr, FR_HDR_SIZE);

    if (!fr_header_valid(hdr)) {
        /* foreign / blank / garbage → start a clean ring. If the old header
         * happened to be a valid GPSDOLIV of a DIFFERENT version we still
         * reset, but preserve the erase count when we can read it. */
        uint16_t ec = 0;
        if (memcmp(hdr, FR_MAGIC_STR, 8) == 0) {
            uint16_t crc = fr_crc16(hdr, 14);
            if (hdr[14] == (uint8_t)(crc & 0xFFu) && hdr[15] == (uint8_t)(crc >> 8))
                ec = (uint16_t)(hdr[12] | (hdr[13] << 8));
        }
        /* count this erase */
        return fr_begin_fresh_count(st, ec);
    }

    st->erase_count = (uint16_t)(hdr[12] | (hdr[13] << 8));

    /* scan every slot: find highest seq across ALL types (for next write's
     * seq), find first blank, count used. Per-type newest is resolved on
     * demand by fr_read_newest(). */
    uint8_t slot[FR_SLOT_SIZE];
    int first_blank = -1;
    for (uint16_t i = 0; i < st->slot_count; i++) {
        st->ops->read(fr_slot_off(st, i), slot, FR_SLOT_SIZE);
        if (fr_slot_blank(slot)) {
            if (first_blank < 0) first_blank = (int)i;
            continue;
        }
        if (!fr_slot_valid(slot)) continue;  /* half-written / corrupt: skip */
        st->used++;
        uint16_t seq = fr_slot_seq(slot);
        if (!st->have_data || fr_seq_newer(seq, st->cur_seq)) {
            st->have_data = 1; st->cur_seq = seq;
        }
    }
    /* next free slot: the first blank, or wrap to full if none */
    st->next_idx = (first_blank >= 0) ? (uint16_t)first_blank : st->slot_count;
    return st->have_data;
}

/* helper split out so fr_begin stays readable; increments erase count */
int fr_begin_fresh_count(fr_state_t *st, uint16_t prev_ec)
{
    uint16_t ec = (uint16_t)(prev_ec + 1u);
    if (fr_fresh(st, ec) != 0) return -1;
    return 0;   /* no data yet */
}

fr_read_status_t fr_read_newest_ex(fr_state_t *st, uint8_t type,
                                   uint8_t *out, uint16_t maxlen,
                                   uint16_t *stored_len)
{
    if (stored_len) *stored_len = 0u;

    /* Slots are appended in physical order within the current ring generation.
     * Track the newest usable record and the newest record of this type. If a
     * corrupt record was written after the usable one, report recovery. */
    uint8_t slot[FR_SLOT_SIZE];
    uint16_t best_seq = 0u, best_idx = 0xFFFFu;
    uint16_t newest_type_idx = 0xFFFFu;

    for (uint16_t i = 0; i < st->slot_count; i++) {
        st->ops->read(fr_slot_off(st, i), slot, FR_SLOT_SIZE);
        if (fr_slot_blank(slot)) continue;
        if (fr_slot_type(slot) != type) continue;

        newest_type_idx = i;
        if (!fr_slot_valid(slot)) continue;

        uint16_t seq = fr_slot_seq(slot);
        if (best_idx == 0xFFFFu || fr_seq_newer(seq, best_seq)) {
            best_seq = seq;
            best_idx = i;
        }
    }

    if (best_idx == 0xFFFFu)
        return (newest_type_idx != 0xFFFFu) ? FR_READ_CHECKSUM : FR_READ_MISSING;

    const int recovered =
        (newest_type_idx != 0xFFFFu && newest_type_idx > best_idx);

    st->ops->read(fr_slot_off(st, best_idx), slot, FR_SLOT_SIZE);
    uint16_t len = fr_slot_len(slot);
    if (stored_len) *stored_len = len;

    if (len > FR_PAYLOAD)
        return FR_READ_LENGTH;

    uint16_t copy_len = len;
    if (copy_len > maxlen) copy_len = maxlen;
    if (copy_len > 0u && out != NULL)
        memcpy(out, slot + FR_SLOT_HDR, copy_len);

    return recovered ? FR_READ_RECOVERED : FR_READ_OK;
}

uint16_t fr_read_newest(fr_state_t *st, uint8_t type,
                        uint8_t *out, uint16_t maxlen)
{
    uint16_t stored_len = 0u;
    fr_read_status_t rs = fr_read_newest_ex(st, type, out, maxlen, &stored_len);
    if (rs != FR_READ_OK && rs != FR_READ_RECOVERED)
        return 0u;
    return (stored_len > maxlen) ? maxlen : stored_len;
}

int fr_write(fr_state_t *st, uint8_t type,
             const uint8_t *data, uint16_t len)
{
    if (len > FR_PAYLOAD) return FR_WRITE_ERROR;

    /* Do not erase from the append primitive. The HAL wrapper must first cache
     * every persistence domain, then rebuild the sector deliberately. */
    if (st->next_idx >= st->slot_count) return FR_WRITE_FULL;

    uint16_t seq = st->have_data ? (uint16_t)(st->cur_seq + 1u) : 1u;

    uint8_t slot[FR_SLOT_SIZE];
    memset(slot, 0, FR_SLOT_SIZE);
    slot[0] = (uint8_t)(seq & 0xFFu);
    slot[1] = (uint8_t)(seq >> 8);
    slot[2] = type;
    slot[3] = 0u;                       /* reserved; keeps the payload aligned */
    slot[4] = (uint8_t)(len & 0xFFu);   /* length is 16-bit: a settings block is */
    slot[5] = (uint8_t)(len >> 8);      /* ~324 B and would truncate in 8 bits   */
    memcpy(slot + FR_SLOT_HDR, data, len);
    uint16_t crc = fr_crc16(slot, FR_SLOT_SIZE - 2);
    slot[FR_SLOT_SIZE - 2] = (uint8_t)(crc & 0xFFu);
    slot[FR_SLOT_SIZE - 1] = (uint8_t)(crc >> 8);

    uint32_t off = fr_slot_off(st, st->next_idx);
    if (st->ops->program(off, slot, FR_SLOT_SIZE) != 0) return FR_WRITE_ERROR;

    /* verify readback */
    uint8_t chk[FR_SLOT_SIZE];
    st->ops->read(off, chk, FR_SLOT_SIZE);
    if (memcmp(chk, slot, FR_SLOT_SIZE) != 0) return FR_WRITE_ERROR;

    st->have_data = 1; st->cur_seq = seq;
    st->next_idx++; st->used++;
    return FR_WRITE_OK;
}

uint32_t fr_erase_count(const fr_state_t *st) { return st->erase_count; }
