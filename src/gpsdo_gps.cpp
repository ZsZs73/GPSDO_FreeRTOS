/**
 * gpsdo_gps.cpp — vGpsTask — GPS NMEA parsing and UBX configuration
 *
 * Part of GPSDO FreeRTOS v1.07
 * Author:   J. M. Niewiński
 * GitHub:   https://github.com/jmnlabs/GPSDO_FreeRTOS
 * Based on: GPSDO v0.06c by André Balsa
 * AI:       Claude Opus 5 (Anthropic), GLM-5.3 Max (Z.ai), Qwen3.8-Max
 *
 *
 * Reads NMEA sentences from Serial1 (38400 Bd), feeds TinyGPS++, and
 * publishes fix data to gGps under xGpsMutex.
 *
 * On first boot, performs auto-baud detection (9600/38400/115200) and
 * sends UBX configuration: stationary navigation mode, 1 Hz update rate.
 * Handles ACK-ACK / ACK-NAK with bounded retries and minimal-mask
 * fallback for stripped NEO-6M firmware variants.
 */

#include "gpsdo_config.h"
#include "gpsdo_serial_tx.h"
#include "gpsdo_state.h"
#include "gpsdo_ubx_timtp.h"
#include "gpsdo_gps.h"
#include "gpsdo_log.h"
#include <Arduino.h>
#include <TinyGPS++.h>
#include <string.h>

static TinyGPSPlus gps;

static gpsdo_gnss_info_t s_gnss_info = {};

const gpsdo_gnss_info_t *gpsdo_gps_info(void)
{
    return &s_gnss_info;
}

/* Serial port for echo / messages */
#if defined(GPSDO_BLUETOOTH_PARALLEL)
  #define CLI_SER g_serial
#elif defined(GPSDO_BLUETOOTH)
  #define CLI_SER Serial2
#else
  #define CLI_SER Serial
#endif

/* ---- UBX configuration ------------------------------------------------ */
#ifdef GPSDO_UBX_CONFIG

static void flush_rx(uint32_t ms)
{
    /* Drain Serial1 RX for `ms` milliseconds, discard all bytes.
     * Called after baud rate transitions to clear framing garbage. */
    uint32_t end = millis() + ms;
    while (millis() < end) {
        while (Serial1.available()) Serial1.read();
        delay(5);
    }
}

static void send_ubx(const uint8_t *msg, uint8_t len)
{
    for (int i = 0; i < len; i++) Serial1.write(msg[i]);
    Serial1.flush();
}

/*
 * ubx_cksum — compute u-blox checksum in-place on msg[2..len-3].
 * Writes CK_A to msg[len-2] and CK_B to msg[len-1].
 */
static void ubx_cksum(uint8_t *msg, uint8_t len)
{
    uint8_t cka = 0, ckb = 0;
    for (uint8_t i = 2; i < len - 2; i++) { cka += msg[i]; ckb += cka; }
    msg[len - 2] = cka;
    msg[len - 1] = ckb;
}

/*
 * recv_ubx_frame — receive one checksum-valid UBX frame of the requested
 * class/id while ignoring interleaved NMEA and unrelated UBX messages.
 */
static bool recv_ubx_frame(uint8_t want_cls, uint8_t want_id,
                           uint8_t *payload, uint16_t payload_cap,
                           uint16_t *payload_len, uint32_t timeout_ms)
{
    enum {
        RX_SYNC1, RX_SYNC2, RX_CLASS, RX_ID,
        RX_LEN1, RX_LEN2, RX_PAYLOAD, RX_CKA, RX_CKB
    } state = RX_SYNC1;

    bool sched_running = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);

    uint8_t  rx_cls = 0, rx_id = 0;
    uint16_t rx_len = 0, pos = 0;
    uint8_t  cka = 0, ckb = 0;

    uint32_t t0 = millis();

    while ((millis() - t0) < timeout_ms) {
        if (!Serial1.available()) {
            if (sched_running) vTaskDelay(pdMS_TO_TICKS(1));
            else               delay(1);
            continue;
        }

        uint8_t b = (uint8_t)Serial1.read();

        switch (state) {
        case RX_SYNC1:
            if (b == 0xB5) state = RX_SYNC2;
            break;

        case RX_SYNC2:
            if (b == 0x62) {
                state = RX_CLASS;
            } else {
                state = (b == 0xB5) ? RX_SYNC2 : RX_SYNC1;
            }
            break;

        case RX_CLASS:
            rx_cls = b;
            cka = b;
            ckb = cka;
            state = RX_ID;
            break;

        case RX_ID:
            rx_id = b;
            cka += b;
            ckb += cka;
            state = RX_LEN1;
            break;

        case RX_LEN1:
            rx_len = b;
            cka += b;
            ckb += cka;
            state = RX_LEN2;
            break;

        case RX_LEN2:
            rx_len |= (uint16_t)b << 8;
            cka += b;
            ckb += cka;
            pos = 0;
            state = (rx_len == 0) ? RX_CKA : RX_PAYLOAD;
            break;

        case RX_PAYLOAD:
            cka += b;
            ckb += cka;

            if (rx_cls == want_cls && rx_id == want_id && pos < payload_cap)
                payload[pos] = b;

            if (++pos >= rx_len)
                state = RX_CKA;
            break;

        case RX_CKA:
            if (b == cka) {
                state = RX_CKB;
            } else {
                state = (b == 0xB5) ? RX_SYNC2 : RX_SYNC1;
            }
            break;

        case RX_CKB:
            if (b == ckb &&
                rx_cls == want_cls &&
                rx_id == want_id &&
                rx_len <= payload_cap) {
                if (payload_len) *payload_len = rx_len;
                return true;
            }

            state = (b == 0xB5) ? RX_SYNC2 : RX_SYNC1;
            break;
        }
    }

    return false;
}

static void copy_ubx_text(char *dst, size_t dst_size,
                          const uint8_t *src, size_t src_size)
{
    if (dst_size == 0) return;

    size_t n = 0;
    while (n < src_size && src[n] != '\0' && n + 1 < dst_size) {
        dst[n] = (char)src[n];
        n++;
    }

    while (n > 0 && dst[n - 1] == ' ')
        n--;

    dst[n] = '\0';
}

/*
 * UBX-MON-VER:
 *   swVersion  CH[30]
 *   hwVersion  CH[10]
 *   extension  CH[30] repeated
 */

/* ---- Detailed GNSS identity/capability state -------------------------- */
typedef enum {
    GNSS_FW_UNKNOWN = 0,
    GNSS_FW_ROM,
    GNSS_FW_FLASH
} gnss_fw_source_t;

static char s_gnss_rom_version[31] = {0};
static char s_gnss_fis[31]         = {0};
static char s_gnss_caps[80]        = {0};
static gnss_fw_source_t s_gnss_fw_source = GNSS_FW_UNKNOWN;
static bool s_gnss_flash_reported = false;

static const char *gnss_ext_value(const char *line, const char *key)
{
    size_t n = strlen(key);
    if (strncmp(line, key, n) != 0) return NULL;
    if (line[n] == '=') return line + n + 1;
    if (line[n] == ' ') {
        const char *p = line + n;
        while (*p == ' ') ++p;
        return p;
    }
    return NULL;
}

static void gnss_copy_cstr(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

enum {
    GNSS_CAP_GPS=1u<<0, GNSS_CAP_SBAS=1u<<1, GNSS_CAP_GLO=1u<<2,
    GNSS_CAP_GAL=1u<<3, GNSS_CAP_BDS=1u<<4, GNSS_CAP_QZSS=1u<<5,
    GNSS_CAP_IMES=1u<<6, GNSS_CAP_NAVIC=1u<<7
};

static uint16_t gnss_caps_from_extension(const char *line)
{
    if (!line || !line[0] || strncmp(line, "GNSS ", 5) == 0) return 0;
    uint16_t caps = 0;
    const char *p = line;
    while (*p) {
        const char *e = strchr(p, ';');
        size_t n = e ? (size_t)(e-p) : strlen(p);
        if      (n==3 && strncmp(p,"GPS",3)==0)   caps |= GNSS_CAP_GPS;
        else if (n==4 && strncmp(p,"SBAS",4)==0)  caps |= GNSS_CAP_SBAS;
        else if (n==3 && strncmp(p,"GLO",3)==0)   caps |= GNSS_CAP_GLO;
        else if (n==3 && strncmp(p,"GAL",3)==0)   caps |= GNSS_CAP_GAL;
        else if (n==3 && strncmp(p,"BDS",3)==0)   caps |= GNSS_CAP_BDS;
        else if (n==4 && strncmp(p,"QZSS",4)==0)  caps |= GNSS_CAP_QZSS;
        else if (n==4 && strncmp(p,"IMES",4)==0)  caps |= GNSS_CAP_IMES;
        else if (n==5 && strncmp(p,"NAVIC",5)==0) caps |= GNSS_CAP_NAVIC;
        if (!e) break;
        p = e + 1;
    }
    return caps;
}

static void gnss_append_cap(const char *name)
{
    size_t used = strlen(s_gnss_caps);
    if (used && used + 2 < sizeof(s_gnss_caps)) {
        s_gnss_caps[used++] = ',';
        s_gnss_caps[used++] = ' ';
        s_gnss_caps[used] = '\0';
    }
    if (used + 1 < sizeof(s_gnss_caps))
        strncat(s_gnss_caps, name, sizeof(s_gnss_caps) - used - 1);
}

static void gnss_format_caps(uint16_t caps)
{
    s_gnss_caps[0] = '\0';
    if (caps & GNSS_CAP_GPS)   gnss_append_cap("GPS");
    if (caps & GNSS_CAP_SBAS)  gnss_append_cap("SBAS");
    if (caps & GNSS_CAP_GLO)   gnss_append_cap("GLONASS");
    if (caps & GNSS_CAP_GAL)   gnss_append_cap("Galileo");
    if (caps & GNSS_CAP_BDS)   gnss_append_cap("BeiDou");
    if (caps & GNSS_CAP_QZSS)  gnss_append_cap("QZSS");
    if (caps & GNSS_CAP_IMES)  gnss_append_cap("IMES");
    if (caps & GNSS_CAP_NAVIC) gnss_append_cap("NavIC");
}

static const char *gnss_fw_source_name(void)
{
    if (s_gnss_fw_source == GNSS_FW_ROM) return "ROM";
    if (s_gnss_fw_source == GNSS_FW_FLASH) return "flash";
    return "unknown";
}

static bool gnss_poll_message(uint8_t cls, uint8_t id,
                              uint8_t *payload, uint16_t payload_cap,
                              uint16_t *payload_len, uint32_t timeout_ms)
{
    uint8_t poll[8] = {0xB5,0x62,cls,id,0x00,0x00,0x00,0x00};
    uint8_t a=0,b=0;
    for (uint8_t i=2; i<6; ++i) { a=(uint8_t)(a+poll[i]); b=(uint8_t)(b+a); }
    poll[6]=a; poll[7]=b;
    flush_rx(20);
    for (uint8_t i=0; i<sizeof(poll); ++i) Serial1.write(poll[i]);
    Serial1.flush();
    return recv_ubx_frame(cls,id,payload,payload_cap,payload_len,timeout_ms);
}

static bool gnss_tim_tp_qerr_from_payload(const uint8_t *payload,
                                                uint16_t len,
                                                int32_t *qerr_ps)
{
    if (!payload || !qerr_ps || len < 16) return false;

    uint32_t q = (uint32_t)payload[8] |
                 ((uint32_t)payload[9] << 8) |
                 ((uint32_t)payload[10] << 16) |
                 ((uint32_t)payload[11] << 24);
    *qerr_ps = (int32_t)q;
    return true;
}

static bool gnss_poll_tim_tp_qerr(int32_t *qerr_ps)
{
    uint8_t payload[32];
    uint16_t len = 0;
    if (!gnss_poll_message(0x0D, 0x01,
                           payload, sizeof(payload), &len, 500))
        return false;
    return gnss_tim_tp_qerr_from_payload(payload, len, qerr_ps);
}

/* Wait for a TIM-TP frame without polling it. Used only as a fallback when
 * CFG-MSG cannot report the current UART1 output rate. */
static bool gnss_wait_tim_tp_qerr(int32_t *qerr_ps, uint32_t timeout_ms)
{
    uint8_t payload[32];
    uint16_t len = 0;
    if (!recv_ubx_frame(0x0D, 0x01,
                        payload, sizeof(payload), &len, timeout_ms))
        return false;
    return gnss_tim_tp_qerr_from_payload(payload, len, qerr_ps);
}

static void gnss_log_detailed_identity(void)
{
    if (s_gnss_info.model[0])
        gpsdo_logf(GPSDO_LOG_INFO, "GPS",
                   "Receiver: %s.", s_gnss_info.model);
    else
        gpsdo_log_event(GPSDO_LOG_INFO, "GPS",
                        "Receiver: UBX; production model ID not reported.");

    gpsdo_logf(GPSDO_LOG_INFO,"GPS",
               "Firmware: SW %s; HW %s; protocol %s; source %s.",
               s_gnss_info.sw_version[0]?s_gnss_info.sw_version:"not reported",
               s_gnss_info.hw_version[0]?s_gnss_info.hw_version:"not reported",
               s_gnss_info.prot_version[0]?s_gnss_info.prot_version:"not reported",
               gnss_fw_source_name());

    if (s_gnss_info.fw_version[0])
        gpsdo_logf(GPSDO_LOG_INFO,"GPS","Product firmware: %s.",s_gnss_info.fw_version);
    if (s_gnss_rom_version[0])
        gpsdo_logf(GPSDO_LOG_INFO,"GPS","Underlying ROM: %s.",s_gnss_rom_version);

    if (s_gnss_caps[0])
        gpsdo_logf(GPSDO_LOG_INFO,"GPS","GNSS capability: %s.",s_gnss_caps);
    else
        gpsdo_log_event(GPSDO_LOG_INFO,"GPS","GNSS capability list not reported.");

    if (s_gnss_flash_reported)
        gpsdo_logf(GPSDO_LOG_INFO,"GPS",
                   "Storage: flash reported (%s); write capability not tested.",
                   s_gnss_fis[0]?s_gnss_fis:"FIS present");
    else if (s_gnss_fw_source == GNSS_FW_FLASH)
        gpsdo_log_event(GPSDO_LOG_INFO,"GPS",
                        "Storage: firmware runs from flash; FIS not reported; write capability not tested.");
    else
        gpsdo_log_event(GPSDO_LOG_INFO,"GPS",
                        "Storage: flash not reported; write capability not tested.");
}


static bool ubx_poll_mon_ver(void)
{
    static const uint8_t poll[] = {0xB5,0x62,0x0A,0x04,0x00,0x00,0x0E,0x34};
    uint8_t payload[400];
    uint16_t len=0;

    for (uint8_t i=0; i<sizeof(poll); ++i) Serial1.write(poll[i]);
    Serial1.flush();

    if (!recv_ubx_frame(0x0A,0x04,payload,sizeof(payload),&len,700))
        return false;
    if (len < 40 || ((len-40u)%30u)!=0u)
        return false;

    copy_ubx_text(s_gnss_info.sw_version,sizeof(s_gnss_info.sw_version),payload,30);
    copy_ubx_text(s_gnss_info.hw_version,sizeof(s_gnss_info.hw_version),payload+30,10);

    s_gnss_info.model[0]='\0';
    s_gnss_info.fw_version[0]='\0';
    s_gnss_info.prot_version[0]='\0';
    s_gnss_rom_version[0]='\0';
    s_gnss_fis[0]='\0';
    s_gnss_caps[0]='\0';
    s_gnss_fw_source=GNSS_FW_UNKNOWN;
    s_gnss_flash_reported=false;

    if (strncmp(s_gnss_info.sw_version,"ROM CORE",8)==0)
        s_gnss_fw_source=GNSS_FW_ROM;
    else if (strncmp(s_gnss_info.sw_version,"EXT CORE",8)==0)
        s_gnss_fw_source=GNSS_FW_FLASH;

    uint16_t caps=0;
    uint16_t ext_count=(uint16_t)((len-40u)/30u);

    for (uint16_t i=0; i<ext_count; ++i) {
        char ext[31];
        copy_ubx_text(ext,sizeof(ext),payload+40u+30u*i,30);
        if (!ext[0]) continue;

        const char *v;
        bool known=false;

        if ((v=gnss_ext_value(ext,"MOD"))!=NULL) {
            gnss_copy_cstr(s_gnss_info.model,sizeof(s_gnss_info.model),v);
            known=true;
        } else if ((v=gnss_ext_value(ext,"FWVER"))!=NULL) {
            gnss_copy_cstr(s_gnss_info.fw_version,sizeof(s_gnss_info.fw_version),v);
            known=true;
        } else if ((v=gnss_ext_value(ext,"PROTVER"))!=NULL) {
            gnss_copy_cstr(s_gnss_info.prot_version,sizeof(s_gnss_info.prot_version),v);
            known=true;
        } else if ((v=gnss_ext_value(ext,"FIS"))!=NULL) {
            gnss_copy_cstr(s_gnss_fis,sizeof(s_gnss_fis),v);
            s_gnss_flash_reported=true;
            known=true;
        } else if ((v=gnss_ext_value(ext,"ROM BASE"))!=NULL) {
            gnss_copy_cstr(s_gnss_rom_version,sizeof(s_gnss_rom_version),v);
            s_gnss_fw_source=GNSS_FW_FLASH;
            known=true;
        }

        if (!known && i==0 && ext[0]>='0' && ext[0]<='9' && strchr(ext,'(')!=NULL) {
            gnss_copy_cstr(s_gnss_rom_version,sizeof(s_gnss_rom_version),ext);
            s_gnss_fw_source=GNSS_FW_FLASH;
            known=true;
        }

        if (!known)
            caps |= gnss_caps_from_extension(ext);
    }

    gnss_format_caps(caps);

    if (s_gnss_fw_source==GNSS_FW_UNKNOWN && s_gnss_info.prot_version[0]) {
        int major=atoi(s_gnss_info.prot_version);
        if (major>0 && major<=17)
            s_gnss_fw_source=GNSS_FW_ROM;
    }

    s_gnss_info.ubx_seen=true;
    s_gnss_info.detected=true;
    return true;
}

/*
 * get_ubx_ack — wait for UBX-ACK-ACK or UBX-ACK-NAK.
 *
 * Two independent byte-level state machines run in parallel so that
 * NMEA bytes flowing in parallel do not confuse the matcher.
 *
 * Key improvement over original:
 *   The inner loop drains Serial1 CONTINUOUSLY — it does NOT call delay()
 *   unless the FIFO is empty.  At 38400 baud ~3840 B/s arrive; the old
 *   code called delay(1) every time no byte was immediately available,
 *   allowing up to 3-4 bytes to pile up per ms. Over a 2000 ms window
 *   the 256-byte ring buffer overflows ~30 times, silently dropping the
 *   10-byte ACK.
 *
 * Returns: +1 = ACK-ACK, -1 = ACK-NAK, 0 = timeout
 */
static const char *ubx_command_name(uint8_t cls, uint8_t id)
{
    if (cls == 0x06) {  /* UBX-CFG */
        switch (id) {
        case 0x00: return "CFG-PRT";
        case 0x01: return "CFG-MSG";
        case 0x08: return "CFG-RATE";
        case 0x09: return "CFG-CFG";
        case 0x1D: return "CFG-TMODE";
        case 0x24: return "CFG-NAV5";
        case 0x31: return "CFG-TP5";
        case 0x3D: return "CFG-TMODE2";
        case 0x8A: return "CFG-VALSET";
        default:   break;
        }
    }

    return nullptr;
}

static int8_t get_ubx_ack(const uint8_t *msg, uint32_t timeout_ms = 1500, bool log_nak = true)
{
    bool sched_running = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);

    uint8_t ack_frame[10], nak_frame[10];
    ack_frame[0]=0xB5; ack_frame[1]=0x62;
    ack_frame[2]=0x05; ack_frame[3]=0x01;
    ack_frame[4]=0x02; ack_frame[5]=0x00;
    ack_frame[6]=msg[2]; ack_frame[7]=msg[3];
    ack_frame[8]=0; ack_frame[9]=0;
    for (int i=2;i<8;i++){ack_frame[8]+=ack_frame[i];ack_frame[9]+=ack_frame[8];}

    memcpy(nak_frame, ack_frame, 10);
    nak_frame[3] = 0x00;
    nak_frame[8]=0; nak_frame[9]=0;
    for (int i=2;i<8;i++){nak_frame[8]+=nak_frame[i];nak_frame[9]+=nak_frame[8];}

    uint8_t ack_id = 0, nak_id = 0;
    uint32_t t0 = millis();

    while ((millis() - t0) < timeout_ms) {
        if (Serial1.available()) {
            /* Drain available bytes without delay — prevents RX ring buffer
             * overflow at 38400 baud when NMEA runs in parallel. */
            uint8_t b = (uint8_t)Serial1.read();

            if (b == ack_frame[ack_id]) {
                if (++ack_id >= 10) return +1;
            } else {
                ack_id = (b == ack_frame[0]) ? 1u : 0u;
            }

            if (b == nak_frame[nak_id]) {
                if (++nak_id >= 10) {
                    if (log_nak) {
                        const uint8_t cls = msg[2];
                        const uint8_t id  = msg[3];
                        const char *name = ubx_command_name(cls, id);

                        if (name) {
                            gpsdo_logf(GPSDO_LOG_WARN, "GPS",
                                       "Receiver rejected %s (UBX %02X/%02X).",
                                       name, cls, id);
                        } else {
                            gpsdo_logf(GPSDO_LOG_WARN, "GPS",
                                       "Receiver rejected UBX command %02X/%02X.",
                                       cls, id);
                        }
                    }
                    return -1;
                }
            } else {
                nak_id = (b == nak_frame[0]) ? 1u : 0u;
            }
        } else {
            /* Yield only when truly nothing to read */
            if (sched_running) vTaskDelay(pdMS_TO_TICKS(1));
            else                delay(1);
        }
    }
    return 0;
}

/*
 * ubx_send_cfg_msg — send UBX-CFG-MSG to enable/disable one NMEA sentence.
 *
 * UBX-CFG-MSG (class=0x06 id=0x01 length=3):
 *   payload[0] = NMEA message class (0xF0 for standard NMEA)
 *   payload[1] = NMEA message ID
 *   payload[2] = output rate (0=disabled, 1=every fix)
 *
 * Waits up to 500 ms for ACK.
 * Returns true on ACK, false on timeout/NAK.
 *
 * Used to silence noisy NMEA sentences before sending CFG-NAV5 so the
 * 256-byte RX ring buffer does not overflow and drop the ACK response.
 */
static bool ubx_send_cfg_msg(uint8_t nmea_class, uint8_t nmea_id, uint8_t rate)
{
    uint8_t msg[11] = {
        0xB5, 0x62,       /* UBX header  */
        0x06, 0x01,       /* CFG-MSG     */
        0x03, 0x00,       /* length = 3  */
        nmea_class, nmea_id, rate,
        0x00, 0x00        /* CK_A, CK_B  */
    };
    ubx_cksum(msg, sizeof(msg));
    send_ubx(msg, sizeof(msg));
    return (get_ubx_ack(msg, 500) == +1);
}

/* Read the current UART1 output rate for one UBX message via CFG-MSG.
 * No receiver setting is changed by this query. */
static bool ubx_poll_cfg_msg_uart1_rate(uint8_t msg_class, uint8_t msg_id,
                                        uint8_t *rate_uart1)
{
    if (!rate_uart1) return false;

    uint8_t poll[10] = {
        0xB5, 0x62,       /* UBX header */
        0x06, 0x01,       /* CFG-MSG    */
        0x02, 0x00,       /* length = 2 */
        msg_class, msg_id,
        0x00, 0x00        /* CK_A, CK_B */
    };
    ubx_cksum(poll, sizeof(poll));

    flush_rx(20);
    send_ubx(poll, sizeof(poll));

    uint8_t payload[8];
    uint16_t len = 0;
    if (!recv_ubx_frame(0x06, 0x01,
                        payload, sizeof(payload), &len, 700))
        return false;

    /* CFG-MSG response: class, id, DDC, UART1, UART2, USB, SPI, reserved. */
    if (len < 8 || payload[0] != msg_class || payload[1] != msg_id)
        return false;

    *rate_uart1 = payload[3];
    return true;
}

/*
 * ubx_reduce_nmea — disable noisy NMEA sentences, keep only GGA + RMC.
 *
 * GY-NEO6MV2 / NEO-6M default output:
 *   GGA (F0 00) ← KEEP — provides fix, alt, sats, HDOP
 *   GLL (F0 01) ← DISABLE — position only, redundant
 *   GSA (F0 02) ← DISABLE — DOP + active sats, ~70 B/s
 *   GSV (F0 03) ← DISABLE — satellites in view, largest output: 3×70 B = 210 B/s
 *   RMC (F0 04) ← KEEP — provides date, time, speed
 *   VTG (F0 05) ← DISABLE — course/speed, redundant
 *
 * At 38400 baud with full NMEA: ~460–710 B/s.
 * After reduction (GGA+RMC only): ~150 B/s = 1 byte every 6.7 ms.
 * The 10-byte ACK response for CFG-NAV5 is now safe from buffer overflow.
 *
 * Returns count of successfully ACKed disable commands (0–4).
 * A return value < 4 means some commands were not ACKed, which is normal
 * for locked/stripped firmware; CFG-NAV5 is still attempted.
 */
static uint8_t ubx_reduce_nmea(void)
{
    uint8_t ok = 0;
    /* Small inter-command gap so module does not see back-to-back UBX
     * while still sending NMEA — 20 ms > one NMEA sentence at 38400. */
    struct { uint8_t cls; uint8_t id; } const disable_list[] = {
        {0xF0, 0x01},   /* GLL */
        {0xF0, 0x02},   /* GSA */
        {0xF0, 0x03},   /* GSV */
        {0xF0, 0x05},   /* VTG */
    };
    for (uint8_t i = 0; i < 4; i++) {
        if (ubx_send_cfg_msg(disable_list[i].cls, disable_list[i].id, 0)) {
            ok++;
        }
        delay(20);
    }
    /* Drain residual NMEA that was queued before disabling */
    flush_rx(150);
    return ok;
}

/* ======================================================================
 * Survey-in / Time Mode for LEA-6T / LEA-M8T timing receivers
 * ====================================================================== */
#ifdef GPSDO_GPS_TIMING

/* Start survey-in. The LEA-6T and LEA-M8T accept different Time Mode
 * commands, so try each and stop at the first ACK:
 *   1. CFG-TMODE2 (0x06 0x3D), 28-byte payload  — LEA-M8T
 *   2. CFG-TMODE  (0x06 0x1D), 28-byte payload  — LEA-6T (u-blox 6)
 *   3. CFG-VALSET (0x06 0x8A), CFG-TMODE-* keys  — ZED-F9T (Gen9)
 *      Gen9 deprecates the legacy CFG-TMODE* frames (gone as of firmware
 *      TIM 2.24) in favour of the configuration-key interface, and reports
 *      survey-in via NAV-SVIN (not TIM-SVIN). Tested on real hardware by
 *      EEVblog user danieljw. The accuracy key is in 0.1 mm units (×10).
 * Returns true on the first ACK. Caller logs which (if any) succeeded. */
static bool ubx_start_survey_in(void)
{
    const uint32_t dur = GPSDO_SVIN_MIN_SECS;     /* seconds */
    const uint32_t acc = GPSDO_SVIN_ACC_LIMIT;    /* mm      */

    /* ---- Variant 1: CFG-TMODE2 (0x3D), 28-byte payload (LEA-M8T) ----
     * timeMode u1@0, reserved1 u1@1, flags u2@2, ecef/lat i4@4..15,
     * fixedPosAcc u4@16, svinMinDur u4@20, svinAccLimit u4@24. */
    {
        uint8_t m[8 + 28];
        memset(m, 0, sizeof(m));
        m[0]=0xB5; m[1]=0x62; m[2]=0x06; m[3]=0x3D; m[4]=28; m[5]=0x00;
        m[6+0]=0x01;            /* timeMode = 1 (survey-in) */
        m[6+20]=dur&0xFF; m[6+21]=(dur>>8)&0xFF; m[6+22]=(dur>>16)&0xFF; m[6+23]=(dur>>24)&0xFF;
        m[6+24]=acc&0xFF; m[6+25]=(acc>>8)&0xFF; m[6+26]=(acc>>16)&0xFF; m[6+27]=(acc>>24)&0xFF;
        ubx_cksum(m, sizeof(m));
        send_ubx(m, sizeof(m));
        if (get_ubx_ack(m, 1500, false) == +1) {
            gpsdo_log_event(GPSDO_LOG_INFO, "GPS",
                            "Timing: survey-in start accepted using CFG-TMODE2.");
            return true;
        }
    }
    flush_rx(150);

    /* ---- Variant 2: CFG-TMODE (0x1D), 28-byte payload (LEA-6T) ----
     * u-blox 6 original Time Mode: timeMode u4@0 (note: u4, not u1+flags),
     * fixedPosX i4@4, fixedPosY i4@8, fixedPosZ i4@12, fixedPosVar u4@16,
     * svinMinDur u4@20, svinAccLimit u4@24. timeMode=1 = survey-in. */
    {
        uint8_t m[8 + 28];
        memset(m, 0, sizeof(m));
        m[0]=0xB5; m[1]=0x62; m[2]=0x06; m[3]=0x1D; m[4]=28; m[5]=0x00;
        m[6+0]=0x01; m[6+1]=0x00; m[6+2]=0x00; m[6+3]=0x00;  /* timeMode u4 = 1 */
        m[6+20]=dur&0xFF; m[6+21]=(dur>>8)&0xFF; m[6+22]=(dur>>16)&0xFF; m[6+23]=(dur>>24)&0xFF;
        m[6+24]=acc&0xFF; m[6+25]=(acc>>8)&0xFF; m[6+26]=(acc>>16)&0xFF; m[6+27]=(acc>>24)&0xFF;
        ubx_cksum(m, sizeof(m));
        send_ubx(m, sizeof(m));
        if (get_ubx_ack(m, 1500, false) == +1) {
            gpsdo_log_event(GPSDO_LOG_INFO, "GPS",
                            "Timing: survey-in start accepted using legacy CFG-TMODE.");
            return true;
        }
    }
    flush_rx(150);

    /* ---- Variant 3: CFG-VALSET (0x8A) with CFG-TMODE-* keys (ZED-F9T) ----
     * Gen9 configuration-key interface. Tested on real hardware by EEVblog
     * user danieljw. One VALSET frame sets three keys:
     *   CFG-TMODE-MODE         0x20030001  U1  = 1 (survey-in)
     *   CFG-TMODE-SVIN_MIN_DUR 0x40030010  U4  = dur [s]
     *   CFG-TMODE-SVIN_ACC_LIMIT 0x40030011 U4 = acc [0.1 mm]  (NOTE the unit!)
     * VALSET payload: version u1=0, layer u1=1 (RAM), reserved u2=0,
     *   then [keyID u4 little-endian][value little-endian] per item.
     * Accuracy: GPSDO_SVIN_ACC_LIMIT is in mm; F9T wants 0.1 mm, so ×10. */
    {
        const uint32_t acc01 = acc * 10u;          /* mm → 0.1 mm */
        uint8_t m[8 + 4 + (4+1) + (4+4) + (4+4)];   /* = 8+4+5+8+8 = 33 */
        memset(m, 0, sizeof(m));
        uint8_t p = 6;                              /* payload write cursor */
        m[0]=0xB5; m[1]=0x62; m[2]=0x06; m[3]=0x8A;
        m[4]=(uint8_t)(sizeof(m)-8); m[5]=0x00;     /* payload length = 25 */
        /* header: version=0, layer=1 (RAM), reserved=0,0 */
        m[p++]=0x00; m[p++]=0x01; m[p++]=0x00; m[p++]=0x00;
        /* key CFG-TMODE-MODE = 1 (U1) */
        m[p++]=0x01; m[p++]=0x00; m[p++]=0x03; m[p++]=0x20;
        m[p++]=0x01;
        /* key CFG-TMODE-SVIN_MIN_DUR = dur (U4) */
        m[p++]=0x10; m[p++]=0x00; m[p++]=0x03; m[p++]=0x40;
        m[p++]=dur&0xFF; m[p++]=(dur>>8)&0xFF; m[p++]=(dur>>16)&0xFF; m[p++]=(dur>>24)&0xFF;
        /* key CFG-TMODE-SVIN_ACC_LIMIT = acc01 (U4, 0.1 mm) */
        m[p++]=0x11; m[p++]=0x00; m[p++]=0x03; m[p++]=0x40;
        m[p++]=acc01&0xFF; m[p++]=(acc01>>8)&0xFF; m[p++]=(acc01>>16)&0xFF; m[p++]=(acc01>>24)&0xFF;
        ubx_cksum(m, sizeof(m));
        send_ubx(m, sizeof(m));
        if (get_ubx_ack(m, 1500, false) == +1) {
            gpsdo_log_event(GPSDO_LOG_INFO, "GPS",
                            "Timing: survey-in start accepted using CFG-VALSET.");
            return true;
        }
    }
    flush_rx(150);

    return false;   /* none ACKed — caller falls back to monitoring */
}


/* Poll TIM-SVIN (0x0D 0x04) and parse the survey-in status.
 * Fills *dur [s], *acc_mm, *valid, *active. Returns true if a TIM-SVIN
 * frame was decoded. Used by both LEA-6T and LEA-M8T (28-byte payload,
 * confirmed in u-center: B5 62 0D 04 1C 00 ...). */
static bool ubx_poll_svin(uint32_t *dur, uint32_t *acc_mm,
                          bool *valid, bool *active)
{
    const uint8_t cls = 0x0D, id = 0x04;   /* TIM-SVIN, 28-byte payload */
    uint8_t poll[8] = { 0xB5,0x62, cls, id, 0,0, 0,0 };
    ubx_cksum(poll, sizeof(poll));
    send_ubx(poll, sizeof(poll));

    /* Read the response with a yielding window. The previous (pre-v0.47)
     * version waited up to 1000 ms with a busy delay(), which starved the
     * display task. This version yields with vTaskDelay() between reads, so
     * the display task keeps running even though the window is generous
     * enough (~500 ms) to reliably catch the module's TIM-SVIN reply (its
     * response latency can be 100-200 ms). Non-UBX bytes (NMEA) seen while
     * scanning are forwarded to TinyGPS++ so the position fix is not
     * disrupted by polling. */
    uint8_t buf[44]; uint8_t n = 0;
    int state = 0;
    const uint8_t MAX_SLICES = 50;          /* 50 × 10 ms = 500 ms cap */
    for (uint8_t slice = 0; slice < MAX_SLICES; slice++) {
        while (Serial1.available()) {
            uint8_t b = Serial1.read();
            if (state == 0) { if (b == 0xB5) state = 1; else gps.encode(b); }
            else if (state == 1) { state = (b == 0x62) ? 2 : 0; }
            else if (state == 2) { state = (b == cls) ? 3 : 0; }
            else if (state == 3) { state = (b == id) ? 4 : 0; n = 0; }
            else { if (n < sizeof(buf)) buf[n++] = b;
                   if (n >= sizeof(buf)) goto done; }
        }
        if (n >= 28) break;                 /* full payload already in */
        /* Before vTaskStartScheduler() this must not be vTaskDelay(): calling it
         * with no scheduler hangs the system. gpsdo_gps_init() now polls once, to
         * see whether the receiver is already in Time Mode, so this path is
         * genuinely reached both before and after the scheduler starts. */
        if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
            vTaskDelay(pdMS_TO_TICKS(10));  /* yield — does NOT starve displays */
        else
            delay(10);
    }
done:
    if (n < 28) return false;
    /* TIM-SVIN payload (buf[0..1] = length, payload starts at buf[2]):
     *   dur(u4)@0, meanX/Y/Z(i4)@4..15, meanV(u4)@16, obs(u4)@20,
     *   valid(u1)@24, active(u1)@25.
     * meanV is the position VARIANCE in mm² — take the square root to get a
     * 1-sigma accuracy in mm. Early in the survey the receiver reports
     * 0xFFFFFFFF ("no estimate yet"); we clamp that to 65535 mm. */
    *dur    = buf[2+0] | (buf[2+1]<<8) | (buf[2+2]<<16) | ((uint32_t)buf[2+3]<<24);
    uint32_t meanV = buf[2+16]| (buf[2+17]<<8)| (buf[2+18]<<16)| ((uint32_t)buf[2+19]<<24);
    /* integer sqrt (Newton) — meanV up to ~4e9 fits in u32 */
    uint32_t r = meanV, x = (meanV >> 1) + 1;
    if (meanV > 1) { while (x < r) { r = x; x = (meanV / x + x) >> 1; } }
    else           { r = meanV; }
    if (r > 65535u) r = 65535u;
    *acc_mm = r;                    /* sqrt(variance) = 1-sigma accuracy [mm] */
    *valid  = buf[2+24] != 0;
    *active = buf[2+25] != 0;
    return true;
}


/* Poll NAV-SVIN (0x01 0x3B) and parse the survey-in status — the Gen9/F9T
 * channel (TIM-SVIN may not answer on an F9T). Tested on real hardware by
 * EEVblog user danieljw.
 * Same out-params as ubx_poll_svin(). 40-byte payload, from u-blox docs:
 *   iTOW(u4)@0, dur(u4)@4 [s], meanX/Y/Z(i4)@8..19, meanXHP/YHP/ZHP@20..22,
 *   reserved@23, meanAcc(u4)@24 [0.1 mm], obs(u4)@28, valid(u1)@32,
 *   active(u1)@33.
 * NOTE meanAcc is a DIRECT accuracy in 0.1 mm units (no sqrt, unlike the
 * TIM-SVIN variance) — convert to mm by /10. */
static bool ubx_poll_svin_nav(uint32_t *dur, uint32_t *acc_mm,
                              bool *valid, bool *active)
{
    const uint8_t cls = 0x01, id = 0x3B;   /* NAV-SVIN, 40-byte payload */
    uint8_t poll[8] = { 0xB5,0x62, cls, id, 0,0, 0,0 };
    ubx_cksum(poll, sizeof(poll));
    send_ubx(poll, sizeof(poll));

    uint8_t buf[52]; uint8_t n = 0;
    int state = 0;
    const uint8_t MAX_SLICES = 50;          /* 50 × 10 ms = 500 ms cap */
    for (uint8_t slice = 0; slice < MAX_SLICES; slice++) {
        while (Serial1.available()) {
            uint8_t b = Serial1.read();
            if (state == 0) { if (b == 0xB5) state = 1; else gps.encode(b); }
            else if (state == 1) { state = (b == 0x62) ? 2 : 0; }
            else if (state == 2) { state = (b == cls) ? 3 : 0; }
            else if (state == 3) { state = (b == id) ? 4 : 0; n = 0; }
            else { if (n < sizeof(buf)) buf[n++] = b;
                   if (n >= sizeof(buf)) goto done2; }
        }
        if (n >= 40) break;                 /* full payload already in */
        /* SAME GUARD AS ubx_poll_svin(). This one was missed when that fix went
         * in, and it is a boot hang, not a cosmetic difference: vTaskDelay()
         * before vTaskStartScheduler() writes through pxCurrentTCB, which is
         * still NULL, so the board takes a hard fault and the default handler
         * spins with interrupts off — no output, no watchdog, nothing but the
         * reset button.
         *
         * It hid because of the order in gpsdo_gps_init(): NAV-SVIN is only
         * polled when TIM-SVIN did not answer inside its 500 ms window. A
         * LEA-6T that replies boots normally; one that is still starting up —
         * which is exactly the cold power-on case, as against a reset where the
         * receiver keeps running — falls through to here and the board stops
         * dead at the same line every time. Hence "it needs a few resets".
         *
         * Symptom-matched to the 20.08 16:04 log: four boots, each ending after
         * "UBX: CFG-NAV5 ACK" and before "LEA-T: starting survey-in", with the
         * reset cause reading PIN/NRST every time — the operator's button, not
         * a watchdog and not a brown-out. A supply fault does not stop at the
         * same source line four times. */
        if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
            vTaskDelay(pdMS_TO_TICKS(10));
        else
            delay(10);
    }
done2:
    if (n < 40) return false;
    /* payload starts at buf[2] (buf[0..1] = length) */
    *dur = buf[2+4] | (buf[2+5]<<8) | (buf[2+6]<<16) | ((uint32_t)buf[2+7]<<24);
    uint32_t meanAcc01 = buf[2+24]| (buf[2+25]<<8)| (buf[2+26]<<16)| ((uint32_t)buf[2+27]<<24);
    uint32_t mm = meanAcc01 / 10u;          /* 0.1 mm → mm */
    if (mm > 65535u) mm = 65535u;
    *acc_mm = mm;
    *valid  = buf[2+32] != 0;
    *active = buf[2+33] != 0;
    return true;
}


/* Non-blocking survey-in monitor — called periodically from vGpsTask AFTER
 * the scheduler is running. Polls TIM-SVIN once and updates the display
 * state. Clears g_svin_pending / g_svin_active when survey-in completes or
 * a safety deadline passes. Called from vGpsTask (scheduler running); the
 * underlying poll yields with vTaskDelay during its short read window, so
 * it does not starve the display task. */
static void ubx_poll_svin_step(void)
{
    static uint32_t svin_deadline = 0;
    static uint8_t  no_reply = 0;
    static uint32_t svin_dur_last = 0;   /* receiver's own elapsed survey time */
    /* Safety backstop tied to the configured minimum: 3 × SVIN_MIN, but never
     * less than 600 s, so a slow-converging survey (weak signal / small
     * antenna) is given a fair chance before we give up and run with whatever
     * fix the module has. */
    uint32_t svin_cap = GPSDO_SVIN_MIN_SECS * 3u;
    if (svin_cap < 600u) svin_cap = 600u;
    if (svin_deadline == 0) svin_deadline = millis() + svin_cap * 1000u;

    uint32_t dur=0, acc=0; bool valid=false, active=false;
    static bool ever_replied = false;
    /* TIM-SVIN first (LEA-6T/M8T). If it does not answer, fall back to
     * NAV-SVIN (ZED-F9T/Gen9). Whichever replies drives the
     * same completion logic below. */
    bool got = ubx_poll_svin(&dur, &acc, &valid, &active);
    if (!got) got = ubx_poll_svin_nav(&dur, &acc, &valid, &active);
    if (got) {
        no_reply = 0;
        ever_replied = true;
        g_svin_dur   = (uint16_t)dur;
        g_svin_acc_m = (uint16_t)(acc / 1000u);   /* mm → m for display */
        svin_dur_last = dur;
        OUT_SERIAL.print("LEA-T: svin dur="); OUT_SERIAL.print(dur);
        OUT_SERIAL.print("s acc=");             OUT_SERIAL.print(acc);
        OUT_SERIAL.print("mm valid=");          OUT_SERIAL.print(valid);
        OUT_SERIAL.print(" active=");           OUT_SERIAL.println(active);

        /* Survey-in is done when EITHER:
         *   (a) the receiver flags the mean position valid (valid=1) — it has
         *       settled, regardless of whether 'active' has flipped yet (some
         *       firmware leaves active=1 briefly after valid goes high), OR
         *   (b) the user's own criteria are met: accuracy below the limit AND
         *       at least the minimum duration elapsed. This covers receivers
         *       that are slow to raise 'valid' even after converging. */
        bool user_ok = (acc <= GPSDO_SVIN_ACC_LIMIT) &&
                       (dur >= GPSDO_SVIN_MIN_SECS);
        if (valid || user_ok) {
            OUT_SERIAL.print("LEA-T: survey-in complete (");
            OUT_SERIAL.print(valid ? "valid flag" : "acc+time criteria");
            OUT_SERIAL.println(") — time-only fix active");
            g_svin_active = false; g_svin_pending = false;
            return;
        }
    } else if (!ever_replied) {
        /* Neither TIM-SVIN nor NAV-SVIN replied AND we have never had one. The
         * module is likely not running a survey (plain nav mode, or the SVIN
         * message output is off) — stop monitoring after ~30 consecutive
         * misses rather than waiting the full safety window. Once a survey HAS
         * replied we never give up here (occasional misses are tolerated; the
         * survey is in progress). */
        if (++no_reply >= 30) {
            OUT_SERIAL.println("LEA-T: no TIM-SVIN/NAV-SVIN response — not in survey mode, stopping monitor");
            g_svin_active = false; g_svin_pending = false;
            return;
        }
    }
    /* Two clocks, and the receiver's is the one that matters. A timing module
     * keeps surveying across a host reset — it has its own power and its own
     * state — and reports its own elapsed time in dur. Measuring patience from
     * OUR boot therefore restarts the clock on every reflash: a survey that has
     * already run for an hour would still hold the foreground for another
     * svin_cap seconds, over and over. Seen on the bench, dur=4450 s against a
     * host uptime of 7 minutes, with the timeout message never printed. So give
     * up when EITHER clock passes the cap. */
    bool host_expired = (millis() > svin_deadline);
    bool svin_expired = (svin_dur_last > svin_cap);
    if (host_expired || svin_expired) {
        OUT_SERIAL.print("LEA-T: survey-in safety timeout (");
        OUT_SERIAL.print(svin_expired ? "receiver has surveyed " : "waited ");
        OUT_SERIAL.print(svin_expired ? svin_dur_last : (millis() / 1000u));
        OUT_SERIAL.println("s) — continuing anyway");
        /* We stop polling, but the receiver does NOT stop surveying. Flag it so
         * the display can keep a quiet indicator up until Time Mode arrives —
         * only mark it if the survey was actually replying, otherwise there is
         * nothing running to wait for. */
        if (ever_replied) g_svin_background = true;
        g_svin_active = false; g_svin_pending = false;
    }
}
#endif /* GPSDO_GPS_TIMING */

/*
 * CFG-NAV5 read-back and verification.
 *
 * Normal receiver configuration is deliberately volatile: this firmware does
 * not issue CFG-CFG/save.  Each initialization reads the receiver's current
 * NAV5 state, changes it only when needed, then polls it again and verifies the
 * values actually in use.  Read-back is authoritative: an ACK with unchanged
 * state is a failure, while a lost ACK followed by the requested state is OK.
 */
typedef struct {
    uint8_t  dyn_model;
    uint8_t  fix_mode;
    int32_t  fixed_alt_cm;
    uint32_t fixed_alt_var_1e4_m2;
    int8_t   min_elev_deg;
    uint16_t p_dop_x10;
    uint16_t t_dop_x10;
    uint16_t p_acc_m;
    uint16_t t_acc_m;
    uint8_t  static_hold_thresh_cm_s;
    uint8_t  dgps_timeout_s;
    uint8_t  cno_thresh_num_svs;
    uint8_t  cno_thresh_dbhz;
    uint16_t static_hold_max_dist_m;
    uint8_t  utc_standard;
} nav5_config_t;

static uint16_t nav5_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t nav5_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool nav5_parse(const uint8_t *p, uint16_t len, nav5_config_t *cfg)
{
    if (!p || !cfg || len < 36) return false;

    cfg->dyn_model               = p[2];
    cfg->fix_mode                = p[3];
    cfg->fixed_alt_cm            = (int32_t)nav5_u32(p + 4);
    cfg->fixed_alt_var_1e4_m2    = nav5_u32(p + 8);
    cfg->min_elev_deg            = (int8_t)p[12];
    cfg->p_dop_x10               = nav5_u16(p + 14);
    cfg->t_dop_x10               = nav5_u16(p + 16);
    cfg->p_acc_m                 = nav5_u16(p + 18);
    cfg->t_acc_m                 = nav5_u16(p + 20);
    cfg->static_hold_thresh_cm_s = p[22];
    cfg->dgps_timeout_s          = p[23];
    cfg->cno_thresh_num_svs      = p[24];
    cfg->cno_thresh_dbhz         = p[25];
    cfg->static_hold_max_dist_m  = nav5_u16(p + 28);
    cfg->utc_standard            = p[30];
    return true;
}

static bool ubx_poll_nav5(nav5_config_t *cfg)
{
    uint8_t payload[36];
    uint16_t len = 0;
    if (!gnss_poll_message(0x06, 0x24, payload, sizeof(payload), &len, 700))
        return false;
    return nav5_parse(payload, len, cfg);
}

static bool nav5_matches(const nav5_config_t *a, const nav5_config_t *b)
{
    return a && b &&
           a->dyn_model    == b->dyn_model &&
           a->fix_mode     == b->fix_mode &&
           a->min_elev_deg == b->min_elev_deg &&
           a->p_dop_x10    == b->p_dop_x10 &&
           a->t_dop_x10    == b->t_dop_x10 &&
           a->p_acc_m      == b->p_acc_m &&
           a->t_acc_m      == b->t_acc_m;
}
static const char *nav5_dyn_name(uint8_t v)
{
    switch (v) {
    case 0: return "portable";
    case 2: return "stationary";
    case 3: return "pedestrian";
    case 4: return "automotive";
    case 5: return "sea";
    case 6: return "airborne <1g";
    case 7: return "airborne <2g";
    case 8: return "airborne <4g";
    case 9: return "wrist";
    default: return NULL;
    }
}

static const char *nav5_fix_name(uint8_t v)
{
    switch (v) {
    case 1: return "2D only";
    case 2: return "3D only";
    case 3: return "automatic 2D/3D";
    default: return NULL;
    }
}

static void nav5_fmt_named(char *buf, size_t cap, uint8_t v,
                           const char *(*name_fn)(uint8_t))
{
    const char *name = name_fn(v);
    if (name) snprintf(buf, cap, "%s", name);
    else      snprintf(buf, cap, "value %u", (unsigned)v);
}

static void nav5_fmt_u32(char *buf, size_t cap, uint32_t v, const char *unit)
{
    if (unit && unit[0]) snprintf(buf, cap, "%lu %s", (unsigned long)v, unit);
    else                 snprintf(buf, cap, "%lu", (unsigned long)v);
}

static void nav5_fmt_i32(char *buf, size_t cap, int32_t v, const char *unit)
{
    if (unit && unit[0]) snprintf(buf, cap, "%ld %s", (long)v, unit);
    else                 snprintf(buf, cap, "%ld", (long)v);
}

static void nav5_fmt_tenths(char *buf, size_t cap, uint16_t v)
{
    snprintf(buf, cap, "%u.%u", (unsigned)(v / 10u), (unsigned)(v % 10u));
}



static void nav5_log_result(const char *label,
                            const char *before, bool had_before, bool before_ok,
                            const char *wanted,
                            const char *after, bool have_after, bool after_ok)
{
    if (had_before && before_ok) {
        if (have_after && after_ok) {
            gpsdo_logf(GPSDO_LOG_INFO, "GPS", "%s: %s - already set.",
                       label, wanted);
        } else if (have_after) {
            gpsdo_logf(GPSDO_LOG_WARN, "GPS",
                       "%s: was %s - FAILED (now %s).",
                       label, wanted, after);
        } else {
            gpsdo_logf(GPSDO_LOG_WARN, "GPS",
                       "%s: was already %s - FAILED (read-back unavailable).",
                       label, wanted);
        }
        return;
    }

    if (!have_after) {
        if (had_before) {
            gpsdo_logf(GPSDO_LOG_WARN, "GPS",
                       "%s: set from %s to %s - FAILED (read-back unavailable).",
                       label, before, wanted);
        } else {
            gpsdo_logf(GPSDO_LOG_WARN, "GPS",
                       "%s: wanted %s - FAILED (read-back unavailable).",
                       label, wanted);
        }
        return;
    }

    if (after_ok) {
        if (had_before) {
            gpsdo_logf(GPSDO_LOG_INFO, "GPS",
                       "%s: set from %s to %s - OK.",
                       label, before, wanted);
        } else {
            gpsdo_logf(GPSDO_LOG_INFO, "GPS",
                       "%s: %s - verified.", label, wanted);
        }
    } else {
        if (had_before) {
            gpsdo_logf(GPSDO_LOG_WARN, "GPS",
                       "%s: set from %s to %s - FAILED (still %s).",
                       label, before, wanted, after);
        } else {
            gpsdo_logf(GPSDO_LOG_WARN, "GPS",
                       "%s: wanted %s - FAILED (read back %s).",
                       label, wanted, after);
        }
    }
}

static void nav5_log_comparison(const nav5_config_t *before, bool had_before,
                                const nav5_config_t *wanted,
                                const nav5_config_t *after, bool have_after)
{
    char b[48], w[48], a[48];

#define NAV5_LOG_U32(label, field, unit) do { \
        nav5_fmt_u32(b, sizeof(b), had_before ? (uint32_t)before->field : 0u, unit); \
        nav5_fmt_u32(w, sizeof(w), (uint32_t)wanted->field, unit); \
        nav5_fmt_u32(a, sizeof(a), have_after ? (uint32_t)after->field : 0u, unit); \
        nav5_log_result(label, b, had_before, had_before && before->field == wanted->field, \
                        w, a, have_after, have_after && after->field == wanted->field); \
    } while (0)

    nav5_fmt_named(b, sizeof(b), had_before ? before->dyn_model : 0, nav5_dyn_name);
    nav5_fmt_named(w, sizeof(w), wanted->dyn_model, nav5_dyn_name);
    nav5_fmt_named(a, sizeof(a), have_after ? after->dyn_model : 0, nav5_dyn_name);
    nav5_log_result("Dynamic model", b, had_before,
                    had_before && before->dyn_model == wanted->dyn_model,
                    w, a, have_after,
                    have_after && after->dyn_model == wanted->dyn_model);

    nav5_fmt_named(b, sizeof(b), had_before ? before->fix_mode : 0, nav5_fix_name);
    nav5_fmt_named(w, sizeof(w), wanted->fix_mode, nav5_fix_name);
    nav5_fmt_named(a, sizeof(a), have_after ? after->fix_mode : 0, nav5_fix_name);
    nav5_log_result("Fix mode", b, had_before,
                    had_before && before->fix_mode == wanted->fix_mode,
                    w, a, have_after,
                    have_after && after->fix_mode == wanted->fix_mode);

    nav5_fmt_i32(b, sizeof(b), had_before ? before->min_elev_deg : 0, "deg");
    nav5_fmt_i32(w, sizeof(w), wanted->min_elev_deg, "deg");
    nav5_fmt_i32(a, sizeof(a), have_after ? after->min_elev_deg : 0, "deg");
    nav5_log_result("Minimum elevation", b, had_before,
                    had_before && before->min_elev_deg == wanted->min_elev_deg,
                    w, a, have_after,
                    have_after && after->min_elev_deg == wanted->min_elev_deg);

    nav5_fmt_tenths(b, sizeof(b), had_before ? before->p_dop_x10 : 0);
    nav5_fmt_tenths(w, sizeof(w), wanted->p_dop_x10);
    nav5_fmt_tenths(a, sizeof(a), have_after ? after->p_dop_x10 : 0);
    nav5_log_result("Position DOP limit", b, had_before,
                    had_before && before->p_dop_x10 == wanted->p_dop_x10,
                    w, a, have_after,
                    have_after && after->p_dop_x10 == wanted->p_dop_x10);

    nav5_fmt_tenths(b, sizeof(b), had_before ? before->t_dop_x10 : 0);
    nav5_fmt_tenths(w, sizeof(w), wanted->t_dop_x10);
    nav5_fmt_tenths(a, sizeof(a), have_after ? after->t_dop_x10 : 0);
    nav5_log_result("Time DOP limit", b, had_before,
                    had_before && before->t_dop_x10 == wanted->t_dop_x10,
                    w, a, have_after,
                    have_after && after->t_dop_x10 == wanted->t_dop_x10);

    NAV5_LOG_U32("Position accuracy limit", p_acc_m, "m");
    NAV5_LOG_U32("Time accuracy limit", t_acc_m, "m");

#undef NAV5_LOG_U32
}
/*
 * ubx_config — establish and verify UBX-CFG-NAV5 runtime configuration.
 *
 * The initial poll is read-only. If every requested value is already present,
 * nothing is written. Otherwise the full NAV5 configuration is attempted and
 * then read back. If the stationary dynamic model still did not take, retry
 * with the dynModel-only mask used by stripped/locked receiver firmware.
 */
static bool ubx_config(void)
{
    /* Only apply settings this GPSDO deliberately depends on.
     * CFG-NAV5 mask 0x0037 = dyn | minEl | posFixMode | posMask | timeMask.
     * Everything else remains receiver-owned and is not treated as a failure. */
    uint8_t setNav[44] = {
        0xB5, 0x62,                         /* UBX sync chars               */
        0x06, 0x24,                         /* class CFG, id NAV5           */
        0x24, 0x00,                         /* payload length = 36          */
        0x37, 0x00,                         /* mask: relevant fields only   */
        0x02,                               /* dynModel = stationary        */
        0x03,                               /* fixMode = auto 2D/3D         */
        0x00, 0x00, 0x00, 0x00,             /* fixedAlt = 0 cm (2D-only)    */
        0x10, 0x27, 0x00, 0x00,             /* fixedAltVar = 1 m^2           */
        0x05,                               /* minElev = 5 deg              */
        0x00,                               /* drLimit: reserved            */
        0xFA, 0x00,                         /* pDop = 25.0                  */
        0xFA, 0x00,                         /* tDop = 25.0                  */
        0x64, 0x00,                         /* pAcc = 100 m                 */
        0x2C, 0x01,                         /* tAcc = 300 m                 */
        0x00,                               /* static hold: not applied     */
        0x00,                               /* DGNSS timeout: not applied   */
        0x00, 0x00,                         /* C/N0 threshold: not applied  */
        0x00, 0x00,                         /* reserved                     */
        0x00, 0x00,                         /* static-hold distance: ignored*/
        0x00,                               /* UTC standard: not applied    */
        0x00,                               /* reserved                     */
        0x00, 0x00, 0x00, 0x00,             /* reserved                     */
        0x00, 0x00                          /* checksum filled below        */
    };
    ubx_cksum(setNav, sizeof(setNav));

    nav5_config_t wanted = {};
    if (!nav5_parse(setNav + 6, 36, &wanted)) {
        gpsdo_log_event(GPSDO_LOG_WARN, "GPS",
                        "Internal NAV5 configuration could not be decoded.");
        return false;
    }

    nav5_config_t before = {};
    bool had_before = ubx_poll_nav5(&before);

    if (had_before && nav5_matches(&before, &wanted)) {
        nav5_log_comparison(&before, true, &wanted, &before, true);
        return true;
    }

    /* Read-back, not ACK, is authoritative for NAV5. Suppress a raw NAK here:
     * if the receiver already ends up with the requested values there is no
     * operator-facing problem to warn about. */
    send_ubx(setNav, sizeof(setNav));
    (void)get_ubx_ack(setNav, 1500, false);

    nav5_config_t after = {};
    bool have_after = ubx_poll_nav5(&after);

    if (have_after && nav5_matches(&after, &wanted)) {
        nav5_log_comparison(&before, had_before, &wanted, &after, true);
        return true;
    }

    /* Preserve the compatibility fallback for the one setting that matters
     * most to a stationary GPSDO. */
    bool need_dyn_retry = !have_after || after.dyn_model != wanted.dyn_model;
    if (need_dyn_retry) {
        uint8_t setNavMin[sizeof(setNav)];
        memcpy(setNavMin, setNav, sizeof(setNav));
        setNavMin[6] = 0x01;
        setNavMin[7] = 0x00;                 /* mask: dynModel only */
        ubx_cksum(setNavMin, sizeof(setNavMin));
        flush_rx(50);
        send_ubx(setNavMin, sizeof(setNavMin));
        (void)get_ubx_ack(setNavMin, 1500, false);

        nav5_config_t final_cfg = {};
        if (ubx_poll_nav5(&final_cfg)) {
            after = final_cfg;
            have_after = true;
        }
    }

    if (!have_after) {
        gpsdo_log_event(GPSDO_LOG_WARN, "GPS",
                        "Navigation configuration could not be verified by read-back.");
        return false;
    }

    nav5_log_comparison(&before, had_before, &wanted, &after, true);
    return nav5_matches(&after, &wanted);
}
#endif /* UBX_CONFIG */

/* ---- Tunnel mode ------------------------------------------------------ */
/*
 * run_tunnel_mode — transparent serial bridge between CLI_SER and Serial1.
 *
 * Fix: notifies DisplayTask once per second so active displays keep
 * updating with the last valid gGps snapshot during the bridge session.
 * Without this notification both displays froze for the entire tunnel
 * timeout (up to 300 s).
 */
volatile uint32_t g_tunnel_baud = 0;   /* T <baud>: GPS UART rate for the bridge (0 = keep) */

static void run_tunnel_mode(void)
{
    /* The bridge ALWAYS runs on the USB CDC (Serial): with Bluetooth enabled
     * the CLI and telemetry live on Serial2, so USB becomes a clean,
     * dedicated service port for u-center; without Bluetooth the 1 Hz report
     * is muted for the session (see vDisplayTask) so nothing pollutes the
     * NMEA/UBX stream. USB CDC has no real baud rate — only the GPS-side
     * UART matters. If the CLI passed "T <baud>", reopen Serial1 at that
     * rate and KEEP it afterwards (u-center may have re-configured the
     * receiver; the firmware must follow to keep parsing NMEA). */
    if (g_tunnel_baud != 0) {
        Serial1.flush();
        Serial1.begin(g_tunnel_baud);
    }
    CLI_SER.println("Entering tunnel mode...");
#if defined(GPSDO_BLUETOOTH) || defined(GPSDO_BLUETOOTH_PARALLEL)
    Serial.println("GPS tunnel active on this USB port.");
#endif
    uint32_t end_ms        = millis() + (TUNNEL_TIMEOUT_SECS * 1000UL);
    uint32_t last_notify   = millis();

    while (millis() < end_ms) {
        /* Drain both directions completely before yielding. */
        while (Serial1.available())
            Serial.write(Serial1.read());

        while (Serial.available())
            Serial1.write(Serial.read());

        /* Notify DisplayTask ~1 Hz so displays stay alive */
        uint32_t now = millis();
        if ((now - last_notify) >= 1000UL) {
            last_notify = now;
            if (xDisplayTask != NULL)
                xTaskNotifyGive(xDisplayTask);
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
    CLI_SER.println("Tunnel mode exited.");
    xEventGroupClearBits(xSysEvents, EVT_TUNNEL_MODE);
}

/* ---- GPS task --------------------------------------------------------- */
/*
 * Read strategy:
 *   - Drain Serial1 RX buffer in bursts of GPS_READ_BURST_MS milliseconds
 *   - Yield for GPS_YIELD_MS between bursts so other tasks run
 *   - TinyGPS++ marks fields as "updated" once per complete sentence
 *   - Publish gGps and notify DisplayTask as soon as time sentence arrives
 *     (time arrives every second; location every second in 1Hz mode)
 *
 * NMEA verbose echo:
 *   - Each echoed byte is collected into a small stack buffer
 *   - Flushed via direct write() call per burst
 *   - Avoids per-byte Serial.write() + mutex acquire overhead
 */
#define GPS_READ_BURST_MS    8u      /* ms to drain RX per burst               */
#define GPS_YIELD_MS         5u      /* ms to yield between bursts              */
#define NMEA_TIMEOUT_MS   3000u      /* mark time invalid after 3s no sentence  */
#define POS_LOST_TIMEOUT_MS 10000u   /* mark pos invalid after 10s without fix  */

/*
 * Two independent staleness timeouts:
 *
 * NMEA_TIMEOUT_MS (3 s):
 *   If NO NMEA sentence at all is received for 3 s, the GPS module has
 *   stopped communicating entirely (power loss, UART failure, etc.).
 *   Clear both valid and pos_valid.
 *
 * POS_LOST_TIMEOUT_MS (10 s):
 *   The GPS module may keep sending NMEA sentences (time, status) even
 *   without an antenna because of its internal RTC / almanac.  In this
 *   case NMEA arrives normally so NMEA_TIMEOUT_MS never fires, but
 *   gps.location.isValid() returns false and the satellite count drops
 *   to 0.  We track the last time a genuine position fix was received
 *   and clear pos_valid (plus zero out position/sats) after 10 s of no
 *   valid location sentence.  This ensures active displays show the
 *   "no fix" state within 10 s of antenna disconnection.
 *
 * gGps.valid (time valid) follows NMEA presence.
 * gGps.pos_valid follows position fix availability.
 * Both are only SET here; they are never set by the display task.
 */
void vGpsTask(void *pvParameters)
{
    (void)pvParameters;

    uint32_t last_sentence_ms  = millis();   /* last NMEA byte received        */
    uint32_t last_pos_valid_ms = millis();   /* last genuine position fix      */
    bool     had_fix           = false;
    bool     had_pos           = false;

#ifdef GPSDO_VERBOSE_NMEA
    /* Small buffer to batch NMEA echo bytes — avoids per-byte mutex acquire */
    static uint8_t echo_buf[128];
    int echo_len = 0;
#endif

    for (;;)
    {
        /* ---- Tunnel mode ---- */
        if (xEventGroupGetBits(xSysEvents) & EVT_TUNNEL_MODE) {
            run_tunnel_mode();
            last_sentence_ms = millis();
            continue;
        }

        /* ---- Drain Serial1 RX for GPS_READ_BURST_MS ---- */
        uint32_t burst_end = millis() + GPS_READ_BURST_MS;
        while (millis() < burst_end) {
            int avail = Serial1.available();
            if (avail <= 0) break;

            /* Read up to 32 bytes per inner loop to stay in burst window */
            int to_read = avail < 32 ? avail : 32;
            for (int i = 0; i < to_read; i++) {
                uint8_t c = Serial1.read();
                gps.encode(c);
                ubx_timtp_feed(c);   /* parallel UBX sniffer for TIM-TP qErr */

#ifdef GPSDO_VERBOSE_NMEA
                /* Only echo in human-readable mode (not tab-delimited) */
                if (!(xEventGroupGetBits(xSysEvents) & EVT_REPORT_TAB)) {
                    if (echo_len < (int)sizeof(echo_buf))
                        echo_buf[echo_len++] = c;
                }
#endif
            }
        }

        /* ---- Flush NMEA echo buffer (under serial mutex) ---- */
#ifdef GPSDO_VERBOSE_NMEA
        if (echo_len > 0) {
            if (xSemaphoreTake(xSerialMutex, portMAX_DELAY) == pdTRUE) {
                CLI_SER.write(echo_buf, echo_len);
                xSemaphoreGive(xSerialMutex);
            }
            echo_len = 0;
        }
#endif

        /* ---- Check for fresh TinyGPS++ data ---- */
        bool time_fresh     = gps.time.isUpdated();
        bool location_fresh = gps.location.isUpdated();
        bool date_fresh     = gps.date.isUpdated();

        if (time_fresh || location_fresh) {
            last_sentence_ms = millis();
            had_fix = true;

            if (xSemaphoreTake(xGpsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (time_fresh) {
                    gGps.hours = gps.time.hour();
                    gGps.mins  = gps.time.minute();
                    gGps.secs  = gps.time.second();
                    gGps.valid = true;
                }
                if (location_fresh && gps.location.isValid()) {
                    gGps.lat       = (float)gps.location.lat();
                    gGps.lon       = (float)gps.location.lng();
                    gGps.alt       = (float)gps.altitude.meters();
                    gGps.sats      = (uint8_t)gps.satellites.value();
                    gGps.hdop      = gps.hdop.value();
                    gGps.pos_valid = true;
                    /* Time Mode detection: a timing receiver in time-only mode
                     * keeps a valid (frozen) position but stops optimising it,
                     * so HDOP is reported as ~99.99 (value >= 5000 = 50.00).
                     * A normal navigation fix with that HDOP would be unusable,
                     * so treat "valid position + absurd HDOP" as Time Mode and
                     * show HDOP:TIME instead of a meaningless number.        */
                    gGps.time_mode = (gGps.hdop >= 5000);
                    /* Time Mode is the survey's real completion signal — if one
                     * was still running past our monitor window, it's done now
                     * and the display indicator can go. */
                    if (gGps.time_mode) g_svin_background = false;
                    last_pos_valid_ms = millis();   /* genuine fix — reset position timer */
                    had_pos = true;
                } else if (location_fresh) {
                    /* GGA/RMC received but no valid fix: update sats/HDOP so the
                     * display shows the actual current satellite count (may be 0)
                     * rather than a stale cached value from before antenna loss. */
                    gGps.sats = (uint8_t)gps.satellites.value();
                    gGps.hdop = gps.hdop.value();
                }
                if (date_fresh) {
                    gGps.day   = gps.date.day();
                    gGps.month = gps.date.month();
                    gGps.year  = (uint16_t)gps.date.year();
                }
                xSemaphoreGive(xGpsMutex);
            }

            /* Feed the central event logger from the receiver's trusted UTC.
             * The logger maintains this time from millis() between updates, so
             * LOG1 timestamps survive position loss and complete NMEA loss. */
            if (time_fresh && gps.time.isValid() && gps.date.isValid()) {
                gpsdo_log_set_utc(
                    (uint16_t)gps.date.year(),
                    (uint8_t)gps.date.month(),
                    (uint8_t)gps.date.day(),
                    (uint8_t)gps.time.hour(),
                    (uint8_t)gps.time.minute(),
                    (uint8_t)gps.time.second());
            }

            /*
             * Notify DisplayTask at most once per second.
             * Without this guard, multiple NMEA sentences decoded in one
             * 8ms burst (GGA, RMC, GSA all arrive in rapid succession)
             * would each trigger a display update, causing 3-5 redraws
             * per second and flooding the BT serial port.
             *
             * Gate: notify only when the GPS second counter has advanced.
             * time_fresh is set by TinyGPS++ for every sentence that carries
             * time — so limit to one notify per unique second value.
             */
            if (time_fresh && xDisplayTask != NULL) {
                static uint8_t last_notified_sec = 0xFF;
                uint8_t cur_sec = gps.time.second();
                if (cur_sec != last_notified_sec) {
                    last_notified_sec = cur_sec;
                    xTaskNotifyGive(xDisplayTask);
                }
            }
        }

        /* ---- Timeout 1: NMEA stream gone (3 s) --------------------------------
         * No bytes at all — module powered off, UART broken, etc.
         * Clear everything: time, position, satellite count.
         * -------------------------------------------------------------------*/
        if (had_fix && (millis() - last_sentence_ms) > NMEA_TIMEOUT_MS) {
            if (xSemaphoreTake(xGpsMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                gGps.valid     = false;
                gGps.pos_valid = false;
                gGps.sats      = 0;
                xSemaphoreGive(xGpsMutex);
            }
            if (xSemaphoreTake(xFreqMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                gFreq.flush_requested = true;
                xSemaphoreGive(xFreqMutex);
            }
            had_fix = false;
            had_pos = false;
        }

        /* ---- Timeout 2: position fix lost (10 s) ---------------------------
         * NMEA is still arriving (time sentences from internal RTC) but no
         * valid location fix — antenna disconnected or signal blocked.
         * Clear pos_valid and zero position data so displays reflect reality.
         * gGps.valid (time) is left intact: the time itself is still valid
         * from the module's internal clock; only the position is stale.
         * -------------------------------------------------------------------*/
        if (had_pos && (millis() - last_pos_valid_ms) > POS_LOST_TIMEOUT_MS) {
            if (xSemaphoreTake(xGpsMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                gGps.pos_valid = false;
                gGps.time_mode = false;
                gGps.lat       = 0.0f;
                gGps.lon       = 0.0f;
                gGps.alt       = 0.0f;
                gGps.sats      = 0;
                gGps.hdop      = 9999;   /* 99.99 — indicates no fix */
                xSemaphoreGive(xGpsMutex);
            }
            if (xSemaphoreTake(xFreqMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                gFreq.flush_requested = true;
                xSemaphoreGive(xFreqMutex);
            }
            had_pos = false;
        }

#ifdef GPSDO_GPS_TIMING
        /* Survey-in progress monitor (scheduler is running now, so this is
         * the safe place to poll — never in gpsdo_gps_init()). Throttle to
         * ~1 Hz so it does not interfere with NMEA draining. */
        if (g_svin_pending) {
            static uint32_t next_svin_poll = 0;
            if (millis() >= next_svin_poll) {
                next_svin_poll = millis() + 1000u;
                ubx_poll_svin_step();
            }
        }
#endif

        /* Age out stale qErr if TIM-TP stops arriving (receiver reset etc.) */
        ubx_timtp_tick(millis());

        /* Yield between bursts — allows higher-priority tasks to run */
        vTaskDelay(pdMS_TO_TICKS(GPS_YIELD_MS));
    }
}

/*
 * gpsdo_gps_init — robust GPS hardware initialisation
 *
 * Problems fixed vs previous version:
 *
 * 1. BLIND BAUD-RATE ASSUMPTION
 *    Original always opened at 9600 and sent PUBX,41 to switch to 38400.
 *    After an STM32 reset (without GPS power cycle) the module is already
 *    at 38400 — PUBX sent at 9600 is ignored, ubx_config() loops forever.
 *    Fix: auto-detect the current baud rate before sending any config.
 *
 * 2. UNVERIFIED BAUD-RATE SWITCH (PUBX,41)
 *    PUBX,41 has no UBX-ACK reply — the module switches baud mid-sentence.
 *    Original waited 100ms blindly, which could land in the middle of a
 *    last NMEA sentence still draining at 9600 baud.
 *    Fix: flush TX completely, then wait 150ms (> max NMEA sentence time
 *    at 9600 = 82ms for longest sentence) before reopening at 38400,
 *    then flush the RX buffer to discard framing garbage.
 *
 * 3. taskYIELD() IN get_ubx_ack() BEFORE SCHEDULER
 *    Called from setup() before vTaskStartScheduler() → taskYIELD() is
 *    either a no-op or causes undefined behaviour on STM32duino port.
 *    Fix: get_ubx_ack() now detects scheduler state and uses delay(1)
 *    before scheduler, vTaskDelay(1) after.
 *
 * 4. LONG BLOCKING delay(3000) × 2 IN setup()
 *    Fix: first delay reduced to 500ms (enough for module cold-start
 *    to emit first NMEA); second delay eliminated — ubx_config() waits
 *    for ACK itself with a proper timeout.
 *
 * Auto-detect strategy:
 *   Try each candidate baud rate in descending order. At each rate,
 *   send a UBX-CFG-PRT poll request (B5 62 06 00 01 00 01 ...) and
 *   wait up to 300ms for any UBX or NMEA response. First rate that
 *   gets a response wins. Falls back to 9600 if nothing responds.
 *
 * Called from setup() before vTaskStartScheduler().
 */

/* Candidate baud rates, most likely first.
 *
 * 19200 was missing: it is one of the rates u-blox modules ship configured to
 * and a board on it would have fallen through the whole sweep.
 *
 * ORDER IS NOT LOAD-BEARING — but only because of the checksum test in
 * nmea_sentence_ok() below, and that is worth recording. With the earlier
 * criterion of "a single 0x24 arrived", it WAS: simulated across every pair of
 * these five rates, a module at 38400 listened to to at 57600 produces a '$' out
 * of framing garbage in 24 runs out of 24, so a list with 57600 ahead of 38400
 * would have mis-detected every 38400 board. Requiring a whole sentence with a
 * valid checksum removes every false positive in the 5x5 matrix, which is what
 * frees the order to be about boot speed alone. Do not weaken the test back to
 * a byte match and keep the order.
 *
 * 38400 first because that is what the boards in the field are set to; 9600
 * second because it is the u-blox factory default. */
static const uint32_t BAUD_CANDIDATES[] = { 38400, 9600, 19200, 57600, 115200 };
static const uint8_t  N_BAUD_CANDIDATES = sizeof(BAUD_CANDIDATES)/sizeof(BAUD_CANDIDATES[0]);

/* UBX-CFG-PRT poll (port 1 = UART1) — asks module to reply with its
 * current port config. Any valid UBX or NMEA response means the baud
 * rate is correct. */
static const uint8_t UBX_POLL_PRT[] = {
    0xB5, 0x62, 0x06, 0x00, 0x01, 0x00, 0x01, 0x08, 0x22
};
/*   B5 62  — header
 *   06 00  — class CFG, id PRT
 *   01 00  — length = 1
 *   01     — port ID = 1 (UART1)
 *   08 22  — checksum (CK_A=0x08, CK_B=0x22)            */

/* One NMEA sentence must fit inside the listening window, and the stream is
 * 1 Hz, so the window has to span a whole period plus the longest sentence.
 * Computed and then simulated: at 350 ms a complete sentence lands inside the
 * window 35-60% of the time depending on rate and how chatty the module is —
 * meaning the sweep missed the TRUE rate about half the time and fell through
 * to the active probe. At 1100 ms it is 100% for every rate from 9600 to
 * 115200 and for both a GGA+RMC-only module and one with all six sentences on.
 *
 * Cost at boot: a wrong candidate burns the full window, so a module at the
 * last candidate costs about 4.4 s before it is found. The OCXO needs 300 s to
 * warm up, so this is not a number worth optimising against correctness. */
#define NMEA_SWEEP_WINDOW_MS  1100u

static int nmea_hexdig(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Is the port carrying REAL NMEA — a complete sentence whose checksum agrees?
 *
 * "A '$' arrived" is a different and much weaker question, and the difference
 * is the whole point. Bytes read at the wrong baud are not random: at an
 * integer ratio every transmitted bit becomes several received bits, so the
 * garbage is dominated by runs (0x80, 0xE0, 0xFE) and a '$' — 0b00100100,
 * which needs alternation — essentially cannot appear. At 1.5:1 it can, and
 * does: 38400 heard at 57600 yields one every time. The checksum closes that
 * door, because garbage agreeing with its own XOR is a 1-in-256 accident that
 * also has to arrive wrapped in $...* with two hex digits.
 *
 * Simulated over all 25 transmit/listen pairs of the five candidate rates,
 * feeding real sentence text through an 8N1 transmitter and a bit-sampling
 * receiver: 25/25 correct, 0 false positives. See tools/uartsim/. */
static bool nmea_sentence_ok(uint32_t window_ms)
{
    char    body[84];          /* NMEA caps a sentence at 82 incl. $ and CRLF */
    uint8_t n  = 0;
    bool    in = false;

    uint32_t t0 = millis();
    while ((millis() - t0) < window_ms) {
        while (Serial1.available()) {
            int ci = Serial1.read();
            if (ci < 0) break;
            char c = (char)ci;

            if (c == '$') { in = true; n = 0; continue; }
            if (!in) continue;

            if (c == '\r' || c == '\n') {
                /* body + '*' + two hex digits */
                if (n >= 4 && body[n - 3] == '*') {
                    uint8_t sum = 0;
                    for (uint8_t k = 0; k < (uint8_t)(n - 3); k++)
                        sum ^= (uint8_t)body[k];
                    int hi = nmea_hexdig(body[n - 2]);
                    int lo = nmea_hexdig(body[n - 1]);
                    if (hi >= 0 && lo >= 0 &&
                        sum == (uint8_t)((hi << 4) | lo))
                        return true;
                }
                in = false; n = 0;
            } else if (n < (uint8_t)sizeof(body)) {
                body[n++] = c;
            } else {
                in = false; n = 0;      /* over-long: not a sentence */
            }
        }
        delay(1);
    }
    return false;
}

static uint32_t detect_nmea_baud(void)
{
    /* Passive sweep FIRST: listen at every candidate baud before any poll is
     * sent. Chinese clones auto-baud their RX onto whatever binary poll
     * arrives and ACK it, while their NMEA TX keeps running at its own rate —
     * the ACK then names the wrong baud and the display sits on "acquiring"
     * forever (field report, Solder Junkie). Listening disturbs nothing: no
     * bytes leave the STM32. */
    for (uint8_t i = 0; i < N_BAUD_CANDIDATES; i++) {
        uint32_t baud = BAUD_CANDIDATES[i];
        Serial1.end();
        Serial1.begin(baud);
        delay(50);
        while (Serial1.available()) Serial1.read();

        if (nmea_sentence_ok(NMEA_SWEEP_WINDOW_MS)) {
            gpsdo_logf(GPSDO_LOG_INFO, "GPS",
                       "NMEA traffic detected at %lu baud.",
                       (unsigned long)baud);

            s_gnss_info.detected = true;
            s_gnss_info.nmea_seen = true;
            s_gnss_info.baud = baud;

            while (Serial1.available()) Serial1.read();
            return baud;
        }
    }
    return 0;   /* no NMEA anywhere — caller may poll actively */
}

static uint32_t detect_gps_baud(void)
{
    uint32_t nmea = detect_nmea_baud();

    if (nmea != 0) {
        /*
         * Presence is already proven by a valid NMEA sentence.
         * MON-VER is best-effort identification; failure does not undo
         * the valid NMEA detection.
         */
        /* Best-effort identity poll. Keep detection quiet here; the
         * detailed identity block later in initialization is the single
         * user-facing receiver result. */
        (void)ubx_poll_mon_ver();
        return nmea;
    }

    /*
     * No valid NMEA was seen. Try a checksum-valid UBX-MON-VER response
     * at each candidate baud. Random serial bytes are not detection.
     */
    for (uint8_t i = 0; i < N_BAUD_CANDIDATES; i++) {
        uint32_t baud = BAUD_CANDIDATES[i];

        Serial1.end();
        Serial1.begin(baud);
        delay(50);
        while (Serial1.available()) Serial1.read();

        if (ubx_poll_mon_ver()) {
            s_gnss_info.baud = baud;
            gpsdo_logf(GPSDO_LOG_INFO, "GPS",
                       "UBX traffic detected at %lu baud.",
                       (unsigned long)baud);
            return baud;
        }

        gpsdo_logf(GPSDO_LOG_INFO, "GPS",
                "No receiver response at %lu baud; trying another serial speed.",
                (unsigned long)baud);
    }

    gpsdo_log_event(GPSDO_LOG_WARN, "GPS",
                    "Receiver did not respond at the tested serial speeds.");
    return 0;
}

/*
 * gpsdo_gps_init — robust GPS hardware initialisation for budget modules
 *
 * Strategy that works reliably on GY-NEO6MV2 / NEO-6M (AliExpress) as
 * well as genuine u-blox evaluation modules:
 *
 * 1. detect_gps_baud() probes all candidate baud rates and returns the
 *    one the module is currently using.
 *
 * 2. ubx_config() is sent AT THE DETECTED BAUD RATE.
 *    Root cause of the "3 attempts all fail" hang: the code previously
 *    switched to 38400 via PUBX,41 then sent UBX at 38400 — but the
 *    NEO-6M on GY-NEO6MV2 defaults to 9600 on power-up (the PUBX,41
 *    may not take effect reliably in every flash variant). The module
 *    received UBX at a baud rate it wasn't listening on → no ACK → hang.
 *    Sending UBX at the detected baud rate avoids this entirely.
 *    GPSDO operation does not require 38400 baud; NMEA at 1 Hz is well
 *    within the 9600 capacity.
 *
 * 3. Optional upgrade to 38400 (GPSDO_PREFER_38400):
 *    Enabled by default. After a successful ubx_config() at the detected
 *    baud, we try to switch to 38400 to reduce ACK latency for any
 *    future UBX polls. This is purely optional and non-fatal.
 *
 * 4. If detect_gps_baud() returns 0 (no response), we default to 9600
 *    and proceed. NMEA may still arrive even if UBX-CFG-PRT poll failed.
 */

/* Enable an opportunistic upgrade to 38400 after successful UBX config.
 * Comment this out to stay at the detected baud rate always. */
#define GPSDO_PREFER_38400


void gpsdo_gps_init(void)
{
    memset(&s_gnss_info, 0, sizeof(s_gnss_info));

#ifdef GPSDO_UBX_CONFIG
    /* ---- Step 1: detect current baud rate ---- */
    uint32_t working_baud = detect_gps_baud();
    if (working_baud == 0) {
        /* No response at any baud — assume 9600 and continue.
         * Some modules need extra settle time after power-on;
         * NMEA may still arrive and GPSDO will work without UBX config. */
        gpsdo_log_event(GPSDO_LOG_WARN, "GPS",
                        "Receiver did not answer the serial-speed probe; using 9600 baud.");
        working_baud = 9600;
        Serial1.end();
        Serial1.begin(9600);
        flush_rx(200);
        gpsdo_log_event(GPSDO_LOG_INFO, "GPS",
                        "Initialization complete without receiver configuration.");
        return;
    }

    /* Ensure Serial1 is open at the confirmed working baud */
    Serial1.end();
    Serial1.begin(working_baud);
    flush_rx(150);

    /* Read-only identity poll at the confirmed receiver baud. */
    if (ubx_poll_mon_ver()) {
        gnss_log_detailed_identity();
    } else {
        gpsdo_log_event(GPSDO_LOG_WARN,"GPS",
                        "Receiver speaks UBX, but MON-VER identification did not reply.");
    }

#ifdef GPSDO_FAKE_UBLOX
    /* Chinese u-blox clones (navigation-grade M8N copies and friends)
     * answer the baud probe but ignore CFG-MSG/CFG-NAV5, lack TMODE2
     * survey-in and TIM-TP, and the stream of unanswered binary config
     * frames can upset their auto-baud engine — the module then stops
     * producing NMEA and the display sits on "acquiring" until a T-tunnel
     * re-probe rescues it (field report, Solder Junkie). Here: probe the
     * baud, send NOTHING else, and let NMEA do the talking. Discipline is
     * unaffected — PPS capture and the counter/detector never needed UBX.
     * Lost with the config: NMEA silencing, stationary mode, survey-in /
     * Time Mode (position stays 3D-nav; the extra phase wander is exactly
     * what the loose default algo-12 limits were sized for) and qErr. */
    gpsdo_log_event(GPSDO_LOG_WARN, "GPS",
                    "Receiver appears clone/compatible; skipping UBX configuration.");
    gpsdo_log_event(GPSDO_LOG_INFO, "GPS",
                    "Initialization complete.");
    return;
#endif

    /* ---- Step 2: silence noisy NMEA sentences ----
     *
     * Root cause of the CFG-NAV5 ACK loss: at 38400 baud NMEA output
     * fills the 256-byte STM32 RX ring buffer in ~67 ms. The 10-byte
     * ACK response for CFG-NAV5 arrives ~100 ms after the command and
     * can be overwritten / dropped before get_ubx_ack() reads it.
     *
     * ubx_reduce_nmea() disables GLL, GSA, GSV, VTG (keeps GGA + RMC).
     * Residual NMEA rate drops from ~500 B/s to ~150 B/s.
     * At 150 B/s the 256-byte buffer holds 1.7 s — safe for a 1500 ms
     * ACK wait with no overflow risk.
     *
     * CFG-MSG commands for each disabled sentence receive their own ACK,
     * which also verifies that UBX write commands work on this firmware.
     */
    uint8_t nmea_ok = ubx_reduce_nmea();
    if (nmea_ok == 4) {
        gpsdo_log_event(GPSDO_LOG_INFO, "GPS",
                        "Disabled 4 of 4 noisy NMEA sentences.");
    } else {
        gpsdo_logf(GPSDO_LOG_WARN, "GPS",
                "Disabled %u of 4 noisy NMEA sentences; some settings were not accepted.",
                (unsigned)nmea_ok);
    }

    /* ---- Step 3: send UBX-CFG-NAV5 at working baud ---- */
    bool cfg_ok = ubx_config();

#ifdef GPSDO_GPS_TIMING
    /* LEA-6T/M8T: replace plain stationary mode with survey-in → Time Mode.
     * Survey-in can be disabled at runtime (CLI "SV 0", stored in EEPROM) —
     * handy for bench testing, where you may want plain nav mode.
     * IMPORTANT: only SEND the survey-in start here. The progress-polling
     * loop must NOT run before the scheduler — it uses vTaskDelay(), which
     * hangs the system if called before vTaskStartScheduler(). Monitoring
     * happens in vGpsTask once the scheduler is running. */
    if (!g_svin_enabled) {
        gpsdo_log_event(GPSDO_LOG_INFO, "GPS",
                        "Timing: survey-in is disabled; staying in navigation mode.");
    } else {
        /* Ask first, command second. After a warm reboot (RB) the receiver keeps
         * its own power and its own state, so a survey completed before the reset
         * is still valid — the position it established has not moved. Starting a
         * fresh one there discards a result that took minutes to reach and drops
         * the module out of Time Mode while it repeats work already done.
         *
         * TIM-SVIN reports valid=1 with active=0 exactly when the receiver is in
         * Time Mode with a finished survey behind it, which is the condition worth
         * skipping for. This runs before the scheduler, which is why ubx_poll_svin()
         * had to stop calling vTaskDelay() unconditionally. */
        bool timing_state_known = false;

        /* Query first: do not disturb a completed Time Mode fix or a survey
         * that is already running. */
        {
            uint32_t d0 = 0, a0 = 0;
            bool v0 = false, act0 = false;

            bool got = false;
            for (uint8_t attempt = 0; attempt < 2 && !got; ++attempt) {
                got = ubx_poll_svin(&d0, &a0, &v0, &act0);
                if (!got)
                    got = ubx_poll_svin_nav(&d0, &a0, &v0, &act0);

                /* A busy receiver can miss one status poll immediately after
                 * configuration. Do not interpret a single miss as "no survey
                 * is running" and then unnecessarily re-issue Time Mode. */
                if (!got && attempt == 0)
                    delay(100);
            }

            if (got && v0 && !act0) {
                timing_state_known = true;

                gpsdo_logf(
                    GPSDO_LOG_INFO, "GPS",
                    "Timing: already in Time Mode from an earlier survey "
                    "(%lu s, %lu m).",
                    (unsigned long)d0,
                    (unsigned long)(a0 / 1000u));

                g_svin_active  = false;
                g_svin_pending = false;
                g_svin_dur     = (uint16_t)((d0 > 65535u) ? 65535u : d0);
                g_svin_acc_m   = (uint16_t)(a0 / 1000u);

            } else if (got && act0) {
                timing_state_known = true;

                gpsdo_logf(
                    GPSDO_LOG_INFO, "GPS",
                    "Timing: survey-in is already active (%lu s, %lu m).",
                    (unsigned long)d0,
                    (unsigned long)(a0 / 1000u));

                g_svin_active  = true;
                g_svin_pending = true;
                g_svin_dur     = (uint16_t)((d0 > 65535u) ? 65535u : d0);
                g_svin_acc_m   = (uint16_t)(a0 / 1000u);
            }
        }

        if (!timing_state_known) {
            if (ubx_start_survey_in()) {
                g_svin_active  = true;
                g_svin_pending = true;
                g_svin_dur     = 0;
                g_svin_acc_m   = 0;

            } else {
                /*
                 * The command was not acknowledged. Query again before
                 * reporting a failure: the ACK may have been missed, or the
                 * receiver may already have entered Time Mode / survey-in.
                 */
                uint32_t d1 = 0, a1 = 0;
                bool v1 = false, act1 = false;

                bool got = ubx_poll_svin(&d1, &a1, &v1, &act1);
                if (!got)
                    got = ubx_poll_svin_nav(&d1, &a1, &v1, &act1);

                if (got && v1 && !act1) {
                    gpsdo_logf(
                        GPSDO_LOG_INFO, "GPS",
                        "Timing: receiver is already in Time Mode "
                        "(%lu s, %lu m).",
                        (unsigned long)d1,
                        (unsigned long)(a1 / 1000u));

                    g_svin_active  = false;
                    g_svin_pending = false;
                    g_svin_dur     = (uint16_t)((d1 > 65535u) ? 65535u : d1);
                    g_svin_acc_m   = (uint16_t)(a1 / 1000u);

                } else if (got && act1) {
                    gpsdo_logf(
                        GPSDO_LOG_INFO, "GPS",
                        "Timing: survey-in is active (%lu s, %lu m).",
                        (unsigned long)d1,
                        (unsigned long)(a1 / 1000u));

                    g_svin_active  = true;
                    g_svin_pending = true;
                    g_svin_dur     = (uint16_t)((d1 > 65535u) ? 65535u : d1);
                    g_svin_acc_m   = (uint16_t)(a1 / 1000u);

                } else {
                    gpsdo_log_event(
                        GPSDO_LOG_INFO, "GPS",
                        "Timing: survey-in unavailable; timing will use the live navigation position.");

                    g_svin_active  = false;
                    g_svin_pending = false;
                    g_svin_dur     = 0;
                    g_svin_acc_m   = 0;
                }
            }
        }
    }
#endif

    /* ---- Step 3: optional upgrade to 38400 ---- */
#ifdef GPSDO_PREFER_38400
    if (working_baud != 38400) {
        /* Suppress VTG to reduce traffic during switch */
        Serial1.print("$PUBX,40,VTG,0,0,0,0,0,0*5E\r\n");
        Serial1.flush();
        delay(50);
        flush_rx(50);

        /* PUBX,41: request switch to 38400
         * Checksum *24 verified correct for this exact string via XOR:
         * PUBX,41,1,0003,0003,38400,0 → XOR = 0x24 */
        Serial1.print("$PUBX,41,1,0003,0003,38400,0*24\r\n");
        Serial1.flush();
        delay(200);    /* > longest NMEA sentence at 9600 baud (~82 ms) */

        /* Reopen and probe to verify the switch succeeded */
        Serial1.end();
        Serial1.begin(38400);
        flush_rx(100);

        /* Verify: send a UBX-CFG-PRT poll and check for any response */
        for (uint8_t b = 0; b < sizeof(UBX_POLL_PRT); b++)
            Serial1.write(UBX_POLL_PRT[b]);
        Serial1.flush();

        bool switched = false;
        uint32_t t0 = millis();
        while ((millis() - t0) < 400) {
            if (Serial1.available()) { switched = true; break; }
            delay(5);
        }
        flush_rx(50);

        if (switched) {
            gpsdo_logf(GPSDO_LOG_INFO, "GPS",
                       "Receiver serial speed changed from %lu to 38400 baud.",
                       (unsigned long)working_baud);
            /* If ubx_config failed at the original baud, retry at 38400 */
            if (!cfg_ok) {
                gpsdo_log_event(GPSDO_LOG_INFO, "GPS",
                                "Retrying receiver configuration at 38400 baud.");
                ubx_config();
            }
        } else {
            /* Switch did not take — fall back to working_baud */
            gpsdo_logf(GPSDO_LOG_WARN, "GPS",
                       "Receiver serial speed remains at %lu baud; change to 38400 was not accepted.",
                       (unsigned long)working_baud);
            Serial1.end();
            Serial1.begin(working_baud);
            flush_rx(100);
        }
    }
#endif /* GPSDO_PREFER_38400 */

#else /* GPSDO_UBX_CONFIG not defined */
    uint32_t working_baud = detect_gps_baud();
    if (working_baud == 0) working_baud = 9600;
    Serial1.end();
    Serial1.begin(working_baud);
    flush_rx(100);
    gpsdo_logf(GPSDO_LOG_INFO, "GPS",
               "Receiver running at %lu baud without UBX configuration.",
               (unsigned long)working_baud);
#endif /* GPSDO_UBX_CONFIG */

    /*
     * TIM-TP carries qErr for the firmware's sawtooth correction.
     *
     * Do not use an early TIM-TP poll as a capability test: a genuine timing
     * receiver may support TIM-TP but have no sample ready yet. First inspect
     * the configured UART1 output rate. If output is enabled, wait briefly for
     * a real sample. If output is disabled, enable it and verify the setting.
     */
    {
        uint8_t tim_tp_rate = 0;
        bool rate_known =
            ubx_poll_cfg_msg_uart1_rate(0x0D, 0x01, &tim_tp_rate);

        int32_t qerr_ps = 0;
        bool sample_seen = false;

        if (rate_known && tim_tp_rate > 0) {
            sample_seen = gnss_wait_tim_tp_qerr(&qerr_ps, 1500);
            if (!sample_seen)
                sample_seen = gnss_poll_tim_tp_qerr(&qerr_ps);

            if (sample_seen) {
                gpsdo_logf(
                    GPSDO_LOG_INFO, "GPS",
                    "Timing: TIM-TP periodic output already available (qErr sample %ld ps).",
                    (long)qerr_ps);
            } else {
                gpsdo_log_event(
                    GPSDO_LOG_INFO, "GPS",
                    "Timing: TIM-TP periodic output is enabled; qErr sample not available yet.");
            }
        } else {
            /*
             * Some compatible receivers do not answer CFG-MSG read-back.
             * Before changing anything, check whether TIM-TP is already
             * streaming anyway.
             */
            if (!rate_known)
                sample_seen = gnss_wait_tim_tp_qerr(&qerr_ps, 1500);

            if (sample_seen) {
                gpsdo_logf(
                    GPSDO_LOG_INFO, "GPS",
                    "Timing: TIM-TP periodic output already available (qErr sample %ld ps).",
                    (long)qerr_ps);
            } else if (!ubx_send_cfg_msg(0x0D, 0x01, 1)) {
                gpsdo_log_event(
                    GPSDO_LOG_WARN, "GPS",
                    "Timing: TIM-TP periodic output could not be enabled; qErr correction will not be used.");
            } else {
                uint8_t verify_rate = 0;
                bool rate_verified =
                    ubx_poll_cfg_msg_uart1_rate(0x0D, 0x01, &verify_rate) &&
                    verify_rate > 0;

                int32_t verify_qerr_ps = 0;
                bool sample_verified =
                    gnss_wait_tim_tp_qerr(&verify_qerr_ps, 1500);
                if (!sample_verified)
                    sample_verified =
                        gnss_poll_tim_tp_qerr(&verify_qerr_ps);

                if (sample_verified) {
                    gpsdo_logf(
                        GPSDO_LOG_INFO, "GPS",
                        "Timing: TIM-TP periodic output enabled and verified (qErr sample %ld ps).",
                        (long)verify_qerr_ps);
                } else if (rate_verified) {
                    gpsdo_log_event(
                        GPSDO_LOG_INFO, "GPS",
                        "Timing: TIM-TP periodic output enabled; qErr sample not available yet.");
                } else {
                    gpsdo_log_event(
                        GPSDO_LOG_WARN, "GPS",
                        "Timing: TIM-TP output change was accepted, but periodic output could not be verified.");
                }
            }
        }
    }

    gpsdo_log_event(GPSDO_LOG_INFO, "GPS",
                    "Initialization complete.");
}
