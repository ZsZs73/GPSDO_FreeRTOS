#include "gpsdo_adc.h"

#include <Arduino.h>

int16_t gpsdo_adc_read_median_settled(uint32_t pin,
                                      int16_t *samples,
                                      size_t count)
{
    if (samples == nullptr || count == 0 || (count & 1u) == 0)
        return 0;
    /*
     * Discard the first conversion after switching ADC1 channels. This lets
     * the mux/sample capacitor settle onto the new source before taking the
     * measurements that matter.
     */
    (void)analogRead(pin);

    /*
     * Insertion-sort while acquiring.  The sample counts used here are tiny
     * (5 for Vctl, 15 for LTIC), so this is simpler than carrying a separate
     * sort pass or general-purpose sorting machinery.
     */
    for (size_t i = 0; i < count; ++i) {
        int16_t v = (int16_t)analogRead(pin);
        size_t j = i;

        while (j > 0 && samples[j - 1] > v) {
            samples[j] = samples[j - 1];
            --j;
        }

        samples[j] = v;
    }

    return samples[count / 2];
}