#include "ma_internal.h"
#include "ma_cheats.h"

#include <ctype.h>
#include <string.h>
#include <glob.h>

// based on picoarch/cheat.c

struct Cheats cheatcodes;

#define CHEAT_MAX_DESC_LEN 27
#define CHEAT_MAX_LINE_LEN 52
#define CHEAT_MAX_LINES    3

static const char *find_val(const char *start) {
	start--;
	while(!isspace(*++start))
		;

	while(isspace(*++start))
		;

	if (*start != '=')
		return NULL;

	while(isspace(*++start))
		;

	// newer cheat files quote every value, older ones quote only strings
	if (*start == '"')
		start++;

	return start;
}

// the count sits on the first line in older cheat files and on the last line in
// newer ones, so scan the whole file and keep the last match
static size_t parse_count(FILE *file) {
	char line[512];
	size_t count = 0;

	while (fgets(line, sizeof(line), file)) {
		const char *ptr = line;
		while (isspace(*ptr))
			ptr++;

		// anchored, so a description mentioning "cheats = " can't hijack us
		if (strncmp(ptr, "cheats", 6))
			continue;

		if ((ptr = find_val(ptr)))
			count = strtoul(ptr, NULL, 10);
	}

	rewind(file);
	return count;
}

static int parse_bool(const char *ptr, int *out) {
	if (!strncasecmp(ptr, "true", 4)) {
		*out = 1;
	} else if (!strncasecmp(ptr, "false", 5)) {
		*out = 0;
	} else {
		return -1;
	}
	return 0;
}

static int parse_string(const char *ptr, char *buf, size_t len) {
	int index = 0;
	size_t input_len = strlen(ptr);

	buf[0] = '\0';

	// find_val already consumed the opening quote; the closing one is checked below
	while (*ptr != '\0' && *ptr != '"' && index < len - 1) {
		if (*ptr == '\\' && index < input_len - 1) {
			ptr++;
			buf[index++] = *ptr++;
		} else if (*ptr == '&' && !strncmp(ptr, "&quot;", 6)) {
			buf[index++] = '"';
			ptr += 6;
		} else {
			buf[index++] = *ptr++;
		}
	}

	if (*ptr != '"') {
		buf[0] = '\0';
		return -1;
	}

	buf[index] = '\0';
	return 0;
}

static int parse_cheats(struct Cheats *cheats, FILE *file) {
	int ret = -1;
	char line[512];
	char buf[512];
	const char *ptr;

	do {
		if (!fgets(line, sizeof(line), file)) {
			ret = 0;
			break;
		}

		if (line[strlen(line) - 1] != '\n' && !feof(file)) {
			LOG_warn("Cheat line too long\n");
			continue;
		}

		if ((ptr = strstr(line, "cheat"))) {
			int index = -1;
			struct Cheat *cheat;
			size_t len;
			sscanf(ptr, "cheat%d", &index);

			if (index < 0 || (size_t)index >= cheats->count)
				continue;
			cheat = &cheats->cheats[index];

			// these would otherwise match the _value and _address tests below
			if (strstr(ptr, "_rumble") || strstr(ptr, "_repeat"))
				continue;

			if (strstr(ptr, "_desc")) {
				ptr = find_val(ptr);
				if (!ptr || parse_string(ptr, buf, sizeof(buf))) {
					LOG_warn("Couldn't parse cheat %d description\n", index);
					continue;
				}

				len = strlen(buf);
				if (len == 0)
					continue;

				cheat->name = calloc(len+1, sizeof(char));
				if (!cheat->name)
					goto finish;

				strncpy((char *)cheat->name, buf, len);
				truncateString((char *)cheat->name, CHEAT_MAX_DESC_LEN);

				if (len >= CHEAT_MAX_DESC_LEN) {
					cheat->info = calloc(len+1, sizeof(char));
					if (!cheat->info)
						goto finish;

					strncpy((char *)cheat->info, buf, len);
					wrapString((char *)cheat->info, CHEAT_MAX_LINE_LEN, CHEAT_MAX_LINES);
				}
			} else if (strstr(ptr, "_code")) {
				ptr = find_val(ptr);
				if (!ptr || parse_string(ptr, buf, sizeof(buf))) {
					LOG_warn("Couldn't parse cheat %d code\n", index);
					continue;
				}

				len = strlen(buf);
				if (len == 0)
					continue;

				cheat->code = calloc(len+1, sizeof(char));
				if (!cheat->code)
					goto finish;

				strncpy((char *)cheat->code, buf, len);
			} else if (strstr(ptr, "_enable")) {
				ptr = find_val(ptr);
				if (!ptr || parse_bool(ptr, &cheat->enabled)) {
					LOG_warn("Couldn't parse cheat %d enabled\n", index);
					continue;
				}
			}
			// the rest describe a memory write, see Cheats_apply
			// note: _address_bit_position must be tested before _address
			else if (strstr(ptr, "_address_bit_position")) {
				if ((ptr = find_val(ptr)))
					cheat->bit_position = strtoul(ptr, NULL, 10);
			} else if (strstr(ptr, "_address")) {
				if ((ptr = find_val(ptr)))
					cheat->address = strtoul(ptr, NULL, 10);
			} else if (strstr(ptr, "_value")) {
				if ((ptr = find_val(ptr)))
					cheat->value = strtoul(ptr, NULL, 10);
			} else if (strstr(ptr, "_cheat_type")) {
				if ((ptr = find_val(ptr)))
					cheat->type = strtoul(ptr, NULL, 10);
			} else if (strstr(ptr, "_memory_search_size")) {
				if ((ptr = find_val(ptr)))
					cheat->size = strtoul(ptr, NULL, 10);
			} else if (strstr(ptr, "_big_endian")) {
				if ((ptr = find_val(ptr)))
					parse_bool(ptr, &cheat->big_endian);
			}
		}
	} while(1);

finish:
	return ret;
}

// return variations with/without extensions and other cruft
// note: CHEAT_MAX_PATHS must be large enough to contain one entry per extension supported by a core, plus ~5 more
// 48 is enough: retroarch cores with most extensions at the moment is VICE VIC-20 at 37 and rom-cleaner at 42
void Cheat_getPaths(char paths[CHEAT_MAX_PATHS][MAX_PATH], int* count) {
	// reserve a few entries at the end, for sanitized name and glob patterns
	const int sanitized_paths_count = 3;

	// Generate possible paths, ordered by most likely to be used (pre v6.2.3 style first)
	sprintf(paths[(*count)++], "%s/%s.cht", core.cheats_dir, game.name); // /mnt/SDCARD/Cheats/GB/Super Example World.<ext>.cht
	if(CFG_getUseExtractedFileName())
		sprintf(paths[(*count)++], "%s/%s.cht", core.cheats_dir, game.alt_name); // /mnt/SDCARD/Cheats/GB/Super Example World (USA).<ext>.cht

	// game.alt_name, but with all extension-like stuff removed (apart from .cht)
	// eg. Super Example World (USA).zip -> Super Example World (USA).cht
	{
		int i = 0;
		char* ext;
		char exts[128];
		if (core.extensions == NULL || strlen(core.extensions) >= sizeof(exts)) {
			LOG_info("Invalid or too long core.extensions\n");
			return;
		}

		strcpy(exts, core.extensions);
		while ((ext = strtok(i ? NULL : exts, "|"))) {
			// sanitized_paths_count slots are reserved for sanitized rom name, see below
			if (*count >= CHEAT_MAX_PATHS - sanitized_paths_count) {
				LOG_info("Maximum cheat paths reached, stopping\n");
				break;
			}

			char rom_name[MAX_PATH];
			if (strlen(game.alt_name) >= MAX_PATH) {
				LOG_info("game.alt_name too long, skipping\n");
				i++;
				continue;
			}

			strcpy(rom_name, game.alt_name);
			char* tmp = strrchr(rom_name, '.');
			if (tmp != NULL && strlen(tmp) > 2 && strlen(tmp) <= 5) {
				tmp[0] = '\0';

				// Add length check before sprintf to prevent buffer overflow
				int needed_len = strlen(core.cheats_dir) + strlen(rom_name) + strlen(ext) + 10; // +10 for "/", ".", ".cht", etc.
				if (needed_len < MAX_PATH) {
					sprintf(paths[(*count)++], "%s/%s.%s.cht", core.cheats_dir, rom_name, ext);
				} else {
					LOG_info("Path too long, skipping: %s/%s.%s.cht\n", core.cheats_dir, rom_name, ext);
				}
			}
			i++;
		}
	}

	// Sanitized: remove extension and other cruft
	// eg. Super Example World (USA).zip -> Super Example World
	//     Super Example World (USA) [!].7z -> Super Example World
	//     Super Example World (USA) (Rev 1).rar -> Super Example World
	// Important: update `sanitized_paths_count` if adding more sanitized variations
	char rom_name[MAX_PATH];
	getDisplayName(game.alt_name, rom_name);
	sprintf(paths[(*count)++], "%s/%s.cht", core.cheats_dir, rom_name); // /mnt/SDCARD/Cheats/GB/Super Example World.cht
	// Respect map.txt: use alias if available
	// eg. 1941.zip	-> 1941: Counter Attack
	if(getAlias(game.path, rom_name))
		sprintf(paths[(*count)++], "%s/%s.cht", core.cheats_dir, rom_name); // /mnt/SDCARD/Cheats/GB/Super Example World.cht

	// Santitized alias, ignoring all extra cruft - including Cheat specifics like "(Game Breaker)" etc.
	// This is a wildcard that may match something unexpected, but also may find something when nothing else does.
	getAlias(game.path, rom_name);
	sprintf(paths[(*count)++], "%s/%s*.cht", core.cheats_dir, rom_name); // /mnt/SDCARD/Cheats/GB/Super Example World*.cht
	// Log all path candidates
	{
		char *list = calloc(*count * (MAX_PATH + 2) + 1, 1); // path + separator for each entry
		if (list != NULL) {
			int i;
			for (i=0; i<*count; i++) {
				strcat(list, paths[i]);
				if (i < *count-1) strcat(list, ", ");
			}
			//LOG_info("Cheat paths to check: %s\n", list);
			free(list);
		}
	}
}

void Cheats_free() {
	size_t i;
	for (i = 0; cheatcodes.cheats && i < cheatcodes.count; i++) {
		struct Cheat *cheat = &cheatcodes.cheats[i];
		if (cheat) {
			free((char *)cheat->name);
			free((char *)cheat->info);
			free((char *)cheat->code);
		}
	}
	free(cheatcodes.cheats);
	cheatcodes.cheats = NULL;
	cheatcodes.count = 0;
}

bool Cheats_load() {
	int success = 0;
	struct Cheats *cheats = &cheatcodes;
	FILE *file = NULL;
	size_t i;

	// we get our paths frrom Cheat_getPaths, some might be wildcards
	char paths[CHEAT_MAX_PATHS][MAX_PATH];
	int path_count = 0;
	Cheat_getPaths(paths, &path_count);
	char filename[MAX_PATH] = {0};
	for (i=0; i<path_count; i++) {
		LOG_info("Checking cheat path: %s\n", paths[i]);
		// handle wildcards
		if (strchr(paths[i],'*')) {
			// Use glob to handle wildcards
			char glob_pattern[MAX_PATH];
			strcpy(glob_pattern, paths[i]);

			glob_t glob_results;
			memset(&glob_results, 0, sizeof(glob_t));
			int glob_ret = glob(glob_pattern, 0, NULL, &glob_results);

			if (glob_ret == 0 && glob_results.gl_pathc > 0) {
				for (size_t gi = 0; gi < glob_results.gl_pathc; ++gi) {
					if (!suffixMatch(".cht", glob_results.gl_pathv[gi])) continue;
					strcpy(filename, glob_results.gl_pathv[gi]);
					if (exists(filename)) {
						LOG_info("Found potential cheat file: %s\n", filename);
						break;
					}
					filename[0] = '\0';
				}
			}
			globfree(&glob_results);
			if (filename[0] == '\0') continue; // no match
		} else {
			strcpy(filename, paths[i]);
			if (!exists(filename)) {
				filename[0] = '\0';
				continue;
			}
		}
		break; // found a valid file
	}
	if (filename[0] == '\0') {
		LOG_info("No cheat file found\n");
		goto finish;
	}

	LOG_info("Loading cheats from %s\n", filename);

	file = fopen(filename, "r");
	if (!file) {
		LOG_error("Couldn't open cheat file: %s\n", filename);
		goto finish;
	}

	cheatcodes.count = parse_count(file);
	if (cheatcodes.count == 0 || cheatcodes.count > CHEAT_MAX_COUNT) {
		LOG_error("Couldn't read cheat count\n");
		goto finish;
	}

	cheatcodes.cheats = calloc(cheatcodes.count, sizeof(struct Cheat));
	if (!cheatcodes.cheats) {
		LOG_error("Couldn't allocate memory for cheats\n");
		goto finish;
	}

	if (parse_cheats(&cheatcodes, file)) {
		LOG_error("Error reading cheat %d\n", i);
		goto finish;
	}

	// memory-write cheats are useless without system ram, so report what we got
	LOG_info("Found %i cheats for the current game (system ram: %zu bytes).\n",
		cheatcodes.count, core.get_memory_size ? core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM) : 0);

	success = 1;
finish:
	if (!success) {
		Cheats_free();
	}

	if (file)
		fclose(file);

	return success;
}

// Cheats from RetroArch's newer format carry no code for the core to interpret,
// they name a memory location to hold at a value. Those have to be rewritten
// every frame, otherwise the game just overwrites them again.
// Addresses are treated as offsets into system RAM; cores that publish a memory
// map (RETRO_ENVIRONMENT_SET_MEMORY_MAPS) may expect a different layout.
void Cheats_apply(void) {
	static const unsigned char widths[] = { 0, 0, 0, 1, 2, 4 }; // by memory_search_size
	unsigned char *ram;
	size_t ram_size, i;

	if (!core.get_memory_data || !core.get_memory_size)
		return;

	ram_size = core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
	ram = core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
	if (!ram || !ram_size)
		return;

	for (i = 0; i < cheatcodes.count; i++) {
		const struct Cheat *cheat = &cheatcodes.cheats[i];
		unsigned char width;
		unsigned value;
		int shift;

		// code means the core applies it, see Core_applyCheats
		if (!cheat->enabled || cheat->code || cheat->type != CHEAT_TYPE_SET_TO_VALUE)
			continue;

		if (cheat->size < 3) {
			// sub-byte cheat: mask into the byte at bit_position
			unsigned char mask = (1 << (1 << cheat->size)) - 1;
			if (cheat->address >= ram_size || cheat->bit_position > 7)
				continue;

			shift = cheat->bit_position;
			ram[cheat->address] = (ram[cheat->address] & ~(mask << shift)) | ((cheat->value & mask) << shift);
			continue;
		}

		// written this way round so address + width can't overflow
		width = cheat->size < sizeof(widths) ? widths[cheat->size] : 0;
		if (!width || width > ram_size || cheat->address > ram_size - width)
			continue;

		value = cheat->value;
		for (shift = 0; shift < width; shift++) {
			int byte = cheat->big_endian ? width - 1 - shift : shift;
			ram[cheat->address + byte] = (value >> (shift * 8)) & 0xff;
		}
	}
}
