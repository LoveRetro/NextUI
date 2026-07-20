#ifndef __PALETTE_H__
#define __PALETTE_H__

#include <stdint.h>
#include <stdbool.h>

// Predefined color palettes: file-based color presets users can pick from in
// Settings, in addition to the fully custom color1-7 controls. "Palette" is
// deliberately scoped to colors only - a future "Theme" feature may bundle
// palettes with other assets (fonts, backgrounds, etc.). See config.h for the
// persisted "currently selected palette" state (CFG_getPaletteName/
// CFG_applyPalette/CFG_clearPalette).

// Highest palette-file format version this build understands. Palette files
// with a higher version= are skipped during enumeration.
#define PALETTE_VERSION_MAX 1
#define PALETTE_NAME_MAX 64   // keep in sync with NextUISettings.paletteName in config.h
#define PALETTE_PATH_MAX 512
#define PALETTE_COLOR_COUNT 7

typedef struct
{
	int version;                    // palette-file format version
	char name[PALETTE_NAME_MAX];    // display label
	char path[PALETTE_PATH_MAX];    // absolute path to the palette file
	bool builtin;                   // true if shipped in RES_PATH/palettes (read-only)
	uint32_t colors[PALETTE_COLOR_COUNT]; // packed RGBA, index 0 == color1
} ColorPalette;

// Enumerate predefined color palettes found on disk.
// Scans RES_PATH/palettes (built-ins, listed first) and SDCARD_PATH/Palettes (user).
// Files whose version= exceeds PALETTE_VERSION_MAX are skipped. Missing colors fall
// back to the CFG defaults. Writes up to max entries into out; returns the count.
int PALETTE_enumerate(ColorPalette *out, int max);

// Apply a palette's colors to the current settings and record it as the selected
// palette. Fires the color-set callback per color; does not modify the palette file.
void PALETTE_apply(const ColorPalette *palette);

#endif
