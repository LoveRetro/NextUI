#pragma once

#include <stdlib.h>
#include <stdbool.h>

#include "defines.h"

#define CHEAT_TYPE_SET_TO_VALUE 1
#define CHEAT_MAX_COUNT         4096
#define CHEAT_MAX_PATHS         48
#define CHEAT_MAX_DISPLAY_PATHS 8
#define CHEAT_MAX_LIST_LENGTH   (CHEAT_MAX_DISPLAY_PATHS * MAX_PATH)

struct Cheat {
	const char *name;
	const char *info;
	int enabled;
	const char *code; // classic format, handed to retro_cheat_set()

	// RetroArch's newer format describes cheats as a memory write instead of a
	// code string (handler = 1). Those have no code and are applied by us.
	unsigned address;
	unsigned value;
	unsigned char type;         // cheat_type, 1 = set to value (the only one we apply)
	unsigned char size;         // memory_search_size, 0-5 = 1,2,4,8,16,32 bits
	unsigned char bit_position; // address_bit_position, sub-byte sizes only
	int big_endian;
};

struct Cheats {
	int enabled;
	size_t count;
	struct Cheat *cheats;
};

extern struct Cheats cheatcodes;

void Cheat_getPaths(char paths[CHEAT_MAX_PATHS][MAX_PATH], int* count);
void Cheats_free(void);
bool Cheats_load(void);
void Cheats_apply(void);
