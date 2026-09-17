#ifndef DISPLAYCAL_H
#define DISPLAYCAL_H

#define DISPLAYCAL_GAIN_SCALE 100
#define DISPLAYCAL_GAIN_MIN 0
#define DISPLAYCAL_GAIN_MAX 200
#define DISPLAYCAL_DEFAULT_ENABLED 0
#define DISPLAYCAL_DEFAULT_RED_GAIN 100
#define DISPLAYCAL_DEFAULT_GREEN_GAIN 100
#define DISPLAYCAL_DEFAULT_BLUE_GAIN 100

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
static const struct DisplayCalDefaults DisplayCalDefaults_Brick = { 1, 100, 92, 58 };

// TrimUI Smart Pro, measured with Spyder 5 Pro and slightly modified by eye to match the Brick
static const struct DisplayCalDefaults DisplayCalDefaults_SmartPro = { 1, 100, 77, 50 };

// TrimUI Brick Pro, TODO
static const struct DisplayCalDefaults DisplayCalDefaults_BrickPro = { 1, 100, 94, 54 };

// Anbernic H700 family. Uncalibrated models remain disabled with neutral gains.
static const struct DisplayCalDefaults DisplayCalDefaults_RG28XX = { 1, 100, 92, 65 };
static const struct DisplayCalDefaults DisplayCalDefaults_RG34XX = { 0, 100, 100, 100 };
static const struct DisplayCalDefaults DisplayCalDefaults_RG34XXSP = { 1, 100, 83, 86 };
static const struct DisplayCalDefaults DisplayCalDefaults_RGSP = { 1, 100, 68, 61 };
static const struct DisplayCalDefaults DisplayCalDefaults_RG35XXPlus = { 0, 100, 100, 100 };
static const struct DisplayCalDefaults DisplayCalDefaults_RG35XXH = { 0, 100, 100, 100 };
static const struct DisplayCalDefaults DisplayCalDefaults_RG35XXSP = { 0, 100, 100, 100 };
static const struct DisplayCalDefaults DisplayCalDefaults_RG35XXPRO = { 0, 100, 100, 100 };
static const struct DisplayCalDefaults DisplayCalDefaults_RG40XXH = { 0, 100, 100, 100 };
static const struct DisplayCalDefaults DisplayCalDefaults_RG40XXV = { 1, 91, 100, 63 };
static const struct DisplayCalDefaults DisplayCalDefaults_RGCubeXX = { 0, 100, 100, 100 };

enum DisplayCalPreset {
	DISPLAYCAL_PRESET_DEFAULT = 0,
	DISPLAYCAL_PRESET_BRICK,
	DISPLAYCAL_PRESET_SMARTPRO,
	DISPLAYCAL_PRESET_BRICKPRO,
	DISPLAYCAL_PRESET_RG28XX,
	DISPLAYCAL_PRESET_RG34XX,
	DISPLAYCAL_PRESET_RG34XXSP,
	DISPLAYCAL_PRESET_RGSP,
	DISPLAYCAL_PRESET_RG35XXPLUS,
	DISPLAYCAL_PRESET_RG35XXH,
	DISPLAYCAL_PRESET_RG35XXSP,
	DISPLAYCAL_PRESET_RG35XXPRO,
	DISPLAYCAL_PRESET_RG40XXH,
	DISPLAYCAL_PRESET_RG40XXV,
	DISPLAYCAL_PRESET_RGCUBEXX
};

static inline DisplayCalDefaults DisplayCal_getDefaultSettings(enum DisplayCalPreset preset) {
	switch(preset) {
		case DISPLAYCAL_PRESET_SMARTPRO: return DisplayCalDefaults_SmartPro;
		case DISPLAYCAL_PRESET_BRICK: return DisplayCalDefaults_Brick;
		case DISPLAYCAL_PRESET_BRICKPRO: return DisplayCalDefaults_BrickPro;
		case DISPLAYCAL_PRESET_RG28XX: return DisplayCalDefaults_RG28XX;
		case DISPLAYCAL_PRESET_RG34XX: return DisplayCalDefaults_RG34XX;
		case DISPLAYCAL_PRESET_RG34XXSP: return DisplayCalDefaults_RG34XXSP;
		case DISPLAYCAL_PRESET_RGSP: return DisplayCalDefaults_RGSP;
		case DISPLAYCAL_PRESET_RG35XXPLUS: return DisplayCalDefaults_RG35XXPlus;
		case DISPLAYCAL_PRESET_RG35XXH: return DisplayCalDefaults_RG35XXH;
		case DISPLAYCAL_PRESET_RG35XXSP: return DisplayCalDefaults_RG35XXSP;
		case DISPLAYCAL_PRESET_RG35XXPRO: return DisplayCalDefaults_RG35XXPRO;
		case DISPLAYCAL_PRESET_RG40XXH: return DisplayCalDefaults_RG40XXH;
		case DISPLAYCAL_PRESET_RG40XXV: return DisplayCalDefaults_RG40XXV;
		case DISPLAYCAL_PRESET_RGCUBEXX: return DisplayCalDefaults_RGCubeXX;
		default: break;
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
