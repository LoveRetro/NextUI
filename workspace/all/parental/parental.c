// Parental.pak -- daily play time budget.
//
// One binary, four modes:
//   parental.elf           the UI (remaining time, and a code-locked settings menu)
//   parental.elf --gate    called by the pre-launch hook; exit 1 refuses the launch
//   parental.elf --watch   backgrounded by the pre-launch hook; warns then stops the game
//   parental.elf --stop    called by the post-launch hook; kills the watcher
//
// Time played is not tracked here: nextui already logs every session through
// gametimectl into the shared sqlite db, so we only aggregate it per day via
// play_activity_get_play_time_since().

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "utils.h"

#include <sqlite3.h>
#include <gametimedb.h>

///////////////////////////////////////
// config

#define PARENTAL_CFG_PATH SHARED_USERDATA_PATH "/parental.txt"
#define PARENTAL_PID_PATH "/tmp/parental.pid"

#define MIN_CODE_LEN 2
#define MAX_CODE_LEN 8

#define DEFAULT_LIMIT_MINUTES 90
#define DEFAULT_WARN_MINUTES 5

#define WATCH_INTERVAL_SEC 10

// How long the "time is up" notice stays on screen before the game is stopped.
// The player keeps playing during it, which is the point: being cut off mid-move
// with no warning is what makes a limit feel broken rather than enforced.
#define STOP_NOTICE_SEC 3

// Emulator paks the budget does not apply to, stored as a comma separated list
// of pak names. Only exemptions are persisted, never the full list of installed
// emulators: a stored full list would be a snapshot, and anything installed
// afterwards would silently fall outside it. An unknown pak must stay blocked.
#define EXEMPT_MAX 512
#define MAX_PAK_NAME 64

typedef struct {
	int limit_minutes;      // <= 0 disables the whole thing
	int warn_minutes;       // not exposed in the UI, edit the file to change it
	unsigned long code_hash;
	int code_len;
	char exempt[EXEMPT_MAX];
	int extra_minutes;      // one-off top-up on top of limit_minutes
	int extra_day;          // startOfToday() when extra_minutes was granted; a
	                         // stale (non-matching) value means it has expired
} Config;

// Buttons a code can be made of. B is deliberately absent: it always means
// "leave this screen", which is what makes code entry need no confirm key.
// START is absent too, it terminates capture when setting a new code.
static const struct {
	int btn;
	const char *name;
} CODE_BUTTONS[] = {
	{ BTN_UP,     "UP"     },
	{ BTN_DOWN,   "DOWN"   },
	{ BTN_LEFT,   "LEFT"   },
	{ BTN_RIGHT,  "RIGHT"  },
	{ BTN_A,      "A"      },
	{ BTN_X,      "X"      },
	{ BTN_Y,      "Y"      },
	{ BTN_L1,     "L1"     },
	{ BTN_R1,     "R1"     },
	{ BTN_SELECT, "SELECT" },
};
#define CODE_BUTTON_COUNT ((int)(sizeof(CODE_BUTTONS) / sizeof(CODE_BUTTONS[0])))

// The default code: UP, UP, DOWN, DOWN.
static const int DEFAULT_CODE[] = { 0, 0, 1, 1 };
#define DEFAULT_CODE_LEN ((int)(sizeof(DEFAULT_CODE) / sizeof(DEFAULT_CODE[0])))

// djb2. Not a security measure -- the threat model here is a child poking
// around with Files.pak, not an attacker. It only keeps the code from being
// readable at a glance.
static unsigned long hashCode(const int *indices, int len)
{
	unsigned long hash = 5381;
	for (int i = 0; i < len; i++) {
		const char *name = CODE_BUTTONS[indices[i]].name;
		for (const char *c = name; *c; c++)
			hash = ((hash << 5) + hash) + (unsigned char)*c;
		hash = ((hash << 5) + hash) + ',';
	}
	return hash;
}

static void configSave(const Config *cfg)
{
	FILE *file = fopen(PARENTAL_CFG_PATH, "w");
	if (!file) return;
	fprintf(file, "limit_minutes=%d\n", cfg->limit_minutes);
	fprintf(file, "warn_minutes=%d\n", cfg->warn_minutes);
	fprintf(file, "code_hash=%lu\n", cfg->code_hash);
	fprintf(file, "code_len=%d\n", cfg->code_len);
	fprintf(file, "exempt=%s\n", cfg->exempt);
	fprintf(file, "extra_minutes=%d\n", cfg->extra_minutes);
	fprintf(file, "extra_day=%d\n", cfg->extra_day);
	fclose(file);
	sync();
}

static void configLoad(Config *cfg)
{
	cfg->limit_minutes = DEFAULT_LIMIT_MINUTES;
	cfg->warn_minutes = DEFAULT_WARN_MINUTES;
	cfg->code_hash = hashCode(DEFAULT_CODE, DEFAULT_CODE_LEN);
	cfg->code_len = DEFAULT_CODE_LEN;
	cfg->exempt[0] = '\0';
	cfg->extra_minutes = 0;
	cfg->extra_day = 0;

	FILE *file = fopen(PARENTAL_CFG_PATH, "r");
	if (!file) {
		configSave(cfg);
		return;
	}

	// must hold a full exempt= line, a short buffer would truncate the list
	// mid-name and silently drop or corrupt the last exemption
	char line[EXEMPT_MAX + 64];
	while (fgets(line, sizeof(line), file)) {
		trimTrailingNewlines(line);
		char *sep = strchr(line, '=');
		if (!sep) continue;
		*sep = '\0';
		const char *key = line;
		const char *value = sep + 1;

		if (strcmp(key, "limit_minutes") == 0) cfg->limit_minutes = atoi(value);
		else if (strcmp(key, "warn_minutes") == 0) cfg->warn_minutes = atoi(value);
		else if (strcmp(key, "code_hash") == 0) cfg->code_hash = strtoul(value, NULL, 10);
		else if (strcmp(key, "code_len") == 0) cfg->code_len = atoi(value);
		else if (strcmp(key, "exempt") == 0) {
			strncpy(cfg->exempt, value, EXEMPT_MAX - 1);
			cfg->exempt[EXEMPT_MAX - 1] = '\0';
		}
		else if (strcmp(key, "extra_minutes") == 0) cfg->extra_minutes = atoi(value);
		else if (strcmp(key, "extra_day") == 0) cfg->extra_day = atoi(value);
	}
	fclose(file);

	if (cfg->warn_minutes < 0) cfg->warn_minutes = 0;
	if (cfg->code_len < MIN_CODE_LEN || cfg->code_len > MAX_CODE_LEN)
		cfg->code_len = DEFAULT_CODE_LEN;
}

///////////////////////////////////////
// exemptions

// Whole-token match, so "GB.pak" never matches inside "GBA.pak".
static bool isExempt(const Config *cfg, const char *name)
{
	const char *p = cfg->exempt;
	size_t len = strlen(name);

	while (*p) {
		const char *end = strchr(p, ',');
		size_t span = end ? (size_t)(end - p) : strlen(p);
		if (span == len && strncmp(p, name, len) == 0) return true;
		if (!end) break;
		p = end + 1;
	}
	return false;
}

// Returns false when the list is full, so the caller can say so rather than
// letting the exemption silently not happen.
static bool exemptAdd(Config *cfg, const char *name)
{
	if (isExempt(cfg, name)) return true;

	size_t used = strlen(cfg->exempt);
	size_t need = strlen(name) + (used ? 1 : 0);
	if (used + need >= EXEMPT_MAX) return false;

	if (used) strcat(cfg->exempt, ",");
	strcat(cfg->exempt, name);
	return true;
}

static void exemptRemove(Config *cfg, const char *name)
{
	char rebuilt[EXEMPT_MAX];
	size_t len = strlen(name);
	const char *p = cfg->exempt;

	rebuilt[0] = '\0';
	while (*p) {
		const char *end = strchr(p, ',');
		size_t span = end ? (size_t)(end - p) : strlen(p);

		if (!(span == len && strncmp(p, name, len) == 0) && span > 0) {
			if (rebuilt[0]) strcat(rebuilt, ",");
			strncat(rebuilt, p, span);
		}
		if (!end) break;
		p = end + 1;
	}

	strcpy(cfg->exempt, rebuilt);
}

///////////////////////////////////////
// installed emulators
//
// Scanned only to populate the settings screen. The result is never written to
// parental.txt -- see the note on EXEMPT_MAX.

#define EMUS_SD_PATH SDCARD_PATH "/Emus/" PLATFORM
#define EMUS_SYSTEM_PATH PAKS_PATH "/Emus"
#define MAX_EMUS 64

typedef struct {
	char name[MAX_PAK_NAME];   // "GBA.pak"
} Emulator;

static int emuCompare(const void *a, const void *b)
{
	return strcasecmp(((const Emulator *)a)->name, ((const Emulator *)b)->name);
}

static int scanEmulatorDir(const char *path, Emulator *emus, int count)
{
	DIR *dir = opendir(path);
	if (!dir) return count;

	struct dirent *entry;
	while ((entry = readdir(dir)) && count < MAX_EMUS) {
		if (entry->d_name[0] == '.') continue;

		size_t len = strlen(entry->d_name);
		if (len < 5 || len >= MAX_PAK_NAME) continue;
		if (strcmp(entry->d_name + len - 4, ".pak") != 0) continue;

		bool seen = false;
		for (int i = 0; i < count; i++)
			if (strcmp(emus[i].name, entry->d_name) == 0) { seen = true; break; }
		if (seen) continue;

		strcpy(emus[count++].name, entry->d_name);
	}

	closedir(dir);
	return count;
}

// Both locations getEmuPath() resolves against, the sd card taking priority.
static int scanEmulators(Emulator *emus)
{
	int count = scanEmulatorDir(EMUS_SD_PATH, emus, 0);
	count = scanEmulatorDir(EMUS_SYSTEM_PATH, emus, count);
	qsort(emus, count, sizeof(Emulator), emuCompare);
	return count;
}

///////////////////////////////////////
// budget

static int startOfToday(void)
{
	time_t now = time(NULL);
	struct tm tm = *localtime(&now);
	tm.tm_hour = 0;
	tm.tm_min = 0;
	tm.tm_sec = 0;
	return (int)mktime(&tm);
}

static int secondsPlayedToday(void)
{
	return play_activity_get_play_time_since(startOfToday());
}

// Extra minutes granted for today, or 0 once the day has moved on. Expires
// on its own -- a stale extra_day just stops counting, nothing to clean up.
static int extraMinutesToday(const Config *cfg)
{
	return cfg->extra_day == startOfToday() ? cfg->extra_minutes : 0;
}

// Seconds left today, or -1 when there is no limit.
static int remainingFrom(const Config *cfg, int played)
{
	if (cfg->limit_minutes <= 0) return -1;
	int budget_minutes = cfg->limit_minutes + extraMinutesToday(cfg);
	int remaining = budget_minutes * 60 - played;
	return remaining < 0 ? 0 : remaining;
}

static int secondsRemaining(const Config *cfg)
{
	if (cfg->limit_minutes <= 0) return -1;
	return remainingFrom(cfg, secondsPlayedToday());
}

///////////////////////////////////////
// headless modes

static int processRunning(const char *name)
{
	DIR *dir = opendir("/proc");
	if (!dir) return 1; // can't tell, assume it is

	int found = 0;
	struct dirent *entry;
	while (!found && (entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;

		char path[64];
		snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
		FILE *file = fopen(path, "r");
		if (!file) continue;

		char comm[64] = { 0 };
		if (fgets(comm, sizeof(comm), file)) {
			trimTrailingNewlines(comm);
			if (strcmp(comm, name) == 0) found = 1;
		}
		fclose(file);
	}

	closedir(dir);
	return found;
}

// exit 0 to allow the launch, 1 to refuse it
static int modeGate(void)
{
	Config cfg;
	configLoad(&cfg);

	if (cfg.limit_minutes <= 0) return EXIT_SUCCESS;
	return secondsRemaining(&cfg) > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int modeWatch(void)
{
	Config cfg;
	configLoad(&cfg);
	if (cfg.limit_minutes <= 0) return EXIT_SUCCESS;

	putInt(PARENTAL_PID_PATH, (int)getpid());

	int warned = 0;
	int warned_final_minute = 0;
	while (1) {
		sleep(WATCH_INTERVAL_SEC);

		// the post-launch hook normally stops us, this is the safety net
		if (!processRunning("minarch.elf")) break;

		int remaining = secondsRemaining(&cfg);
		if (remaining <= 0) {
			LOG_info("parental: budget exhausted, stopping the game\n");
			// say so before pulling the plug, reusing the same SIGUSR2 path as
			// the warning above so minarch needs no second mechanism
			putFile(NOTIFY_PATH, "Time is up");
			system("killall -USR2 minarch.elf");
			sleep(STOP_NOTICE_SEC);
			system("killall -USR1 minarch.elf");
			break;
		}

		if (!warned && remaining <= cfg.warn_minutes * 60) {
			char msg[64];
			snprintf(msg, sizeof(msg), "Stopping in %d min", cfg.warn_minutes);
			putFile(NOTIFY_PATH, msg);
			system("killall -USR2 minarch.elf");
			warned = 1;
		}

		// last-minute heads up: same notify path, plus the led=1 directive so
		// minarch also blinks (LIGHT_PROFILE_APP_WARNING) for the final stretch
		if (!warned_final_minute && remaining <= 60) {
			putFile(NOTIFY_PATH, "1 minute left\nled=1");
			system("killall -USR2 minarch.elf");
			warned_final_minute = 1;
		}
	}

	unlink(PARENTAL_PID_PATH);
	return EXIT_SUCCESS;
}

static int modeStop(void)
{
	if (exists(PARENTAL_PID_PATH)) {
		int pid = getInt(PARENTAL_PID_PATH);
		if (pid > 1) kill(pid, SIGTERM);
		unlink(PARENTAL_PID_PATH);
	}
	return EXIT_SUCCESS;
}

///////////////////////////////////////
// UI

enum {
	SCREEN_HOME,
	SCREEN_CODE,
	SCREEN_ADMIN,
	SCREEN_LIMIT,
	SCREEN_EXTRA,
	SCREEN_NEWCODE,
	SCREEN_EXEMPT,
};

enum {
	ADMIN_LIMIT,
	ADMIN_EXTRA,
	ADMIN_EXEMPT,
	ADMIN_CODE,
	ADMIN_COUNT,
};

enum {
	FIELD_HOURS,
	FIELD_MINUTES,
	FIELD_COUNT,
};

#define ROW_HEIGHT (PILL_SIZE + 6)
#define SLOT_GAP 6
#define TOAST_DURATION_MS 1500

static bool quit = false;
static SDL_Surface *screen;

static char toast_message[64] = { 0 };
static uint32_t toast_until = 0;

static void sigHandler(int sig)
{
	switch (sig) {
	case SIGINT:
	case SIGTERM:
		quit = true;
		break;
	default:
		break;
	}
}

static void toast(const char *message)
{
	strncpy(toast_message, message, sizeof(toast_message) - 1);
	toast_message[sizeof(toast_message) - 1] = '\0';
	toast_until = SDL_GetTicks() + TOAST_DURATION_MS;
}

static int renderText(const char *text, TTF_Font *font, SDL_Color color, int x, int y)
{
	SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
	if (!surface) return 0;
	int width = surface->w;
	SDL_BlitSurface(surface, NULL, screen, &(SDL_Rect){ x, y });
	SDL_FreeSurface(surface);
	return width;
}

static int textWidth(const char *text, TTF_Font *font)
{
	int w = 0, h = 0;
	TTF_SizeUTF8(font, text, &w, &h);
	return w;
}

static int renderTextCentered(const char *text, TTF_Font *font, SDL_Color color, int y)
{
	return renderText(text, font, color, (screen->w - textWidth(text, font)) / 2, y);
}

static void renderTitle(const char *name, int show_setting)
{
	int max_width = screen->w - SCALE1(PADDING * 2);
	if (screen->w >= SCALE1(320)) {
		int ow = GFX_blitHardwareGroup(screen, show_setting);
		max_width = screen->w - SCALE1(PADDING * 2) - ow;
	}

	char title[256];
	int text_width = GFX_truncateText(font.large, name, title, max_width, SCALE1(BUTTON_PADDING * 2));
	max_width = MIN(max_width, text_width);

	SDL_Surface *text = TTF_RenderUTF8_Blended(font.large, title, COLOR_WHITE);
	GFX_blitPill(ASSET_BLACK_PILL, screen, &(SDL_Rect){ SCALE1(PADDING), SCALE1(PADDING), max_width, SCALE1(PILL_SIZE) });
	if (text) {
		SDL_BlitSurface(text, &(SDL_Rect){ 0, 0, max_width - SCALE1(BUTTON_PADDING * 2), text->h }, screen,
		                &(SDL_Rect){ SCALE1(PADDING + BUTTON_PADDING), SCALE1(PADDING + 4) });
		SDL_FreeSurface(text);
	}
}

// A full width selectable row, same look as the launcher's lists.
static void renderRow(const char *label, const char *value, int row, bool selected)
{
	int x = SCALE1(PADDING);
	int y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN) + row * SCALE1(ROW_HEIGHT);
	int w = screen->w - SCALE1(PADDING * 2);

	GFX_blitPill(selected ? ASSET_WHITE_PILL : ASSET_BLACK_PILL, screen,
	             &(SDL_Rect){ x, y, w, SCALE1(PILL_SIZE) });

	SDL_Color color = selected ? COLOR_BLACK : COLOR_WHITE;
	renderText(label, font.medium, color, x + SCALE1(BUTTON_PADDING), y + SCALE1(4));
	if (value) {
		int vw = textWidth(value, font.medium);
		renderText(value, font.medium, color, x + w - SCALE1(BUTTON_PADDING) - vw, y + SCALE1(4));
	}
}

// A track + fill bar: a plain rectangular 1px stroke (outer white rect,
// black rect inset 1px punched back on top of it) with the fill inset 2px
// further inside that stroke. fraction is clamped to [0,1].
//
// Narrower than renderRow's full-width pills -- right up against the
// screen edge (same margin as the rows) the stroke has no breathing room.
static void renderProgressBar(int y, float fraction)
{
	int bar_margin = SCALE1(PADDING * 2);
	int x = bar_margin;
	int w = screen->w - bar_margin * 2;
	int h = SCALE1(14);

	if (fraction < 0) fraction = 0;
	if (fraction > 1) fraction = 1;

	int stroke = SCALE1(1);
	int inset = stroke + SCALE1(2); // stroke + margin, from the outer edge to the fill

	SDL_FillRect(screen, &(SDL_Rect){ x, y, w, h }, RGB_WHITE);
	SDL_FillRect(screen, &(SDL_Rect){ x + stroke, y + stroke, w - stroke * 2, h - stroke * 2 }, RGB_BLACK);

	int fill_max_w = w - inset * 2;
	int fill_w = (int)(fill_max_w * fraction);
	if (fill_w > 0) SDL_FillRect(screen, &(SDL_Rect){ x + inset, y + inset, fill_w, h - inset * 2 }, RGB_WHITE);
}

// How many rows fit between the title and the button hints.
static int listRows(void)
{
	int top = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);
	int bottom = screen->h - SCALE1(PILL_SIZE + PADDING * 2);
	int rows = (bottom - top) / SCALE1(ROW_HEIGHT);
	return rows < 1 ? 1 : rows;
}

static void renderToast(void)
{
	if (!toast_message[0] || SDL_GetTicks() >= toast_until) return;

	int tw = textWidth(toast_message, font.medium);
	int w = tw + SCALE1(BUTTON_PADDING * 2);
	int x = (screen->w - w) / 2;
	int y = screen->h - SCALE1(PADDING + PILL_SIZE * 2 + BUTTON_MARGIN);

	GFX_blitPill(ASSET_WHITE_PILL, screen, &(SDL_Rect){ x, y, w, SCALE1(PILL_SIZE) });
	renderText(toast_message, font.medium, COLOR_BLACK, x + SCALE1(BUTTON_PADDING), y + SCALE1(4));
}

// The code entry widget, shared by the unlock screen and the change-code screen.
// Filled slots show the button name so the parent can see what they typed.
static void renderCodeSlots(const int *entered, int entered_len, int slot_count, int y)
{
	int slot_w = SCALE1(56);
	int gap = SCALE1(SLOT_GAP);
	int total = slot_count * slot_w + (slot_count - 1) * gap;

	// shrink the slots rather than overflow on narrow screens
	int available = screen->w - SCALE1(PADDING * 2);
	if (total > available) {
		slot_w = (available - (slot_count - 1) * gap) / slot_count;
		total = slot_count * slot_w + (slot_count - 1) * gap;
	}

	int x = (screen->w - total) / 2;
	for (int i = 0; i < slot_count; i++) {
		bool filled = i < entered_len;
		GFX_blitPill(filled ? ASSET_WHITE_PILL : ASSET_BLACK_PILL, screen,
		             &(SDL_Rect){ x, y, slot_w, SCALE1(PILL_SIZE) });
		if (filled) {
			const char *name = CODE_BUTTONS[entered[i]].name;
			char label[16];
			GFX_truncateText(font.small, name, label, slot_w, SCALE1(4));
			int lw = textWidth(label, font.small);
			renderText(label, font.small, COLOR_BLACK, x + (slot_w - lw) / 2, y + SCALE1(6));
		}
		x += slot_w + gap;
	}
}

// Returns the CODE_BUTTONS index that was just pressed, or -1.
static int pollCodeButton(void)
{
	for (int i = 0; i < CODE_BUTTON_COUNT; i++)
		if (PAD_justPressed(CODE_BUTTONS[i].btn)) return i;
	return -1;
}

int main(int argc, char *argv[])
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--gate") == 0) return modeGate();
		if (strcmp(argv[i], "--watch") == 0) return modeWatch();
		if (strcmp(argv[i], "--stop") == 0) return modeStop();
	}

	Config cfg;
	configLoad(&cfg);

	InitSettings();
	PWR_setCPUSpeed(CPU_SPEED_AUTO);

	screen = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();

	signal(SIGINT, sigHandler);
	signal(SIGTERM, sigHandler);

	int state = SCREEN_HOME;
	int admin_selected = ADMIN_LIMIT;

	int code_entered[MAX_CODE_LEN];
	int code_len = 0;

	// change-code screen: capture, then confirm
	int new_code[MAX_CODE_LEN];
	int new_len = 0;
	int confirming = 0;

	// limit editor
	int edit_hours = 0;
	int edit_minutes = 0;
	int edit_field = FIELD_HOURS;

	// emulator exemptions, scanned when the screen is opened
	Emulator emus[MAX_EMUS];
	int emu_count = 0;
	int emu_selected = 0;
	int emu_top = 0;

	// Only one thing runs in the foreground at a time on this device, so no
	// game session can legitimately still be "open" while we're the ones
	// running -- if one is, it's a row nextui.elf never got to close (that
	// only happens when its own launcher reinitializes, not when a Tools
	// pak like this one is opened directly). Sweep it so the total below is
	// a real, static number instead of ticking up live from a stale row.
	play_activity_stop_all();

	int played = secondsPlayedToday();
	uint32_t refreshed_at = SDL_GetTicks();

	int dirty = 1;
	int show_setting = 0;
	while (!quit) {
		GFX_startFrame();
		PAD_poll();

		// This might be too harsh, but ignore all combos with MENU (most likely a shortcut for someone else)
		if (PAD_justPressed(BTN_MENU)) {
			// ?
		}
		else switch (state) {

		case SCREEN_HOME:
			if (PAD_justPressed(BTN_A)) {
				code_len = 0;
				state = SCREEN_CODE;
				dirty = 1;
			}
			else if (PAD_justPressed(BTN_B)) {
				quit = true;
			}
			break;

		case SCREEN_CODE:
			if (PAD_justPressed(BTN_B)) {
				state = SCREEN_HOME;
				dirty = 1;
			}
			else {
				int pressed = pollCodeButton();
				if (pressed >= 0 && code_len < cfg.code_len) {
					code_entered[code_len++] = pressed;
					dirty = 1;

					if (code_len == cfg.code_len) {
						if (hashCode(code_entered, code_len) == cfg.code_hash) {
							admin_selected = ADMIN_LIMIT;
							state = SCREEN_ADMIN;
						}
						else {
							toast("Wrong code");
							code_len = 0;
						}
					}
				}
			}
			break;

		case SCREEN_ADMIN:
			if (PAD_justRepeated(BTN_UP)) {
				admin_selected = (admin_selected - 1 + ADMIN_COUNT) % ADMIN_COUNT;
				dirty = 1;
			}
			else if (PAD_justRepeated(BTN_DOWN)) {
				admin_selected = (admin_selected + 1) % ADMIN_COUNT;
				dirty = 1;
			}
			else if (PAD_justPressed(BTN_A)) {
				if (admin_selected == ADMIN_LIMIT) {
					edit_hours = cfg.limit_minutes / 60;
					edit_minutes = cfg.limit_minutes % 60;
					edit_field = FIELD_HOURS;
					state = SCREEN_LIMIT;
				}
				else if (admin_selected == ADMIN_EXTRA) {
					int extra = extraMinutesToday(&cfg);
					edit_hours = extra / 60;
					edit_minutes = extra % 60;
					edit_field = FIELD_HOURS;
					state = SCREEN_EXTRA;
				}
				else if (admin_selected == ADMIN_EXEMPT) {
					emu_count = scanEmulators(emus);
					emu_selected = 0;
					emu_top = 0;
					state = SCREEN_EXEMPT;
				}
				else {
					new_len = 0;
					code_len = 0;
					confirming = 0;
					state = SCREEN_NEWCODE;
				}
				dirty = 1;
			}
			else if (PAD_justPressed(BTN_B)) {
				state = SCREEN_HOME;
				dirty = 1;
			}
			break;

		case SCREEN_LIMIT:
			if (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_RIGHT)) {
				edit_field = (edit_field + 1) % FIELD_COUNT;
				dirty = 1;
			}
			else if (PAD_justRepeated(BTN_UP)) {
				if (edit_field == FIELD_HOURS) edit_hours = (edit_hours + 1) % 24;
				else edit_minutes = (edit_minutes + 5) % 60;
				dirty = 1;
			}
			else if (PAD_justRepeated(BTN_DOWN)) {
				if (edit_field == FIELD_HOURS) edit_hours = (edit_hours + 23) % 24;
				else edit_minutes = (edit_minutes + 55) % 60;
				dirty = 1;
			}
			else if (PAD_justPressed(BTN_A)) {
				cfg.limit_minutes = edit_hours * 60 + edit_minutes;
				configSave(&cfg);
				toast(cfg.limit_minutes > 0 ? "Limit saved" : "Limit disabled");
				state = SCREEN_ADMIN;
				dirty = 1;
			}
			else if (PAD_justPressed(BTN_B)) {
				state = SCREEN_ADMIN;
				dirty = 1;
			}
			break;

		case SCREEN_EXTRA:
			if (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_RIGHT)) {
				edit_field = (edit_field + 1) % FIELD_COUNT;
				dirty = 1;
			}
			else if (PAD_justRepeated(BTN_UP)) {
				if (edit_field == FIELD_HOURS) edit_hours = (edit_hours + 1) % 24;
				else edit_minutes = (edit_minutes + 5) % 60;
				dirty = 1;
			}
			else if (PAD_justRepeated(BTN_DOWN)) {
				if (edit_field == FIELD_HOURS) edit_hours = (edit_hours + 23) % 24;
				else edit_minutes = (edit_minutes + 55) % 60;
				dirty = 1;
			}
			else if (PAD_justPressed(BTN_A)) {
				cfg.extra_minutes = edit_hours * 60 + edit_minutes;
				cfg.extra_day = startOfToday();
				configSave(&cfg);
				toast(cfg.extra_minutes > 0 ? "Extra time added" : "Extra time cleared");
				state = SCREEN_ADMIN;
				dirty = 1;
			}
			else if (PAD_justPressed(BTN_B)) {
				state = SCREEN_ADMIN;
				dirty = 1;
			}
			break;

		case SCREEN_EXEMPT: {
			int rows = listRows();

			if (PAD_justRepeated(BTN_UP) && emu_count > 0) {
				emu_selected = (emu_selected - 1 + emu_count) % emu_count;
				dirty = 1;
			}
			else if (PAD_justRepeated(BTN_DOWN) && emu_count > 0) {
				emu_selected = (emu_selected + 1) % emu_count;
				dirty = 1;
			}
			else if (PAD_justPressed(BTN_A) && emu_count > 0) {
				const char *name = emus[emu_selected].name;
				if (isExempt(&cfg, name)) exemptRemove(&cfg, name);
				else if (!exemptAdd(&cfg, name)) toast("Too many exemptions");
				dirty = 1;
			}
			else if (PAD_justPressed(BTN_B)) {
				configSave(&cfg);
				state = SCREEN_ADMIN;
				dirty = 1;
			}

			// keep the selection inside the visible window
			if (emu_selected < emu_top) emu_top = emu_selected;
			if (emu_selected >= emu_top + rows) emu_top = emu_selected - rows + 1;
			break;
		}

		case SCREEN_NEWCODE:
			if (PAD_justPressed(BTN_B)) {
				state = SCREEN_ADMIN;
				dirty = 1;
			}
			else if (PAD_justPressed(BTN_START)) {
				if (!confirming) {
					if (new_len < MIN_CODE_LEN) {
						toast("Too short");
					}
					else {
						confirming = 1;
						code_len = 0;
					}
				}
				else {
					if (code_len == new_len && memcmp(code_entered, new_code, new_len * sizeof(int)) == 0) {
						cfg.code_hash = hashCode(new_code, new_len);
						cfg.code_len = new_len;
						configSave(&cfg);
						toast("Code updated");
						state = SCREEN_ADMIN;
					}
					else {
						toast("Codes differ");
						confirming = 0;
						new_len = 0;
						code_len = 0;
					}
				}
				dirty = 1;
			}
			else {
				int pressed = pollCodeButton();
				if (pressed >= 0) {
					if (!confirming) {
						if (new_len < MAX_CODE_LEN) new_code[new_len++] = pressed;
					}
					else {
						if (code_len < MAX_CODE_LEN) code_entered[code_len++] = pressed;
					}
					dirty = 1;
				}
			}
			break;
		}

		PWR_update(&dirty, &show_setting, NULL, NULL);

		// the played total moves on its own while a session is open elsewhere
		uint32_t now = SDL_GetTicks();
		if (now - refreshed_at >= 1000) {
			refreshed_at = now;
			int fresh = secondsPlayedToday();
			if (fresh != played) {
				played = fresh;
				if (state == SCREEN_HOME) dirty = 1;
			}
		}

		if (toast_message[0]) {
			if (now >= toast_until) toast_message[0] = '\0';
			dirty = 1;
		}

		if (dirty) {
			GFX_clear(screen);
			renderTitle("Parental", show_setting);

			int content_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);

			switch (state) {

			case SCREEN_HOME: {
				char buffer[64];
				char formatted[25];

				// from the cached total (see the "played" refresh above), so this
				// bar does not move just from having the screen open
				int extra_today = extraMinutesToday(&cfg);
				int budget_minutes = cfg.limit_minutes + extra_today;

				int remaining = remainingFrom(&cfg, played);
				if (remaining < 0) renderTextCentered("No limit", font.large, COLOR_WHITE, content_y + SCALE1(16));
				else {
					serializeTime(formatted, remaining);
					snprintf(buffer, sizeof(buffer), "%s left today", formatted);
					renderTextCentered(buffer, font.large, remaining > 0 ? COLOR_WHITE : COLOR_LIGHT_TEXT, content_y + SCALE1(16));

					float fraction = budget_minutes > 0 ? (float)remaining / (budget_minutes * 60) : 0;
					renderProgressBar(content_y + SCALE1(46), fraction);
				}

				serializeTime(formatted, played);
				snprintf(buffer, sizeof(buffer), "Played today  %s", formatted);
				renderTextCentered(buffer, font.small, COLOR_DARK_TEXT, content_y + SCALE1(66));

				if (cfg.limit_minutes > 0) {
					char limit[25], extra[32] = { 0 };
					serializeTime(limit, cfg.limit_minutes * 60);
					if (extra_today > 0) {
						char extra_time[25];
						serializeTime(extra_time, extra_today * 60);
						snprintf(extra, sizeof(extra), "  (+%s today)", extra_time);
					}
					snprintf(buffer, sizeof(buffer), "Daily limit  %s%s", limit, extra);
					renderTextCentered(buffer, font.small, COLOR_DARK_TEXT, content_y + SCALE1(82));
				}

				renderRow("Restricted Access", NULL, 3, true);

				if (show_setting) GFX_blitHardwareHints(screen, show_setting);
				else GFX_blitButtonGroup((char *[]){ "A", "ENTER", NULL }, 0, screen, 0);
				GFX_blitButtonGroup((char *[]){ "B", "EXIT", NULL }, 1, screen, 1);
				break;
			}

			case SCREEN_CODE:
				renderTextCentered("Enter code", font.medium, COLOR_WHITE, content_y + SCALE1(10));
				renderCodeSlots(code_entered, code_len, cfg.code_len, content_y + SCALE1(40));

				if (show_setting) GFX_blitHardwareHints(screen, show_setting);
				GFX_blitButtonGroup((char *[]){ "B", "BACK", NULL }, 1, screen, 1);
				break;

			case SCREEN_ADMIN: {
				char limit[25];
				if (cfg.limit_minutes > 0) serializeTime(limit, cfg.limit_minutes * 60);
				else snprintf(limit, sizeof(limit), "Off");

				char extra[25];
				int extra_today = extraMinutesToday(&cfg);
				if (extra_today > 0) serializeTime(extra, extra_today * 60);
				else snprintf(extra, sizeof(extra), "None");

				renderRow("Daily limit", limit, 0, admin_selected == ADMIN_LIMIT);
				renderRow("Extra time", extra, 1, admin_selected == ADMIN_EXTRA);
				renderRow("Emulators", cfg.exempt[0] ? "Some allowed" : "All blocked", 2, admin_selected == ADMIN_EXEMPT);
				renderRow("Change code", NULL, 3, admin_selected == ADMIN_CODE);

				if (show_setting) GFX_blitHardwareHints(screen, show_setting);
				else GFX_blitButtonGroup((char *[]){ "U/D", "SELECT", "A", "EDIT", NULL }, 0, screen, 0);
				GFX_blitButtonGroup((char *[]){ "B", "BACK", NULL }, 1, screen, 1);
				break;
			}

			case SCREEN_LIMIT: {
				renderTextCentered("Daily limit", font.medium, COLOR_WHITE, content_y + SCALE1(10));

				char hours[8], minutes[8];
				snprintf(hours, sizeof(hours), "%02dh", edit_hours);
				snprintf(minutes, sizeof(minutes), "%02dm", edit_minutes);

				int hw = textWidth(hours, font.large);
				int mw = textWidth(minutes, font.large);
				int gap = SCALE1(16);
				int x = (screen->w - (hw + gap + mw)) / 2;
				int y = content_y + SCALE1(44);

				renderText(hours, font.large, COLOR_WHITE, x, y);
				renderText(minutes, font.large, COLOR_WHITE, x + hw + gap, y);

				// underline the field the d-pad is acting on, like Clock.pak
				int uy = y + SCALE1(FONT_LARGE + 6);
				if (edit_field == FIELD_HOURS)
					GFX_blitPill(ASSET_UNDERLINE, screen, &(SDL_Rect){ x, uy, hw });
				else
					GFX_blitPill(ASSET_UNDERLINE, screen, &(SDL_Rect){ x + hw + gap, uy, mw });

				if (show_setting) GFX_blitHardwareHints(screen, show_setting);
				else GFX_blitButtonGroup((char *[]){ "L/R", "FIELD", "U/D", "ADJUST", NULL }, 0, screen, 0);
				GFX_blitButtonGroup((char *[]){ "A", "SAVE", "B", "BACK", NULL }, 1, screen, 1);
				break;
			}

			case SCREEN_EXTRA: {
				renderTextCentered("Extra time today", font.medium, COLOR_WHITE, content_y + SCALE1(10));

				char hours[8], minutes[8];
				snprintf(hours, sizeof(hours), "%02dh", edit_hours);
				snprintf(minutes, sizeof(minutes), "%02dm", edit_minutes);

				int hw = textWidth(hours, font.large);
				int mw = textWidth(minutes, font.large);
				int gap = SCALE1(16);
				int x = (screen->w - (hw + gap + mw)) / 2;
				int y = content_y + SCALE1(44);

				renderText(hours, font.large, COLOR_WHITE, x, y);
				renderText(minutes, font.large, COLOR_WHITE, x + hw + gap, y);

				int uy = y + SCALE1(FONT_LARGE + 6);
				if (edit_field == FIELD_HOURS)
					GFX_blitPill(ASSET_UNDERLINE, screen, &(SDL_Rect){ x, uy, hw });
				else
					GFX_blitPill(ASSET_UNDERLINE, screen, &(SDL_Rect){ x + hw + gap, uy, mw });

				renderTextCentered("Resets at midnight", font.small, COLOR_DARK_TEXT, content_y + SCALE1(80));

				if (show_setting) GFX_blitHardwareHints(screen, show_setting);
				else GFX_blitButtonGroup((char *[]){ "L/R", "FIELD", "U/D", "ADJUST", NULL }, 0, screen, 0);
				GFX_blitButtonGroup((char *[]){ "A", "SAVE", "B", "BACK", NULL }, 1, screen, 1);
				break;
			}

			case SCREEN_EXEMPT: {
				if (emu_count == 0) {
					renderTextCentered("No emulators found", font.medium, COLOR_DARK_TEXT, content_y + SCALE1(16));
				}
				else {
					int rows = listRows();
					for (int i = 0; i < rows && emu_top + i < emu_count; i++) {
						int idx = emu_top + i;
						// the pak suffix is noise once they are all listed together
						char label[MAX_PAK_NAME];
						snprintf(label, sizeof(label), "%.*s",
						         (int)(strlen(emus[idx].name) - 4), emus[idx].name);
						renderRow(label, isExempt(&cfg, emus[idx].name) ? "Allowed" : "Blocked",
						          i, idx == emu_selected);
					}
				}

				if (show_setting) GFX_blitHardwareHints(screen, show_setting);
				else GFX_blitButtonGroup((char *[]){ "U/D", "SELECT", "A", "TOGGLE", NULL }, 0, screen, 0);
				GFX_blitButtonGroup((char *[]){ "B", "SAVE", NULL }, 1, screen, 1);
				break;
			}

			case SCREEN_NEWCODE:
				renderTextCentered(confirming ? "Repeat new code" : "Enter new code", font.medium, COLOR_WHITE, content_y + SCALE1(10));
				if (confirming) renderCodeSlots(code_entered, code_len, new_len, content_y + SCALE1(40));
				else renderCodeSlots(new_code, new_len, MAX(new_len + 1, MIN_CODE_LEN), content_y + SCALE1(40));

				renderTextCentered("START when done", font.small, COLOR_DARK_TEXT, content_y + SCALE1(80));

				if (show_setting) GFX_blitHardwareHints(screen, show_setting);
				GFX_blitButtonGroup((char *[]){ "B", "BACK", NULL }, 1, screen, 1);
				break;
			}

			renderToast();

			GFX_flip(screen);
			dirty = 0;
		}
		else
			GFX_sync();
	}

	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();

	return EXIT_SUCCESS;
}
