/*
 * gpsdo_hw.cpp
 *
 * Low-level hardware identification and startup diagnostics.
 *
 * Responsibilities:
 *   - Report the controller reset cause.
 *   - Identify the MCU clock input source (crystal or external clock).
 *   - Perform startup hardware POST checks.
 *   - Maintain the detected hardware inventory in gpsdo_hw_info_t.
 *
 * Reset cause is boot information, not a POST result.
 * POST entries report whether required hardware paths are operational.
 */

#include "gpsdo_hw.h"
#include <Wire.h>
#include "gpsdo_config.h"
#include "gpsdo_log.h"

#include <Arduino.h>
#include <string.h>

static gpsdo_hw_info_t s_hw = {
    GPSDO_HW_UNKNOWN,
    GPSDO_MCU_CLOCK_UNKNOWN
};

/* ----------------------------------------------------------------------
 * Reset cause
 *
 * Report why the controller restarted using operator-facing terminology.
 * STM32-specific reset flags stay internal to this module.
 *
 * Multiple causes may be latched at once, so all applicable causes are
 * reported. The flags are cleared only after they have been logged.
 *
 * This is informational boot diagnostics, not a POST test.
 * ---------------------------------------------------------------------- */
static void append_reset_cause(char *buf, size_t len, const char *cause)
{
    if (buf[0] != '\0')
        strncat(buf, " + ", len - strlen(buf) - 1);

    strncat(buf, cause, len - strlen(buf) - 1);
}


void gpsdo_hw_log_reset_cause(void)
{
    const uint32_t csr = RCC->CSR;

    char cause[96] = "";

    if (csr & RCC_CSR_LPWRRSTF)
        append_reset_cause(cause, sizeof(cause), "low-power reset");

    if (csr & (RCC_CSR_WWDGRSTF | RCC_CSR_IWDGRSTF))
        append_reset_cause(cause, sizeof(cause), "watchdog");

    if (csr & RCC_CSR_SFTRSTF)
        append_reset_cause(cause, sizeof(cause), "software restart");

    if (csr & RCC_CSR_PORRSTF) {
        append_reset_cause(cause, sizeof(cause), "power on");
    } else if (csr & RCC_CSR_BORRSTF) {
        append_reset_cause(cause, sizeof(cause), "supply voltage drop");
    }

    if (csr & RCC_CSR_PINRSTF)
        append_reset_cause(cause, sizeof(cause), "reset pin");

    if (cause[0] == '\0')
        strcpy(cause, "unknown");

    gpsdo_logf(GPSDO_LOG_INFO, "BOOT", "Reset cause: %s.", cause);

    /* Clear latched reset causes after recording them. */
    RCC->CSR |= RCC_CSR_RMVF;
}

/* ----------------------------------------------------------------------
 * Detect what is connected to the STM32 HSE input.
 *
 * HSEBYP = 0:
 *   STM32 crystal oscillator is being used.
 *   Typical Black Pill.
 *
 * HSEBYP = 1:
 *   PH0/OSC_IN is being driven by an external clock signal.
 *   Standalone GPSDO board with OCXO-derived clock.
 *
 * "bypass" is deliberately NOT exposed in user-visible diagnostics.
 * ---------------------------------------------------------------------- */
static gpsdo_mcu_clock_input_t detect_mcu_clock_input(void)
{
    if ((RCC->CR & RCC_CR_HSEBYP) != 0)
        return GPSDO_MCU_CLOCK_EXTERNAL;

    return GPSDO_MCU_CLOCK_CRYSTAL;
}


static const char *mcu_clock_input_name(gpsdo_mcu_clock_input_t input)
{
    switch (input) {
        case GPSDO_MCU_CLOCK_CRYSTAL:
            return "crystal";

        case GPSDO_MCU_CLOCK_EXTERNAL:
            return "external clock";

        default:
            return "unknown";
    }
}


/* ----------------------------------------------------------------------
 * Verify that SYSCLK is actually using the HSE path.
 *
 * PASS:
 *   SYSCLK = HSE directly
 *       or
 *   SYSCLK = PLL and PLL source = HSE
 *
 * FAIL:
 *   SYSCLK = HSI
 *   PLL source = HSI
 *   HSE/PLL not ready
 *
 * This does NOT measure frequency.
 * ---------------------------------------------------------------------- */
static bool mcu_running_from_hse(void)
{
    const uint32_t sws = RCC->CFGR & RCC_CFGR_SWS;

    /* SYSCLK directly from HSE */
    if (sws == RCC_CFGR_SWS_HSE) {
        return (RCC->CR & RCC_CR_HSERDY) != 0;
    }

    /* SYSCLK from PLL */
    if (sws == RCC_CFGR_SWS_PLL) {
        const bool pll_from_hse =
            (RCC->PLLCFGR & RCC_PLLCFGR_PLLSRC) != 0;

        const bool hse_ready =
            (RCC->CR & RCC_CR_HSERDY) != 0;

        const bool pll_ready =
            (RCC->CR & RCC_CR_PLLRDY) != 0;

        return pll_from_hse && hse_ready && pll_ready;
    }

    return false;
}


/* ----------------------------------------------------------------------
 * Startup POST
 * ---------------------------------------------------------------------- */
void gpsdo_hw_post_begin(void)
{
    s_hw.mcu_clock_input = detect_mcu_clock_input();

    const bool pass = mcu_running_from_hse();

    s_hw.mcu_clock =
        pass ? GPSDO_HW_PRESENT : GPSDO_HW_FAILED;

    const char *source =
        mcu_clock_input_name(s_hw.mcu_clock_input);

    /* Structured log */
    if (pass) {
        gpsdo_logf(GPSDO_LOG_INFO, "POST", "MCU clock source: %s.", source);
    } else if (strcmp(source, "crystal") == 0) {
        gpsdo_log_event(
            GPSDO_LOG_WARN, "POST",
            "MCU clock check failed: processor is not running from the crystal oscillator.");
    } else if (strcmp(source, "external clock") == 0) {
        gpsdo_log_event(
            GPSDO_LOG_WARN, "POST",
            "MCU clock check failed: processor is not running from the external clock input.");
    } else {
        gpsdo_log_event(
            GPSDO_LOG_WARN, "POST",
            "MCU clock check failed: clock source could not be identified.");
    }
}


const gpsdo_hw_info_t *gpsdo_hw_info(void)
{
    return &s_hw;
}

/* ======================================================================
 * Boot-time hardware discovery
 *
 * Discovery is synchronous and runs before the scheduler. Runtime tasks
 * consume this inventory; they do not decide what hardware exists.
 * ====================================================================== */

static gpsdo_hw_device_state_t s_devices[GPSDO_HW_DEVICE_COUNT];
static gpsdo_ocxo_temp_info_t s_ocxo_temp = {
    GPSDO_OCXO_TEMP_NONE, 0u, 0u, 0.0f
};

static bool hw_i2c_probe(uint8_t addr)
{
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() != 0)
        return false;

    /* Keep the dual check previously used by the display task. */
    return Wire.requestFrom(addr, (uint8_t)1) > 0;
}

static bool hw_i2c_read_reg8(uint8_t addr, uint8_t reg, uint8_t *value)
{
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0)
        return false;

    if (Wire.requestFrom(addr, (uint8_t)1) != 1)
        return false;

    *value = Wire.read();
    return true;
}

static bool hw_i2c_read_reg16(uint8_t addr, uint8_t reg, uint16_t *value)
{
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0)
        return false;

    if (Wire.requestFrom(addr, (uint8_t)2) != 2)
        return false;

    *value = ((uint16_t)Wire.read() << 8) | (uint16_t)Wire.read();
    return true;
}

static bool hw_bosch_id(uint8_t addr, uint8_t expected)
{
    uint8_t id = 0;
    return hw_i2c_read_reg8(addr, 0xD0u, &id) && id == expected;
}

static void hw_device_set(gpsdo_hw_device_t dev,
                          bool enabled,
                          uint8_t address,
                          bool present)
{
    gpsdo_hw_device_state_t &s = s_devices[(unsigned)dev];
    s.enabled     = enabled;
    s.detectable  = enabled;
    s.present     = enabled && present;
    s.initialized = false;
    s.address     = address;
}

static void hw_post_i2c(gpsdo_hw_device_t dev,
                        const char *name,
                        const char *missing_consequence)
{
    const gpsdo_hw_device_state_t &s = s_devices[(unsigned)dev];
    if (!s.enabled)
        return;

    if (s.present) {
        gpsdo_logf(GPSDO_LOG_INFO, "POST",
                   "%s detected at I2C address 0x%02X.",
                   name, (unsigned)s.address);
    } else {
        gpsdo_logf(GPSDO_LOG_WARN, "POST",
                   "%s not detected at I2C address 0x%02X; %s",
                   name, (unsigned)s.address, missing_consequence);
    }
}

void gpsdo_hw_discover(void)
{
    memset(s_devices, 0, sizeof(s_devices));

#ifdef GPSDO_AHT10
    hw_device_set(GPSDO_HW_AHT, true, 0x38u, hw_i2c_probe(0x38u));
    hw_post_i2c(GPSDO_HW_AHT, "AHT10/AHT20",
                "enclosure temperature and humidity unavailable.");
#endif

#ifdef GPSDO_BMP280_I2C
    /* BMP280 chip ID register 0xD0 = 0x58. */
    hw_device_set(GPSDO_HW_BMP280, true, 0x77u,
                  hw_bosch_id(0x77u, 0x58u));
    hw_post_i2c(GPSDO_HW_BMP280, "BMP280",
                "enclosure temperature and pressure unavailable.");
#endif

#ifdef GPSDO_BME280_I2C
    /* BME280 chip ID register 0xD0 = 0x60. */
    hw_device_set(GPSDO_HW_BME280, true, 0x76u,
                  hw_bosch_id(0x76u, 0x60u));
    hw_post_i2c(GPSDO_HW_BME280, "BME280",
                "enclosure temperature, pressure and humidity unavailable.");
#endif

#ifdef GPSDO_INA219
    hw_device_set(GPSDO_HW_INA219, true, 0x40u, hw_i2c_probe(0x40u));
    hw_post_i2c(GPSDO_HW_INA219, "INA219",
                "OCXO voltage and current measurements unavailable.");
#endif

#ifdef GPSDO_OCXO_TEMP
    {
        const uint8_t addr = (uint8_t)GPSDO_OCXO_TEMP_ADDRESS;
        gpsdo_ocxo_temp_type_t type = GPSDO_OCXO_TEMP_NONE;

        if (hw_i2c_probe(addr)) {
            uint16_t id16 = 0u;
            uint8_t id8 = 0u;

            /* Strongest signatures first. */
            if (hw_i2c_read_reg16(addr, 0x0Fu, &id16) && id16 == 0x1116u) {
                type = GPSDO_OCXO_TEMP_TMP116;
            } else if (hw_i2c_read_reg16(addr, 0x0Fu, &id16) &&
                       id16 == 0x7500u) {
                type = GPSDO_OCXO_TEMP_TMP1075;
            } else if (hw_i2c_read_reg8(addr, 0x05u, &id8) &&
                       id8 == 0xA1u) {
                type = GPSDO_OCXO_TEMP_SE95;
            } else {
                /*
                 * PCT2075 has no dedicated device-ID register. Among the four
                 * explicitly supported candidates it is the remaining device,
                 * and register 0x04 is its readable Tidle register.
                 *
                 * Do not write probe values here: POST discovery stays
                 * non-destructive.
                 */
                uint8_t tidle = 0u;
                if (hw_i2c_read_reg8(addr, 0x04u, &tidle))
                    type = GPSDO_OCXO_TEMP_PCT2075;
                else
                    type = GPSDO_OCXO_TEMP_UNKNOWN;
            }
        }

        s_ocxo_temp.type = type;
        s_ocxo_temp.address = addr;

        switch (type) {
        case GPSDO_OCXO_TEMP_PCT2075:
            s_ocxo_temp.resolution_bits = 11u;
            s_ocxo_temp.resolution_c = 0.125f;
            break;
        case GPSDO_OCXO_TEMP_TMP1075:
            s_ocxo_temp.resolution_bits = 12u;
            s_ocxo_temp.resolution_c = 0.0625f;
            break;
        case GPSDO_OCXO_TEMP_SE95:
            s_ocxo_temp.resolution_bits = 13u;
            s_ocxo_temp.resolution_c = 0.03125f;
            break;
        case GPSDO_OCXO_TEMP_TMP116:
            s_ocxo_temp.resolution_bits = 16u;
            s_ocxo_temp.resolution_c = 0.0078125f;
            break;
        default:
            s_ocxo_temp.resolution_bits = 0u;
            s_ocxo_temp.resolution_c = 0.0f;
            break;
        }

        hw_device_set(GPSDO_HW_OCXO_TEMP, true, addr,
                      type != GPSDO_OCXO_TEMP_NONE);

        if (type == GPSDO_OCXO_TEMP_NONE) {
            gpsdo_logf(
                GPSDO_LOG_WARN, "POST",
                "OCXO temperature sensor not detected at I2C address 0x%02X; temperature measurement unavailable.",
                (unsigned)addr);
        } else if (type == GPSDO_OCXO_TEMP_UNKNOWN) {
            gpsdo_logf(
                GPSDO_LOG_WARN, "POST",
                "OCXO temperature sensor detected at I2C address 0x%02X, but its type is unknown; temperature measurement unavailable.",
                (unsigned)addr);
        } else {
            gpsdo_logf(
                GPSDO_LOG_INFO, "POST",
                "%s OCXO temperature sensor detected at I2C address 0x%02X.",
                gpsdo_hw_ocxo_temp_name(type), (unsigned)addr);
        }
    }
#endif

#ifdef GPSDO_OLED
    hw_device_set(GPSDO_HW_OLED, true, 0x3Cu, hw_i2c_probe(0x3Cu));
    hw_post_i2c(GPSDO_HW_OLED, "OLED display",
                "OLED output unavailable.");
#endif
}

const char *gpsdo_hw_ocxo_temp_name(gpsdo_ocxo_temp_type_t type)
{
    switch (type) {
    case GPSDO_OCXO_TEMP_PCT2075: return "PCT2075";
    case GPSDO_OCXO_TEMP_TMP1075: return "TMP1075";
    case GPSDO_OCXO_TEMP_SE95:    return "SE95";
    case GPSDO_OCXO_TEMP_TMP116:  return "TMP116";
    case GPSDO_OCXO_TEMP_UNKNOWN: return "unknown";
    default:                      return "none";
    }
}

const gpsdo_ocxo_temp_info_t *gpsdo_hw_ocxo_temp_info(void)
{
    return &s_ocxo_temp;
}

const gpsdo_hw_device_state_t *gpsdo_hw_device_state(gpsdo_hw_device_t device)
{
    if ((unsigned)device >= (unsigned)GPSDO_HW_DEVICE_COUNT)
        return nullptr;
    return &s_devices[(unsigned)device];
}

bool gpsdo_hw_is_present(gpsdo_hw_device_t device)
{
    const gpsdo_hw_device_state_t *s = gpsdo_hw_device_state(device);
    return s && s->enabled && s->detectable && s->present;
}

void gpsdo_hw_set_initialized(gpsdo_hw_device_t device, bool initialized)
{
    if ((unsigned)device >= (unsigned)GPSDO_HW_DEVICE_COUNT)
        return;

    gpsdo_hw_device_state_t &s = s_devices[(unsigned)device];
    s.initialized = s.present && initialized;
}
