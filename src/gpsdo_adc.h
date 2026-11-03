#ifndef GPSDO_ADC_H
#define GPSDO_ADC_H

#include <stddef.h>
#include <stdint.h>

/*
 * Shared ADC acquisition helpers.
 *
 * ADC1 is shared by several inputs.  After selecting a channel, discard the
 * first conversion so the ADC mux/sample capacitor can settle, then take an
 * odd-sized burst and return its median to reject impulsive conversion
 * glitches.
 *
 * The caller owns any scheduler/ADC interlock.  This module deliberately knows
 * nothing about Vctl, LTIC, PPS timing, filtering, calibration or control.
 */

/*
 * samples must point to storage for count int16_t values.
 * count must be odd and greater than zero.
 */
int16_t gpsdo_adc_read_median_settled(uint32_t pin,
                                      int16_t *samples,
                                      size_t count);

#endif /* GPSDO_ADC_H */
