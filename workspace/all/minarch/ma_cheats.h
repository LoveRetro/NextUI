#pragma once

#include <stdlib.h>
#include <stdbool.h>

#include "defines.h"

#define CHEAT_MAX_PATHS         48
#define CHEAT_MAX_DISPLAY_PATHS 8
#define CHEAT_MAX_LIST_LENGTH   (CHEAT_MAX_DISPLAY_PATHS * MAX_PATH)

struct Cheat {
	const char *name;
	const char *info;
	int enabled;
	const char *code;
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
