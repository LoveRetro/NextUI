#ifndef __msettings_h__
#define __msettings_h__

#define SETTINGS_DEFAULT_BRIGHTNESS 2
#define SETTINGS_DEFAULT_COLORTEMP 20
#define SETTINGS_DEFAULT_CONTRAST 0
#define SETTINGS_DEFAULT_SATURATION 0
#define SETTINGS_DEFAULT_EXPOSURE 0
#define SETTINGS_DEFAULT_VOLUME 8
#define SETTINGS_DEFAULT_HEADPHONE_VOLUME 4

#define SETTINGS_DEFAULT_MUTE_NO_CHANGE -69

void InitSettings(void);
void QuitSettings(void);
int InitializedSettings(void);

int GetBrightness(void);
int GetColortemp(void);
int GetContrast(void);
int GetSaturation(void);
int GetExposure(void);
int GetVolume(void);

void SetRawBrightness(int value); // 0-255
void SetRawColortemp(int value); // 0-255
void SetRawContrast(int value); // 0-100
void SetRawSaturation(int value); // 0-100
void SetRawExposure(int value); // 0-100
void SetRawVolume(int value); // 0-100

void SetBrightness(int value); // 0-10
void SetColortemp(int value); // 0-40
void SetContrast(int value); // -4-5
void SetSaturation(int value); // -5-5
void SetExposure(int value); // -4-5
void SetVolume(int value); // 0-20

int GetJack(void);
void SetJack(int value); // 0-1

#define AUDIO_SINK_DEFAULT 0 // use system default, usually speaker (or jack if plugged in)
#define AUDIO_SINK_BLUETOOTH 1 // software control via bluealsa, not a separate card
#define AUDIO_SINK_USBDAC 2 // assumes being exposed as card 1 to alsa
int GetAudioSink(void);
void SetAudioSink(int value);

int GetHDMI(void);
void SetHDMI(int value); // 0-1

int GetMute(void);
void SetMute(int value); // 0-1

// custom mute mode persistence layer

int GetMutedBrightness(void);
int GetMutedColortemp(void);
int GetMutedContrast(void);
int GetMutedSaturation(void);
int GetMutedExposure(void);
int GetMutedVolume(void);
int GetMuteDisablesDpad(void);
int GetMuteEmulatesJoystick(void);
int GetMuteTurboA(void);
int GetMuteTurboB(void);
int GetMuteTurboX(void);
int GetMuteTurboY(void);
int GetMuteTurboL1(void);
int GetMuteTurboL2(void);
int GetMuteTurboR1(void);
int GetMuteTurboR2(void);

void SetMutedBrightness(int);
void SetMutedColortemp(int);
void SetMutedContrast(int);
void SetMutedSaturation(int);
void SetMutedExposure(int);
void SetMutedVolume(int);
void SetMuteDisablesDpad(int);
void SetMuteEmulatesJoystick(int);
void SetMuteTurboA(int);
void SetMuteTurboB(int);
void SetMuteTurboX(int);
void SetMuteTurboY(int);
void SetMuteTurboL1(int);
void SetMuteTurboL2(int);
void SetMuteTurboR1(int);
void SetMuteTurboR2(int);

#endif  // __msettings_h__
