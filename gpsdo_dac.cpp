/*
 * gpsdo_dac.cpp — control-voltage output: PWM or sigma-delta.
 *
 * Part of GPSDO FreeRTOS v1.05
 * See gpsdo_dac.h for why this layer exists and what the two write widths mean.
 */
#include <Arduino.h>
#include "gpsdo_config.h"
#include "gpsdo_dac.h"
#include "gpsdo_health.h"

#if defined(GPSDO_DAC_EXT)
  #include "dac_ext.h"
#endif
#if defined(GPSDO_PWM_DITHER)
  #include "gpsdo_pwm24.h"
#endif

/* The authoritative control value is 24-bit. Everything else is a view of it:
 * the 16-bit reading the displays and the flash ring use is this rounded, and
 * the fraction the loop steers with is this divided by 256. Holding one number
 * rather than two removes the possibility of them disagreeing. */
static uint32_t s_code24 = 0;

/* 1..65535 in 16-bit units, expressed in 24-bit units. The fine path is clamped
 * to the same band clamp_pwm() enforces on the coarse one, so no route to the
 * pin can reach a code another route could not. */
#define DAC_CODE24_MIN  (1u << 8)
#define DAC_CODE24_MAX  (65535u << 8)

bool gpsdo_dac_begin(void)
{
#if defined(GPSDO_DAC_EXT)
    return dac_ext_begin();
#elif defined(GPSDO_PWM_DITHER)
    return pwm24_begin();
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
    /* A coarse write states a whole-LSB intent — a sweep point, a ramp step, a
     * value typed at the CLI — so it clears the fraction rather than carrying
     * one nobody asked for. That is the whole reason the fraction lives in this
     * file: the twenty coarse call sites get this for free. */
    s_code24 = (uint32_t)v << 8;
#if defined(GPSDO_DAC_EXT)
    /* Scale rather than shift: the external part is not necessarily 24 bits, and
     * 16-bit full scale must still map to its full scale exactly or the constants
     * CT measured against the PWM path would all be off. */
    dac_ext_write(((uint32_t)v * DAC_EXT_MAX) / 65535u);
#elif defined(GPSDO_PWM_DITHER)
    /* Left-shift, not scale: the 16-bit value becomes the top 16 bits of the
     * 24-bit code, so every existing setting keeps exactly the voltage it had and
     * the constants CT measured stay valid. The low 8 bits wait for a loop that
     * calls gpsdo_dac_write24(). */
    pwm24_write((uint32_t)v << 8);
#else
    analogWrite(PIN_VCTL_PWM, v);
#endif
}

void gpsdo_dac_write24(uint32_t v)
{
    if (v > 0x00FFFFFFu) v = 0x00FFFFFFu;
    s_code24 = v;
    health_note_output((uint16_t)((v + 128u) >> 8));
#if defined(GPSDO_DAC_EXT)
    dac_ext_write((uint32_t)(((uint64_t)v * DAC_EXT_MAX) / 0x00FFFFFFu));
#elif defined(GPSDO_PWM_DITHER)
    pwm24_write(v);                     /* native: this is what it is for */
#else
    /* Plain analogWrite resolves 16 bits, so the fraction is rounded away on
     * the pin. It is still kept in s_code24, because the loop accumulating it
     * across cycles is what turns a run of sub-LSB corrections into a whole
     * one — the fraction earns its keep even where the hardware cannot show it
     * within a single write. */
    analogWrite(PIN_VCTL_PWM, (uint16_t)((v + 128u) >> 8));
#endif
}

void gpsdo_dac_write16f(double v16)
{
    if (!(v16 == v16)) return;          /* NaN: refuse rather than clamp to a rail */
    double c = v16 * 256.0 + 0.5;       /* round, not truncate */
    uint32_t code;
    if (c <= (double)DAC_CODE24_MIN)      code = DAC_CODE24_MIN;
    else if (c >= (double)DAC_CODE24_MAX) code = DAC_CODE24_MAX;
    else                                  code = (uint32_t)c;
    gpsdo_dac_write24(code);
}

uint16_t gpsdo_dac_last16(void)
{
    /* Rounded, so the number on the display is the nearest 16-bit code to what
     * is on the pin. Truncating would make a value sitting at .99 read one LSB
     * low for its whole life. */
    return (uint16_t)((s_code24 + 128u) >> 8);
}

double   gpsdo_dac_last16f(void) { return (double)s_code24 / 256.0; }
uint32_t gpsdo_dac_last24(void)  { return s_code24; }

bool gpsdo_dac_fine_available(void)
{
#if defined(GPSDO_DAC_EXT) || defined(GPSDO_PWM_DITHER)
    return true;
#else
    return false;
#endif
}
