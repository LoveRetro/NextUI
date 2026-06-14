#ifndef DISPLAYCAL_H
#define DISPLAYCAL_H

#include <stddef.h>

#define DISPLAYCAL_GAIN_SCALE 100
#define DISPLAYCAL_GAIN_MIN 0
#define DISPLAYCAL_GAIN_MAX 200

#define DISPLAYCAL_DEFAULT_ENABLED 1
#define DISPLAYCAL_DEFAULT_RED_GAIN 1.0
#define DISPLAYCAL_DEFAULT_GREEN_GAIN 0.92
#define DISPLAYCAL_DEFAULT_BLUE_GAIN 0.58

#define DISPLAYCAL_DEFAULT_RED_GAIN_VALUE ((int)(DISPLAYCAL_DEFAULT_RED_GAIN * DISPLAYCAL_GAIN_SCALE + 0.5))
#define DISPLAYCAL_DEFAULT_GREEN_GAIN_VALUE ((int)(DISPLAYCAL_DEFAULT_GREEN_GAIN * DISPLAYCAL_GAIN_SCALE + 0.5))
#define DISPLAYCAL_DEFAULT_BLUE_GAIN_VALUE ((int)(DISPLAYCAL_DEFAULT_BLUE_GAIN * DISPLAYCAL_GAIN_SCALE + 0.5))

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	double red_gain;
	double green_gain;
	double blue_gain;
} DisplayCalGains;

void DisplayCal_initGains(DisplayCalGains *gains);
double DisplayCal_clampGain(double value);
int DisplayCal_clampGainValue(int value);
void DisplayCal_formatGainValue(int value, char *output, size_t output_size);
int DisplayCal_applyGains(const DisplayCalGains *gains);
int DisplayCal_enableWithValues(int red_gain, int green_gain, int blue_gain);
int DisplayCal_disable(void);

#ifdef __cplusplus
}
#endif

#endif
