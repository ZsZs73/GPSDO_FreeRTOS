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
        strncat(buf, "+", len - strlen(buf) - 1);

    strncat(buf, cause, len - strlen(buf) - 1);
}


void gpsdo_hw_log_reset_cause(void)
{
    const uint32_t csr = RCC->CSR;

    char cause[96] = "";

    if (csr & RCC_CSR_LPWRRSTF)
        append_reset_cause(cause, sizeof(cause), "low_power");

    if (csr & (RCC_CSR_WWDGRSTF | RCC_CSR_IWDGRSTF))
        append_reset_cause(cause, sizeof(cause), "watchdog");

    if (csr & RCC_CSR_SFTRSTF)
        append_reset_cause(cause, sizeof(cause), "software_restart");

    if (csr & RCC_CSR_PORRSTF) {
        append_reset_cause(cause, sizeof(cause), "power_on");
    } else if (csr & RCC_CSR_BORRSTF) {
        append_reset_cause(cause, sizeof(cause), "supply_drop");
    }

    if (csr & RCC_CSR_PINRSTF)
        append_reset_cause(cause, sizeof(cause), "reset_pin");

    if (cause[0] == '\0')
        strcpy(cause, "unknown");

    gpsdo_log_eventf(
        GPSDO_LOG_INFO,
        "BOOT",
        "RESET",
        "cause=%s",
        cause
    );

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
            return "external_clock";

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
    gpsdo_log_eventf(
        pass ? GPSDO_LOG_INFO : GPSDO_LOG_WARN,
        "POST",
        "MCU_CLOCK",
        "result=%s source=%s",
        pass ? "PASS" : "FAIL",
        source
    );
}


const gpsdo_hw_info_t *gpsdo_hw_info(void)
{
    return &s_hw;
}
