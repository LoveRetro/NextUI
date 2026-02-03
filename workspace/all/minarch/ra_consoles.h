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
 * Get the RetroAchievements console ID for a given EMU tag.
 * @param emu_tag The NextUI emulator tag (e.g., "GB", "SFC", "PS")
 * @return The RC_CONSOLE_* constant, or RC_CONSOLE_UNKNOWN if not supported
 */
static inline int RA_getConsoleId(const char* emu_tag) {
	if (!emu_tag || !*emu_tag) {
		return RC_CONSOLE_UNKNOWN;
	}
	
	// Nintendo
	if (strcmp(emu_tag, "FC") == 0)      return RC_CONSOLE_NINTENDO;           // Famicom/NES
	if (strcmp(emu_tag, "FDS") == 0)     return RC_CONSOLE_FAMICOM_DISK_SYSTEM; // Famicom Disk System
	if (strcmp(emu_tag, "SFC") == 0)     return RC_CONSOLE_SUPER_NINTENDO;     // Super Famicom/SNES
	if (strcmp(emu_tag, "SUPA") == 0)    return RC_CONSOLE_SUPER_NINTENDO;     // Super Famicom (Supafaust)
	if (strcmp(emu_tag, "GB") == 0)      return RC_CONSOLE_GAMEBOY;            // Game Boy
	if (strcmp(emu_tag, "GBC") == 0)     return RC_CONSOLE_GAMEBOY_COLOR;      // Game Boy Color
	if (strcmp(emu_tag, "SGB") == 0)     return RC_CONSOLE_GAMEBOY;            // Super Game Boy
	if (strcmp(emu_tag, "GBA") == 0)     return RC_CONSOLE_GAMEBOY_ADVANCE;    // Game Boy Advance
	if (strcmp(emu_tag, "MGBA") == 0)    return RC_CONSOLE_GAMEBOY_ADVANCE;    // GBA (mGBA)
	if (strcmp(emu_tag, "VB") == 0)      return RC_CONSOLE_VIRTUAL_BOY;        // Virtual Boy
	if (strcmp(emu_tag, "PKM") == 0)     return RC_CONSOLE_POKEMON_MINI;       // Pokemon Mini
	
	// Sega
	if (strcmp(emu_tag, "MD") == 0)      return RC_CONSOLE_MEGA_DRIVE;         // Mega Drive/Genesis
	if (strcmp(emu_tag, "32X") == 0)     return RC_CONSOLE_SEGA_32X;           // Sega 32X
	if (strcmp(emu_tag, "SEGACD") == 0)  return RC_CONSOLE_SEGA_CD;            // Sega CD
	if (strcmp(emu_tag, "SMS") == 0)     return RC_CONSOLE_MASTER_SYSTEM;      // Master System
	if (strcmp(emu_tag, "GG") == 0)      return RC_CONSOLE_GAME_GEAR;          // Game Gear
	if (strcmp(emu_tag, "SG1000") == 0)  return RC_CONSOLE_SG1000;             // SG-1000
	
	// Sony
	if (strcmp(emu_tag, "PS") == 0)      return RC_CONSOLE_PLAYSTATION;        // PlayStation
	if (strcmp(emu_tag, "PSX") == 0)     return RC_CONSOLE_PLAYSTATION;        // PlayStation (SwanStation)
	
	// NEC
	if (strcmp(emu_tag, "PCE") == 0)     return RC_CONSOLE_PC_ENGINE;          // TurboGrafx-16/PC Engine
	
	// Atari
	if (strcmp(emu_tag, "A2600") == 0)   return RC_CONSOLE_ATARI_2600;         // Atari 2600
	if (strcmp(emu_tag, "A5200") == 0)   return RC_CONSOLE_ATARI_5200;         // Atari 5200
	if (strcmp(emu_tag, "A7800") == 0)   return RC_CONSOLE_ATARI_7800;         // Atari 7800
	if (strcmp(emu_tag, "LYNX") == 0)    return RC_CONSOLE_ATARI_LYNX;         // Atari Lynx
	
	// SNK
	if (strcmp(emu_tag, "NGP") == 0)     return RC_CONSOLE_NEOGEO_POCKET;      // Neo Geo Pocket
	if (strcmp(emu_tag, "NGPC") == 0)    return RC_CONSOLE_NEOGEO_POCKET;      // Neo Geo Pocket Color
	
	// Arcade
	if (strcmp(emu_tag, "FBN") == 0)     return RC_CONSOLE_ARCADE;             // FinalBurn Neo (varies)
	
	// Home Computers
	if (strcmp(emu_tag, "C64") == 0)     return RC_CONSOLE_COMMODORE_64;       // Commodore 64
	if (strcmp(emu_tag, "C128") == 0)    return RC_CONSOLE_COMMODORE_64;       // Commodore 128 (uses C64 for RA)
	if (strcmp(emu_tag, "VIC") == 0)     return RC_CONSOLE_VIC20;              // Commodore VIC-20
	if (strcmp(emu_tag, "PET") == 0)     return RC_CONSOLE_UNKNOWN;            // Commodore PET (no RA support)
	if (strcmp(emu_tag, "PLUS4") == 0)   return RC_CONSOLE_UNKNOWN;            // Commodore Plus/4 (no RA support)
	if (strcmp(emu_tag, "CPC") == 0)     return RC_CONSOLE_AMSTRAD_PC;         // Amstrad CPC
	if (strcmp(emu_tag, "MSX") == 0)     return RC_CONSOLE_MSX;                // MSX
	if (strcmp(emu_tag, "PUAE") == 0)    return RC_CONSOLE_AMIGA;              // Amiga (PUAE)
	
	// Other
	if (strcmp(emu_tag, "COLECO") == 0)  return RC_CONSOLE_COLECOVISION;       // ColecoVision
	if (strcmp(emu_tag, "P8") == 0)      return RC_CONSOLE_PICO;               // PICO-8 (fantasy console)
	if (strcmp(emu_tag, "PRBOOM") == 0)  return RC_CONSOLE_UNKNOWN;            // PrBoom (DOOM - no RA)
	
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
