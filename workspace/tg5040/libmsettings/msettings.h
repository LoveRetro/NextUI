#ifndef __msettings_h__
#define __msettings_h__

void InitSettings(void);
void QuitSettings(void);

int GetBrightness(void);
int GetColortemp(void);
int GetVolume(void);
int GetRumble(void);

void SetRawBrightness(int value); // 0-255
void SetRawColortemp(int value); // 0-255
void SetRawVolume(int value); // 0-100
void SetRawRumble(int value); // 0-10

void SetBrightness(int value); // 0-10
void SetColortemp(int value); // 0-10
void SetVolume(int value); // 0-20
void SetRumble(int value); // 0-10

int GetJack(void);
void SetJack(int value); // 0-1

int GetHDMI(void);
void SetHDMI(int value); // 0-1

int GetMute(void);
void SetMute(int value); // 0-1

#endif  // __msettings_h__
