#ifndef DISPLAYCAL_H
#define DISPLAYCAL_H

#define DISPLAYCAL_GAIN_SCALE 100
#define DISPLAYCAL_GAIN_MIN 0
#define DISPLAYCAL_GAIN_MAX 200

#ifdef __cplusplus
extern "C" {
#endif

// Clamp a display calibration gain value to the supported 0-200 range.
int DisplayCal_clampGainValue(int value);

// Apply the LUT using integer red, green, and blue gains in the 0-200 range.
// A value of 100 is neutral.
int DisplayCal_enableWithValues(int red_gain, int green_gain, int blue_gain);

// Load the identity LUT, then disable gamma correction.
int DisplayCal_disable(void);

#ifdef __cplusplus
}
#endif

#endif
