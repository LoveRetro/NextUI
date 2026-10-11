#include <stdio.h>
#include <unistd.h>
#include "msettings.h"

int main (int argc, char *argv[]) {
	InitSettings();
	
	sleep(1);
	SetVolume(GetVolume());
	SetBrightness(GetBrightness());
	SetColortemp(GetColortemp());
	SetContrast(GetContrast());
	SetSaturation(GetSaturation());
	SetExposure(GetExposure());
	SetRawDisplayCal(
		GetDisplayCalEnabled(),
		GetDisplayCalRedGain(),
		GetDisplayCalGreenGain(),
		GetDisplayCalBlueGain());
	return 0;
}
