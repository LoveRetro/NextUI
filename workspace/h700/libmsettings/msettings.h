#ifndef __msettings_h__
#define __msettings_h__

#define SETTINGS_DEFAULT_BRIGHTNESS 4
#define SETTINGS_DEFAULT_COLORTEMP 20
#define SETTINGS_DEFAULT_CONTRAST 0
#define SETTINGS_DEFAULT_SATURATION 0
#define SETTINGS_DEFAULT_EXPOSURE 0
#define SETTINGS_DEFAULT_VOLUME 8
#define SETTINGS_DEFAULT_HEADPHONE_VOLUME 4
#define SETTINGS_DEFAULT_FAN_SPEED 0

#define SETTINGS_DEFAULT_MUTE_NO_CHANGE -69

void InitSettings(void);
void QuitSettings(void);
int InitializedSettings(void);

int GetBrightness(void);
int GetColortemp(void);
int GetContrast(void);
int GetSaturation(void);
int GetExposure(void);
int GetDisplayCalEnabled(void);
int GetDisplayCalRedGain(void);
int GetDisplayCalGreenGain(void);
int GetDisplayCalBlueGain(void);
int GetVolume(void);

void SetRawBrightness(int value); // 0-255
void SetRawColortemp(int value); // 0-255
void SetRawContrast(int value); // 0-100
void SetRawSaturation(int value); // 0-100
void SetRawExposure(int value); // 0-100
void SetRawDisplayCal(int enabled, int red_gain, int green_gain, int blue_gain);
void SetRawVolume(int value); // 0-100

void SetBrightness(int value); // 0-10
void SetColortemp(int value); // 0-40
void SetContrast(int value); // -4-5
void SetSaturation(int value); // -5-5
void SetExposure(int value); // -4-5
void SetDisplayCalEnabled(int value); // 0-1
void SetDisplayCalRedGain(int value); // 0-200, 100 is neutral
void SetDisplayCalGreenGain(int value); // 0-200, 100 is neutral
void SetDisplayCalBlueGain(int value); // 0-200, 100 is neutral
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

// unused
inline int GetFanSpeed(void) {
    return 0;
}
inline void SetFanSpeed(int value) {
    // do nothing
}

// H700 has no FN switch. Shared UI code still references this API.
static inline int GetMute(void) { return 0; }
static inline void SetMute(int value) { (void)value; }
static inline int GetMutedBrightness(void) { return SETTINGS_DEFAULT_MUTE_NO_CHANGE; }
static inline void SetMutedBrightness(int value) { (void)value; }
static inline int GetMutedColortemp(void) { return SETTINGS_DEFAULT_MUTE_NO_CHANGE; }
static inline void SetMutedColortemp(int value) { (void)value; }
static inline int GetMutedContrast(void) { return SETTINGS_DEFAULT_MUTE_NO_CHANGE; }
static inline void SetMutedContrast(int value) { (void)value; }
static inline int GetMutedSaturation(void) { return SETTINGS_DEFAULT_MUTE_NO_CHANGE; }
static inline void SetMutedSaturation(int value) { (void)value; }
static inline int GetMutedExposure(void) { return SETTINGS_DEFAULT_MUTE_NO_CHANGE; }
static inline void SetMutedExposure(int value) { (void)value; }
static inline int GetMutedVolume(void) { return SETTINGS_DEFAULT_MUTE_NO_CHANGE; }
static inline void SetMutedVolume(int value) { (void)value; }
static inline int GetMuteDisablesDpad(void) { return 0; }
static inline void SetMuteDisablesDpad(int value) { (void)value; }
static inline int GetMuteEmulatesJoystick(void) { return 0; }
static inline void SetMuteEmulatesJoystick(int value) { (void)value; }
static inline int GetMuteTurboA(void) { return 0; }
static inline void SetMuteTurboA(int value) { (void)value; }
static inline int GetMuteTurboB(void) { return 0; }
static inline void SetMuteTurboB(int value) { (void)value; }
static inline int GetMuteTurboX(void) { return 0; }
static inline void SetMuteTurboX(int value) { (void)value; }
static inline int GetMuteTurboY(void) { return 0; }
static inline void SetMuteTurboY(int value) { (void)value; }
static inline int GetMuteTurboL1(void) { return 0; }
static inline void SetMuteTurboL1(int value) { (void)value; }
static inline int GetMuteTurboL2(void) { return 0; }
static inline void SetMuteTurboL2(int value) { (void)value; }
static inline int GetMuteTurboR1(void) { return 0; }
static inline void SetMuteTurboR1(int value) { (void)value; }
static inline int GetMuteTurboR2(void) { return 0; }
static inline void SetMuteTurboR2(int value) { (void)value; }

#endif  // __msettings_h__
