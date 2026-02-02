#ifndef SHORTCUTS_H
#define SHORTCUTS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "defines.h"
#include "utils.h"

#define MAX_SHORTCUTS 12

// Forward declarations
typedef struct Entry Entry;
typedef struct Array Array;

// Initialize shortcuts (call in Menu_init)
void Shortcuts_init(void);

// Cleanup shortcuts (call in Menu_quit)
void Shortcuts_quit(void);

// Check if a shortcut exists for the given path (without SDCARD_PATH prefix)
int Shortcuts_exists(char* path);

// Add a shortcut for the given entry
void Shortcuts_add(Entry* entry);

// Remove a shortcut for the given entry
void Shortcuts_remove(Entry* entry);

// Check if inside Tools folder
int Shortcuts_isInToolsFolder(char* path);

// Check if inside a console directory (parent is ROMS_PATH)
int Shortcuts_isInConsoleDir(char* path);

// Get shortcuts count
int Shortcuts_getCount(void);

// Get shortcut path at index (without SDCARD_PATH prefix)
char* Shortcuts_getPath(int index);

// Get shortcut name at index
char* Shortcuts_getName(int index);

// Validate and clean up stale shortcuts (returns 1 if any were removed)
int Shortcuts_validate(void);

// Extract PAK basename from path (e.g., "/path/to/Retroarch.pak" -> "Retroarch")
// Returns pointer to static buffer, caller should copy if needed
char* Shortcuts_getPakBasename(const char* path);

// Handle confirmation action (1=add, 2=remove) and reboot
void Shortcuts_confirmAction(int action, Entry* entry);

#endif // SHORTCUTS_H
