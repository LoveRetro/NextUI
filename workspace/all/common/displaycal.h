#ifndef DISPLAYCAL_H
#define DISPLAYCAL_H

#define DISPLAYCAL_GAIN_SCALE 100
#define DISPLAYCAL_GAIN_MIN 0
#define DISPLAYCAL_GAIN_MAX 200
#define DISPLAYCAL_DEFAULT_ENABLED 0
#define DISPLAYCAL_DEFAULT_RED_GAIN 100
#define DISPLAYCAL_DEFAULT_GREEN_GAIN 100
#define DISPLAYCAL_DEFAULT_BLUE_GAIN 100
#define DISPLAYCAL_BRICK_DEFAULT_ENABLED 1
#define DISPLAYCAL_BRICK_DEFAULT_RED_GAIN DISPLAYCAL_DEFAULT_RED_GAIN
#define DISPLAYCAL_BRICK_DEFAULT_GREEN_GAIN 92
#define DISPLAYCAL_BRICK_DEFAULT_BLUE_GAIN 58

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DisplayCalDefaults {
	int enabled;
	int red_gain;
	int green_gain;
	int blue_gain;
} DisplayCalDefaults;

static inline DisplayCalDefaults DisplayCal_getDefaultSettings(int use_brick_overrides) {
	DisplayCalDefaults defaults = {
		use_brick_overrides ? DISPLAYCAL_BRICK_DEFAULT_ENABLED : DISPLAYCAL_DEFAULT_ENABLED,
		use_brick_overrides ? DISPLAYCAL_BRICK_DEFAULT_RED_GAIN : DISPLAYCAL_DEFAULT_RED_GAIN,
		use_brick_overrides ? DISPLAYCAL_BRICK_DEFAULT_GREEN_GAIN : DISPLAYCAL_DEFAULT_GREEN_GAIN,
		use_brick_overrides ? DISPLAYCAL_BRICK_DEFAULT_BLUE_GAIN : DISPLAYCAL_DEFAULT_BLUE_GAIN,
	};
	return defaults;
}

// Clamp a display calibration gain value to the supported 0-200 range.
static inline int DisplayCal_clampGainValue(int value) {
	if (value < DISPLAYCAL_GAIN_MIN)
		return DISPLAYCAL_GAIN_MIN;
	if (value > DISPLAYCAL_GAIN_MAX)
		return DISPLAYCAL_GAIN_MAX;
	return value;
}

// Apply the LUT using integer red, green, and blue gains in the 0-200 range.
// A value of 100 is neutral.
int DisplayCal_enableWithValues(int red_gain, int green_gain, int blue_gain);

// Load the identity LUT, then disable gamma correction.
int DisplayCal_disable(void);

#ifdef __cplusplus
}
#endif

#endif
