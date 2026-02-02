#include "shortcuts.h"
#include "defines.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

///////////////////////////////////////
// Internal types

typedef struct Shortcut {
	char* path;  // without SDCARD_PATH prefix
	char* name;  // display name
} Shortcut;

typedef struct Array {
	int count;
	int capacity;
	void** items;
} Array;

// Entry struct (must match nextui.c)
struct Entry {
	char* path;
	char* name;
	char* unique;
	int type;
	int alpha;
};

///////////////////////////////////////
// Internal Array functions

static Array* Array_new(void) {
	Array* self = malloc(sizeof(Array));
	self->count = 0;
	self->capacity = 8;
	self->items = malloc(sizeof(void*) * self->capacity);
	return self;
}

static void Array_push(Array* self, void* item) {
	if (self->count >= self->capacity) {
		self->capacity *= 2;
		self->items = realloc(self->items, sizeof(void*) * self->capacity);
	}
	self->items[self->count++] = item;
}

static void* Array_pop(Array* self) {
	if (self->count == 0) return NULL;
	return self->items[--self->count];
}

static void Array_remove(Array* self, void* item) {
	if (self->count == 0 || item == NULL) return;
	int i = 0;
	while (self->items[i] != item) i++;
	for (int j = i; j < self->count - 1; j++) {
		self->items[j] = self->items[j + 1];
	}
	self->count--;
}

static void Array_free(Array* self) {
	free(self->items);
	free(self);
}

///////////////////////////////////////
// Shortcut functions

static Shortcut* Shortcut_new(char* path, char* name) {
	Shortcut* self = malloc(sizeof(Shortcut));
	self->path = strdup(path);
	self->name = name ? strdup(name) : NULL;
	return self;
}

static void Shortcut_free(Shortcut* self) {
	free(self->path);
	if (self->name) free(self->name);
	free(self);
}

static int ShortcutArray_indexOf(Array* self, char* path) {
	for (int i = 0; i < self->count; i++) {
		Shortcut* shortcut = self->items[i];
		if (exactMatch(shortcut->path, path)) return i;
	}
	return -1;
}

static void ShortcutArray_free(Array* self) {
	for (int i = 0; i < self->count; i++) {
		Shortcut_free(self->items[i]);
	}
	Array_free(self);
}

static int ShortcutCompare(const void* a, const void* b) {
	Shortcut* sa = *(Shortcut**)a;
	Shortcut* sb = *(Shortcut**)b;
	// Compare by name (case-insensitive)
	return strcasecmp(sa->name ? sa->name : sa->path, sb->name ? sb->name : sb->path);
}

static void ShortcutArray_sort(Array* self) {
	if (self && self->count > 1) {
		qsort(self->items, self->count, sizeof(void*), ShortcutCompare);
	}
}

///////////////////////////////////////
// Global state

static Array* shortcuts = NULL;

///////////////////////////////////////
// Save/Load functions

static void saveShortcuts(void) {
	FILE* file = fopen(SHORTCUTS_PATH, "w");
	if (file) {
		for (int i = 0; i < shortcuts->count; i++) {
			Shortcut* shortcut = shortcuts->items[i];
			fputs(shortcut->path, file);
			if (shortcut->name) {
				fputs("\t", file);
				fputs(shortcut->name, file);
			}
			putc('\n', file);
		}
		fclose(file);
	}
}

static int loadShortcuts(void) {
	if (shortcuts) {
		ShortcutArray_free(shortcuts);
	}
	shortcuts = Array_new();
	int removed_any = 0;

	FILE* file = fopen(SHORTCUTS_PATH, "r");
	if (file) {
		char line[256];
		while (fgets(line, 256, file) != NULL) {
			normalizeNewline(line);
			trimTrailingNewlines(line);
			if (strlen(line) == 0) continue;

			char* path = line;
			char* name = NULL;
			char* tmp = strchr(line, '\t');
			if (tmp) {
				tmp[0] = '\0';
				name = tmp + 1;
			}

			// Validate that the tool still exists
			char sd_path[256];
			sprintf(sd_path, "%s%s", SDCARD_PATH, path);

			if (exists(sd_path)) {
				Array_push(shortcuts, Shortcut_new(path, name));
			} else {
				removed_any = 1;
			}
		}
		fclose(file);
	}

	// Sort alphabetically
	ShortcutArray_sort(shortcuts);

	// Auto-clean: re-save if any were removed
	if (removed_any) saveShortcuts();

	return shortcuts->count > 0;
}

///////////////////////////////////////
// Public API

void Shortcuts_init(void) {
	shortcuts = Array_new();
	loadShortcuts();
}

void Shortcuts_quit(void) {
	if (shortcuts) {
		ShortcutArray_free(shortcuts);
		shortcuts = NULL;
	}
}

int Shortcuts_exists(char* path) {
	if (!shortcuts) return 0;
	return ShortcutArray_indexOf(shortcuts, path) != -1;
}

void Shortcuts_add(Entry* entry) {
	if (!shortcuts || !entry) return;

	char* path = entry->path + strlen(SDCARD_PATH);
	if (Shortcuts_exists(path)) return;

	while (shortcuts->count >= MAX_SHORTCUTS) {
		Shortcut_free(Array_pop(shortcuts));
	}
	Array_push(shortcuts, Shortcut_new(path, entry->name));
	ShortcutArray_sort(shortcuts);
	saveShortcuts();
}

void Shortcuts_remove(Entry* entry) {
	if (!shortcuts || !entry) return;

	char* path = entry->path + strlen(SDCARD_PATH);
	int idx = ShortcutArray_indexOf(shortcuts, path);
	if (idx != -1) {
		Shortcut* shortcut = shortcuts->items[idx];
		Array_remove(shortcuts, shortcut);
		Shortcut_free(shortcut);
		saveShortcuts();
	}
}

int Shortcuts_isInToolsFolder(char* path) {
	char tools_path[256];
	snprintf(tools_path, sizeof(tools_path), "%s/Tools/%s", SDCARD_PATH, PLATFORM);
	return prefixMatch(tools_path, path);
}

int Shortcuts_isInConsoleDir(char* path) {
	char parent_dir[256];
	strncpy(parent_dir, path, sizeof(parent_dir) - 1);
	parent_dir[sizeof(parent_dir) - 1] = '\0';
	char* last_slash = strrchr(parent_dir, '/');
	if (last_slash) *last_slash = '\0';
	return exactMatch(parent_dir, ROMS_PATH);
}

int Shortcuts_getCount(void) {
	return shortcuts ? shortcuts->count : 0;
}

char* Shortcuts_getPath(int index) {
	if (!shortcuts || index < 0 || index >= shortcuts->count) return NULL;
	Shortcut* shortcut = shortcuts->items[index];
	return shortcut->path;
}

char* Shortcuts_getName(int index) {
	if (!shortcuts || index < 0 || index >= shortcuts->count) return NULL;
	Shortcut* shortcut = shortcuts->items[index];
	return shortcut->name;
}

int Shortcuts_validate(void) {
	if (!shortcuts) return 0;

	int needs_save = 0;
	for (int i = shortcuts->count - 1; i >= 0; i--) {
		Shortcut* shortcut = shortcuts->items[i];
		char sd_path[256];
		sprintf(sd_path, "%s%s", SDCARD_PATH, shortcut->path);

		if (!exists(sd_path)) {
			Array_remove(shortcuts, shortcut);
			Shortcut_free(shortcut);
			needs_save = 1;
		}
	}

	if (needs_save) saveShortcuts();
	return needs_save;
}

char* Shortcuts_getPakBasename(const char* path) {
	static char basename[256];

	// Extract filename from path
	const char* pakname = strrchr(path, '/');
	pakname = pakname ? pakname + 1 : path;

	// Copy and remove .pak extension
	strncpy(basename, pakname, sizeof(basename) - 1);
	basename[sizeof(basename) - 1] = '\0';
	char* dot = strrchr(basename, '.');
	if (dot) *dot = '\0';

	return basename;
}

void Shortcuts_confirmAction(int action, Entry* entry) {
	if (action == 1) {
		Shortcuts_add(entry);
	} else {
		Shortcuts_remove(entry);
	}
}
