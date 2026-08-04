/*
 * gpsdo_dac.cpp — control-voltage output: PWM or sigma-delta.
 *
 * Part of GPSDO FreeRTOS v1.03
 * See gpsdo_dac.h for why this layer exists and what the two write widths mean.
 */
#include <Arduino.h>
#include "gpsdo_config.h"
#include "gpsdo_dac.h"
#include "gpsdo_health.h"

#if defined(GPSDO_DAC_EXT)
  #include "dac_ext.h"
#endif

static uint16_t s_last16 = 0;

bool gpsdo_dac_begin(void)
{
#if defined(GPSDO_DAC_EXT)
    return dac_ext_begin();
#else
    /* The PWM path needs no bring-up beyond what the sketch already does to the
     * timer; kept as a call so setup() reads the same either way. */
    return true;
#endif
}

void gpsdo_dac_write16(uint16_t v)
{
    /* Every correction passes through here whatever produced it, so the
     * self-assessment needs no hook in the control loops themselves. */
    health_note_output(v);
    s_last16 = v;
#if defined(GPSDO_DAC_EXT)
    /* Scale rather than shift: the external part is not necessarily 24 bits, and
     * 16-bit full scale must still map to its full scale exactly or the constants
     * CT measured against the PWM path would all be off. */
    dac_ext_write(((uint32_t)v * DAC_EXT_MAX) / 65535u);
#else
    analogWrite(PIN_VCTL_PWM, v);
#endif
}

void gpsdo_dac_write24(uint32_t v)
{
    if (v > 0x00FFFFFFu) v = 0x00FFFFFFu;
    s_last16 = (uint16_t)(v >> 8);
#if defined(GPSDO_DAC_EXT)
    dac_ext_write((uint32_t)(((uint64_t)v * DAC_EXT_MAX) / 0x00FFFFFFu));
#else
    analogWrite(PIN_VCTL_PWM, s_last16);
#endif
}

uint16_t gpsdo_dac_last16(void) { return s_last16; }
