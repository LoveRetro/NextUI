#ifndef DISPLAYCAL_H
#define DISPLAYCAL_H

#include <stddef.h>

#define DISPLAYCAL_GAIN_SCALE 100
#define DISPLAYCAL_GAIN_MIN 0
#define DISPLAYCAL_GAIN_MAX 200

#ifdef __cplusplus
extern "C" {
#endif

// Clamp a scaled integer gain value to the supported range.
int DisplayCal_clampGainValue(int value);

// Format a scaled integer gain value as a decimal string, e.g. 92 -> "0.92".
void DisplayCal_formatGainValue(int value, char *output, size_t output_size);

// Apply the LUT using scaled integer red, green, and blue gains.
int DisplayCal_enableWithValues(int red_gain, int green_gain, int blue_gain);

// Load the identity LUT, then disable gamma correction.
int DisplayCal_disable(void);

#ifdef __cplusplus
}
#endif

#endif
