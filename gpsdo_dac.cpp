/*
 * gpsdo_dac.cpp — control-voltage output: PWM or sigma-delta.
 *
 * Part of GPSDO FreeRTOS v1.06
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

/* Default DITH, per the note in gpsdo_dac.h. Resolved against what is compiled
 * in at bring-up, so this initial value is a request rather than a promise. */
volatile uint8_t g_dac_path = DAC_PATH_DITH;

bool gpsdo_dac_path_available(uint8_t path)
{
    switch (path) {
#if defined(GPSDO_PWM_DITHER)
    case DAC_PATH_DITH: return true;
#endif
#if defined(GPSDO_DAC_EXT)
    case DAC_PATH_EXT:  return true;
#endif
    /* PWM is always available: with the dither engine present it is that
     * engine driven at whole-LSB granularity, and without it, analogWrite. */
    case DAC_PATH_PWM:  return true;
    default: return false;
    }
}

uint8_t gpsdo_dac_path_resolve(uint8_t want)
{
    if (gpsdo_dac_path_available(want)) return want;
    if (gpsdo_dac_path_available(DAC_PATH_DITH)) return DAC_PATH_DITH;
    if (gpsdo_dac_path_available(DAC_PATH_PWM))  return DAC_PATH_PWM;
    return DAC_PATH_EXT;
}

const char *gpsdo_dac_path_name(uint8_t path)
{
    switch (path) {
    case DAC_PATH_PWM:  return "PWM";
    case DAC_PATH_DITH: return "DITH";
    case DAC_PATH_EXT:  return "EXT";
    default:            return "?";
    }
}

/* 1..65535 in 16-bit units, expressed in 24-bit units. The fine path is clamped
 * to the same band clamp_pwm() enforces on the coarse one, so no route to the
 * pin can reach a code another route could not. */
#define DAC_CODE24_MIN  (1u << 8)
#define DAC_CODE24_MAX  (65535u << 8)

static void dac_emit(uint32_t code24, uint16_t code16);

bool gpsdo_dac_begin(void)
{
    /* Bring up EVERY path that is compiled in, not just the selected one. The
     * jumper decides which signal reaches the filter, and a path that is not
     * initialised until it is first selected would give a glitch — or a dead
     * output — at the moment of switching, which is the worst possible moment.
     * The unselected drivers sit there holding their last code and nothing is
     * connected to them. */
    bool ok = true;
#if defined(GPSDO_DAC_EXT)
    if (!dac_ext_begin()) ok = false;
#endif
#if defined(GPSDO_PWM_DITHER)
    if (!pwm24_begin())   ok = false;
#endif
    /* Plain PWM needs no bring-up beyond what the sketch already does to the
     * timer. Resolve the requested path against what actually came up. */
    g_dac_path = gpsdo_dac_path_resolve(g_dac_path);
    return ok;
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
    dac_emit(s_code24, v);
}

/* The one place a code reaches hardware. Both write widths funnel through it so
 * the runtime path is tested once rather than in two places that could drift
 * apart — which is the same argument that created this file. */
static void dac_emit(uint32_t code24, uint16_t code16)
{
    switch (g_dac_path) {
#if defined(GPSDO_DAC_EXT)
    case DAC_PATH_EXT:
        /* Scale rather than shift: the external part is not necessarily 24 bits,
         * and 16-bit full scale must still map to its full scale exactly or the
         * constants CT measured against the PWM path would all be off. */
        dac_ext_write((uint32_t)(((uint64_t)code24 * DAC_EXT_MAX) / 0x00FFFFFFu));
        return;
#endif
#if defined(GPSDO_PWM_DITHER)
    case DAC_PATH_DITH:
        pwm24_write(code24);            /* native: this is what it is for */
        return;
    case DAC_PATH_PWM:
        /* Same engine, low eight bits cleared: every table entry identical, so
         * a constant duty cycle and the same voltage plain PWM would give. No
         * DMA teardown, nothing to fail at the moment of switching. */
        pwm24_write(code24 & 0x00FFFF00u);
        return;
#else
    case DAC_PATH_PWM:
        analogWrite(PIN_VCTL_PWM, code16);
        return;
#endif
    default:
        break;
    }
    /* Selected path is not compiled in. Cannot happen once resolve() has run at
     * bring-up and on every CLI change, but silence here would mean a frozen
     * control voltage, so fall back to the one path that always exists. */
#if defined(GPSDO_PWM_DITHER)
    pwm24_write(code24 & 0x00FFFF00u);
#else
    analogWrite(PIN_VCTL_PWM, code16);
#endif
}

void gpsdo_dac_write24(uint32_t v)
{
    if (v > 0x00FFFFFFu) v = 0x00FFFFFFu;
    s_code24 = v;
    health_note_output((uint16_t)((v + 128u) >> 8));
    /* Under PWM and under plain analogWrite the fraction is rounded away on the
     * pin. It is still kept in s_code24, because the loop accumulating it across
     * cycles is what turns a run of sub-LSB corrections into a whole one — the
     * fraction earns its keep even where the hardware cannot show it inside a
     * single write. */
    dac_emit(v, (uint16_t)((v + 128u) >> 8));
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
    /* A property of the ACTIVE path, not of the build. Selecting PWM on a board
     * that has the dither engine really does give up the sub-LSB resolution, and
     * the loop must be told so rather than going on writing fractions that the
     * whole-LSB path throws away. */
    switch (g_dac_path) {
    case DAC_PATH_DITH: return gpsdo_dac_path_available(DAC_PATH_DITH);
    case DAC_PATH_EXT:  return gpsdo_dac_path_available(DAC_PATH_EXT);
    default:            return false;   /* PWM: whole LSBs only */
    }
}
