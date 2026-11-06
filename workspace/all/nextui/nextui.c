#include <stdio.h>
#include <stdlib.h>
#include <msettings.h>
#include <sys/types.h>
#include <dirent.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>  // For dirname()
#include "defines.h"
#include "api.h"
#include "utils.h"
#include "config.h"
#include <sys/resource.h>
#include <pthread.h>
#include <assert.h>
#include <limits.h>

///////////////////////////////////////

typedef struct Array {
	int count;
	int capacity;
	void** items;
} Array;

static Array* Array_new(void) {
	Array* self = malloc(sizeof(Array));
	self->count = 0;
	self->capacity = 8;
	self->items = malloc(sizeof(void*) * self->capacity);
	return self;
}
static void Array_push(Array* self, void* item) {
	if (self->count>=self->capacity) {
		self->capacity *= 2;
		self->items = realloc(self->items, sizeof(void*) * self->capacity);
	}
	self->items[self->count++] = item;
}
static void Array_unshift(Array* self, void* item) {
	if (self->count==0) return Array_push(self, item);
	Array_push(self, NULL); // ensures we have enough capacity
	for (int i=self->count-2; i>=0; i--) {
		self->items[i+1] = self->items[i];
	}
	self->items[0] = item;
}
static void* Array_pop(Array* self) {
	if (self->count==0) return NULL;
	return self->items[--self->count];
}
static void Array_remove(Array* self, void* item) {
	if (self->count==0 || item == NULL)
		return;
	int i = 0;
	while (self->items[i] != item) i++;
	for (int j = i; j < self->count-1; j++)
		self->items[j] = self->items[j+1];
	self->count--;
}
static void Array_reverse(Array* self) {
	int end = self->count-1;
	int mid = self->count/2;
	for (int i=0; i<mid; i++) {
		void* item = self->items[i];
		self->items[i] = self->items[end-i];
		self->items[end-i] = item;
	}
}
static void Array_free(Array* self) {
	free(self->items);
	free(self);
}
static void Array_yoink(Array* self, Array* other) {
	// append entries to self and take ownership
	for (int i = 0; i < other->count; i++)
        Array_push(self, other->items[i]);
    Array_free(other); // `self` now owns the entries
}

static int StringArray_indexOf(Array* self, char* str) {
	for (int i=0; i<self->count; i++) {
		if (exactMatch(self->items[i], str)) return i;
	}
	return -1;
}
static void StringArray_free(Array* self) {
	for (int i=0; i<self->count; i++) {
		free(self->items[i]);
	}
	Array_free(self);
}

///////////////////////////////////////

typedef struct Hash {
	Array* keys;
	Array* values;
} Hash; // not really a hash

static Hash* Hash_new(void) {
	Hash* self = malloc(sizeof(Hash));
	self->keys = Array_new();
	self->values = Array_new();
	return self;
}
static void Hash_free(Hash* self) {
	StringArray_free(self->keys);
	StringArray_free(self->values);
	free(self);
}
static void Hash_set(Hash* self, char* key, char* value) {
	Array_push(self->keys, strdup(key));
	Array_push(self->values, strdup(value));
}
static char* Hash_get(Hash* self, char* key) {
	int i = StringArray_indexOf(self->keys, key);
	if (i==-1) return NULL;
	return self->values->items[i];
}

///////////////////////////////////////

enum EntryType {
	ENTRY_DIR,
	ENTRY_PAK,
	ENTRY_ROM,
	ENTRY_DIP,
};
typedef struct Entry {
	char* path;
	char* name;
	char* unique;
	int type;
	int alpha; // index in parent Directory's alphas Array, which points to the index of an Entry in its entries Array :sweat_smile:
} Entry;

static Entry* Entry_new(char* path, int type) {
	char display_name[256];
	getDisplayName(path, display_name);
	Entry* self = malloc(sizeof(Entry));
	self->path = strdup(path);
	self->name = strdup(display_name);
	self->unique = NULL;
	self->type = type;
	self->alpha = 0;
	return self;
}

static Entry* Entry_newNamed(char* path, int type, char* displayName) {
	Entry *self = Entry_new(path, type);
	self->name = strdup(displayName);
	return self;
}

static void Entry_free(Entry* self) {
	free(self->path);
	free(self->name);
	if (self->unique) free(self->unique);
	free(self);
}

static int EntryArray_indexOf(Array* self, char* path) {
	for (int i=0; i<self->count; i++) {
		Entry* entry = self->items[i];
		if (exactMatch(entry->path, path)) return i;
	}
	return -1;
}
static int EntryArray_sortEntry(const void* a, const void* b) {
	Entry* item1 = *(Entry**)a;
	Entry* item2 = *(Entry**)b;
	return strcasecmp(item1->name, item2->name);
}
static void EntryArray_sort(Array* self) {
	qsort(self->items, self->count, sizeof(void*), EntryArray_sortEntry);
}

static void EntryArray_free(Array* self) {
	for (int i=0; i<self->count; i++) {
		Entry_free(self->items[i]);
	}
	Array_free(self);
}

///////////////////////////////////////

#define INT_ARRAY_MAX 27
typedef struct IntArray {
	int count;
	int items[INT_ARRAY_MAX];
} IntArray;
static IntArray* IntArray_new(void) {
	IntArray* self = malloc(sizeof(IntArray));
	self->count = 0;
	memset(self->items, 0, sizeof(int) * INT_ARRAY_MAX);
	return self;
}
static void IntArray_push(IntArray* self, int i) {
	self->items[self->count++] = i;
}
static void IntArray_free(IntArray* self) {
	free(self);
}

///////////////////////////////////////

typedef struct Directory {
	char* path;
	char* name;
	Array* entries;
	IntArray* alphas;
	// rendering
	int selected;
	int start;
	int end;
	int grid_col; // first visible column in grid layout
} Directory;

// "All games" root: when enabled, the root directory lists every game of every
// system (A-Z) instead of the system list. all_games_root is set by getRoot()
// each time the root is built.
static int all_games_root = 0;

static int getIndexChar(char* str) {
	char i = 0;
	char c = tolower(str[0]);
	if (c>='a' && c<='z') i = (c-'a')+1;
	return i;
}

static void getUniqueName(Entry* entry, char* out_name) {
	char* filename = strrchr(entry->path, '/')+1;
	char emu_tag[256];
	getEmuName(entry->path, emu_tag);

	char *tmp;
	strcpy(out_name, entry->name);
	tmp = out_name + strlen(out_name);
	strcpy(tmp, " (");
	tmp = out_name + strlen(out_name);
	strcpy(tmp, emu_tag);
	tmp = out_name + strlen(out_name);
	strcpy(tmp, ")");
}

static void Directory_index(Directory* self) {
    int is_collection = prefixMatch(COLLECTIONS_PATH, self->path);
    int skip_index = exactMatch(FAUX_RECENT_PATH, self->path) || is_collection; // not alphabetized

    if (all_games_root && exactMatch(self->path, SDCARD_PATH)) {
        // map.txt aliases were already applied per system by getAllGames(), and
        // duplicate names across systems are intentionally left untagged.
        int alpha = -1;
        int index = 0;
        for (int i = 0; i < self->entries->count; i++) {
            Entry* entry = self->entries->items[i];
            int a = getIndexChar(entry->name);
            // a merged list can contain more than INT_ARRAY_MAX index changes
            // (eg. symbols sorting after letters), keep the remainder in the last group
            if (a != alpha && self->alphas->count < INT_ARRAY_MAX) {
                index = self->alphas->count;
                IntArray_push(self->alphas, i);
                alpha = a;
            }
            entry->alpha = index;
        }
        return;
    }

    Hash* map = NULL;
    char map_path[256];
    sprintf(map_path, "%s/map.txt", is_collection ? COLLECTIONS_PATH : self->path);

    if (exists(map_path)) {
        FILE* file = fopen(map_path, "r");
        if (file) {
            map = Hash_new();
            char line[256];
            while (fgets(line, 256, file) != NULL) {
                normalizeNewline(line);
                trimTrailingNewlines(line);
                if (strlen(line) == 0) continue; // skip empty lines

                char* tmp = strchr(line, '\t');
                if (tmp) {
                    tmp[0] = '\0';
                    char* key = line;
                    char* value = tmp + 1;
                    Hash_set(map, key, strdup(value)); // Ensure strdup to store value properly
                }
            }
            fclose(file);

            int resort = 0;
            int filter = 0;
            for (int i = 0; i < self->entries->count; i++) {
                Entry* entry = self->entries->items[i];
                char* filename = strrchr(entry->path, '/') + 1;
                char* alias = Hash_get(map, filename);
                if (alias) {
                    free(entry->name);  // Free before overwriting
                    entry->name = strdup(alias);
                    resort = 1;
                    if (!filter && hide(entry->name)) filter = 1;
                }
            }

            if (filter) {
                Array* entries = Array_new();
                for (int i = 0; i < self->entries->count; i++) {
                    Entry* entry = self->entries->items[i];
                    if (hide(entry->name)) {
                        Entry_free(entry); // Ensure Entry_free handles all memory cleanup
                    } else {
                        Array_push(entries, entry);
                    }
                }
                Array_free(self->entries);
                self->entries = entries;
            }
            if (resort) EntryArray_sort(self->entries);
        }
    }

    Entry* prior = NULL;
    int alpha = -1;
    int index = 0;
    for (int i = 0; i < self->entries->count; i++) {
        Entry* entry = self->entries->items[i];
        if (map) {
            char* filename = strrchr(entry->path, '/') + 1;
            char* alias = Hash_get(map, filename);
            if (alias) {
                free(entry->name);  // Free before overwriting
                entry->name = strdup(alias);
            }
        }

        if (prior != NULL && exactMatch(prior->name, entry->name)) {
            free(prior->unique);
            free(entry->unique);
            prior->unique = NULL;
            entry->unique = NULL;

            char* prior_filename = strrchr(prior->path, '/') + 1;
            char* entry_filename = strrchr(entry->path, '/') + 1;
            if (exactMatch(prior_filename, entry_filename)) {
                char prior_unique[256];
                char entry_unique[256];
                getUniqueName(prior, prior_unique);
                getUniqueName(entry, entry_unique);

                prior->unique = strdup(prior_unique);
                entry->unique = strdup(entry_unique);
            } else {
                prior->unique = strdup(prior_filename);
                entry->unique = strdup(entry_filename);
            }
        }

        if (!skip_index) {
            int a = getIndexChar(entry->name);
            if (a != alpha) {
                index = self->alphas->count;
                IntArray_push(self->alphas, i);
                alpha = a;
            }
            entry->alpha = index;
        }

        prior = entry;
    }

    if (map) Hash_free(map);  // Free the map at the end
}

static Array* getRoot(void);
static Array* getRoms(void);
static Array* getRecents(void);
static Array* getCollection(char* path);
static Array* getDiscs(char* path);
static Array* getEntries(char* path);

static Directory* Directory_new(char* path, int selected) {
	char display_name[256];
	getDisplayName(path, display_name);

	Directory* self = malloc(sizeof(Directory));
	self->path = strdup(path);
	self->name = strdup(display_name);
	if (exactMatch(path, SDCARD_PATH)) {
		self->entries = getRoot();
	}
	else if (exactMatch(path, FAUX_RECENT_PATH)) {
		self->entries = getRecents();
	}
	else if (exactMatch(path, ROMS_PATH)) {
		self->entries = getRoms();
	}
	else if (!exactMatch(path, COLLECTIONS_PATH) && prefixMatch(COLLECTIONS_PATH, path) && suffixMatch(".txt", path)) {
		self->entries = getCollection(path);
	}
	else if (suffixMatch(".m3u", path)) {
		self->entries = getDiscs(path);
	}
	else {
		self->entries = getEntries(path);
	}
	self->alphas = IntArray_new();
	self->selected = selected;
	self->start = 0;
	self->end = 0;
	self->grid_col = 0;
	Directory_index(self);
	return self;
}
static void Directory_free(Directory* self) {
	free(self->path);
	free(self->name);
	EntryArray_free(self->entries);
	IntArray_free(self->alphas);
	free(self);
}

static void DirectoryArray_pop(Array* self) {
	Directory_free(Array_pop(self));
}
static void DirectoryArray_free(Array* self) {
	for (int i=0; i<self->count; i++) {
		Directory_free(self->items[i]);
	}
	Array_free(self);
}

///////////////////////////////////////

typedef struct Recent {
	char* path; // NOTE: this is without the SDCARD_PATH prefix!
	char* alias;
	int available;
} Recent;
 // yiiikes
static char* recent_alias = NULL;

static int hasEmu(char* emu_name);
static Recent* Recent_new(char* path, char* alias) {
	Recent* self = malloc(sizeof(Recent));

	char sd_path[256]; // only need to get emu name
	sprintf(sd_path, "%s%s", SDCARD_PATH, path);

	char emu_name[256];
	getEmuName(sd_path, emu_name);

	self->path = strdup(path);
	self->alias = alias ? strdup(alias) : NULL;
	self->available = hasEmu(emu_name);
	return self;
}
static void Recent_free(Recent* self) {
	free(self->path);
	if (self->alias) free(self->alias);
	free(self);
}

static int RecentArray_indexOf(Array* self, char* str) {
	for (int i=0; i<self->count; i++) {
		Recent* item = self->items[i];
		if (exactMatch(item->path, str)) return i;
	}
	return -1;
}
static void RecentArray_free(Array* self) {
	for (int i=0; i<self->count; i++) {
		Recent_free(self->items[i]);
	}
	Array_free(self);
}

///////////////////////////////////////

static Directory* top;
static Array* stack; // DirectoryArray
static Array* recents; // RecentArray
static Array *quick; // EntryArray
static Array *quickActions; // EntryArray

static int quit = 0;
static int can_resume = 0;
static int should_resume = 0; // set to 1 on BTN_RESUME but only if can_resume==1
static int has_preview = 0;
static int simple_mode = 0;
static int switcher_selected = 0;
static char slot_path[256];
static char preview_path[256];
static int animationdirection = 0;

static int restore_depth = -1;
static int restore_relative = -1;
static int restore_selected = 0;
static int restore_start = 0;
static int restore_end = 0;
static int startgame = 0;
///////////////////////////////////////

#define MAX_RECENTS 24 // a multiple of all menu rows
static void saveRecents(void) {
	FILE* file = fopen(RECENT_PATH, "w");
	if (file) {
		for (int i=0; i<recents->count; i++) {
			Recent* recent = recents->items[i];
			fputs(recent->path, file);
			if (recent->alias) {
				fputs("\t", file);
				fputs(recent->alias, file);
			}
			putc('\n', file);
		}
		fclose(file);
	}
}
static void addRecent(char* path, char* alias) {
	path += strlen(SDCARD_PATH); // makes paths platform agnostic
	int id = RecentArray_indexOf(recents, path);
	if (id==-1) { // add
		while (recents->count>=MAX_RECENTS) {
			Recent_free(Array_pop(recents));
		}
		Array_unshift(recents, Recent_new(path, alias));
	}
	else if (id>0) { // bump to top
		for (int i=id; i>0; i--) {
			void* tmp = recents->items[i-1];
			recents->items[i-1] = recents->items[i];
			recents->items[i] = tmp;
		}
	}
	saveRecents();
}

static Entry* entryFromPakName(char* pak_name)
{
	char pak_path[256];
	// Check in Tools
	sprintf(pak_path, "%s/Tools/%s/%s.pak", SDCARD_PATH, PLATFORM, pak_name);
	if(exists(pak_path))
		return Entry_newNamed(pak_path, ENTRY_PAK, pak_name);

	// Check in Emus
	sprintf(pak_path, "%s/Emus/%s.pak", PAKS_PATH, pak_name);
	if(exists(pak_path))
		return Entry_newNamed(pak_path, ENTRY_PAK, pak_name);

	// Check in platform Emus
	sprintf(pak_path, "%s/Emus/%s/%s.pak", SDCARD_PATH, PLATFORM, pak_name);
	if(exists(pak_path))
		return Entry_newNamed(pak_path, ENTRY_PAK, pak_name);

	return NULL;
}

static int hasEmu(char* emu_name) {
	char pak_path[256];
	sprintf(pak_path, "%s/Emus/%s.pak/launch.sh", PAKS_PATH, emu_name);
	if (exists(pak_path)) return 1;

	sprintf(pak_path, "%s/Emus/%s/%s.pak/launch.sh", SDCARD_PATH, PLATFORM, emu_name);
	return exists(pak_path);
}
static int hasCue(char* dir_path, char* cue_path) { // NOTE: dir_path not rom_path
	char* tmp = strrchr(dir_path, '/') + 1; // folder name
	sprintf(cue_path, "%s/%s.cue", dir_path, tmp);
	return exists(cue_path);
}
static int hasM3u(char* rom_path, char* m3u_path) { // NOTE: rom_path not dir_path
	char* tmp;

	strcpy(m3u_path, rom_path);
	tmp = strrchr(m3u_path, '/') + 1;
	tmp[0] = '\0';

	// path to parent directory
	char base_path[256];
	strcpy(base_path, m3u_path);

	tmp = strrchr(m3u_path, '/');
	tmp[0] = '\0';

	// get parent directory name
	char dir_name[256];
	tmp = strrchr(m3u_path, '/');
	strcpy(dir_name, tmp);

	// dir_name is also our m3u file name
	tmp = m3u_path + strlen(m3u_path);
	strcpy(tmp, dir_name);

	// add extension
	tmp = m3u_path + strlen(m3u_path);
	strcpy(tmp, ".m3u");

	return exists(m3u_path);
}

static int hasRecents(void) {
	LOG_info("hasRecents %s\n", RECENT_PATH);
	int has = 0;
	RecentArray_free(recents);
	recents = Array_new();

	Array* parent_paths = Array_new();
	if (exists(CHANGE_DISC_PATH)) {
		char sd_path[256];
		getFile(CHANGE_DISC_PATH, sd_path, 256);
		if (exists(sd_path)) {
			char* disc_path = sd_path + strlen(SDCARD_PATH); // makes path platform agnostic
			Recent* recent = Recent_new(disc_path, NULL);
			if (recent->available) has += 1;
			Array_push(recents, recent);

			char parent_path[256];
			strcpy(parent_path, disc_path);
			char* tmp = strrchr(parent_path, '/') + 1;
			tmp[0] = '\0';
			Array_push(parent_paths, strdup(parent_path));
		}
		unlink(CHANGE_DISC_PATH);
	}

	FILE *file = fopen(RECENT_PATH, "r"); // newest at top
	if (file) {
		char line[256];
		while (fgets(line,256,file)!=NULL) {
			normalizeNewline(line);
			trimTrailingNewlines(line);
			if (strlen(line)==0) continue; // skip empty lines

			// LOG_info("line: %s\n", line);

			char* path = line;
			char* alias = NULL;
			char* tmp = strchr(line,'\t');
			if (tmp) {
				tmp[0] = '\0';
				alias = tmp+1;
			}

			char sd_path[256];
			sprintf(sd_path, "%s%s", SDCARD_PATH, path);
			if (exists(sd_path)) {
				if (recents->count<MAX_RECENTS) {
					// this logic replaces an existing disc from a multi-disc game with the last used
					char m3u_path[256];
					if (hasM3u(sd_path, m3u_path)) { // TODO: this might tank launch speed
						char parent_path[256];
						strcpy(parent_path, path);
						char* tmp = strrchr(parent_path, '/') + 1;
						tmp[0] = '\0';

						int found = 0;
						for (int i=0; i<parent_paths->count; i++) {
							char* path = parent_paths->items[i];
							if (prefixMatch(path, parent_path)) {
								found = 1;
								break;
							}
						}
						if (found) continue;

						Array_push(parent_paths, strdup(parent_path));
					}

					// LOG_info("path:%s alias:%s\n", path, alias);

					Recent* recent = Recent_new(path, alias);
					if (recent->available) has += 1;
					Array_push(recents, recent);
				}
			}
		}
		fclose(file);
	}

	saveRecents();

	StringArray_free(parent_paths);
	return has>0;
}
static int hasCollections(void) {
	int has = 0;
	if (!exists(COLLECTIONS_PATH)) return has;

	DIR *dh = opendir(COLLECTIONS_PATH);
	struct dirent *dp;
	while((dp = readdir(dh)) != NULL) {
		if (hide(dp->d_name)) continue;
		has = 1;
		break;
	}
	closedir(dh);
	return has;
}
static int hasRoms(char* dir_name) {
	int has = 0;
	char emu_name[256];
	char rom_path[256];

	getEmuName(dir_name, emu_name);

	// check for emu pak
	if (!hasEmu(emu_name)) return has;

	// check for at least one non-hidden file (we're going to assume it's a rom)
	sprintf(rom_path, "%s/%s/", ROMS_PATH, dir_name);
	DIR *dh = opendir(rom_path);
	if (dh!=NULL) {
		struct dirent *dp;
		while((dp = readdir(dh)) != NULL) {
			if (hide(dp->d_name)) continue;
			has = 1;
			break;
		}
		closedir(dh);
	}
	// if (!has) printf("No roms for %s!\n", dir_name);
	return has;
}

static void getToolsPath(char* out, size_t out_size) {
	snprintf(out, out_size, "%s/Tools/%s", SDCARD_PATH, PLATFORM);
}

static int isToolsEntry(Entry* entry) {
	char tools_path[256];
	getToolsPath(tools_path, sizeof(tools_path));
	return exactMatch(entry->path, tools_path);
}

static int hasTools(void) {
	char tools_path[256];
	getToolsPath(tools_path, sizeof(tools_path));
	return exists(tools_path);
}

// The Tools folder entry for a root page, when it should be listed there.
static void pushToolsEntry(Array* root) {
	if (!hasTools() || !CFG_getShowTools() || simple_mode) return;
	char tools_path[256];
	getToolsPath(tools_path, sizeof(tools_path));
	Array_push(root, Entry_new(tools_path, ENTRY_DIR));
}

static Array* getRoms()
{
	Array* entries = Array_new();
    DIR* dh = opendir(ROMS_PATH);
    if (dh) {
        struct dirent* dp;
        char full_path[256];
        snprintf(full_path, sizeof(full_path), "%s/", ROMS_PATH);
        char* tmp = full_path + strlen(full_path);

        Array* emus = Array_new();
        while ((dp = readdir(dh)) != NULL) {
            if (hide(dp->d_name)) continue;
            if (hasRoms(dp->d_name)) {
                strcpy(tmp, dp->d_name);
                Array_push(emus, Entry_new(full_path, ENTRY_DIR));
            }
        }
        closedir(dh); // Ensure directory is closed immediately after use

        EntryArray_sort(emus);
        Entry* prev_entry = NULL;
        for (int i = 0; i < emus->count; i++) {
            Entry* entry = emus->items[i];
            if (prev_entry && exactMatch(prev_entry->name, entry->name)) {
                Entry_free(entry);
                continue;
            }
            Array_push(entries, entry);
            prev_entry = entry;
        }
        Array_free(emus); // Only frees container, entries now owns the items
    }

	// Handle mapping logic
    char map_path[256];
    snprintf(map_path, sizeof(map_path), "%s/map.txt", ROMS_PATH);
    if (entries->count > 0 && exists(map_path)) {
        FILE* file = fopen(map_path, "r");
        if (file) {
            Hash* map = Hash_new();
            char line[256];

            while (fgets(line, sizeof(line), file)) {
                normalizeNewline(line);
                trimTrailingNewlines(line);
                if (strlen(line) == 0) continue; // Skip empty lines

                char* tmp = strchr(line, '\t');
                if (tmp) {
                    *tmp = '\0';
                    char* key = line;
                    char* value = tmp + 1;
                    Hash_set(map, key, strdup(value)); // Ensure strdup
                }
            }
            fclose(file);

            int resort = 0;
            for (int i = 0; i < entries->count; i++) {
                Entry* entry = entries->items[i];
                char* filename = strrchr(entry->path, '/') + 1;
                char* alias = Hash_get(map, filename);
                if (alias) {
                    free(entry->name);  // Free before overwriting
                    entry->name = strdup(alias);
                    resort = 1;
                }
            }
            if (resort) EntryArray_sort(entries);
            Hash_free(map);
        }
    }

	return entries;
}

static Array* getCollections(void)
{
	DIR* dh = opendir(COLLECTIONS_PATH);
	if (dh) {
		struct dirent* dp;
		char full_path[256];
		snprintf(full_path, sizeof(full_path), "%s/", COLLECTIONS_PATH);
		char* tmp = full_path + strlen(full_path);

		Array* collections = Array_new();
		while ((dp = readdir(dh)) != NULL) {
			if (hide(dp->d_name)) continue;
			strcpy(tmp, dp->d_name);
			Array_push(collections, Entry_new(full_path, ENTRY_DIR)); // Collections are fake directories
		}
		closedir(dh); // Close immediately after use
		EntryArray_sort(collections);
		return collections;
	}
	return NULL;
}

static Array* getQuickEntries(void) {
	Array* entries = Array_new();

	// We assume Menu_init was already called and populated this
	if (recents && recents->count)
		Array_push(entries, Entry_newNamed(FAUX_RECENT_PATH, ENTRY_DIR, "Recents"));

	if (hasCollections())
		Array_push(entries, Entry_new(COLLECTIONS_PATH, ENTRY_DIR));

	// Not sure we need this, its just a button press away (B)
	Array_push(entries, Entry_newNamed(ROMS_PATH, ENTRY_DIR, "Games"));

	// Add tools if applicable
    if (hasTools() && !simple_mode) {
		char tools_path[256];
		getToolsPath(tools_path, sizeof(tools_path));
        Array_push(entries, Entry_new(tools_path, ENTRY_DIR));
    }

	return entries;
}

// Display name for a Quick Menu item, shown in the title pill. Toggles are
// identified by their internal names (see toggleQuick), so only relabel here.
static const char* QuickMenu_label(Entry* item) {
	if (!item) return "";
	if (!strcmp(item->name, "Wifi")) return "Wi-Fi";
	if (!strcmp(item->name, "Poweroff")) return "Power off";
	return item->name;
}

static Array* getQuickToggles(void) {
	Array *entries = Array_new();

	Entry *settings = entryFromPakName("Settings");
	if (settings)
		Array_push(entries, settings);

	Entry *store = entryFromPakName("Pak Store");
	if (store)
		Array_push(entries, store);

	// quick actions
	if(WIFI_supported())
		Array_push(entries, Entry_new("Wifi", ENTRY_DIP));
	if(BT_supported())
		Array_push(entries, Entry_new("Bluetooth", ENTRY_DIP));
	if(PLAT_supportsDeepSleep() && !simple_mode)
		Array_push(entries, Entry_new("Sleep", ENTRY_DIP));
	Array_push(entries, Entry_new("Reboot", ENTRY_DIP));
	Array_push(entries, Entry_new("Poweroff", ENTRY_DIP));

	return entries;
}

// Applies <dir_path>/map.txt display-name aliases to entries the same way
// Directory_index() does for a single folder: renames, drops entries whose
// alias marks them hidden, and resorts if anything was renamed.
static Array* applyDirMap(Array* entries, const char* dir_path) {
	char map_path[256];
	snprintf(map_path, sizeof(map_path), "%s/map.txt", dir_path);
	if (!exists(map_path)) return entries;

	FILE* file = fopen(map_path, "r");
	if (!file) return entries;

	Hash* map = Hash_new();
	char line[256];
	while (fgets(line, sizeof(line), file) != NULL) {
		normalizeNewline(line);
		trimTrailingNewlines(line);
		if (strlen(line) == 0) continue;

		char* tmp = strchr(line, '\t');
		if (tmp) {
			tmp[0] = '\0';
			Hash_set(map, line, strdup(tmp + 1));
		}
	}
	fclose(file);

	int resort = 0;
	Array* kept = Array_new();
	for (int i = 0; i < entries->count; i++) {
		Entry* entry = entries->items[i];
		char* filename = strrchr(entry->path, '/') + 1;
		char* alias = Hash_get(map, filename);
		if (alias) {
			free(entry->name);
			entry->name = strdup(alias);
			resort = 1;
		}
		if (hide(entry->name)) Entry_free(entry);
		else Array_push(kept, entry);
	}
	Array_free(entries);
	Hash_free(map);

	if (resort) EntryArray_sort(kept);
	return kept;
}

// Every game of every visible system, sorted A-Z, for the "All games" root page.
// Systems come from getRoms() so hidden/empty systems and collation of sibling
// folders match the regular system list.
// Subfolders inside a system (multi-disc games, user folders) stay as folders.
static Array* getAllGames(void) {
	Array* all = Array_new();
	Array* systems = getRoms();

	for (int i = 0; i < systems->count; i++) {
		Entry* system = systems->items[i];
		Array* games = applyDirMap(getEntries(system->path), system->path);
		Array_yoink(all, games);
	}
	EntryArray_free(systems);

	EntryArray_sort(all);
	return all;
}

static Array* getRoot(void) {
	if (CFG_getAllGamesView()) {
		// hasRecents() (re)loads the recents list the Quick Menu and game switcher
		// rely on, and consumes a pending disc change, so it must still run here
		hasRecents();
		Array* all = getAllGames();
		if (all->count > 0) {
			all_games_root = 1;
			// after the A-Z games, same as the end of the system list
			pushToolsEntry(all);
			return all;
		}
		// no games anywhere: fall back to the regular root so Tools etc. stay reachable
		EntryArray_free(all);
	}
	all_games_root = 0;

    Array* root = Array_new();

    if (hasRecents() && CFG_getShowRecents())
		Array_push(root, Entry_new(FAUX_RECENT_PATH, ENTRY_DIR));

	Array *entries = getRoms();

	// Handle collections
	if (hasCollections() && CFG_getShowCollections()) {
        if (entries->count) {
            Array_push(root, Entry_new(COLLECTIONS_PATH, ENTRY_DIR));
        } else { // No visible systems, promote collections to root
			Array *collections = getCollections();
			Array_yoink(entries, collections);
        }
    }

    // Move entries to root
	Array_yoink(root, entries);

	// Add tools if applicable
	pushToolsEntry(root);

    return root;
}

static Entry* entryFromRecent(Recent* recent)
{
	if(!recent || !recent->available)
		return NULL;

	char sd_path[256];
	sprintf(sd_path, "%s%s", SDCARD_PATH, recent->path);
	int type = suffixMatch(".pak", sd_path) ? ENTRY_PAK : ENTRY_ROM; // ???
	Entry* entry = Entry_new(sd_path, type);
	if (recent->alias) {
		free(entry->name);
		entry->name = strdup(recent->alias);
	}
	return entry;
}

static Array* getRecents(void) {
	Array* entries = Array_new();
	for (int i=0; i<recents->count; i++) {
		Recent *recent = recents->items[i];
		Entry *entry = entryFromRecent(recent);
		if(entry)
			Array_push(entries, entry);
	}
	return entries;
}

static Array* getCollection(char* path) {
	Array* entries = Array_new();
	FILE* file = fopen(path, "r");
	if (file) {
		char line[256];
		while (fgets(line,256,file)!=NULL) {
			normalizeNewline(line);
			trimTrailingNewlines(line);
			if (strlen(line)==0) continue; // skip empty lines

			char sd_path[256];
			sprintf(sd_path, "%s%s", SDCARD_PATH, line);
			if (exists(sd_path)) {
				int type = suffixMatch(".pak", sd_path) ? ENTRY_PAK : ENTRY_ROM; // ???
				Array_push(entries, Entry_new(sd_path, type));

				// char emu_name[256];
				// getEmuName(sd_path, emu_name);
				// if (hasEmu(emu_name)) {
					// Array_push(entries, Entry_new(sd_path, ENTRY_ROM));
				// }
			}
		}
		fclose(file);
	}
	return entries;
}
static Array* getDiscs(char* path){

	// TODO: does path have SDCARD_PATH prefix?

	Array* entries = Array_new();

	char base_path[256];
	strcpy(base_path, path);
	char* tmp = strrchr(base_path, '/') + 1;
	tmp[0] = '\0';

	// TODO: limit number of discs supported (to 9?)
	FILE* file = fopen(path, "r");
	if (file) {
		char line[256];
		int disc = 0;
		while (fgets(line,256,file)!=NULL) {
			normalizeNewline(line);
			trimTrailingNewlines(line);
			if (strlen(line)==0) continue; // skip empty lines

			char disc_path[256];
			sprintf(disc_path, "%s%s", base_path, line);

			if (exists(disc_path)) {
				disc += 1;
				Entry* entry = Entry_new(disc_path, ENTRY_ROM);
				free(entry->name);
				char name[16];
				sprintf(name, "Disc %i", disc);
				entry->name = strdup(name);
				Array_push(entries, entry);
			}
		}
		fclose(file);
	}
	return entries;
}
static int getFirstDisc(char* m3u_path, char* disc_path) { // based on getDiscs() natch
	int found = 0;

	char base_path[256];
	strcpy(base_path, m3u_path);
	char* tmp = strrchr(base_path, '/') + 1;
	tmp[0] = '\0';

	FILE* file = fopen(m3u_path, "r");
	if (file) {
		char line[256];
		while (fgets(line,256,file)!=NULL) {
			normalizeNewline(line);
			trimTrailingNewlines(line);
			if (strlen(line)==0) continue; // skip empty lines

			sprintf(disc_path, "%s%s", base_path, line);

			if (exists(disc_path)) found = 1;
			break;
		}
		fclose(file);
	}
	return found;
}

static void addEntries(Array* entries, char* path) {
	DIR *dh = opendir(path);
	if (dh!=NULL) {
		struct dirent *dp;
		char* tmp;
		char full_path[256];
		sprintf(full_path, "%s/", path);
		tmp = full_path + strlen(full_path);
		while((dp = readdir(dh)) != NULL) {
			if (hide(dp->d_name)) continue;
			strcpy(tmp, dp->d_name);
			int is_dir = dp->d_type==DT_DIR;
			int type;
			if (is_dir) {
				// TODO: this should make sure launch.sh exists
				if (suffixMatch(".pak", dp->d_name)) {
					type = ENTRY_PAK;
				}
				else {
					type = ENTRY_DIR;
				}
			}
			else {
				if (prefixMatch(COLLECTIONS_PATH, full_path)) {
					type = ENTRY_DIR; // :shrug:
				}
				else {
					type = ENTRY_ROM;
				}
			}
			Array_push(entries, Entry_new(full_path, type));
		}
		closedir(dh);
	}
}

static int isConsoleDir(char* path) {
	char* tmp;
	char parent_dir[256];
	strcpy(parent_dir, path);
	tmp = strrchr(parent_dir, '/');
	tmp[0] = '\0';

	return exactMatch(parent_dir, ROMS_PATH);
}

static Array* getEntries(char* path){
	Array* entries = Array_new();

	if (isConsoleDir(path)) { // top-level console folder, might collate
		char collated_path[256];
		strcpy(collated_path, path);
		char* tmp = strrchr(collated_path, '(');
		// 1 because we want to keep the opening parenthesis to avoid collating "Game Boy Color" and "Game Boy Advance" into "Game Boy"
		// but conditional so we can continue to support a bare tag name as a folder name
		if (tmp) tmp[1] = '\0';

		DIR *dh = opendir(ROMS_PATH);
		if (dh!=NULL) {
			struct dirent *dp;
			char full_path[256];
			sprintf(full_path, "%s/", ROMS_PATH);
			tmp = full_path + strlen(full_path);
			// while loop so we can collate paths, see above
			while((dp = readdir(dh)) != NULL) {
				if (hide(dp->d_name)) continue;
				if (dp->d_type!=DT_DIR) continue;
				strcpy(tmp, dp->d_name);

				if (!prefixMatch(collated_path, full_path)) continue;
				addEntries(entries, full_path);
			}
			closedir(dh);
		}
	}
	else addEntries(entries, path); // just a subfolder

	EntryArray_sort(entries);
	return entries;
}

///////////////////////////////////////

static void queueNext(char* cmd) {
	LOG_info("cmd: %s\n", cmd);
	putFile("/tmp/next", cmd);
	quit = 1;
}

// based on https://stackoverflow.com/a/31775567/145965
static int replaceString(char *line, const char *search, const char *replace) {
   char *sp; // start of pattern
   if ((sp = strstr(line, search)) == NULL) {
      return 0;
   }
   int count = 1;
   int sLen = strlen(search);
   int rLen = strlen(replace);
   if (sLen > rLen) {
      // move from right to left
      char *src = sp + sLen;
      char *dst = sp + rLen;
      while((*dst = *src) != '\0') { dst++; src++; }
   } else if (sLen < rLen) {
      // move from left to right
      int tLen = strlen(sp) - sLen;
      char *stop = sp + rLen;
      char *src = sp + sLen + tLen;
      char *dst = sp + rLen + tLen;
      while(dst >= stop) { *dst = *src; dst--; src--; }
   }
   memcpy(sp, replace, rLen);
   count += replaceString(sp + rLen, search, replace);
   return count;
}
static char* escapeSingleQuotes(char* str) {
	// why not call replaceString directly?
	// call points require the modified string be returned
	// but replaceString is recursive and depends on its
	// own return value (but does it need to?)
	replaceString(str, "'", "'\\''");
	return str;
}

///////////////////////////////////////

static void readyResumePath(char* rom_path, int type) {
	char* tmp;
	can_resume = 0;
	has_preview = 0;
	char path[256];
	strcpy(path, rom_path);

	if (!prefixMatch(ROMS_PATH, path)) return;

	char auto_path[256];
	if (type==ENTRY_DIR) {
		if (!hasCue(path, auto_path)) { // no cue?
			tmp = strrchr(auto_path, '.') + 1; // extension
			strcpy(tmp, "m3u"); // replace with m3u
			if (!exists(auto_path)) return; // no m3u
		}
		strcpy(path, auto_path); // cue or m3u if one exists
	}

	if (!suffixMatch(".m3u", path)) {
		char m3u_path[256];
		if (hasM3u(path, m3u_path)) {
			// change path to m3u path
			strcpy(path, m3u_path);
		}
	}

	char emu_name[256];
	getEmuName(path, emu_name);

	char rom_file[256];
	tmp = strrchr(path, '/') + 1;
	strcpy(rom_file, tmp);

	sprintf(slot_path, "%s/.minui/%s/%s.txt", SHARED_USERDATA_PATH, emu_name, rom_file); // /.userdata/.minui/<EMU>/<romname>.ext.txt
	can_resume = exists(slot_path);

	// slot_path contains a single integer representing the last used slot
	if (can_resume) {
		char slot[16];
		getFile(slot_path, slot, 16);
		int s = atoi(slot);
		sprintf(preview_path, "%s/.minui/%s/%s.%0d.bmp", SHARED_USERDATA_PATH, emu_name, rom_file, s); // /.userdata/.minui/<EMU>/<romname>.ext.<n>.bmp
		has_preview = exists(preview_path);
	}
}
static void readyResume(Entry* entry) {
	readyResumePath(entry->path, entry->type);
}

static void saveLast(char* path);
static void loadLast(void);

static int autoResume(void) {
	// NOTE: bypasses recents

	if (!exists(AUTO_RESUME_PATH)) return 0;

	char path[256];
	getFile(AUTO_RESUME_PATH, path, 256);
	unlink(AUTO_RESUME_PATH);
	sync();

	// make sure rom still exists
	char sd_path[256];
	sprintf(sd_path, "%s%s", SDCARD_PATH, path);
	if (!exists(sd_path)) return 0;

	// make sure emu still exists
	char emu_name[256];
	getEmuName(sd_path, emu_name);

	char emu_path[256];
	getEmuPath(emu_name, emu_path);

	if (!exists(emu_path)) return 0;

	// putFile(LAST_PATH, FAUX_RECENT_PATH); // saveLast() will crash here because top is NULL

	char act[256];
	sprintf(act, "gametimectl.elf start '%s'", escapeSingleQuotes(sd_path));
	system(act);

	char cmd[256];
	// dont escape sd_path again because it was already escaped for gametimectl and function modifies input str aswell
	sprintf(cmd, "'%s' '%s'", escapeSingleQuotes(emu_path), sd_path);
	putInt(RESUME_SLOT_PATH, AUTO_RESUME_SLOT);
	queueNext(cmd);
	return 1;
}

static void openPak(char* path) {
	// NOTE: escapeSingleQuotes() modifies the passed string
	// so we need to save the path before we call that
	// if (prefixMatch(ROMS_PATH, path)) {
	// 	addRecent(path);
	// }
	saveLast(path);

	char cmd[256];
	sprintf(cmd, "'%s/launch.sh'", escapeSingleQuotes(path));
	queueNext(cmd);
}

// Run the action bound to a user-assignable button (0 == FN1, 1 == FN2).
// Returns 1 if something was launched.
static int runFnAction(int index) {
	const char* action = CFG_getFnAction(index);
	if (!action || !action[0]) return 0; // unassigned

	// only "launch a Tools pak" exists so far; ignore kinds we don't know so a card
	// configured on a newer build just does nothing here instead of misfiring
	size_t prefix_len = strlen(FN_ACTION_PAK_PREFIX);
	if (strncmp(action, FN_ACTION_PAK_PREFIX, prefix_len)!=0) return 0;

	const char* rel = action + prefix_len;
	if (!rel[0]) return 0;

	char pak_path[256];
	snprintf(pak_path, sizeof(pak_path), "%s/Tools/%s/%s", SDCARD_PATH, PLATFORM, rel);

	char launch_path[256];
	snprintf(launch_path, sizeof(launch_path), "%s/launch.sh", pak_path);
	if (!exists(launch_path)) return 0; // stale binding, eg. the pak was deleted

	// unlike openPak() we save where the user *is*, not the pak itself, so exiting
	// the pak comes back to the same spot in the list
	if (top && top->entries->count>0) {
		Entry* entry = top->entries->items[top->selected];
		saveLast(entry->path);
	}

	char cmd[256];
	// NOTE: escapeSingleQuotes() modifies pak_path in place
	snprintf(cmd, sizeof(cmd), "'%s/launch.sh'", escapeSingleQuotes(pak_path));
	queueNext(cmd);
	return 1;
}
static void openRom(char* path, char* last) {
	LOG_info("openRom(%s,%s)\n", path, last);

	char sd_path[256];
	strcpy(sd_path, path);

	char m3u_path[256];
	int has_m3u = hasM3u(sd_path, m3u_path);

	char recent_path[256];
	strcpy(recent_path, has_m3u ? m3u_path : sd_path);

	if (has_m3u && suffixMatch(".m3u", sd_path)) {
		getFirstDisc(m3u_path, sd_path);
	}

	char emu_name[256];
	getEmuName(sd_path, emu_name);

	if (should_resume) {
		char slot[16];
		getFile(slot_path, slot, 16);
		putFile(RESUME_SLOT_PATH, slot);
		should_resume = 0;

		if (has_m3u) {
			char rom_file[256];
			strcpy(rom_file, strrchr(m3u_path, '/') + 1);

			// get disc for state
			char disc_path_path[256];
			sprintf(disc_path_path, "%s/.minui/%s/%s.%s.txt", SHARED_USERDATA_PATH, emu_name, rom_file, slot); // /.userdata/arm-480/.minui/<EMU>/<romname>.ext.0.txt

			if (exists(disc_path_path)) {
				// switch to disc path
				char disc_path[256];
				getFile(disc_path_path, disc_path, 256);
				if (disc_path[0]=='/') strcpy(sd_path, disc_path); // absolute
				else { // relative
					strcpy(sd_path, m3u_path);
					char* tmp = strrchr(sd_path, '/') + 1;
					strcpy(tmp, disc_path);
				}
			}
		}
	}
	else putInt(RESUME_SLOT_PATH,8); // resume hidden default state

	char emu_path[256];
	getEmuPath(emu_name, emu_path);

	// NOTE: escapeSingleQuotes() modifies the passed string
	// so we need to save the path before we call that
	addRecent(recent_path, recent_alias); // yiiikes
	saveLast(last==NULL ? sd_path : last);
	char act[256];
	sprintf(act, "gametimectl.elf start '%s'", escapeSingleQuotes(sd_path));
	system(act);
	char cmd[256];
	// dont escape sd_path again because it was already escaped for gametimectl and function modifies input str aswell
	sprintf(cmd, "'%s' '%s'", escapeSingleQuotes(emu_path), sd_path);
	queueNext(cmd);
}

static bool isDirectSubdirectory(const Directory* parent, const char* child_path) {
    const char* parent_path = parent->path;

    size_t parent_len = strlen(parent_path);
    size_t child_len = strlen(child_path);

    // Child must be longer than parent to be a subdirectory
    if (child_len <= parent_len || strncmp(child_path, parent_path, parent_len) != 0) {
        return false;
    }

    // Next char after parent path must be '/'
    if (child_path[parent_len] != '/') return false;

    // Walk through the child path after parent, skipping PLATFORM segments
    const char* cursor = child_path + parent_len + 1; // skip the slash

    int levels = 0;
    while (*cursor) {
        const char* next = strchr(cursor, '/');
        size_t segment_len = next ? (size_t)(next - cursor) : strlen(cursor);

        if (segment_len == 0) break;

        // Copy segment into a buffer to compare
        char segment[PATH_MAX];
        if (segment_len >= PATH_MAX) return false;
        strncpy(segment, cursor, segment_len);
        segment[segment_len] = '\0';

        // Count level only if it's not PLATFORM
        if (strcmp(segment, PLATFORM) != 0 && strcmp(segment, "Roms") != 0) {
            levels++;
        }

        if (!next) break;
        cursor = next + 1;
    }

    return (levels == 1);  // exactly one meaningful level deeper
}

Array* pathToStack(const char* path) {
	Array* array = Array_new();

	if (!path || strlen(path) == 0) return array;

	if (!prefixMatch(SDCARD_PATH, path)) return array;

	// Always include root directory
	Directory* root_dir = Directory_new(SDCARD_PATH, 0);
	root_dir->start = 0;
	root_dir->end = (root_dir->entries->count < MAIN_ROW_COUNT) ? root_dir->entries->count : MAIN_ROW_COUNT;
	Array_push(array, root_dir);

	if (exactMatch(path, SDCARD_PATH)) return array;

	char temp_path[PATH_MAX];
	strcpy(temp_path, SDCARD_PATH);
	size_t current_len = strlen(SDCARD_PATH);

	const char* cursor = path + current_len;
	if (*cursor == '/') cursor++;

	while (*cursor) {
		const char* next = strchr(cursor, '/');
		size_t segment_len = next ? (size_t)(next - cursor) : strlen(cursor);
		if (segment_len == 0 || segment_len >= PATH_MAX) break;

		char segment[PATH_MAX];
		strncpy(segment, cursor, segment_len);
		segment[segment_len] = '\0';

		// Append '/' if needed
		if (temp_path[current_len - 1] != '/') {
			if (current_len + 1 >= PATH_MAX) break;
			temp_path[current_len++] = '/';
			temp_path[current_len] = '\0';
		}

		// Append segment
		if (current_len + segment_len >= PATH_MAX) break;
		strcat(temp_path, segment);
		current_len += segment_len;

		if (strcmp(segment, PLATFORM) == 0) {
			// Merge with previous directory
			if (array->count > 0) {
				// Remove the previous directory
				Directory* last = (Directory*)array->items[array->count - 1];
				Array_pop(array);
				Directory_free(last); // assuming you have a Directory_free

				// Replace with updated one using combined path
				Directory* merged = Directory_new(temp_path, 0);
				merged->start = 0;
				merged->end = (merged->entries->count < MAIN_ROW_COUNT) ? merged->entries->count : MAIN_ROW_COUNT;
				Array_push(array, merged);
			}
		} else {
			Directory* dir = Directory_new(temp_path, 0);
			dir->start = 0;
			dir->end = (dir->entries->count < MAIN_ROW_COUNT) ? dir->entries->count : MAIN_ROW_COUNT;
			Array_push(array, dir);
		}

		if (!next) break;
		cursor = next + 1;
	}

	return array;
}

static void openDirectory(char* path, int auto_launch) {
	char auto_path[256];
	if (hasCue(path, auto_path) && auto_launch) {
		openRom(auto_path, path);
		return;
	}

	char m3u_path[256];
	strcpy(m3u_path, auto_path);
	char* tmp = strrchr(m3u_path, '.') + 1; // extension
	strcpy(tmp, "m3u"); // replace with m3u
	if (exists(m3u_path) && auto_launch) {
		auto_path[0] = '\0';
		if (getFirstDisc(m3u_path, auto_path)) {
			openRom(auto_path, path);
			return;
		}
		// TODO: doesn't handle empty m3u files
	}

	// If this is the exact same directory for some reason, just return.
	if(top && strcmp(top->path, path) == 0)
		return;

	// If this path is a direct subdirectory of top, push it on top of the stack
	// If it isnt, we need to recreate the stack to keep navigation consistent
	if(!top || isDirectSubdirectory(top, path)) {
		int selected = 0;
		int start = 0;
		int end = 0;
		if (top && top->entries->count>0) {
			if (restore_depth==stack->count && top->selected==restore_relative) {
				selected = restore_selected;
				start = restore_start;
				end = restore_end;
			}
		}

		top = Directory_new(path, selected);
		top->start = start;
		top->end = end ? end : ((top->entries->count<MAIN_ROW_COUNT) ? top->entries->count : MAIN_ROW_COUNT);

		Array_push(stack, top);
	}
	else {
		// keep a copy of path, which might be a reference into stack which is about to be freed
		char temp_path[256];
		strcpy(temp_path, path);

		// construct a fresh stack by walking upwards until SDCARD_ROOT
		DirectoryArray_free(stack);

		stack = pathToStack(temp_path);
		top = stack->items[stack->count - 1];
	}
}

static void closeDirectory(void) {
	restore_selected = top->selected;
	restore_start = top->start;
	restore_end = top->end;
	DirectoryArray_pop(stack);
	restore_depth = stack->count;
	top = stack->items[stack->count-1];
	restore_relative = top->selected;
}

static void cleanupImageLoaderPool(void); // Forward declaration

static void toggleQuick(Entry* self)
{
	if(!self)
		return;

	if(!strcmp(self->name, "Wifi")) {
		WIFI_enable(!WIFI_enabled());
	}
	else if(!strcmp(self->name, "Bluetooth")) {
		BT_enable(!BT_enabled());
	}
	else if(!strcmp(self->name, "Sleep")) {
		PWR_sleep();
	}
	else if(!strcmp(self->name, "Reboot")) {
		cleanupImageLoaderPool();  // Stop worker threads before SDL cleanup
		PWR_powerOff(1);
	}
	else if(!strcmp(self->name, "Poweroff")) {
		cleanupImageLoaderPool();  // Stop worker threads before SDL cleanup
		PWR_powerOff(0);
	}
}

static void Entry_open(Entry* self) {
	recent_alias = self->name;  // yiiikes
	if (self->type==ENTRY_ROM) {
		startgame = 1;
		char *last = NULL;
		if (prefixMatch(COLLECTIONS_PATH, top->path)) {
			char* tmp;
			char filename[256];

			tmp = strrchr(self->path, '/');
			if (tmp) strcpy(filename, tmp+1);

			char last_path[256];
			sprintf(last_path, "%s/%s", top->path, filename);
			last = last_path;
		}
		openRom(self->path, last);
	}
	else if (self->type==ENTRY_PAK) {
		startgame = 1;
		openPak(self->path);
	}
	else if (self->type==ENTRY_DIR) {
		openDirectory(self->path, 1);
	}
	else if (self->type==ENTRY_DIP) {
		toggleQuick(self);
	}
}

///////////////////////////////////////

static void saveLast(char* path) {
	// special case for recently played
	if (exactMatch(top->path, FAUX_RECENT_PATH)) {
		// NOTE: that we don't have to save the file because
		// your most recently played game will always be at
		// the top which is also the default selection
		path = FAUX_RECENT_PATH;
	}
	putFile(LAST_PATH, path);
}
static void loadLast(void) { // call after loading root directory
	if (!exists(LAST_PATH)) return;

	char last_path[256];
	getFile(LAST_PATH, last_path, 256);

	char full_path[256];
	strcpy(full_path, last_path);

	// "All games" root: games sit directly at the root, so select the game (or the
	// game's folder, eg. multi-disc) instead of walking Roms/<system>/...
	if (all_games_root && stack->count == 1 && prefixMatch(ROMS_PATH "/", full_path)) {
		for (int i = 0; i < top->entries->count; i++) {
			Entry* entry = top->entries->items[i];
			size_t len = strlen(entry->path);
			if (exactMatch(entry->path, full_path) || (prefixMatch(entry->path, full_path) && full_path[len] == '/')) {
				top->selected = i;
				if (i >= top->end) {
					top->start = i;
					top->end = top->start + MAIN_ROW_COUNT;
					if (top->end > top->entries->count) {
						top->end = top->entries->count;
						top->start = top->end - MAIN_ROW_COUNT;
						if (top->start < 0) top->start = 0;
					}
				}
				break;
			}
		}
		if (top->selected >= 0 && top->selected < top->entries->count)
			readyResume(top->entries->items[top->selected]);
		return;
	}

	char* tmp;
	char filename[256];
	tmp = strrchr(last_path, '/');
	if (tmp) strcpy(filename, tmp);

	Array* last = Array_new();
	while (!exactMatch(last_path, SDCARD_PATH)) {
		Array_push(last, strdup(last_path));

		char* slash = strrchr(last_path, '/');
		last_path[(slash-last_path)] = '\0';
	}

	// "All games" root has no Recents/Collections entries (they live in the Quick
	// Menu), so open that top-level folder directly and restore within it.
	if (all_games_root && stack->count == 1 && last->count > 0) {
		char* first = last->items[last->count - 1];
		if (exactMatch(first, FAUX_RECENT_PATH) || exactMatch(first, COLLECTIONS_PATH)) {
			char* path = Array_pop(last);
			openDirectory(path, 0);
			free(path);
		}
	}

	while (last->count>0) {
		char* path = Array_pop(last);
		if (!exactMatch(path, ROMS_PATH)) { // romsDir is effectively root as far as restoring state after a game
			char collated_path[256];
			collated_path[0] = '\0';
			if (suffixMatch(")", path) && isConsoleDir(path)) {
				strcpy(collated_path, path);
				tmp = strrchr(collated_path, '(');
				if (tmp) tmp[1] = '\0'; // 1 because we want to keep the opening parenthesis to avoid collating "Game Boy Color" and "Game Boy Advance" into "Game Boy"
			}

			for (int i=0; i<top->entries->count; i++) {
				Entry* entry = top->entries->items[i];

				// NOTE: strlen() is required for collated_path, '\0' wasn't reading as NULL for some reason
				if (exactMatch(entry->path, path) || (strlen(collated_path) && prefixMatch(collated_path, entry->path)) || (prefixMatch(COLLECTIONS_PATH, full_path) && suffixMatch(filename, entry->path))) {
					top->selected = i;
					if (i>=top->end) {
						top->start = i;
						top->end = top->start + MAIN_ROW_COUNT;
						if (top->end>top->entries->count) {
							top->end = top->entries->count;
							top->start = top->end - MAIN_ROW_COUNT;
						}
					}
					if (last->count==0 && !exactMatch(entry->path, FAUX_RECENT_PATH) && !(!exactMatch(entry->path, COLLECTIONS_PATH) && prefixMatch(COLLECTIONS_PATH, entry->path))) break; // don't show contents of auto-launch dirs

					if (entry->type==ENTRY_DIR) {
						openDirectory(entry->path, 0);
						break;
					}
				}
			}
		}
		free(path); // we took ownership when we popped it
	}

	StringArray_free(last);

	if (top->selected >= 0 && top->selected < top->entries->count) {
		Entry *selected_entry = top->entries->items[top->selected];
		readyResume(selected_entry);
	}
}

///////////////////////////////////////

static void QuickMenu_init(void) {
	quick = getQuickEntries();
	quickActions = getQuickToggles();
}
static void QuickMenu_quit(void) {
	EntryArray_free(quick);
	EntryArray_free(quickActions);
}

static void Menu_init(void) {
	stack = Array_new(); // array of open Directories
	recents = Array_new();

	openDirectory(SDCARD_PATH, 0);
	loadLast(); // restore state when available

	QuickMenu_init(); // needs Menu_init
}
static void Menu_quit(void) {
	RecentArray_free(recents);
	DirectoryArray_free(stack);

	QuickMenu_quit();
}

///////////////////////////////////////

static int dirty = 1;
static int previous_row = 0;
static int previous_depth = 0;

///////////////////////////////////////

enum {
	ANIM_NONE = 0,
	SLIDE_LEFT = 1,
	SLIDE_RIGHT = 2,
};

typedef void (*BackgroundLoadedCallback)(SDL_Surface* surface);

typedef struct {
    char imagePath[MAX_PATH];
    BackgroundLoadedCallback callback;
    void* userData;
} LoadBackgroundTask;

typedef struct finishedTask {
	int startX;
	int targetX;
	int startY;
	int targetY;
	int targetTextY;
	int move_y;
	int move_w;
	int move_h;
	int frames;
	int done;
	void* userData;
	  char *entry_name;
	SDL_Rect dst;
} finishedTask;

typedef void (*AnimTaskCallback)(finishedTask *task);
typedef struct AnimTask {
	int startX;
	int targetX;
	int startY;
	int targetY;
	int targetTextY;
	int move_w;
	int move_h;
	int frames;
	AnimTaskCallback callback;
	void* userData;
	 char *entry_name;
	SDL_Rect dst;
} AnimTask;

typedef struct TaskNode {
    LoadBackgroundTask* task;
    struct TaskNode* next;
} TaskNode;
typedef struct AnimTaskNode {
	AnimTask* task;
    struct AnimTaskNode* next;
} AnimTaskNode;

static TaskNode* taskBGQueueHead = NULL;
static TaskNode* taskBGQueueTail = NULL;
static TaskNode* taskThumbQueueHead = NULL;
static TaskNode* taskThumbQueueTail = NULL;
static AnimTaskNode* animTaskQueueHead = NULL;
static AnimTaskNode* animTtaskQueueTail = NULL;
static SDL_mutex* bgqueueMutex = NULL;
static SDL_mutex* thumbqueueMutex = NULL;
static SDL_mutex* animqueueMutex = NULL;
static SDL_cond* bgqueueCond = NULL;
static SDL_cond* thumbqueueCond = NULL;
static SDL_cond* animqueueCond = NULL;

static SDL_mutex* bgMutex = NULL;
static SDL_mutex* thumbMutex = NULL;
static SDL_mutex* animMutex = NULL;
static SDL_mutex* frameMutex = NULL;
static SDL_mutex* fontMutex = NULL;
static SDL_cond* flipCond = NULL;

static SDL_Thread* bgLoadThread = NULL;
static SDL_Thread* thumbLoadThread = NULL;
static SDL_Thread* animWorkerThread = NULL;

static SDL_atomic_t workerThreadsShutdown; // Flag to signal threads to exit (atomic for thread safety)

static SDL_Surface* folderbgbmp = NULL;
static SDL_Surface* thumbbmp = NULL;
static SDL_Surface* screen = NULL; // Must be assigned externally
static SDL_Surface* globalpill = NULL;
static SDL_Surface* globalText = NULL;

static int had_thumb = 0;
static int ox;
static int oy;
static SDL_atomic_t animationDrawAtomic;
static SDL_atomic_t needDrawAtomic;

static inline void setAnimationDraw(int v) { SDL_AtomicSet(&animationDrawAtomic, v); }
static inline int getAnimationDraw(void) { return SDL_AtomicGet(&animationDrawAtomic); }
static inline void setNeedDraw(int v) { SDL_AtomicSet(&needDrawAtomic, v); }
static inline int getNeedDraw(void) { return SDL_AtomicGet(&needDrawAtomic); }

///////////////////////////////////////
// Grid layout: tiles laid out column-major (top to bottom, then the next column)
// so the content scrolls horizontally. Up/Down step through tiles in order
// (wrapping into the neighbouring column at a column's ends), Left/Right move
// between columns. Used for every game list screen when the content layout
// setting is "Grid"; the list renderer is untouched otherwise.

typedef struct GridLayout {
	int rows;
	int cols;   // visible columns
	int tile_w;
	int tile_h;
	int gap;
	int x;      // left edge of the first visible column
	int y;      // top edge of the first row
} GridLayout;

static int Grid_active(void) {
	return CFG_getContentLayout() == CONTENT_LAYOUT_GRID;
}

static void Grid_getLayout(GridLayout* g) {
	// leave the top row for the title pill and hardware status, the bottom row for button hints
	int margin = GFX_getScreenMargin();
	int top = margin + SCALE1(PILL_SIZE + PADDING);
	int bottom = screen->h - margin - SCALE1(PILL_SIZE + PADDING);
	int area_w = screen->w - margin * 2;
	int area_h = bottom - top;

	g->gap = SCALE1(BUTTON_MARGIN * 2);
	g->rows = clamp(CFG_getGridRows(), GRID_ROWS_MIN, GRID_ROWS_MAX);
	g->tile_h = (area_h - g->gap * (g->rows - 1)) / g->rows;
	g->tile_w = g->tile_h;

	// keep at least a few columns visible so horizontal scrolling reads as a grid,
	// tiles become portrait (box art shaped) when rows are tall
	const int min_cols = 3;
	int max_tile_w = (area_w - g->gap * (min_cols - 1)) / min_cols;
	if (g->tile_w > max_tile_w) g->tile_w = max_tile_w;

	g->cols = (area_w + g->gap) / (g->tile_w + g->gap);
	if (g->cols < 1) g->cols = 1;

	int used_w = g->cols * g->tile_w + (g->cols - 1) * g->gap;
	g->x = (screen->w - used_w) / 2;
	g->y = top;
}

// Returns the new selection after moving by (dx, dy) tiles. Up/Down step through
// the tiles in order, so the top of a column continues to the bottom of the
// previous one and the bottom continues to the top of the next. wrap allows
// jumping from the first tile/column to the last (and back), only used on a fresh
// press so holding the d-pad stops at the edges, same as the list.
static int Grid_move(int selected, int total, int rows, int dx, int dy, int wrap) {
	if (total <= 0) return 0;
	int row = selected % rows;
	int col = selected / rows;
	int last_col = (total - 1) / rows;

	if (dy < 0) {
		if (selected > 0) return selected - 1;
		return wrap ? total - 1 : selected;
	}
	if (dy > 0) {
		if (selected + 1 < total) return selected + 1;
		return wrap ? 0 : selected;
	}

	if (dx > 0) {
		if (col < last_col) return MIN(selected + rows, total - 1);
		return wrap ? MIN(row, total - 1) : selected;
	}
	if (dx < 0) {
		if (col > 0) return selected - rows;
		return wrap ? MIN(last_col * rows + row, total - 1) : selected;
	}
	return selected;
}

// Keeps the selected tile's column on screen.
static void Grid_scrollTo(Directory* dir, const GridLayout* g) {
	int total_cols = (dir->entries->count + g->rows - 1) / g->rows;
	int col = dir->selected / g->rows;
	if (col < dir->grid_col) dir->grid_col = col;
	if (col >= dir->grid_col + g->cols) dir->grid_col = col - g->cols + 1;
	dir->grid_col = clamp(dir->grid_col, 0, MAX(0, total_cols - g->cols));
}

// Tile art uses the same files as game art: <parent>/.media/<name>.png.
// Folders (eg. systems) fall back to their own .media/bg.png.
static int Grid_findArt(Entry* entry, char* out, size_t out_size) {
	char path_copy[MAX_PATH];
	strncpy(path_copy, entry->path, sizeof(path_copy) - 1);
	path_copy[sizeof(path_copy) - 1] = '\0';

	char* name = strrchr(path_copy, '/');
	if (!name) return 0;
	*name = '\0';
	name++;

	char base[MAX_PATH];
	strncpy(base, name, sizeof(base) - 1);
	base[sizeof(base) - 1] = '\0';
	if (entry->type != ENTRY_DIR) {
		char* dot = strrchr(base, '.');
		if (dot) *dot = '\0';
	}

	snprintf(out, out_size, "%s/.media/%s.png", path_copy, base);
	if (exists(out)) return 1;

	if (entry->type == ENTRY_DIR) {
		snprintf(out, out_size, "%s/.media/bg.png", entry->path);
		if (exists(out)) return 1;
	}
	return 0;
}

// Tile corner radius: the "Game art corner radius" setting, so tiles match game art
// elsewhere in the UI, limited on small tiles so they don't turn into circles.
static int Grid_cornerRadius(int w, int h) {
	return MAX(0, MIN(SCALE1(CFG_getThumbnailRadius()), MIN(w, h) / 4));
}

// Fills rect on screen with color and rounds its corners (pixels outside the
// radius become transparent, letting the background layer show through).
static void Grid_fillRounded(SDL_Rect* rect, Uint32 color, int radius) {
	SDL_FillRect(screen, rect, color);
	if (radius <= 0) return;

	// GFX_ApplyRoundedCorners_8888 writes pixels directly, keep it on-surface
	SDL_Rect clipped;
	if (!SDL_IntersectRect(rect, &(SDL_Rect){ 0, 0, screen->w, screen->h }, &clipped)) return;
	if (clipped.w != rect->w || clipped.h != rect->h) return; // partially off-screen, leave square
	GFX_ApplyRoundedCorners_8888(screen, rect, radius);
}

static SDL_Surface* Grid_loadArt(const char* path, int max_w, int max_h) {
	SDL_Surface* image = IMG_Load(path);
	if (!image) return NULL;
	SDL_Surface* src = SDL_ConvertSurfaceFormat(image, screen->format->format, 0);
	SDL_FreeSurface(image);
	if (!src) return NULL;

	double scale = MIN((double)max_w / src->w, (double)max_h / src->h);
	int w = MAX(1, (int)(src->w * scale));
	int h = MAX(1, (int)(src->h * scale));

	SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, w, h, screen->format->BitsPerPixel, screen->format->format);
	if (dst) {
		SDL_FillRect(dst, NULL, 0);
		SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
#if SDL_VERSION_ATLEAST(2, 0, 16)
		if (SDL_SoftStretchLinear(src, NULL, dst, NULL) != 0)
#endif
			SDL_BlitScaled(src, NULL, dst, NULL);

		int radius = MIN(Grid_cornerRadius(max_w, max_h), MIN(w, h) / 2);
		if (radius > 0)
			GFX_ApplyRoundedCorners_8888(dst, &(SDL_Rect){0, 0, w, h}, radius);
		SDL_SetSurfaceBlendMode(dst, SDL_BLENDMODE_BLEND);
	}
	SDL_FreeSurface(src);
	return dst;
}

// Small LRU cache of scaled tile art keyed by entry path and tile size. Entries
// without art are cached too (surface == NULL) so they aren't probed every frame.
#define GRID_ART_CACHE_SIZE 48
typedef struct GridArt {
	char key[MAX_PATH];
	int w;
	int h;
	SDL_Surface* surface;
	unsigned int used;
} GridArt;
static GridArt grid_art[GRID_ART_CACHE_SIZE];
static unsigned int grid_art_clock = 0;

static SDL_Surface* Grid_getArt(Entry* entry, int max_w, int max_h) {
	int victim = -1;
	for (int i = 0; i < GRID_ART_CACHE_SIZE; i++) {
		GridArt* slot = &grid_art[i];
		if (!slot->key[0]) {
			if (victim < 0 || grid_art[victim].key[0]) victim = i; // prefer an empty slot
			continue;
		}
		if (slot->w == max_w && slot->h == max_h && exactMatch(slot->key, entry->path)) {
			slot->used = ++grid_art_clock;
			return slot->surface;
		}
		if (victim < 0 || (grid_art[victim].key[0] && slot->used < grid_art[victim].used)) victim = i;
	}

	GridArt* slot = &grid_art[victim];
	if (slot->surface) SDL_FreeSurface(slot->surface);
	slot->surface = NULL;

	char art_path[MAX_PATH];
	if (Grid_findArt(entry, art_path, sizeof(art_path)))
		slot->surface = Grid_loadArt(art_path, max_w, max_h);

	strncpy(slot->key, entry->path, sizeof(slot->key) - 1);
	slot->key[sizeof(slot->key) - 1] = '\0';
	slot->w = max_w;
	slot->h = max_h;
	slot->used = ++grid_art_clock;
	return slot->surface;
}

// The Tools tile (system list and all games view) shows the Quick Menu's Tools
// icon instead of a letter when it has no art of its own. Loaded from the largest
// asset and scaled down to the tile, kept while the size is unchanged.
static SDL_Surface* grid_tools_icon = NULL;
static int grid_tools_icon_px = 0;

static SDL_Surface* Grid_getToolsIcon(int px) {
	if (grid_tools_icon_px == px) return grid_tools_icon;
	if (grid_tools_icon) SDL_FreeSurface(grid_tools_icon);
	grid_tools_icon = NULL;
	grid_tools_icon_px = px;

	SDL_Surface* image = IMG_Load(SDCARD_PATH "/.system/res/Tools@4x.png");
	if (!image) return NULL;
	SDL_Surface* src = SDL_ConvertSurfaceFormat(image, screen->format->format, 0);
	SDL_FreeSurface(image);
	if (!src) return NULL;

	SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, px, px, screen->format->BitsPerPixel, screen->format->format);
	if (dst) {
		SDL_FillRect(dst, NULL, 0);
		SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
#if SDL_VERSION_ATLEAST(2, 0, 16)
		if (SDL_SoftStretchLinear(src, NULL, dst, NULL) != 0)
#endif
			SDL_BlitScaled(src, NULL, dst, NULL);
		SDL_SetSurfaceBlendMode(dst, SDL_BLENDMODE_BLEND);
	}
	SDL_FreeSurface(src);
	grid_tools_icon = dst;
	return grid_tools_icon;
}

// Tiles without art show the first letter of the name, rendered with the UI font
// opened at a size that suits the tile (kept open while the tile size is unchanged).
static TTF_Font* grid_letter_font = NULL;
static int grid_letter_font_px = 0;

static TTF_Font* Grid_getLetterFont(int px) {
	if (grid_letter_font && grid_letter_font_px == px) return grid_letter_font;

	const char* path = GFX_getSystemFontPath();
	TTF_Font* opened = (path && path[0]) ? TTF_OpenFont(path, px) : NULL;
	if (!opened) return font.large;
	TTF_SetFontStyle(opened, CFG_getFontStyle());

	if (grid_letter_font) TTF_CloseFont(grid_letter_font);
	grid_letter_font = opened;
	grid_letter_font_px = px;
	return grid_letter_font;
}

// First letter or digit of name (skipping leading punctuation like "[" or "'"),
// uppercased. Non-ASCII characters are copied whole as UTF-8. out needs 5 bytes.
static void Grid_getInitial(const char* name, char* out) {
	const unsigned char* pick = NULL;
	for (const unsigned char* c = (const unsigned char*)name; *c; c++) {
		if (*c >= 0xC0 || isalnum(*c)) { pick = c; break; }
	}
	if (!pick) pick = (const unsigned char*)name;
	if (!pick[0]) { strcpy(out, "?"); return; }

	int len = 1;
	if (pick[0] >= 0xF0) len = 4;
	else if (pick[0] >= 0xE0) len = 3;
	else if (pick[0] >= 0xC0) len = 2;

	int i = 0;
	for (; i < len && pick[i]; i++) out[i] = pick[i];
	out[i] = '\0';
	if (len == 1) out[0] = toupper((unsigned char)out[0]);
}

static void Grid_freeArt(void) {
	for (int i = 0; i < GRID_ART_CACHE_SIZE; i++) {
		if (grid_art[i].surface) SDL_FreeSurface(grid_art[i].surface);
		grid_art[i].surface = NULL;
		grid_art[i].key[0] = '\0';
	}
	if (grid_letter_font) TTF_CloseFont(grid_letter_font);
	grid_letter_font = NULL;
	grid_letter_font_px = 0;
	if (grid_tools_icon) SDL_FreeSurface(grid_tools_icon);
	grid_tools_icon = NULL;
	grid_tools_icon_px = 0;
}

// Draws label in a pill in the top-left corner, eg. the selected grid tile or
// Quick Menu item. It sits the screen margin away from the edges, like the status
// pill and button hints, and always keeps one margin of space before the status
// pill on the right (ow is that pill's width, from GFX_blitHardwareGroup).
static void drawTitlePill(const char* label, int ow) {
	if (!label || !label[0]) return;

	int margin = GFX_getScreenMargin();
	char display_name[256];
	int max_width = MAX(0, screen->w - margin * 3 - ow);
	SDL_LockMutex(fontMutex);
	int text_width = GFX_getTextWidth(font.large, label, display_name, max_width, SCALE1(BUTTON_PADDING * 2));
	int pill_width = MIN(max_width, text_width);
	if (pill_width > 0)
		GFX_blitPillDark(ASSET_WHITE_PILL, screen, &(SDL_Rect){ margin, margin, pill_width, SCALE1(PILL_SIZE) });
	SDL_Surface* text = TTF_RenderUTF8_Blended(font.large, display_name, uintToColour(THEME_COLOR5_255));
	SDL_UnlockMutex(fontMutex);
	int text_w = pill_width - SCALE1(BUTTON_PADDING * 2);
	if (text && text_w > 0) {
		int text_offset_y = (SCALE1(PILL_SIZE) - text->h + 1) >> 1;
		SDL_BlitSurface(text, &(SDL_Rect){ 0, 0, text_w, text->h }, screen,
			&(SDL_Rect){ margin + SCALE1(BUTTON_PADDING), margin + text_offset_y });
	}
	if (text) SDL_FreeSurface(text);
}

// Draws the visible tiles of dir onto screen, plus a title pill with the selected
// entry's name in the top row (when names are shown). ow is the width taken by
// the hardware status group in the top-right corner.
static void Grid_render(Directory* dir, int show_names, int ow) {
	GridLayout g;
	Grid_getLayout(&g);
	Grid_scrollTo(dir, &g);

	int total = dir->entries->count;
	int first = dir->grid_col * g.rows;
	int last = MIN(total, first + g.cols * g.rows);
	int border = SCALE1(2);

	SDL_Color text_color = uintToColour(THEME_COLOR4_255);
	SDL_Color selected_text_color = uintToColour(THEME_COLOR5_255);
	SDL_Color tile_rgb = uintToColour(THEME_COLOR3_255);
	Uint32 tile_fill = SDL_MapRGBA(screen->format, tile_rgb.r, tile_rgb.g, tile_rgb.b, 0xC0);

	// art-less tiles show a single large initial; no wrapping or "..." truncation,
	// which GFX_truncateText can't do safely in very narrow tiles
	SDL_LockMutex(fontMutex);
	TTF_Font* letter_font = Grid_getLetterFont(MAX(1, MIN(g.tile_w, g.tile_h) * 55 / 100));
	SDL_UnlockMutex(fontMutex);

	for (int i = first; i < last; i++) {
		Entry* entry = dir->entries->items[i];
		int col = i / g.rows - dir->grid_col;
		int row = i % g.rows;
		int is_selected = (i == dir->selected);
		SDL_Rect tile = {
			g.x + col * (g.tile_w + g.gap),
			g.y + row * (g.tile_h + g.gap),
			g.tile_w,
			g.tile_h
		};

		int radius = Grid_cornerRadius(tile.w, tile.h);
		SDL_Surface* art = Grid_getArt(entry, tile.w, tile.h);
		if (art) {
			SDL_Rect art_rect = { tile.x + (tile.w - art->w) / 2, tile.y + (tile.h - art->h) / 2, art->w, art->h };
			if (is_selected) {
				// frame follows the art's rounded corners, offset by the border width
				int art_radius = MIN(radius, MIN(art->w, art->h) / 2);
				SDL_Rect frame = { art_rect.x - border, art_rect.y - border, art_rect.w + border * 2, art_rect.h + border * 2 };
				Grid_fillRounded(&frame, THEME_COLOR1, art_radius > 0 ? art_radius + border : 0);
			}
			SDL_BlitSurface(art, NULL, screen, &art_rect);
			continue;
		}

		// no art: a plain tile with the name's first letter (or the Tools icon)
		Grid_fillRounded(&tile, is_selected ? THEME_COLOR1 : tile_fill, radius);

		if (isToolsEntry(entry)) {
			// same share of the tile as the Quick Menu gives its icons (48 of 72)
			SDL_Surface* icon = Grid_getToolsIcon(MAX(1, MIN(tile.w, tile.h) * 2 / 3));
			if (icon) {
				SDL_Rect dst = { tile.x + (tile.w - icon->w) / 2, tile.y + (tile.h - icon->h) / 2, 0, 0 };
				GFX_blitSurfaceColor(icon, NULL, screen, &dst, is_selected ? THEME_COLOR5 : THEME_COLOR4);
				continue;
			}
		}

		char* label = entry->name;
		trimSortingMeta(&label);
		char initial[5];
		Grid_getInitial(label, initial);

		SDL_LockMutex(fontMutex);
		SDL_Surface* letter = TTF_RenderUTF8_Blended(letter_font, initial, is_selected ? selected_text_color : text_color);
		SDL_UnlockMutex(fontMutex);
		if (letter) {
			// clip to the tile in case a glyph renders larger than expected
			SDL_Rect src = { MAX(0, (letter->w - tile.w) / 2), MAX(0, (letter->h - tile.h) / 2), MIN(letter->w, tile.w), MIN(letter->h, tile.h) };
			SDL_Rect dst = { tile.x + (tile.w - src.w) / 2, tile.y + (tile.h - src.h) / 2, 0, 0 };
			SDL_BlitSurface(letter, &src, screen, &dst);
			SDL_FreeSurface(letter);
		}
	}

	// title pill with the selected entry's full name
	if (show_names && dir->selected >= 0 && dir->selected < total) {
		Entry* entry = dir->entries->items[dir->selected];
		char* label = entry->unique ? entry->unique : entry->name;
		trimSortingMeta(&label);
		drawTitlePill(label, ow);
	}
}

static void updatePillTextSurface(const char* entry_name, int move_w, SDL_Color text_color) {
	int crop_w = move_w - SCALE1(BUTTON_PADDING * 2);
	if (crop_w <= 0) return;

	SDL_LockMutex(fontMutex);
	SDL_Surface* tmp = TTF_RenderUTF8_Blended(font.large, entry_name, text_color);
	SDL_UnlockMutex(fontMutex);
	if (!tmp) return;

	SDL_Surface* converted = SDL_ConvertSurfaceFormat(tmp, screen->format->format, 0);
	SDL_FreeSurface(tmp);
	if (!converted) return;

	SDL_Rect crop_rect = { 0, 0, crop_w, converted->h };
	SDL_Surface* cropped = SDL_CreateRGBSurfaceWithFormat(
		0, crop_rect.w, crop_rect.h, screen->format->BitsPerPixel, screen->format->format
	);
	if (cropped) {
		SDL_BlitSurface(converted, &crop_rect, cropped, NULL);
	}
	SDL_FreeSurface(converted);
	if (!cropped) return;

	SDL_LockMutex(animMutex);
	if (globalText) SDL_FreeSurface(globalText);
	globalText = cropped;
	SDL_UnlockMutex(animMutex);
}
int folderbgchanged=0;
int thumbchanged=0;

// queue a new image load task :D
#define MAX_QUEUE_SIZE 1

int currentBGQueueSize = 0;
int currentThumbQueueSize = 0;
int currentAnimQueueSize = 0;

void enqueueBGTask(LoadBackgroundTask* task) {
	SDL_LockMutex(bgqueueMutex);
    TaskNode* node = (TaskNode*)malloc(sizeof(TaskNode));
    node->task = task;
    node->next = NULL;

    // If queue is full, drop the oldest task (head)
    if (currentBGQueueSize >= MAX_QUEUE_SIZE) {
        TaskNode* oldNode = taskBGQueueHead;
        if (oldNode) {
            taskBGQueueHead = oldNode->next;
            if (!taskBGQueueHead) {
                taskBGQueueTail = NULL;
            }
            if (oldNode->task) {
                free(oldNode->task);  // Only if task was malloc'd
            }
            free(oldNode);
            currentBGQueueSize--;
        }
    }

    // Enqueue the new task
    if (taskBGQueueTail) {
        taskBGQueueTail->next = node;
        taskBGQueueTail = node;
    } else {
        taskBGQueueHead = taskBGQueueTail = node;
    }

    currentBGQueueSize++;
    SDL_CondSignal(bgqueueCond);
    SDL_UnlockMutex(bgqueueMutex);
}
void enqueueThumbTask(LoadBackgroundTask* task) {
	SDL_LockMutex(thumbqueueMutex);
    TaskNode* node = (TaskNode*)malloc(sizeof(TaskNode));
    node->task = task;
    node->next = NULL;

    // If queue is full, drop the oldest task (head)
    if (currentThumbQueueSize >= MAX_QUEUE_SIZE) {
        TaskNode* oldNode = taskThumbQueueHead;
        if (oldNode) {
            taskThumbQueueHead = oldNode->next;
            if (!taskThumbQueueHead) {
                taskThumbQueueTail = NULL;
            }
            if (oldNode->task) {
                free(oldNode->task);  // Only if task was malloc'd
            }
            free(oldNode);
            currentThumbQueueSize--;
        }
    }

    // Enqueue the new task
    if (taskThumbQueueTail) {
        taskThumbQueueTail->next = node;
        taskThumbQueueTail = node;
    } else {
        taskThumbQueueHead = taskThumbQueueTail = node;
    }

    currentThumbQueueSize++;
    SDL_CondSignal(thumbqueueCond);
    SDL_UnlockMutex(thumbqueueMutex);
}

// Worker thread
int BGLoadWorker(void* unused) {
    while (!SDL_AtomicGet(&workerThreadsShutdown)) {
        SDL_LockMutex(bgqueueMutex);
        while (!taskBGQueueHead && !SDL_AtomicGet(&workerThreadsShutdown)) {
        	SDL_CondWait(bgqueueCond, bgqueueMutex);
        }
        if (SDL_AtomicGet(&workerThreadsShutdown)) {
            SDL_UnlockMutex(bgqueueMutex);
            break;
        }
        TaskNode* node = taskBGQueueHead;
        taskBGQueueHead = node->next;
        if (!taskBGQueueHead) taskBGQueueTail = NULL;
        SDL_UnlockMutex(bgqueueMutex);
		// give processor lil space in between queue items for other shit
		//SDL_Delay(100);
        LoadBackgroundTask* task = node->task;
        free(node);

        SDL_Surface* result = NULL;
        if (access(task->imagePath, F_OK) == 0) {
            SDL_Surface* image = IMG_Load(task->imagePath);
            if (image) {
                SDL_Surface* imageRGBA = SDL_ConvertSurfaceFormat(image, screen->format->format, 0);
                SDL_FreeSurface(image);
                result = imageRGBA;
            }
        }

        if (task->callback) {
			task->callback(result);
		}
        free(task);
		SDL_LockMutex(bgqueueMutex);
		if (!taskBGQueueHead) taskBGQueueTail = NULL;
		currentBGQueueSize--;  // <-- add this
		SDL_UnlockMutex(bgqueueMutex);
    }
    return 0;
}
int ThumbLoadWorker(void* unused) {
    while (!SDL_AtomicGet(&workerThreadsShutdown)) {
        SDL_LockMutex(thumbqueueMutex);
        while (!taskThumbQueueHead && !SDL_AtomicGet(&workerThreadsShutdown)) {
        	SDL_CondWait(thumbqueueCond, thumbqueueMutex);
        }
        if (SDL_AtomicGet(&workerThreadsShutdown)) {
            SDL_UnlockMutex(thumbqueueMutex);
            break;
        }
        TaskNode* node = taskThumbQueueHead;
        taskThumbQueueHead = node->next;
        if (!taskThumbQueueHead) taskThumbQueueTail = NULL;
        SDL_UnlockMutex(thumbqueueMutex);
		// give processor lil space in between queue items for other shit
		//SDL_Delay(100);
        LoadBackgroundTask* task = node->task;
        free(node);

        SDL_Surface* result = NULL;
        if (access(task->imagePath, F_OK) == 0) {
            SDL_Surface* image = IMG_Load(task->imagePath);
            if (image) {
                SDL_Surface* imageRGBA = SDL_ConvertSurfaceFormat(image, screen->format->format, 0);
                SDL_FreeSurface(image);
                result = imageRGBA;
            }
        }

        if (task->callback) {
			task->callback(result);
		}
        free(task);
		SDL_LockMutex(thumbqueueMutex);
		if (!taskThumbQueueHead) taskThumbQueueTail = NULL;
		currentThumbQueueSize--;  // <-- add this
		SDL_UnlockMutex(thumbqueueMutex);
    }
    return 0;
}

void startLoadFolderBackground(const char* imagePath, BackgroundLoadedCallback callback, void* userData) {
    LoadBackgroundTask* task = malloc(sizeof(LoadBackgroundTask));
    if (!task) return;

 	snprintf(task->imagePath, sizeof(task->imagePath), "%s", imagePath);
    task->callback = callback;
    task->userData = userData;
    enqueueBGTask(task);
}

void onBackgroundLoaded(SDL_Surface* surface) {
	SDL_LockMutex(bgMutex);
	folderbgchanged = 1;
	if (folderbgbmp) SDL_FreeSurface(folderbgbmp);
    if (!surface) {
		folderbgbmp = NULL;
		SDL_UnlockMutex(bgMutex);
		return;
	}
    folderbgbmp = surface;
	setNeedDraw(1);
	SDL_UnlockMutex(bgMutex);
}

void startLoadThumb(const char* thumbpath, BackgroundLoadedCallback callback, void* userData) {
    LoadBackgroundTask* task = malloc(sizeof(LoadBackgroundTask));
    if (!task) return;

    snprintf(task->imagePath, sizeof(task->imagePath), "%s", thumbpath);
    task->callback = callback;
    task->userData = userData;
    enqueueThumbTask(task);
}
void onThumbLoaded(SDL_Surface* surface) {
	SDL_LockMutex(thumbMutex);
	thumbchanged = 1;
	if (thumbbmp) SDL_FreeSurface(thumbbmp);
    if (!surface) {
		thumbbmp = NULL;
		SDL_UnlockMutex(thumbMutex);
		return;
	}


    thumbbmp = surface;
	int img_w = thumbbmp->w;
	int img_h = thumbbmp->h;
	double aspect_ratio = (double)img_h / img_w;
	int max_w = (int)(screen->w * CFG_getGameArtWidth());
	int max_h = (int)(screen->h * 0.6);
	int new_w = max_w;
	int new_h = (int)(new_w * aspect_ratio);

	if (new_h > max_h) {
		new_h = max_h;
		new_w = (int)(new_h / aspect_ratio);
	}

	GFX_ApplyRoundedCorners_8888(
		thumbbmp,
		&(SDL_Rect){0, 0, thumbbmp->w, thumbbmp->h},
		SCALE1((float)CFG_getThumbnailRadius() * ((float)img_w / (float)new_w))
	);
	setNeedDraw(1);
	SDL_UnlockMutex(thumbMutex);
}

SDL_Rect pillRect;
int pilltargetY =0;
int pilltargetTextY =0;
void animcallback(finishedTask *task) {
	SDL_LockMutex(animMutex);
	pillRect = task->dst;
	if(pillRect.w > 0 && pillRect.h > 0) {
		pilltargetY = +screen->w; // move offscreen
		if(task->done) {
			pilltargetY = task->targetY;
			pilltargetTextY = task->targetTextY;
		}
		setNeedDraw(1);
	}
	SDL_UnlockMutex(animMutex);
	setAnimationDraw(1);
}
bool frameReady = true;
bool pillanimdone = false;

int animWorker(void* unused) {
	  while (!SDL_AtomicGet(&workerThreadsShutdown)) {
 		SDL_LockMutex(animqueueMutex);
    while (!animTaskQueueHead && !SDL_AtomicGet(&workerThreadsShutdown)) {
        SDL_CondWait(animqueueCond, animqueueMutex);
    }
    if (SDL_AtomicGet(&workerThreadsShutdown)) {
        SDL_UnlockMutex(animqueueMutex);
        break;
    }
    AnimTaskNode* node = animTaskQueueHead;
    animTaskQueueHead = node->next;
    if (!animTaskQueueHead) animTtaskQueueTail = NULL;
		SDL_UnlockMutex(animqueueMutex);

    AnimTask* task = node->task;
		finishedTask* finaltask = (finishedTask*)malloc(sizeof(finishedTask));
		int total_frames = task->frames;
		for (int frame = 0; frame <= total_frames; frame++) {
			// Check for shutdown at start of each frame
			if (SDL_AtomicGet(&workerThreadsShutdown)) break;

			float t = (float)frame / total_frames;
			if (t > 1.0f) t = 1.0f;

			int current_x = task->startX + (int)((task->targetX - task->startX) * t);
			int current_y = task->startY + (int)(( task->targetY -  task->startY) * t);

			SDL_Rect moveDst = { current_x, current_y, task->move_w, task->move_h };
			finaltask->dst = moveDst;
			finaltask->entry_name = task->entry_name;
			finaltask->move_w = task->move_w;
			finaltask->move_h = task->move_h;
			finaltask->targetY = task->targetY;
			finaltask->targetTextY = task->targetTextY;
			finaltask->move_y = SCALE1(PADDING + task->targetY) + (task->targetTextY - task->targetY);
			finaltask->done = 0;
			if(frame >= total_frames) finaltask->done=1;
			task->callback(finaltask);
			SDL_LockMutex(frameMutex);
			while (!frameReady && !SDL_AtomicGet(&workerThreadsShutdown)) {
				SDL_CondWait(flipCond, frameMutex);
			}
			frameReady = false;
			SDL_UnlockMutex(frameMutex);

		}
		SDL_LockMutex(animqueueMutex);
		if (!animTaskQueueHead) animTtaskQueueTail = NULL;
		currentAnimQueueSize--;  // <-- add this
		SDL_UnlockMutex(animqueueMutex);

		SDL_LockMutex(animMutex);
		pillanimdone = true;
		free(finaltask);
		SDL_UnlockMutex(animMutex);
	}
}

void enqueueanmimtask(AnimTask* task) {
    AnimTaskNode* node = (AnimTaskNode*)malloc(sizeof(AnimTaskNode));
    node->task = task;
    node->next = NULL;

    SDL_LockMutex(animqueueMutex);
	pillanimdone = false;
    // If queue is full, drop the oldest task (head)
    if (currentAnimQueueSize >= 1) {
        AnimTaskNode* oldNode = animTaskQueueHead;
        if (oldNode) {
            animTaskQueueHead = oldNode->next;
            if (!animTaskQueueHead) {
                animTtaskQueueTail = NULL;
            }
            if (oldNode->task) {
                free(oldNode->task);  // Only if task was malloc'd
            }
            free(oldNode);
            currentAnimQueueSize--;
        }
    }

    // Enqueue the new task
    if (animTtaskQueueTail) {
        animTtaskQueueTail->next = node;
        animTtaskQueueTail = node;
    } else {
        animTaskQueueHead = animTtaskQueueTail = node;
    }

    currentAnimQueueSize++;
    SDL_CondSignal(animqueueCond);
    SDL_UnlockMutex(animqueueMutex);
}

void animPill(AnimTask *task) {
	task->callback = animcallback;
	enqueueanmimtask(task);
}

void initImageLoaderPool() {
	// Initialize shutdown flag to 0
	SDL_AtomicSet(&workerThreadsShutdown, 0);
	SDL_AtomicSet(&animationDrawAtomic, 1);
	SDL_AtomicSet(&needDrawAtomic, 0);

    thumbqueueMutex = SDL_CreateMutex();
    bgqueueMutex = SDL_CreateMutex();
    bgqueueCond = SDL_CreateCond();
    thumbqueueCond = SDL_CreateCond();
	bgMutex = SDL_CreateMutex();
	thumbMutex = SDL_CreateMutex();
	animMutex = SDL_CreateMutex();
	animqueueMutex = SDL_CreateMutex();
	animqueueCond = SDL_CreateCond();
	frameMutex = SDL_CreateMutex();
	fontMutex = SDL_CreateMutex();
	flipCond = SDL_CreateCond();

    bgLoadThread = SDL_CreateThread(BGLoadWorker, "BGLoadWorker", NULL);
    thumbLoadThread = SDL_CreateThread(ThumbLoadWorker, "ThumbLoadWorker", NULL);
	animWorkerThread = SDL_CreateThread(animWorker, "animWorker", NULL);
}

void cleanupImageLoaderPool() {
	// Signal all worker threads to exit (atomic set for thread safety)
	SDL_AtomicSet(&workerThreadsShutdown, 1);

	// Wake up all waiting threads
	if (bgqueueCond) SDL_CondSignal(bgqueueCond);
	if (thumbqueueCond) SDL_CondSignal(thumbqueueCond);
	if (animqueueCond) SDL_CondSignal(animqueueCond);
	if (flipCond) SDL_CondSignal(flipCond);  // Wake up animWorker if stuck waiting for frame flip

	// Wait for all worker threads to finish
	if (bgLoadThread) {
		SDL_WaitThread(bgLoadThread, NULL);
		bgLoadThread = NULL;
	}
	if (thumbLoadThread) {
		SDL_WaitThread(thumbLoadThread, NULL);
		thumbLoadThread = NULL;
	}
	if (animWorkerThread) {
		SDL_WaitThread(animWorkerThread, NULL);
		animWorkerThread = NULL;
	}

	// Small delay to ensure llvmpipe/OpenGL threads have completed any pending operations
	SDL_Delay(10);

	// Acquire and release each mutex before destroying to ensure no thread is in a critical section
	// This creates a memory barrier and ensures proper synchronization
	if (bgqueueMutex) { SDL_LockMutex(bgqueueMutex); SDL_UnlockMutex(bgqueueMutex); }
	if (thumbqueueMutex) { SDL_LockMutex(thumbqueueMutex); SDL_UnlockMutex(thumbqueueMutex); }
	if (animqueueMutex) { SDL_LockMutex(animqueueMutex); SDL_UnlockMutex(animqueueMutex); }
	if (bgMutex) { SDL_LockMutex(bgMutex); SDL_UnlockMutex(bgMutex); }
	if (thumbMutex) { SDL_LockMutex(thumbMutex); SDL_UnlockMutex(thumbMutex); }
	if (animMutex) { SDL_LockMutex(animMutex); SDL_UnlockMutex(animMutex); }
	if (frameMutex) { SDL_LockMutex(frameMutex); SDL_UnlockMutex(frameMutex); }
	if (fontMutex) { SDL_LockMutex(fontMutex); SDL_UnlockMutex(fontMutex); }

	// Destroy mutexes and condition variables
	if (bgqueueMutex) SDL_DestroyMutex(bgqueueMutex);
	if (thumbqueueMutex) SDL_DestroyMutex(thumbqueueMutex);
	if (animqueueMutex) SDL_DestroyMutex(animqueueMutex);
	if (bgMutex) SDL_DestroyMutex(bgMutex);
	if (thumbMutex) SDL_DestroyMutex(thumbMutex);
	if (animMutex) SDL_DestroyMutex(animMutex);
	if (frameMutex) SDL_DestroyMutex(frameMutex);
	if (fontMutex) SDL_DestroyMutex(fontMutex);

	if (bgqueueCond) SDL_DestroyCond(bgqueueCond);
	if (thumbqueueCond) SDL_DestroyCond(thumbqueueCond);
	if (animqueueCond) SDL_DestroyCond(animqueueCond);
	if (flipCond) SDL_DestroyCond(flipCond);

	// Set pointers to NULL after destruction
	bgqueueMutex = NULL;
	thumbqueueMutex = NULL;
	animqueueMutex = NULL;
	bgMutex = NULL;
	thumbMutex = NULL;
	animMutex = NULL;
	frameMutex = NULL;
	fontMutex = NULL;
	bgqueueCond = NULL;
	thumbqueueCond = NULL;
	animqueueCond = NULL;
	flipCond = NULL;
}
///////////////////////////////////////

int main (int argc, char *argv[]) {
	// LOG_info("time from launch to:\n");
	// unsigned long main_begin = SDL_GetTicks();
	// unsigned long first_draw = 0;

	if (autoResume()) return 0; // nothing to do

	simple_mode = exists(SIMPLE_MODE_PATH);

	LOG_info("NextUI\n");
	InitSettings();

	screen = GFX_init(MODE_MAIN);
// LOG_info("- graphics init: %lu\n", SDL_GetTicks() - main_begin);

	PAD_init();
	// LOG_info("- input init: %lu\n", SDL_GetTicks() - main_begin);
	VIB_init();
	PWR_init();
	if (!HAS_POWER_BUTTON && !simple_mode) PWR_disableSleep();
	// LOG_info("- power init: %lu\n", SDL_GetTicks() - main_begin);

	// start my threaded image loader :D
	initImageLoaderPool();
	Menu_init();
	int qm_row = 0;
	int qm_col = 0;
	int qm_slot = 0;
	int qm_shift = 0;
	int qm_slots = QUICK_SWITCHER_COUNT > quick->count ? quick->count : QUICK_SWITCHER_COUNT;
	// LOG_info("- menu init: %lu\n", SDL_GetTicks() - main_begin);

	int lastScreen = SCREEN_OFF;
	int currentScreen = CFG_getDefaultView();

	if(exists(GAME_SWITCHER_PERSIST_PATH)) {
		// consider this "consumed", dont bring up the switcher next time we regularly exit a game
		unlink(GAME_SWITCHER_PERSIST_PATH);
		currentScreen = SCREEN_GAMESWITCHER;
	}

	// add a nice fade into the game switcher
	if(currentScreen == SCREEN_GAMESWITCHER)
		lastScreen = SCREEN_GAME;

	// make sure we have no running games logged as active anymore (we might be launching back into the UI here)
	system("gametimectl.elf stop_all");

	GFX_setVsync(VSYNC_STRICT);
	PWR_setCPUSpeed(CPU_SPEED_AUTO);

	PAD_reset();
	GFX_clearLayers(LAYER_ALL);
	GFX_clear(screen);

	int show_setting = 0; // 1=brightness,2=volume
	int was_online = PWR_isOnline();
    int had_bt = PLAT_btIsConnected();
	int had_sink = GetAudioSink();

	int selected_row = top->selected - top->start;
	float targetY;
	float previousY;
	int is_scrolling = 0;
	bool list_show_entry_names = true;

	char folderBgPath[1024];
	folderbgbmp = NULL;

	SDL_Surface * blackBG = SDL_CreateRGBSurfaceWithFormat(0,screen->w,screen->h,screen->format->BitsPerPixel,screen->format->format);
	SDL_FillRect(blackBG,NULL,SDL_MapRGBA(screen->format,0,0,0,255));

	SDL_LockMutex(animMutex);
	globalpill = SDL_CreateRGBSurfaceWithFormat(SDL_SWSURFACE, screen->w, SCALE1(PILL_SIZE), FIXED_DEPTH, screen->format->format);
	globalText = SDL_CreateRGBSurfaceWithFormat(SDL_SWSURFACE, screen->w, SCALE1(PILL_SIZE), FIXED_DEPTH, screen->format->format);
	static int globallpillW = 0;
	SDL_UnlockMutex(animMutex);

	//LOG_info("Start time time %ims\n",SDL_GetTicks());
	while (!quit) {
		GFX_startFrame();
		unsigned long now = SDL_GetTicks();

		PAD_poll();

		int selected = top->selected;
		int total = top->entries->count;

		PWR_update(&dirty, &show_setting, NULL, NULL);

		// user-assignable buttons, handled before the per-screen input so they work
		// from the game list, the game switcher and the quick menu alike.
		// skipped while the brightness/volume overlay owns input, same as L1/R1 below.
		if (!show_setting) {
			if (PAD_justPressed(BTN_FN1)) runFnAction(0);
			else if (PAD_justPressed(BTN_FN2)) runFnAction(1);
			else if (PAD_justPressed(BTN_FN3)) runFnAction(2);
		}

		int is_online = PWR_isOnline();
		if (was_online!=is_online)
			dirty = 1;
		was_online = is_online;

        int has_bt = PLAT_btIsConnected();
        if (had_bt != has_bt)
            dirty = 1;
        had_bt = has_bt;

		int has_sink = GetAudioSink();
		if (had_sink != has_sink)
			dirty = 1;
		had_sink = has_sink;

		int gsanimdir = ANIM_NONE;

		if (currentScreen == SCREEN_QUICKMENU) {
			int qm_total = qm_row == 0 ? quick->count : quickActions->count;

			if (PAD_justPressed(BTN_B) || PAD_tappedMenu(now)) {
				currentScreen = SCREEN_GAMELIST;
				folderbgchanged = 1; // The background painting code is a clusterfuck, just force a repaint here
				dirty = 1;
			}
			else if (PAD_justReleased(BTN_A)) {
				Entry *selected = qm_row == 0 ? quick->items[qm_col] : quickActions->items[qm_col];
				if(selected->type != ENTRY_DIP) {
					currentScreen = SCREEN_GAMELIST;
					total = top->entries->count;
					// prevent restoring list state, game list screen currently isnt our nav origin
					top->selected = 0;
					top->start = 0;
					top->end = top->start + MAIN_ROW_COUNT;
					restore_depth = -1;
					restore_relative = -1;
					restore_selected = 0;
					restore_start = 0;
					restore_end = 0;
				}
				Entry_open(selected);
				if (top->entries->count > 0) readyResume(top->entries->items[top->selected]);
				dirty = 1;
			}
			else if (PAD_justPressed(BTN_RIGHT)) {
				if(qm_row == 0 && qm_total > qm_slots) {
					qm_col++;
					if(qm_col >= qm_total) {
						qm_col = 0;
						qm_shift = 0;
						qm_slot = 0;
					}
					else {
						qm_slot++;
						if(qm_slot >= qm_slots) {
							qm_slot = qm_slots - 1;
							qm_shift++;
						}
					}
				}
				else {
					qm_col += 1;
					if(qm_col >= qm_total) {
						qm_col = 0;
					}
				}
				dirty = 1;
			}
			else if (PAD_justPressed(BTN_LEFT)) {
				if(qm_row == 0  && qm_total > qm_slots) {
					qm_col -= 1;
					if(qm_col < 0) {
						qm_col = qm_total - 1;
						qm_shift = qm_total - qm_slots;
						qm_slot = qm_slots - 1;
					}
					else {
						qm_slot--;
						if(qm_slot < 0) {
							qm_slot = 0;
							qm_shift--;
						}
					}
				}
				else {
					qm_col -= 1;
					if(qm_col < 0) {
						qm_col = qm_total - 1;
					}
				}
				dirty = 1;
			}
			else if(PAD_justPressed(BTN_DOWN)) {
				if(qm_row == 0) {
					qm_row = 1;
					qm_col = 0;
					dirty = 1;
				}
			}
			else if(PAD_justPressed(BTN_UP)) {
				if(qm_row == 1) {
					qm_row = 0;
					qm_col = qm_slot + qm_shift;
					dirty = 1;
				}
			}
		}
		else if(currentScreen == SCREEN_GAMESWITCHER) {
			if (PAD_justPressed(BTN_B) || PAD_tappedSelect(now)) {
				currentScreen = SCREEN_GAMELIST;
				switcher_selected = 0;
				dirty = 1;
				folderbgchanged = 1; // The background painting code is a clusterfuck, just force a repaint here
			}
			else if (recents->count > 0 && PAD_justReleased(BTN_A)) {
				// this will drop us back into game switcher after leaving the game
				putFile(GAME_SWITCHER_PERSIST_PATH, "unused");
				startgame = 1;
				Entry *selectedEntry = entryFromRecent(recents->items[switcher_selected]);
				should_resume = can_resume;
				Entry_open(selectedEntry);
				dirty = 1;
				Entry_free(selectedEntry);
			}
			else if (recents->count > 0 && PAD_justReleased(BTN_Y)) {
				// remove
				Recent* recentEntry = recents->items[switcher_selected--];
				Array_remove(recents, recentEntry);
				Recent_free(recentEntry);
				saveRecents();
				if(switcher_selected < 0)
					switcher_selected = recents->count - 1; // wrap
				dirty = 1;
			}
			else if (PAD_justPressed(BTN_RIGHT)) {
				switcher_selected++;
				if(switcher_selected == recents->count)
					switcher_selected = 0; // wrap
				dirty = 1;
				gsanimdir = SLIDE_LEFT;
			}
			else if (PAD_justPressed(BTN_LEFT)) {
				switcher_selected--;
				if(switcher_selected < 0)
					switcher_selected = recents->count - 1; // wrap
				dirty = 1;
				gsanimdir = SLIDE_RIGHT;
			}
		}
		else {
			if (PAD_tappedMenu(now)) {
				currentScreen = SCREEN_QUICKMENU;
				qm_col = 0;
				qm_row = 0;
				qm_shift = 0;
				qm_slot = 0;
				dirty = 1;
				folderbgchanged = 1; // The background painting code is a clusterfuck, just force a repaint here
				if (!HAS_POWER_BUTTON && !simple_mode) PWR_enableSleep();
			}
			else if (PAD_tappedSelect(now)) {
				currentScreen = SCREEN_GAMESWITCHER;
				switcher_selected = 0;
				dirty = 1;
			}
			else if (total>0 && Grid_active()) {
				GridLayout grid;
				Grid_getLayout(&grid);
				if (PAD_justRepeated(BTN_UP))
					selected = Grid_move(selected, total, grid.rows, 0, -1, PAD_justPressed(BTN_UP));
				else if (PAD_justRepeated(BTN_DOWN))
					selected = Grid_move(selected, total, grid.rows, 0, 1, PAD_justPressed(BTN_DOWN));
				else if (PAD_justRepeated(BTN_LEFT))
					selected = Grid_move(selected, total, grid.rows, -1, 0, PAD_justPressed(BTN_LEFT));
				else if (PAD_justRepeated(BTN_RIGHT))
					selected = Grid_move(selected, total, grid.rows, 1, 0, PAD_justPressed(BTN_RIGHT));
			}
			else if (total>0) {
				if (PAD_justRepeated(BTN_UP)) {
					if (selected==0 && !PAD_justPressed(BTN_UP)) {
						// stop at top
					}
					else {
						selected -= 1;
						if (selected<0) {
							selected = total-1;
							int start = total - MAIN_ROW_COUNT;
							top->start = (start<0) ? 0 : start;
							top->end = total;
						}
						else if (selected<top->start) {
							top->start -= 1;
							top->end -= 1;
						}
					}
				}
				else if (PAD_justRepeated(BTN_DOWN)) {
					if (selected==total-1 && !PAD_justPressed(BTN_DOWN)) {
						// stop at bottom
					}
					else {
						selected += 1;
						if (selected>=total) {
							selected = 0;
							top->start = 0;
							top->end = (total<MAIN_ROW_COUNT) ? total : MAIN_ROW_COUNT;
						}
						else if (selected>=top->end) {
							top->start += 1;
							top->end += 1;
						}
					}
				}
				if (PAD_justRepeated(BTN_LEFT)) {
					selected -= MAIN_ROW_COUNT;
					if (selected<0) {
						selected = 0;
						top->start = 0;
						top->end = (total<MAIN_ROW_COUNT) ? total : MAIN_ROW_COUNT;
					}
					else if (selected<top->start) {
						top->start -= MAIN_ROW_COUNT;
						if (top->start<0) top->start = 0;
						top->end = top->start + MAIN_ROW_COUNT;
					}
				}
				else if (PAD_justRepeated(BTN_RIGHT)) {
					selected += MAIN_ROW_COUNT;
					if (selected>=total) {
						selected = total-1;
						int start = total - MAIN_ROW_COUNT;
						top->start = (start<0) ? 0 : start;
						top->end = total;
					}
					else if (selected>=top->end) {
						top->end += MAIN_ROW_COUNT;
						if (top->end>total) top->end = total;
						top->start = top->end - MAIN_ROW_COUNT;
					}
				}
			}

			if (PAD_justRepeated(BTN_L1) && !PAD_isPressed(BTN_R1) && !PWR_ignoreSettingInput(BTN_L1, show_setting)) { // previous alpha
				Entry* entry = top->entries->items[selected];
				int i = entry->alpha-1;
				if (i>=0) {
					selected = top->alphas->items[i];
					if (total>MAIN_ROW_COUNT) {
						top->start = selected;
						top->end = top->start + MAIN_ROW_COUNT;
						if (top->end>total) top->end = total;
						top->start = top->end - MAIN_ROW_COUNT;
					}
				}
			}
			else if (PAD_justRepeated(BTN_R1) && !PAD_isPressed(BTN_L1) && !PWR_ignoreSettingInput(BTN_R1, show_setting)) { // next alpha
				Entry* entry = top->entries->items[selected];
				int i = entry->alpha+1;
				if (i<top->alphas->count) {
					selected = top->alphas->items[i];
					if (total>MAIN_ROW_COUNT) {
						top->start = selected;
						top->end = top->start + MAIN_ROW_COUNT;
						if (top->end>total) top->end = total;
						top->start = top->end - MAIN_ROW_COUNT;
					}
				}
			}

			if (selected!=top->selected) {
				top->selected = selected;
				dirty = 1;
			}

			Entry* entry = top->entries->items[top->selected];

			if (dirty && total>0)
				readyResume(entry);

			if (total>0 && can_resume && PAD_justReleased(BTN_RESUME)) {
				should_resume = 1;
				Entry_open(entry);

				dirty = 1;
			}
			else if (total>0 && PAD_justPressed(BTN_A)) {
				Entry_open(entry);
				if(entry->type == ENTRY_DIR) {
					animationdirection = SLIDE_LEFT;
					total = top->entries->count;
				}
				dirty = 1;

				if (total>0) readyResume(top->entries->items[top->selected]);
			}
			else if (PAD_justPressed(BTN_B) && stack->count>1) {
				closeDirectory();
				animationdirection = SLIDE_RIGHT;
				total = top->entries->count;
				dirty = 1;

				if (total>0) readyResume(top->entries->items[top->selected]);
			}
		}

		if(dirty) {
			SDL_Surface *tmpOldScreen = NULL;
			SDL_Surface * switcherSur = NULL;
			// NOTE:22 This causes slowdown when CFG_getMenuTransitions is set to false because animationdirection turns > 0 somewhere but is never set back to 0 and so this code runs on every action, will fix later
			if(animationdirection != ANIM_NONE || (lastScreen==SCREEN_GAMELIST && currentScreen == SCREEN_GAMESWITCHER)) {
				if(tmpOldScreen) SDL_FreeSurface(tmpOldScreen);
				tmpOldScreen = GFX_captureRendererToSurface();
				SDL_SetSurfaceBlendMode(tmpOldScreen,SDL_BLENDMODE_BLEND);
			}

			// clear only background layer on start
			if(lastScreen==SCREEN_GAME || lastScreen==SCREEN_OFF) {
				GFX_clearLayers(LAYER_ALL);
			}
			else {
				GFX_clearLayers(LAYER_TRANSITION);
				if(lastScreen!=SCREEN_GAMELIST)
					GFX_clearLayers(LAYER_THUMBNAIL);
				GFX_clearLayers(LAYER_SCROLLTEXT);
				GFX_clearLayers(LAYER_IDK2);
			}
			GFX_clear(screen);

			if(CFG_getGameSwitcherCurtain() > 0 && currentScreen == SCREEN_GAMESWITCHER) {
				GFX_blitTopCurtain(screen);
				GFX_blitBottomCurtain(screen);
			}

			int ow = GFX_blitHardwareGroup(screen, show_setting);
			if (currentScreen == SCREEN_QUICKMENU) {
				if(lastScreen != SCREEN_QUICKMENU) {
					GFX_clearLayers(LAYER_BACKGROUND);
					GFX_clearLayers(LAYER_THUMBNAIL);
				}

				Entry *current = qm_row == 0 ? quick->items[qm_col] : quickActions->items[qm_col];
				char newBgPath[MAX_PATH];
				char fallbackBgPath[MAX_PATH];

				sprintf(newBgPath, SDCARD_PATH "/.media/quick_%s%s.png", current->name, 
					!strcmp(current->name,"Wifi") && !CFG_getWifi() || 							// wifi or wifi_off, based on state
					!strcmp(current->name,"Bluetooth") && !CFG_getBluetooth() ? "_off" : "");	// bluetooth or bluetooth_off, based on state
				sprintf(fallbackBgPath, SDCARD_PATH "/.media/quick.png");

				// background
				if(!exists(newBgPath))
					strncpy(newBgPath, fallbackBgPath, sizeof(newBgPath) - 1);

				if(strcmp(newBgPath, folderBgPath) != 0) {
					strncpy(folderBgPath, newBgPath, sizeof(folderBgPath) - 1);
					startLoadFolderBackground(newBgPath, onBackgroundLoaded, NULL);
				}

				// buttons (duped and trimmed from below)
				if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
				else GFX_blitButtonGroup((char*[]){ BTN_SLEEP==BTN_POWER?"POWER":"MENU","SLEEP",  NULL }, 0, screen, 0);

				GFX_blitButtonGroup((char*[]){ "B","BACK", "A","OPEN", NULL }, 1, screen, 1);

				if(CFG_getShowQuickswitcherUI()) {
					// the bottom row is icons only, so name the selected item in the top row
					if (CFG_getShowQuickMenuTitle())
						drawTitlePill(QuickMenu_label(current), ow);

					#define MENU_ITEM_SIZE 72 // item size, top line
					#define MENU_MARGIN_Y 32 // space between main UI elements and quick menu
					#define MENU_MARGIN_X 40 // space between main UI elements and quick menu
					#define MENU_ITEM_MARGIN 18 // space between items, top line
					#define MENU_TOGGLE_MARGIN 8 // space between items, bottom line
					#define MENU_LINE_MARGIN 8 // space between top and bottom line

					// this is flexible, not sure I like it at smaller scales than 3 though
					//int item_size = screen->h - SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN + // top pill area
					//	MENU_MARGIN_Y + MENU_LINE_MARGIN + PILL_SIZE + MENU_MARGIN_Y + // our own area
					//	BUTTON_MARGIN + PILL_SIZE + PADDING); // bottom pill area

					int item_space_y = screen->h - SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN + // top pill area
						MENU_MARGIN_Y + MENU_LINE_MARGIN + PILL_SIZE + MENU_MARGIN_Y + // our own area
						BUTTON_MARGIN + PILL_SIZE + PADDING);
					int item_size = SCALE1(MENU_ITEM_SIZE);
					int item_extra_y = item_space_y - item_size;
					int item_space_x = screen->w - SCALE1(PADDING + MENU_MARGIN_X + MENU_MARGIN_X + PADDING);
					// extra left margin for the first item in order to properly center all of them in the
					// available space
					int item_inset_x = (item_space_x - SCALE1(qm_slots * MENU_ITEM_SIZE + (qm_slots - 1) * MENU_ITEM_MARGIN)) / 2;

					// primary
					ox = SCALE1(PADDING + MENU_MARGIN_X) + item_inset_x;
					oy = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN + MENU_MARGIN_Y) + item_extra_y / 2;
					// just to keep selection visible.
					// every display should be able to fit three items, we shift horizontally to accomodate.
					ox -= qm_shift * (item_size + SCALE1(MENU_ITEM_MARGIN));
					for (int c = 0; c < quick->count; c++)
					{
						SDL_Rect item_rect = {ox, oy, item_size, item_size};
						Entry *item = quick->items[c];

						SDL_Color text_color = uintToColour(THEME_COLOR4_255);
						uint32_t item_color = THEME_COLOR3;
						uint32_t icon_color = THEME_COLOR4;

						if(qm_row == 0 && qm_col == c) {
							text_color = uintToColour(THEME_COLOR5_255);
							item_color = THEME_COLOR1;
							icon_color = THEME_COLOR5;
						}

						GFX_blitRectColor(ASSET_STATE_BG, screen, &item_rect, item_color);

						char icon_path[MAX_PATH];
						sprintf(icon_path, SDCARD_PATH "/.system/res/%s@%ix.png", item->name, FIXED_SCALE);
						SDL_Surface* bmp = IMG_Load(icon_path);
						if(bmp) {
							SDL_Surface* converted = SDL_ConvertSurfaceFormat(bmp, screen->format->format, 0);
							if (converted) {
								SDL_FreeSurface(bmp);
								bmp = converted;
							}
						}
						if(bmp) {
							// Calculate the position to center the source surface
							int x = (item_rect.w - bmp->w) / 2;
							int y = (item_rect.h - SCALE1(FONT_TINY + BUTTON_MARGIN) - bmp->h) / 2;
							SDL_Rect destRect = { ox+x, oy+y, 0, 0 };  // width/height not required
							//SDL_BlitSurface(bmp, NULL, screen, &destRect);

							GFX_blitSurfaceColor(bmp, NULL, screen, &destRect, icon_color);
						}

						int w, h;
						GFX_sizeText(font.tiny, item->name, SCALE1(FONT_TINY), &w, &h);
						SDL_Rect text_rect = {item_rect.x + (item_size - w) / 2, item_rect.y + item_size - h - SCALE1(BUTTON_MARGIN), w, h};
						GFX_blitText(font.tiny, item->name, SCALE1(FONT_TINY), text_color, screen, &text_rect);

						ox += item_rect.w + SCALE1(MENU_ITEM_MARGIN);
					}

					// secondary
					ox = SCALE1(PADDING + MENU_MARGIN_X);
					ox += (screen->w - SCALE1(PADDING + MENU_MARGIN_X + MENU_MARGIN_X + PADDING) - SCALE1(quickActions->count * PILL_SIZE) - SCALE1((quickActions->count - 1) * MENU_TOGGLE_MARGIN))/2;
					oy = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN + MENU_MARGIN_Y + MENU_LINE_MARGIN) + item_size + item_extra_y / 2;
					for (int c = 0; c < quickActions->count; c++) {
						SDL_Rect item_rect = {ox, oy, SCALE1(PILL_SIZE), SCALE1(PILL_SIZE)};
						Entry *item = quickActions->items[c];

						SDL_Color text_color = uintToColour(THEME_COLOR4_255);
						uint32_t item_color = THEME_COLOR3;
						uint32_t icon_color = THEME_COLOR4;

						if(qm_row == 1 && qm_col == c) {
							text_color = uintToColour(THEME_COLOR5_255);
							item_color = THEME_COLOR1;
							icon_color = THEME_COLOR5;
						}

						GFX_blitPillColor(ASSET_WHITE_PILL, screen, &item_rect, item_color, RGB_WHITE);

						int asset = ASSET_WIFI_OFF;
						if (!strcmp(item->name,"Wifi"))
							asset = CFG_getWifi() ? ASSET_WIFI : ASSET_WIFI_OFF;
						else if (!strcmp(item->name,"Bluetooth"))
							asset = CFG_getBluetooth() ? ASSET_BLUETOOTH : ASSET_BLUETOOTH_OFF;
						else if (!strcmp(item->name,"Sleep"))
							asset = ASSET_SUSPEND;
						else if (!strcmp(item->name,"Reboot"))
							asset = ASSET_RESTART;
						else if (!strcmp(item->name,"Poweroff"))
							asset = ASSET_POWEROFF;
						else if (!strcmp(item->name,"Settings"))
							asset = ASSET_SETTINGS;
						else if (!strcmp(item->name,"Pak Store"))
							asset = ASSET_STORE;

						SDL_Rect rect;
						GFX_assetRect(asset, &rect);
						int x = item_rect.x;
						int y = item_rect.y;
						x += (SCALE1(PILL_SIZE) - rect.w) / 2;
						y += (SCALE1(PILL_SIZE) - rect.h) / 2;

						GFX_blitAssetColor(asset, NULL, screen, &(SDL_Rect){x,y}, icon_color);

						ox += item_rect.w + SCALE1(MENU_TOGGLE_MARGIN);
					}
				}
				lastScreen = SCREEN_QUICKMENU;
			}
			else if(startgame) {
				//pilltargetY = +screen->w;
				//animationdirection = ANIM_NONE;
				GFX_clearLayers(LAYER_ALL);
				GFX_clear(screen);
				GFX_flipHidden();
				GFX_animateSurfaceOpacity(tmpOldScreen,0,0,screen->w,screen->h,255,0,CFG_getMenuTransitions() ? 150:20,LAYER_BACKGROUND);
			}
			else if(currentScreen == SCREEN_GAMESWITCHER) {
				GFX_clearLayers(LAYER_ALL);
				ox = 0;
				oy = 0;

				// For all recents with resumable state (i.e. has savegame), show game switcher carousel
				if(recents->count > 0) {
					Entry *selectedEntry = entryFromRecent(recents->items[switcher_selected]);
					readyResume(selectedEntry);
					// title pill
					{
						int max_width = screen->w - SCALE1(PADDING * 2) - ow;

						char display_name[256];
						int text_width = GFX_truncateText(font.large, selectedEntry->name, display_name, max_width, SCALE1(BUTTON_PADDING*2));
						max_width = MIN(max_width, text_width);

						SDL_Surface* text;
						SDL_Color textColor = uintToColour(THEME_COLOR6_255);
						SDL_LockMutex(fontMutex);
						text = TTF_RenderUTF8_Blended(font.large, display_name, textColor);
						SDL_UnlockMutex(fontMutex);
						const int text_offset_y = (SCALE1(PILL_SIZE) - text->h + 1) >> 1;
						GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){
							SCALE1(PADDING),
							SCALE1(PADDING),
							max_width,
							SCALE1(PILL_SIZE)
						});
						SDL_BlitSurface(text, &(SDL_Rect){
							0,
							0,
							max_width-SCALE1(BUTTON_PADDING*2),
							text->h
						}, screen, &(SDL_Rect){
							SCALE1(PADDING+BUTTON_PADDING),
							SCALE1(PADDING) + text_offset_y,
						});
						SDL_FreeSurface(text);
					}

					GFX_blitButtonGroup((char*[]){ "B","BACK",  NULL }, 0, screen, 0);

					if(can_resume) GFX_blitButtonGroup((char*[]){ "Y", "REMOVE", "A","RESUME", NULL }, 1, screen, 1);
					else GFX_blitButtonGroup((char*[]){ "A","OPEN", NULL }, 1, screen, 1);

					if(has_preview) {
						// lotta memory churn here

						SDL_Surface* bmp = IMG_Load(preview_path);
						SDL_Surface* raw_preview = SDL_ConvertSurfaceFormat(bmp, screen->format->format, 0);
						if (raw_preview) {
							SDL_FreeSurface(bmp);
							bmp = raw_preview;
						}
						if(bmp) {
							int aw = screen->w;
							int ah = screen->h;
							int ax = 0;
							int ay = 0;

							float aspectRatio = (float)bmp->w / (float)bmp->h;
							float screenRatio = (float)screen->w / (float)screen->h;

							if (screenRatio > aspectRatio) {
								aw = (int)(screen->h * aspectRatio);
								ah = screen->h;
							} else {
								aw = screen->w;
								ah = (int)(screen->w / aspectRatio);
							}
							ax = (screen->w - aw) / 2;
							ay = (screen->h - ah) / 2;

							if(lastScreen == SCREEN_GAME) {
								// need to flip once so streaming_texture1 is updated
								GFX_flipHidden();
								GFX_animateSurfaceOpacity(bmp,0,0,screen->w,screen->h,0,255,CFG_getMenuTransitions() ? 150:20,LAYER_ALL);
							} else if(lastScreen == SCREEN_GAMELIST) {

								GFX_drawOnLayer(blackBG,0,0,screen->w,screen->h,1.0f,0,LAYER_BACKGROUND);
								GFX_drawOnLayer(bmp,ax,ay,aw, ah,1.0f,0,LAYER_BACKGROUND);
								GFX_flipHidden();
								SDL_Surface *tmpNewScreen = GFX_captureRendererToSurface();
								GFX_clearLayers(LAYER_ALL);
								folderbgchanged=1;
								GFX_drawOnLayer(tmpOldScreen,0,0,screen->w, screen->h,1.0f,0,LAYER_ALL);
								GFX_animateSurface(tmpNewScreen,0,0-screen->h,0,0,screen->w,screen->h,CFG_getMenuTransitions() ? 100:20,255,255,LAYER_BACKGROUND);
								SDL_FreeSurface(tmpNewScreen);

							} else if(lastScreen == SCREEN_GAMESWITCHER) {
								GFX_flipHidden();
								GFX_drawOnLayer(blackBG,0,0,screen->w, screen->h,1.0f,0,LAYER_BACKGROUND);
								if(gsanimdir == SLIDE_LEFT)
									GFX_animateSurface(bmp,ax+screen->w,ay,ax,ay,aw,ah,CFG_getMenuTransitions() ? 80:20,0,255,LAYER_ALL);
								else if(gsanimdir == SLIDE_RIGHT)
									GFX_animateSurface(bmp,ax-screen->w,ay,ax,ay,aw,ah,CFG_getMenuTransitions() ? 80:20,0,255,LAYER_ALL);

								GFX_drawOnLayer(bmp,ax,ay,aw,ah,1.0f,0,LAYER_BACKGROUND);
							} else if(lastScreen == SCREEN_QUICKMENU) {
								GFX_flipHidden();
								GFX_drawOnLayer(blackBG,0,0,screen->w, screen->h,1.0f,0,LAYER_BACKGROUND);
								GFX_drawOnLayer(bmp,ax,ay,aw,ah,1.0f,0,LAYER_BACKGROUND);
							}
							SDL_FreeSurface(bmp);  // Free after rendering
						}
					}
					else {
						SDL_Rect preview_rect = {ox,oy,screen->w,screen->h};
						SDL_Surface * tmpsur = SDL_CreateRGBSurfaceWithFormat(0,screen->w,screen->h,screen->format->BitsPerPixel,screen->format->format);
						SDL_FillRect(tmpsur, &preview_rect, THEME_COLOR7);
						if(lastScreen == SCREEN_GAME) {
							GFX_animateSurfaceOpacity(tmpsur,0,0,screen->w,screen->h,255,0,CFG_getMenuTransitions() ? 150:20,LAYER_BACKGROUND);
						} else if(lastScreen == SCREEN_GAMELIST) {
							GFX_animateSurface(tmpsur,0,0-screen->h,0,0,screen->w,screen->h,CFG_getMenuTransitions() ? 100:20,255,255,LAYER_ALL);
						} else if(lastScreen == SCREEN_GAMESWITCHER) {
							GFX_flipHidden();
							if(gsanimdir == SLIDE_LEFT)
								GFX_animateSurface(tmpsur,0+screen->w,0,0,0,screen->w,screen->h,CFG_getMenuTransitions() ? 80:20,0,255,LAYER_ALL);
							else if(gsanimdir == SLIDE_RIGHT)
								GFX_animateSurface(tmpsur,0-screen->w,0,0,0,screen->w,screen->h,CFG_getMenuTransitions() ? 80:20,0,255,LAYER_ALL);
						}
						SDL_FreeSurface(tmpsur);
						GFX_blitMessage(font.large, "No Preview", screen, &preview_rect);
					}
					Entry_free(selectedEntry);
				}
				else {
					SDL_Rect preview_rect = {ox,oy,screen->w,screen->h};
					SDL_FillRect(screen, &preview_rect, 0);
					GFX_blitMessage(font.large, "No Recents", screen, &preview_rect);
					GFX_blitButtonGroup((char*[]){ "B","BACK", NULL }, 1, screen, 1);
				}

				GFX_flipHidden();

				if(switcherSur) SDL_FreeSurface(switcherSur);
				switcherSur = GFX_captureRendererToSurface();
				lastScreen = SCREEN_GAMESWITCHER;
			}
			else { // if currentscreen == SCREEN_GAMELIST
				// background and game art file path stuff
				Entry* entry = top->entries->items[top->selected];
				assert(entry);
				char tmp_path[MAX_PATH];
				strncpy(tmp_path, entry->path, sizeof(tmp_path) - 1);
				tmp_path[sizeof(tmp_path) - 1] = '\0';

				char* res_name = strrchr(tmp_path, '/');
				if (res_name) res_name++;

				char path_copy[1024];
				strncpy(path_copy, entry->path, sizeof(path_copy) - 1);
				path_copy[sizeof(path_copy) - 1] = '\0';

				char* rompath = dirname(path_copy);

				char res_copy[1024];
				strncpy(res_copy, res_name, sizeof(res_copy) - 1);
				res_copy[sizeof(res_copy) - 1] = '\0';

				char* dot = strrchr(res_copy, '.');
				if (dot) *dot = '\0';

				static int lastType = -1;

				// this is only a choice on the root folder
				// (the "All games" root lists games, not folders, so names always show)
				list_show_entry_names = stack->count > 1 || CFG_getShowFolderNamesAtRoot() || all_games_root;

				// load folder background
				char defaultBgPath[512];
				snprintf(defaultBgPath, sizeof(defaultBgPath), SDCARD_PATH "/bg.png");

				if(((entry->type == ENTRY_DIR || entry->type == ENTRY_ROM) && CFG_getRomsUseFolderBackground())) {
					char *newBg = entry->type == ENTRY_DIR ? entry->path:rompath;
					if((strcmp(newBg, folderBgPath) != 0 || lastType != entry->type) && sizeof(folderBgPath) != 1) {
						lastType = entry->type;
						char tmppath[512];
						strncpy(folderBgPath, newBg, sizeof(folderBgPath) - 1);
						if (entry->type == ENTRY_DIR)
							snprintf(tmppath, sizeof(tmppath), "%s/.media/bg.png", folderBgPath);
						else if (entry->type == ENTRY_ROM)
							snprintf(tmppath, sizeof(tmppath), "%s/.media/bglist.png", folderBgPath);
						if(!exists(tmppath)) {
							// Safeguard: If no background is available, still render the text to leave the user a way out
							list_show_entry_names = true;
							snprintf(tmppath, sizeof(tmppath), defaultBgPath, folderBgPath);
						}
						startLoadFolderBackground(tmppath, onBackgroundLoaded, NULL);
					}
				}
				else if(strcmp(defaultBgPath, folderBgPath) != 0 && exists(defaultBgPath)) {
					strncpy(folderBgPath, defaultBgPath, sizeof(folderBgPath) - 1);
					startLoadFolderBackground(defaultBgPath, onBackgroundLoaded, NULL);
				}
				else {
					// Safeguard: If no background is available, still render the text to leave the user a way out
					list_show_entry_names = true;
				}
				// grid tiles show their own art, and the selection pill/scroll text layers are list-only
				int grid_mode = Grid_active();
				int grid_show_names = list_show_entry_names;
				if (grid_mode) {
					list_show_entry_names = false;
					had_thumb = 0;
					ox = screen->w;
				}

				// load game thumbnails
				if (total > 0 && !grid_mode) {
					if(CFG_getShowGameArt()) {
						char thumbpath[1024];
						snprintf(thumbpath, sizeof(thumbpath), "%s/.media/%s.png", rompath, res_copy);
						had_thumb = 0;
						startLoadThumb(thumbpath, onThumbLoaded, NULL);
						int max_w = (int)(screen->w - (screen->w * CFG_getGameArtWidth()));
						int max_h = (int)(screen->h * 0.6);
						int new_w = max_w;
						int new_h = max_h;
						if(exists(thumbpath)) {
							ox = (int)(max_w) - SCALE1(BUTTON_MARGIN*5);
							had_thumb = 1;
						}
						else
							ox = screen->w;
					}
				}

				// buttons
				if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
				else if (can_resume) GFX_blitButtonGroup((char*[]){ "X","RESUME",  NULL }, 0, screen, 0);
				else GFX_blitButtonGroup((char*[]){
					BTN_SLEEP==BTN_POWER?"POWER":"MENU",
					BTN_SLEEP==BTN_POWER||simple_mode?"SLEEP":"INFO",
					NULL }, 0, screen, 0);

				if (total==0) {
					if (stack->count>1) {
						GFX_blitButtonGroup((char*[]){ "B","BACK",  NULL }, 0, screen, 1);
					}
				}
				else {
					if (stack->count>1) {
						GFX_blitButtonGroup((char*[]){ "B","BACK", "A","OPEN", NULL }, 1, screen, 1);
					}
					else {
						GFX_blitButtonGroup((char*[]){ "A","OPEN", NULL }, 0, screen, 1);
					}
				}

				// list
				if (total > 0) {
					selected_row = top->selected - top->start;
					previousY = previous_row * PILL_SIZE;
					targetY = selected_row * PILL_SIZE;
					SDL_Color text_color = uintToColour(THEME_COLOR4_255); // list text color
					if (grid_mode) {
						Grid_render(top, grid_show_names, ow);
						is_scrolling = 0;
					}
					for (int i = top->start, j = 0; !grid_mode && i < top->end; i++, j++) {
						Entry* entry = top->entries->items[i];
						char* entry_name = entry->name;
						char* entry_unique = entry->unique;
						int available_width = MAX(0,(had_thumb ? ox + SCALE1(BUTTON_MARGIN) : screen->w - SCALE1(BUTTON_MARGIN)) - SCALE1(PADDING * 2));
						bool row_is_selected = (j == selected_row);
						bool row_is_top = (i == top->start);
						bool row_has_moved = (previous_row != selected_row || previous_depth != stack->count);
						// a larger screen margin moves the status pill further in, keep clear of it
						if (row_is_top && !(had_thumb))
							available_width -= ow + MAX(0, GFX_getScreenMargin() - SCALE1(PADDING));

						trimSortingMeta(&entry_name);
						if (entry_unique) // Only render if a unique name exists
							trimSortingMeta(&entry_unique);

						char display_name[256];
						int text_width = GFX_getTextWidth(font.large, entry_unique ? entry_unique : entry_name, display_name, available_width, SCALE1(BUTTON_PADDING * 2));
						int max_width = MIN(available_width, text_width);

						// This spaghetti is preventing white text on white pill when volume/color temp is shown,
						// dont ask me why. This all needs to get tossed out and redone properly later.
						SDL_Color text_color = uintToColour(THEME_COLOR4_255);
						int notext = 0;
						if(!row_has_moved && row_is_selected) {
							text_color = uintToColour(THEME_COLOR5_255);
							notext = 1;
						}

						SDL_LockMutex(fontMutex);
						SDL_Surface* text = TTF_RenderUTF8_Blended(font.large, entry_name, text_color);
						SDL_Surface* text_unique = TTF_RenderUTF8_Blended(font.large, display_name, COLOR_DARK_TEXT);
						SDL_UnlockMutex(fontMutex);
						// TODO: Use actual font metrics to center, this only works in simple cases
						const int text_offset_y = (SCALE1(PILL_SIZE) - text->h + 1) >> 1;
						if (row_is_selected) {
							is_scrolling = list_show_entry_names && GFX_textShouldScroll(font.large,display_name, max_width - SCALE1(BUTTON_PADDING*2), fontMutex);
							GFX_resetScrollText();
							bool should_animate = previous_depth == stack->count;
							SDL_LockMutex(animMutex);
							if(globalpill) {
								SDL_FreeSurface(globalpill);
								globalpill=NULL;
							}
							globalpill = SDL_CreateRGBSurfaceWithFormat(SDL_SWSURFACE, max_width, SCALE1(PILL_SIZE), FIXED_DEPTH, screen->format->format);
							GFX_blitPillDark(ASSET_WHITE_PILL, globalpill, &(SDL_Rect){0,0, max_width, SCALE1(PILL_SIZE)});
							globallpillW =  max_width;
							SDL_UnlockMutex(animMutex);
							updatePillTextSurface(entry_name, max_width, uintToColour(THEME_COLOR5_255));
							AnimTask* task = malloc(sizeof(AnimTask));
							task->startX = SCALE1(BUTTON_MARGIN);
							task->startY = SCALE1(previousY+PADDING);
							task->targetX = SCALE1(BUTTON_MARGIN);
							task->targetY = SCALE1(targetY+PADDING);
							task->targetTextY = SCALE1(PADDING + targetY) + text_offset_y;
							pilltargetTextY = task->targetTextY;
							task->move_w = max_width;
							task->move_h = SCALE1(PILL_SIZE);
							task->frames = should_animate && CFG_getMenuAnimations() ? 3:1;
							task->entry_name = strdup(notext ? " " : entry_name);
							animPill(task);
						}
						SDL_Rect text_rect = { 0, 0, max_width - SCALE1(BUTTON_PADDING*2), text->h };
						SDL_Rect dest_rect = { SCALE1(BUTTON_MARGIN + BUTTON_PADDING), SCALE1(PADDING + (j * PILL_SIZE)) + text_offset_y };

						if(list_show_entry_names) {
							SDL_BlitSurface(text_unique, &text_rect, screen, &dest_rect);
							SDL_BlitSurface(text, &text_rect, screen, &dest_rect);
						}
						SDL_FreeSurface(text_unique); // Free after use
						SDL_FreeSurface(text); // Free after use
					}
					if(lastScreen==SCREEN_GAMESWITCHER) {
						if(switcherSur) {
							// update cpu surface here first
							GFX_clearLayers(LAYER_ALL);
							folderbgchanged=1;

							GFX_flipHidden();
							GFX_animateSurface(switcherSur,0,0,0,0-screen->h,screen->w,screen->h,CFG_getMenuTransitions() ? 100:20,255,255,LAYER_BACKGROUND);
							animationdirection = ANIM_NONE;
						}
					}
					if(lastScreen==SCREEN_OFF) {
						GFX_animateSurfaceOpacity(blackBG,0,0,screen->w,screen->h,255,0,CFG_getMenuTransitions() ? 200:20,LAYER_THUMBNAIL);
					}

					previous_row = selected_row;
					previous_depth = stack->count;
				}
				else {
					// TODO: for some reason screen's dimensions end up being 0x0 in GFX_blitMessage...
					GFX_blitMessage(font.large, "Empty folder", screen, &(SDL_Rect){0,0,screen->w,screen->h}); //, NULL);
				}

				lastScreen = SCREEN_GAMELIST;
			}

			if(animationdirection != ANIM_NONE) {
				if(CFG_getMenuTransitions()) {
					SDL_LockMutex(bgMutex);
					if(folderbgbmp) {
						GFX_drawOnLayer(folderbgbmp, 0, 0, screen->w, screen->h, 1.0f, 0, LAYER_BACKGROUND);
					} else {
						GFX_clearLayers(LAYER_BACKGROUND);
					}
					SDL_UnlockMutex(bgMutex);
					folderbgchanged = 1;
					GFX_clearLayers(LAYER_TRANSITION);
					// Draw the selection pill at its target position before capturing the new
					// screen, so the cursor is present in the incoming slide surface rather than
					// blinking in after the transition animation ends.
					if (currentScreen == SCREEN_GAMELIST && total > 0 && globalpill && list_show_entry_names) {
						SDL_LockMutex(animMutex);
						GFX_drawOnLayer(globalpill, SCALE1(BUTTON_MARGIN), (int)SCALE1(targetY + PADDING), globallpillW, globalpill->h, 1.0f, 0, LAYER_TRANSITION);
						if (globalText) {
							GFX_drawOnLayer(globalText, SCALE1(BUTTON_MARGIN + BUTTON_PADDING), pilltargetTextY, globalText->w, globalText->h, 1.0f, 0, LAYER_SCROLLTEXT);
						}
						SDL_UnlockMutex(animMutex);
					}
					GFX_flipHidden();
					SDL_Surface *tmpNewScreen = GFX_captureRendererToSurface();
					SDL_SetSurfaceBlendMode(tmpNewScreen,SDL_BLENDMODE_BLEND);
					GFX_clearLayers(LAYER_SCROLLTEXT); // baked into tmpNewScreen; must clear or it floats above the animation
					GFX_clearLayers(LAYER_THUMBNAIL);
					{
						int _duration = (CFG_getMenuTransitions() == TRANSITION_COMFY)
							? TRANSITION_COMFY_DURATION
							: TRANSITION_SNAPPY_DURATION;
						if(animationdirection == SLIDE_LEFT) GFX_animateAndFadeSurface(tmpOldScreen,0,0,-250,0,FIXED_WIDTH,FIXED_HEIGHT,_duration,tmpNewScreen,FIXED_WIDTH,0,0,0,FIXED_WIDTH,FIXED_HEIGHT,255,255,LAYER_THUMBNAIL,TRANSITION_CURVE,TRANSITION_CURVE,TRANSITION_INTENSITY);
						if(animationdirection == SLIDE_RIGHT) GFX_animateAndFadeSurface(tmpNewScreen,-250,0,0,0,FIXED_WIDTH,FIXED_HEIGHT,_duration,tmpOldScreen,0,0,FIXED_WIDTH,0,FIXED_WIDTH,FIXED_HEIGHT,255,255,LAYER_THUMBNAIL,TRANSITION_CURVE,TRANSITION_CURVE,TRANSITION_INTENSITY);
					}
					GFX_clearLayers(LAYER_THUMBNAIL);
					SDL_FreeSurface(tmpNewScreen);
				}
				// animation done
				animationdirection = ANIM_NONE;
			}

			if(lastScreen == SCREEN_QUICKMENU) {
				SDL_LockMutex(bgMutex);
				if(folderbgchanged) {
					if(folderbgbmp)
						GFX_drawOnLayer(folderbgbmp,0, 0, screen->w, screen->h,1.0f,0,LAYER_BACKGROUND);
					else {
						GFX_clearLayers(LAYER_BACKGROUND);
					}
					folderbgchanged = 0;
				}
				SDL_UnlockMutex(bgMutex);
			}
			else if(lastScreen == SCREEN_GAMELIST) {
				SDL_LockMutex(bgMutex);
				if(folderbgchanged) {
					if(folderbgbmp)
						GFX_drawOnLayer(folderbgbmp,0, 0, screen->w, screen->h,1.0f,0,LAYER_BACKGROUND);
					else {
						GFX_clearLayers(LAYER_BACKGROUND);
					}
					folderbgchanged = 0;
				}
				SDL_UnlockMutex(bgMutex);
				SDL_LockMutex(thumbMutex);
				if(thumbbmp && thumbchanged) {
					int img_w = thumbbmp->w;
					int img_h = thumbbmp->h;
					double aspect_ratio = (double)img_h / img_w;
					int max_w = (int)(screen->w * CFG_getGameArtWidth());
					int max_h = (int)(screen->h * 0.6);
					int new_w = max_w;
					int new_h = (int)(new_w * aspect_ratio);

					if (new_h > max_h) {
						new_h = max_h;
						new_w = (int)(new_h / aspect_ratio);
					}

					int target_x = screen->w-(new_w + SCALE1(BUTTON_MARGIN*3));
					int target_y = (int)(screen->h * 0.50);
					int center_y = target_y - (new_h / 2); // FIX: use new_h instead of thumbbmp->h
					GFX_clearLayers(LAYER_THUMBNAIL);
					GFX_drawOnLayer(thumbbmp,target_x,center_y,new_w,new_h,1.0f,0,LAYER_THUMBNAIL);
				} else if(thumbchanged) {
					GFX_clearLayers(LAYER_THUMBNAIL);
				}
				SDL_UnlockMutex(thumbMutex);

				GFX_clearLayers(LAYER_TRANSITION);
				GFX_clearLayers(LAYER_SCROLLTEXT);

				SDL_LockMutex(animMutex);
				if (list_show_entry_names) {
					GFX_drawOnLayer(globalpill, pillRect.x, pillRect.y, globallpillW, globalpill->h, 1.0f, 0, LAYER_TRANSITION);
					GFX_drawOnLayer(globalText, SCALE1(BUTTON_MARGIN + BUTTON_PADDING), pilltargetTextY, globalText->w, globalText->h, 1.0f, 0, LAYER_SCROLLTEXT);
				}
				SDL_UnlockMutex(animMutex);
			}
			if(!startgame) // dont flip if game gonna start
				GFX_flip(screen);

			dirty = 0;
		} else if(getAnimationDraw() || folderbgchanged || thumbchanged || is_scrolling) {
			// honestly this whole thing is here only for the scrolling text, I set it now to run this at 30fps which is enough for scrolling text, should move this to seperate animation function eventually
			Uint32 now = SDL_GetTicks();
			Uint32 frame_start = now;
			static char cached_display_name[256] = "";
			SDL_LockMutex(bgMutex);
			if(folderbgchanged) {
				if(folderbgbmp)
					GFX_drawOnLayer(folderbgbmp,0, 0, screen->w, screen->h,1.0f,0,LAYER_BACKGROUND);
				else {
					GFX_clearLayers(LAYER_BACKGROUND);
				}
				folderbgchanged = 0;
			}
			SDL_UnlockMutex(bgMutex);
			SDL_LockMutex(thumbMutex);
			if(thumbbmp && thumbchanged) {
				int img_w = thumbbmp->w;
				int img_h = thumbbmp->h;
				double aspect_ratio = (double)img_h / img_w;

				int max_w = (int)(screen->w * CFG_getGameArtWidth());
				int max_h = (int)(screen->h * 0.6);

				int new_w = max_w;
				int new_h = (int)(new_w * aspect_ratio);

				if (new_h > max_h) {
					new_h = max_h;
					new_w = (int)(new_h / aspect_ratio);
				}

				int target_x = screen->w-(new_w + SCALE1(BUTTON_MARGIN*3));
				int target_y = (int)(screen->h * 0.50);
				int center_y = target_y - (new_h / 2); // FIX: use new_h instead of thumbbmp->h
				GFX_clearLayers(LAYER_THUMBNAIL);
				GFX_drawOnLayer(thumbbmp,target_x,center_y,new_w,new_h,1.0f,0,LAYER_THUMBNAIL);
				thumbchanged = 0;
			} else if(thumbchanged) {
				GFX_clearLayers(LAYER_THUMBNAIL);
				thumbchanged = 0;
			}
			SDL_UnlockMutex(thumbMutex);
			SDL_LockMutex(animMutex);
			if (getAnimationDraw()) {
				GFX_clearLayers(LAYER_TRANSITION);
				if (list_show_entry_names)
					GFX_drawOnLayer(globalpill, pillRect.x, pillRect.y, globallpillW, globalpill->h, 1.0f, 0, LAYER_TRANSITION);
				setAnimationDraw(0);
			}
			SDL_UnlockMutex(animMutex);
			if (currentScreen != SCREEN_GAMESWITCHER && currentScreen != SCREEN_QUICKMENU) {
				if(is_scrolling && pillanimdone && currentAnimQueueSize < 1) {
					int ow = GFX_blitHardwareGroup(screen, show_setting);
					Entry* entry = top->entries->items[top->selected];
					trimSortingMeta(&entry->name);
					char* entry_text = entry->name;
					if (entry->unique) {
						trimSortingMeta(&entry->unique);
						entry_text = entry->unique;
					}

					int available_width = (had_thumb ? ox + SCALE1(BUTTON_MARGIN) : screen->w - SCALE1(BUTTON_MARGIN)) - SCALE1(PADDING * 2);
					if (top->selected == top->start && !had_thumb)
						available_width -= ow + MAX(0, GFX_getScreenMargin() - SCALE1(PADDING));

					SDL_Color text_color = uintToColour(THEME_COLOR5_255);

					int text_width = GFX_getTextWidth(font.large, entry_text, cached_display_name, available_width, SCALE1(BUTTON_PADDING * 2));
					int max_width = MIN(available_width, text_width);
					int text_offset_y = (SCALE1(PILL_SIZE) - TTF_FontHeight(font.large) + 1) >> 1;

					GFX_clearLayers(LAYER_SCROLLTEXT);
					if (list_show_entry_names) {
						GFX_scrollTextTexture(
							font.large,
							entry_text,
							SCALE1(BUTTON_MARGIN + BUTTON_PADDING), SCALE1(PADDING + previous_row * PILL_SIZE) + text_offset_y,
							max_width - SCALE1(BUTTON_PADDING * 2),
							0,
							text_color,
							1,
							fontMutex  // Thread-safe font access
						);
					}
				}
				else {
					GFX_clearLayers(LAYER_TRANSITION);
					GFX_clearLayers(LAYER_SCROLLTEXT);
					SDL_LockMutex(animMutex);
					if (list_show_entry_names) {
						GFX_drawOnLayer(globalpill, pillRect.x, pillRect.y, globallpillW, globalpill->h, 1.0f, 0, LAYER_TRANSITION);
						GFX_drawOnLayer(globalText, SCALE1(BUTTON_MARGIN + BUTTON_PADDING),pilltargetTextY, globalText->w, globalText->h, 1.0f, 0, LAYER_SCROLLTEXT);
					}
					SDL_UnlockMutex(animMutex);
					PLAT_GPU_Flip();
				}
			}
			else {
				GFX_sync();
			}
			dirty = 0;
		}
		else {
			// want to draw only if needed
			SDL_LockMutex(bgqueueMutex);
			SDL_LockMutex(thumbqueueMutex);
			SDL_LockMutex(animqueueMutex);
			if(getNeedDraw()) {
				PLAT_GPU_Flip();
				setNeedDraw(0);
			} else {
				GFX_sync();
			}
			SDL_UnlockMutex(animqueueMutex);
			SDL_UnlockMutex(thumbqueueMutex);
			SDL_UnlockMutex(bgqueueMutex);
		}

		SDL_LockMutex(frameMutex);
		frameReady = true;
		SDL_CondSignal(flipCond);
		SDL_UnlockMutex(frameMutex);

		// animation does not carry over between loops, this should only ever be set by
		// input handling and directly consumed by the following render pass
		assert(animationdirection == ANIM_NONE);

		// handle HDMI change
		static int had_hdmi = -1;
		int has_hdmi = GetHDMI();
		if (had_hdmi==-1) had_hdmi = has_hdmi;
		if (has_hdmi!=had_hdmi) {
			had_hdmi = has_hdmi;

			Entry* entry = top->entries->items[top->selected];
			LOG_info("restarting after HDMI change... (%s)\n", entry->path);
			saveLast(entry->path); // NOTE: doesn't work in Recents (by design)
			sleep(1); // cable-seating debounce; the output switch itself happens at next app start
			quit = 1;
		}
	}

	Menu_quit();
	PWR_quit();
	PAD_quit();

	// Cleanup worker threads and their synchronization primitives
	cleanupImageLoaderPool();

	GFX_quit(); // Cleanup video subsystem first to stop GPU threads

	// Now safe to free surfaces after GPU threads are stopped
	if(blackBG)	SDL_FreeSurface(blackBG);
	if (folderbgbmp) SDL_FreeSurface(folderbgbmp);
	if (thumbbmp) SDL_FreeSurface(thumbbmp);
	Grid_freeArt();

	QuitSettings();
}
