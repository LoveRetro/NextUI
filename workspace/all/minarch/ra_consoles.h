#ifndef __RA_CONSOLES_H__
#define __RA_CONSOLES_H__

/**
 * RetroAchievements Console ID Mapping for NextUI
 * 
 * Maps NextUI EMU_TAGs to rcheevos RC_CONSOLE_* constants.
 * This is used to identify games when loading them for achievement tracking.
 */

#include "rc_consoles.h"
#include <string.h>

/**
 * Mapping entry from EMU tag to RetroAchievements console ID
 */
typedef struct {
	const char* emu_tag;
	int console_id;
} RA_ConsoleMapping;

/**
 * Lookup table mapping NextUI EMU tags to RC_CONSOLE_* constants.
 * Sorted alphabetically by emu_tag for readability/maintainability.
 */
static const RA_ConsoleMapping ra_console_table[] = {
	// Atari
	{ "A2600",   RC_CONSOLE_ATARI_2600 },
	{ "A5200",   RC_CONSOLE_ATARI_5200 },
	{ "A7800",   RC_CONSOLE_ATARI_7800 },
	// Sega 32X
	{ "32X",     RC_CONSOLE_SEGA_32X },
	// Commodore
	{ "C128",    RC_CONSOLE_COMMODORE_64 },  // Uses C64 for RA
	{ "C64",     RC_CONSOLE_COMMODORE_64 },
	// ColecoVision
	{ "COLECO", RC_CONSOLE_COLECOVISION },
	// Amstrad
	{ "CPC",     RC_CONSOLE_AMSTRAD_PC },
	// Nintendo
	{ "FC",      RC_CONSOLE_NINTENDO },
	// FinalBurn Neo
	{ "FBN",     RC_CONSOLE_ARCADE },
	// Famicom Disk System
	{ "FDS",     RC_CONSOLE_FAMICOM_DISK_SYSTEM },
	// Game Boy
	{ "GB",      RC_CONSOLE_GAMEBOY },
	// Game Boy Advance
	{ "GBA",     RC_CONSOLE_GAMEBOY_ADVANCE },
	// Game Boy Color
	{ "GBC",     RC_CONSOLE_GAMEBOY_COLOR },
	// Game Gear
	{ "GG",      RC_CONSOLE_GAME_GEAR },
	// Atari Lynx
	{ "LYNX",    RC_CONSOLE_ATARI_LYNX },
	// Mega Drive/Genesis
	{ "MD",      RC_CONSOLE_MEGA_DRIVE },
	// GBA (mGBA)
	{ "MGBA",    RC_CONSOLE_GAMEBOY_ADVANCE },
	// MSX
	{ "MSX",     RC_CONSOLE_MSX },
	// Neo Geo Pocket
	{ "NGP",     RC_CONSOLE_NEOGEO_POCKET },
	// Neo Geo Pocket Color
	{ "NGPC",    RC_CONSOLE_NEOGEO_POCKET },
	// PICO-8
	{ "P8",      RC_CONSOLE_PICO },
	// PC Engine
	{ "PCE",     RC_CONSOLE_PC_ENGINE },
	// Not supported (no RA)
	{ "PET",     RC_CONSOLE_UNKNOWN },
	// Pokemon Mini
	{ "PKM",     RC_CONSOLE_POKEMON_MINI },
	// Not supported (no RA)
	{ "PLUS4",   RC_CONSOLE_UNKNOWN },
	// PrBoom (no RA)
	{ "PRBOOM", RC_CONSOLE_UNKNOWN },
	// PlayStation
	{ "PS",      RC_CONSOLE_PLAYSTATION },
	// PlayStation (SwanStation)
	{ "PSX",     RC_CONSOLE_PLAYSTATION },
	// Amiga
	{ "PUAE",    RC_CONSOLE_AMIGA },
	// Sega CD
	{ "SEGACD", RC_CONSOLE_SEGA_CD },
	// Super Famicom/SNES
	{ "SFC",     RC_CONSOLE_SUPER_NINTENDO },
	// SG-1000
	{ "SG1000", RC_CONSOLE_SG1000 },
	// Super Game Boy
	{ "SGB",     RC_CONSOLE_GAMEBOY },
	// Master System
	{ "SMS",     RC_CONSOLE_MASTER_SYSTEM },
	// Super Famicom (Supafaust)
	{ "SUPA",    RC_CONSOLE_SUPER_NINTENDO },
	// Virtual Boy
	{ "VB",      RC_CONSOLE_VIRTUAL_BOY },
	// VIC-20
	{ "VIC",     RC_CONSOLE_VIC20 },
};

#define RA_CONSOLE_TABLE_SIZE (sizeof(ra_console_table) / sizeof(ra_console_table[0]))

/**
 * Get the RetroAchievements console ID for a given EMU tag.
 * @param emu_tag The NextUI emulator tag (e.g., "GB", "SFC", "PS")
 * @return The RC_CONSOLE_* constant, or RC_CONSOLE_UNKNOWN if not supported
 */
static inline int RA_getConsoleId(const char* emu_tag) {
	if (!emu_tag || !*emu_tag) {
		return RC_CONSOLE_UNKNOWN;
	}
	
	for (size_t i = 0; i < RA_CONSOLE_TABLE_SIZE; i++) {
		if (strcmp(emu_tag, ra_console_table[i].emu_tag) == 0) {
			return ra_console_table[i].console_id;
		}
	}
	
	return RC_CONSOLE_UNKNOWN;
}

/**
 * Check if achievements are supported for a given EMU tag.
 * @param emu_tag The NextUI emulator tag
 * @return 1 if supported, 0 if not
 */
static inline int RA_isConsoleSupported(const char* emu_tag) {
	return RA_getConsoleId(emu_tag) != RC_CONSOLE_UNKNOWN;
}

/**
 * Get a display name for the console.
 * Uses rcheevos rc_console_name() internally.
 * @param emu_tag The NextUI emulator tag
 * @return Human-readable console name, or "Unknown" if not supported
 */
static inline const char* RA_getConsoleName(const char* emu_tag) {
	int console_id = RA_getConsoleId(emu_tag);
	if (console_id == RC_CONSOLE_UNKNOWN) {
		return "Unknown";
	}
	return rc_console_name(console_id);
}

#endif // __RA_CONSOLES_H__
