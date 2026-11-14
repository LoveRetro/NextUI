// h700 RGB LEDs
//
// Included from platform.c (like generic_video.c et al) so the 13 makefiles
// that compile platform.c directly don't each need a new source file.
//
// Only three SKUs have RGB LEDs: rg40xxh, rg40xxv, and rgcubexx.
// They are NOT in /sys/class/leds and there is no led_anim driver (that is
// TrimUI only) -- they hang off a separate MCU reached over UART5 at 115200
// 8N1, powered by the axp2202 mcu_pwr rail. The protocol is
//
//     <mode> <brightness> <payload...> <checksum>
//
// with checksum = sum(preceding bytes) & 0xFF, written as raw bytes.
//
//     mode 1     solid, payload 8x(R,G,B) for the right bank then 8x(R,G,B)
//                for the left -- 16 LED positions in two banks of 8
//     mode 2/3/4 breath fast/med/slow, same per-bank colour payload as solid
//     mode 5/6   rainbow mono/multi, payload <1> <1> <speed 0-255>
//
// The MCU animates on its own, exactly like the led_anim kernel driver does on
// TrimUI, so there is no userspace animation here -- we translate NextUI's
// effect ids onto the modes above and let the firmware run them.
//
// Hardware limits that shape the design: one brightness byte per frame (global,
// not per zone) and a single effect for the whole strip. Zone 0 governs the
// effect and speed; brightness is the max across zones. Solid and breath modes
// scale each bank's colour for its own brightness. Rainbow brightness is global.

#include <termios.h>
#include <sys/file.h>
#include <string.h>

// H700_LED_TTY / H700_MCU_PWR live in platform.c: detect_device() probes for
// them, and it runs before this file is included.

// MCU modes
#define MCU_SOLID 1
#define MCU_BREATH_FAST 2
#define MCU_BREATH_MED 3
#define MCU_BREATH_SLOW 4
#define MCU_RAINBOW 5
#define MCU_RAINBOW_MULTI 6

#define LED_PIXELS 16				  // addressable positions, two banks of 8
#define LED_BANK (LED_PIXELS / 2)	  // positions per bank
#define LED_FRAME_MAX (2 + LED_PIXELS * 3 + 1) // mode + brightness + payload + checksum

// NextUI effect ids are a shared, TrimUI-derived space (see api.h). We expose
// the subset the MCU can render and translate the rest to the nearest mode.
// Id 9 is "Twinkle" on TrimUI; we reuse it for the MCU's second rainbow, which
// is safe because h700 keeps its own settings file (PLAT_getLedSettingsFile).
#define H700_EFFECT_BREATHE 2
#define H700_EFFECT_STATIC 4
#define H700_EFFECT_RAINBOW 8
#define H700_EFFECT_RAINBOW_MULTI 9

static const struct {
	int id;
	const char *name;
} h700_led_effects[] = {
	{H700_EFFECT_STATIC, "Static"},
	{H700_EFFECT_BREATHE, "Breathe"},
	{H700_EFFECT_RAINBOW, "Rainbow"},
	{H700_EFFECT_RAINBOW_MULTI, "Rainbow Multi"},
};
#define H700_EFFECT_COUNT ((int)(sizeof(h700_led_effects) / sizeof(h700_led_effects[0])))

typedef struct {
	uint32_t color; // 0xRRGGBB
	int effect;		// shared effect id
	int speed;		// ms, as LedControl stores it
	int brightness; // 0-100
} LedZone;

static struct {
	int fd;
	pthread_mutex_t lock;
	LedZone zone[MAX_LIGHTS];
	uint8_t last[LED_FRAME_MAX];
	int last_len; // 0 = cache invalid, always resend
} led = {
	.fd = -1,
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

///////////////////////////////
// frame assembly

static int LED_breathModeForSpeed(int speed) {
	// LedControl's speed row runs 0-4900 in steps of 100; the MCU only offers
	// three breath rates, so split that range into thirds.
	if (speed < 1700) return MCU_BREATH_FAST;
	if (speed < 3300) return MCU_BREATH_MED;
	return MCU_BREATH_SLOW;
}

static int LED_modeForEffect(int effect, int speed) {
	switch (effect) {
	case 1: return MCU_BREATH_FAST;				  // Linear
	case H700_EFFECT_BREATHE: return LED_breathModeForSpeed(speed);
	case 3: return MCU_BREATH_SLOW;				  // Interval Breathe (low/critical battery)
	case H700_EFFECT_STATIC: return MCU_SOLID;
	case 5: return MCU_BREATH_FAST;				  // Blink 1 - the MCU has no blink
	case 6: return MCU_BREATH_MED;				  // Blink 2
	case 7: return MCU_BREATH_SLOW;				  // Blink 3
	case H700_EFFECT_RAINBOW: return MCU_RAINBOW;
	case H700_EFFECT_RAINBOW_MULTI: return MCU_RAINBOW_MULTI;
	default: return MCU_SOLID;					  // per-position effects we can't render
	}
}

static void LED_putBank(uint8_t *buf, int *n, const LedZone *zone, int brightness) {
	int level = zone->brightness;
	if (level < 0) level = 0;
	if (level > brightness) level = brightness;

	// The MCU brightness applies to both banks; dim this bank through its RGB.
	uint8_t r = (zone->color >> 16) & 0xFF;
	uint8_t g = (zone->color >> 8) & 0xFF;
	uint8_t b = zone->color & 0xFF;
	if (brightness > 0) {
		r = r * level / brightness;
		g = g * level / brightness;
		b = b * level / brightness;
	}
	else {
		r = g = b = 0;
	}
	for (int i = 0; i < LED_BANK; i++) {
		buf[(*n)++] = r;
		buf[(*n)++] = g;
		buf[(*n)++] = b;
	}
}

// builds a frame from the current zone state, returns its length
static int LED_buildFrame(uint8_t *buf) {
	int count = dev_num_leds;
	if (count < 1) return 0;
	if (count > MAX_LIGHTS) count = MAX_LIGHTS;

	// brightness is a single byte for the whole strip
	int brightness = 0;
	for (int i = 0; i < count; i++)
		if (led.zone[i].brightness > brightness) brightness = led.zone[i].brightness;
	if (brightness > 100) brightness = 100;
	if (brightness < 0) brightness = 0;

	// LIGHT_PROFILE_OFF zeroes brightness and must actually extinguish, so
	// force a black solid frame rather than leaving an effect running dim.
	int mode = brightness == 0
				   ? MCU_SOLID
				   : LED_modeForEffect(led.zone[0].effect, led.zone[0].speed);

	int n = 0;
	buf[n++] = mode;
	buf[n++] = (brightness * 255) / 100;

	if (mode == MCU_RAINBOW || mode == MCU_RAINBOW_MULTI) {
		int speed = led.zone[0].speed;
		if (speed < 0) speed = 0;
		if (speed > 4900) speed = 4900;
		buf[n++] = 1;
		buf[n++] = 1;
		buf[n++] = (speed * 255) / 4900;
	}
	else {
		// Right bank first, as on muOS and KNULLI. Mirror the single bank on RG40XX V.
		LED_putBank(buf, &n, &led.zone[count > 1 ? 1 : 0], brightness);
		LED_putBank(buf, &n, &led.zone[0], brightness);
	}

	int sum = 0;
	for (int i = 0; i < n; i++)
		sum += buf[i];
	buf[n++] = sum & 0xFF;

	return n;
}

// caller must hold led.lock
static void LED_commitLocked(void) {
	if (led.fd < 0) return;

	uint8_t buf[LED_FRAME_MAX];
	int len = LED_buildFrame(buf);
	if (len <= 0) return;

	if (led.last_len == len && memcmp(buf, led.last, len) == 0)
		return; // nothing changed, don't touch the wire

	// The fd is non-blocking on purpose: ambient mode repaints from the
	// libretro per-frame input poll, and a blocking write of a 51 byte frame
	// costs 4.4ms at 115200 -- a quarter of a 60fps frame budget. Writing into
	// the tty buffer instead is a memcpy, and the frame drains long before the
	// next one arrives (51 bytes every 16.7ms against 11520 bytes/sec).
	ssize_t written = write(led.fd, buf, len);
	if (written != len) {
		// EAGAIN means the buffer is full; leave the cache alone so the next
		// commit retries rather than assuming this frame landed
		led.last_len = 0;
		return;
	}

	memcpy(led.last, buf, len);
	led.last_len = len;
}

static void LED_commit(void) {
	pthread_mutex_lock(&led.lock);
	LED_commitLocked();
	pthread_mutex_unlock(&led.lock);
}

// The MCU loses its state when mcu_pwr is cut over sleep, so the cached frame
// must not suppress the repaint on wake.
static void LED_invalidateCache(void) {
	pthread_mutex_lock(&led.lock);
	led.last_len = 0;
	pthread_mutex_unlock(&led.lock);
}

///////////////////////////////
// port lifecycle

static void LED_openPort(void) {
	if (led.fd >= 0) return;

	putInt(H700_MCU_PWR, 1);
	usleep(100 * 1000); // let the MCU rail come up before the first frame

	int fd = open(H700_LED_TTY, O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		LOG_warn("LED: unable to open %s: %s\n", H700_LED_TTY, strerror(errno));
		return;
	}

	// nextui hands off to minarch and back; if both ever held the port at once
	// their frames would interleave into garbage the MCU can't parse
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		LOG_warn("LED: %s already claimed, skipping\n", H700_LED_TTY);
		close(fd);
		return;
	}

	struct termios tio;
	if (tcgetattr(fd, &tio) < 0) {
		LOG_warn("LED: tcgetattr failed: %s\n", strerror(errno));
		close(fd);
		return;
	}
	cfmakeraw(&tio);
	cfsetispeed(&tio, B115200);
	cfsetospeed(&tio, B115200);
	tio.c_cflag |= CLOCAL | CREAD | CS8;
	tio.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
	if (tcsetattr(fd, TCSANOW, &tio) < 0) {
		LOG_warn("LED: tcsetattr failed: %s\n", strerror(errno));
		close(fd);
		return;
	}

	led.fd = fd;
	led.last_len = 0;
	LOG_info("LED: %s open, %i zone(s)\n", H700_LED_TTY, dev_num_leds);
}

// turn the LEDs off and drop the MCU rail - poweroff only, never on a normal
// process exit (nextui <-> minarch handoffs would blink the LEDs dark)
static void LED_shutdown(void) {
	if (led.fd < 0) return;

	pthread_mutex_lock(&led.lock);
	for (int i = 0; i < MAX_LIGHTS; i++)
		led.zone[i].brightness = 0;
	led.last_len = 0;
	LED_commitLocked();
	// the frame is queued in the tty buffer; give it the ~4.4ms it needs to
	// reach the MCU before the rail goes away
	tcdrain(led.fd);
	close(led.fd);
	led.fd = -1;
	pthread_mutex_unlock(&led.lock);

	putInt(H700_MCU_PWR, 0);
}

///////////////////////////////
// platform hooks

int PLAT_getNumLeds(void) {
	detect_device();
	return dev_num_leds;
}

const char *PLAT_getLedSettingsFile(void) {
	// deliberately not "ledsettings.txt": shared userdata can come off a card
	// that was in a TrimUI, and its 3-5 sections would be mis-assigned here
	return "ledsettings_h700.txt";
}

const char *PLAT_getLedLabel(int index) {
	switch (index) {
	case 0: return "Left";
	case 1: return "Right";
	default: return NULL;
	}
}

int PLAT_getLedEffectCount(void) { return H700_EFFECT_COUNT; }

int PLAT_getLedEffectId(int index) {
	if (index < 0 || index >= H700_EFFECT_COUNT) return H700_EFFECT_STATIC;
	return h700_led_effects[index].id;
}

const char *PLAT_getLedEffectName(int effect_id) {
	for (int i = 0; i < H700_EFFECT_COUNT; i++)
		if (h700_led_effects[i].id == effect_id) return h700_led_effects[i].name;
	return NULL;
}

void PLAT_initDefaultLeds(void) {
	detect_device();
	if (dev_num_leds > 0)
		lightsDefault[0] = (LightSettings){"Left", "l", H700_EFFECT_STATIC, 1000, 100, 0x440044, 0x440044, 0, {}, 1, 100, 0};
	if (dev_num_leds > 1)
		lightsDefault[1] = (LightSettings){"Right", "r", H700_EFFECT_STATIC, 1000, 100, 0x440044, 0x440044, 0, {}, 1, 100, 0};
}

void PLAT_initLeds(LightSettings *lights) {
	PLAT_initDefaultLeds();
	if (dev_num_leds < 1) return;

	FILE *file = PLAT_OpenSettings(PLAT_getLedSettingsFile());
	if (file == NULL) {
		LOG_warn("Unable to open led settings file\n");
	}
	else {
		char line[256];
		int current_light = -1;
		while (fgets(line, sizeof(line), file)) {
			if (line[0] == '[') {
				char light_name[255];
				if (sscanf(line, "[%49[^]]]", light_name) == 1) {
					current_light++;
					if (current_light < dev_num_leds) {
						strncpy(lights[current_light].name, light_name, 255 - 1);
						lights[current_light].name[255 - 1] = '\0';
						// cycles is never persisted by LedControl, and anything
						// other than -1 would make every effect a one-shot
						lights[current_light].cycles = -1;
					}
					else {
						LOG_info("Maximum number of lights (%d) exceeded. Ignoring further sections.\n", dev_num_leds);
						current_light = -1;
					}
				}
			}
			else if (current_light >= 0 && current_light < dev_num_leds) {
				int temp_value;
				uint32_t temp_color;
				char filename[255];

				if (sscanf(line, "filename=%s", filename) == 1) {
					strncpy(lights[current_light].filename, filename, 255 - 1);
					lights[current_light].filename[255 - 1] = '\0';
					continue;
				}
				if (sscanf(line, "effect=%d", &temp_value) == 1) {
					lights[current_light].effect = temp_value;
					continue;
				}
				if (sscanf(line, "color1=%x", &temp_color) == 1) {
					lights[current_light].color1 = temp_color;
					continue;
				}
				if (sscanf(line, "color2=%x", &temp_color) == 1) {
					lights[current_light].color2 = temp_color;
					continue;
				}
				if (sscanf(line, "speed=%d", &temp_value) == 1) {
					lights[current_light].speed = temp_value;
					continue;
				}
				if (sscanf(line, "brightness=%d", &temp_value) == 1) {
					lights[current_light].brightness = temp_value;
					continue;
				}
				if (sscanf(line, "trigger=%d", &temp_value) == 1) {
					lights[current_light].trigger = temp_value;
					continue;
				}
				if (sscanf(line, "inbrightness=%d", &temp_value) == 1) {
					lights[current_light].inbrightness = temp_value;
					continue;
				}
			}
		}
		fclose(file);
	}

	LED_openPort();
}

// The setters below only stage state. LEDS_updateLeds() calls all five of them
// for every light, so committing eagerly would put four redundant frames (and
// one with the second zone still stale) on the wire per update. PLAT_setLedEffect
// is the last call for a given light -- as on TrimUI, where it is likewise the
// point the settings get applied -- so the frame goes out once the final zone
// has been staged.

static int LED_zoneIndex(LightSettings *led_settings) {
	if (!led_settings) return -1;
	if (exactMatch(led_settings->filename, "l")) return 0;
	if (exactMatch(led_settings->filename, "r")) return 1;
	return -1;
}

// returns the staged zone, or NULL if this light isn't ours
static LedZone *LED_zoneFor(LightSettings *led_settings) {
	int index = LED_zoneIndex(led_settings);
	if (index < 0 || index >= dev_num_leds) return NULL;
	return &led.zone[index];
}

void PLAT_setLedInbrightness(LightSettings *led_settings) {
	pthread_mutex_lock(&led.lock);
	LedZone *zone = LED_zoneFor(led_settings);
	if (zone) zone->brightness = led_settings->inbrightness;
	pthread_mutex_unlock(&led.lock);
}

void PLAT_setLedBrightness(LightSettings *led_settings) {
	pthread_mutex_lock(&led.lock);
	LedZone *zone = LED_zoneFor(led_settings);
	if (zone) zone->brightness = led_settings->brightness;
	pthread_mutex_unlock(&led.lock);
}

void PLAT_setLedColor(LightSettings *led_settings) {
	pthread_mutex_lock(&led.lock);
	LedZone *zone = LED_zoneFor(led_settings);
	if (zone) zone->color = led_settings->color1;
	pthread_mutex_unlock(&led.lock);
}

void PLAT_setLedEffectSpeed(LightSettings *led_settings) {
	pthread_mutex_lock(&led.lock);
	LedZone *zone = LED_zoneFor(led_settings);
	if (zone) zone->speed = led_settings->speed;
	pthread_mutex_unlock(&led.lock);
}

// the MCU has no cycle count - effects run until something replaces them
void PLAT_setLedEffectCycles(LightSettings *led_settings) {}

void PLAT_setLedEffect(LightSettings *led_settings) {
	pthread_mutex_lock(&led.lock);
	int index = LED_zoneIndex(led_settings);
	LedZone *zone = (index >= 0 && index < dev_num_leds) ? &led.zone[index] : NULL;
	if (zone) zone->effect = led_settings->effect;
	// commit once the last zone of this pass has been staged
	int commit = zone && index == dev_num_leds - 1;
	if (commit) LED_commitLocked();
	pthread_mutex_unlock(&led.lock);
}
