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

#define DISPLAYCAL_SMARTPRO_DEFAULT_ENABLED 1
#define DISPLAYCAL_SMARTPRO_DEFAULT_RED_GAIN DISPLAYCAL_DEFAULT_RED_GAIN
#define DISPLAYCAL_SMARTPRO_DEFAULT_GREEN_GAIN 77
#define DISPLAYCAL_SMARTPRO_DEFAULT_BLUE_GAIN 50

#define DISPLAYCAL_BRICKPRO_DEFAULT_ENABLED 1
#define DISPLAYCAL_BRICKPRO_DEFAULT_RED_GAIN DISPLAYCAL_DEFAULT_RED_GAIN
#define DISPLAYCAL_BRICKPRO_DEFAULT_GREEN_GAIN 94
#define DISPLAYCAL_BRICKPRO_DEFAULT_BLUE_GAIN 54

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DisplayCalDefaults {
	int enabled;
	int red_gain;
	int green_gain;
	int blue_gain;
} DisplayCalDefaults;

// TrimUI Brick, measured with X-Rite i1Display Pro, calibrated to sRGB D65 2.2
static const struct DisplayCalDefaults DisplayCalDefaults_Brick = {
	DISPLAYCAL_BRICK_DEFAULT_ENABLED,
	DISPLAYCAL_BRICK_DEFAULT_RED_GAIN,
	DISPLAYCAL_BRICK_DEFAULT_GREEN_GAIN,
	DISPLAYCAL_BRICK_DEFAULT_BLUE_GAIN
};

// TrimUI Smart Pro, measured with Spyder 5 Pro and slightly modified by eye to match the Brick
static const struct DisplayCalDefaults DisplayCalDefaults_SmartPro = {
	DISPLAYCAL_SMARTPRO_DEFAULT_ENABLED,
	DISPLAYCAL_SMARTPRO_DEFAULT_RED_GAIN,
	DISPLAYCAL_SMARTPRO_DEFAULT_GREEN_GAIN,
	DISPLAYCAL_SMARTPRO_DEFAULT_BLUE_GAIN
};

// TrimUI Brick Pro, TODO
static const struct DisplayCalDefaults DisplayCalDefaults_BrickPro = {
	DISPLAYCAL_BRICKPRO_DEFAULT_ENABLED,
	DISPLAYCAL_BRICKPRO_DEFAULT_RED_GAIN,
	DISPLAYCAL_BRICKPRO_DEFAULT_GREEN_GAIN,
	DISPLAYCAL_BRICKPRO_DEFAULT_BLUE_GAIN
};

enum DisplayCalPreset {
	DISPLAYCAL_PRESET_DEFAULT = 0,
	DISPLAYCAL_PRESET_BRICK,
	DISPLAYCAL_PRESET_SMARTPRO,
	DISPLAYCAL_PRESET_BRICKPRO,
};

static inline DisplayCalDefaults DisplayCal_getDefaultSettings(enum DisplayCalPreset preset) {
	if(preset == DISPLAYCAL_PRESET_SMARTPRO) {
		return DisplayCalDefaults_SmartPro;
	}
	if(preset == DISPLAYCAL_PRESET_BRICK) {
		return DisplayCalDefaults_Brick;
	}
	if(preset == DISPLAYCAL_PRESET_BRICKPRO) {
		return DisplayCalDefaults_BrickPro;
	}
	// Default preset
	DisplayCalDefaults defaults = {
		DISPLAYCAL_DEFAULT_ENABLED,
		DISPLAYCAL_DEFAULT_RED_GAIN,
		DISPLAYCAL_DEFAULT_GREEN_GAIN,
		DISPLAYCAL_DEFAULT_BLUE_GAIN,
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
