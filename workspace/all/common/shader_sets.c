#include "shader_sets.h"
#include "defines.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static bool isSafeComponent(const char *value)
{
	if (!value || !value[0] || !strcmp(value, ".") || !strcmp(value, ".."))
		return false;

	return !strchr(value, '/') && !strchr(value, '\\');
}

static bool isConfigFile(const char *name)
{
	size_t len;

	if (!name || name[0] == '.')
		return false;

	len = strlen(name);
	return len > 4 && !strcmp(name + len - 4, ".cfg");
}

static bool isRegularFile(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int compareNames(const void *left, const void *right)
{
	const char *a = *(const char * const *)left;
	const char *b = *(const char * const *)right;
	int result = strcasecmp(a, b);
	return result ? result : strcmp(a, b);
}

static void readActiveName(char *name, size_t name_size)
{
	FILE *file;

	if (!name || name_size == 0)
		return;

	name[0] = '\0';
	file = fopen(SHADER_SET_STATE_PATH, "r");
	if (!file)
		return;

	if (fgets(name, name_size, file))
		name[strcspn(name, "\r\n")] = '\0';

	fclose(file);
}

static bool writeActiveName(const char *name)
{
	char temp_path[MAX_PATH];
	FILE *file;
	int fd;

	if (!name || !name[0]) {
		if (unlink(SHADER_SET_STATE_PATH) == 0 || errno == ENOENT) {
			sync();
			return true;
		}
		return false;
	}

	if (snprintf(temp_path, sizeof(temp_path), "%s.tmp.%ld",
			SHADER_SET_STATE_PATH, (long)getpid()) >= (int)sizeof(temp_path))
		return false;

	file = fopen(temp_path, "w");
	if (!file)
		return false;

	if (fprintf(file, "%s\n", name) < 0 || fflush(file) != 0) {
		fclose(file);
		unlink(temp_path);
		return false;
	}

	fd = fileno(file);
	if (fd >= 0 && fsync(fd) != 0) {
		fclose(file);
		unlink(temp_path);
		return false;
	}

	if (fclose(file) != 0) {
		unlink(temp_path);
		return false;
	}

	if (rename(temp_path, SHADER_SET_STATE_PATH) != 0) {
		unlink(temp_path);
		return false;
	}

	sync();
	return true;
}

bool ShaderSets_list(ShaderSetList *list)
{
	DIR *dir;
	struct dirent *entry;
	char **names;
	int count = 1;

	if (!list)
		return false;

	list->names = NULL;
	list->count = 0;

	names = calloc(1, sizeof(char *));
	if (!names)
		return false;

	names[0] = strdup("");
	if (!names[0]) {
		free(names);
		return false;
	}

	dir = opendir(SHADER_SETS_PATH);
	if (dir) {
		while ((entry = readdir(dir)) != NULL) {
			char path[MAX_PATH];
			char *stem;
			char **expanded;
			size_t len;

			if (!isConfigFile(entry->d_name))
				continue;

			if (snprintf(path, sizeof(path), "%s/%s",
				SHADER_SETS_PATH, entry->d_name) >= (int)sizeof(path))
				continue;

			if (!isRegularFile(path))
				continue;

			len = strlen(entry->d_name) - 4;
			stem = strndup(entry->d_name, len);
			if (!stem)
				goto fail;

			if (!strcasecmp(stem, SHADER_SET_DISABLED_LABEL)) {
				free(stem);
				continue;
			}

			expanded = realloc(names, sizeof(char *) * (count + 1));
			if (!expanded) {
				free(stem);
				goto fail;
			}

			names = expanded;
			names[count++] = stem;
		}
		closedir(dir);
	}

	if (count > 2)
		qsort(names + 1, count - 1, sizeof(char *), compareNames);

	list->names = names;
	list->count = count;
	return true;

fail:
	if (dir)
		closedir(dir);
	for (int i = 0; i < count; i++)
		free(names[i]);
	free(names);
	return false;
}

void ShaderSets_freeList(ShaderSetList *list)
{
	if (!list)
		return;

	for (int i = 0; i < list->count; i++)
		free(list->names[i]);
	free(list->names);
	list->names = NULL;
	list->count = 0;
}

const char *ShaderSets_displayName(const char *name)
{
	return name && name[0] ? name : SHADER_SET_DISABLED_LABEL;
}

int ShaderSets_activeIndex(const ShaderSetList *list)
{
	char active[MAX_PATH];

	if (!list || !list->names || list->count <= 0)
		return 0;

	readActiveName(active, sizeof(active));
	if (!isSafeComponent(active))
		return 0;

	for (int i = 1; i < list->count; i++) {
		if (!strcmp(list->names[i], active))
			return i;
	}

	writeActiveName("");
	return 0;
}

bool ShaderSets_getActive(char *name, size_t name_size)
{
	ShaderSetList list;
	FILE *file;

	if (!name || name_size == 0)
		return false;

	name[0] = '\0';
	file = fopen(SHADER_SET_STATE_PATH, "r");
	if (!file)
		return errno == ENOENT;
	if (fgets(name, name_size, file))
		name[strcspn(name, "\r\n")] = '\0';
	if (ferror(file) || fclose(file) != 0) {
		name[0] = '\0';
		return false;
	}
	if (!name[0])
		return true;
	if (!isSafeComponent(name)) {
		name[0] = '\0';
		return false;
	}

	if (!ShaderSets_list(&list))
		return false;

	for (int i = 1; i < list.count; i++) {
		if (!strcmp(list.names[i], name)) {
			ShaderSets_freeList(&list);
			return true;
		}
	}

	ShaderSets_freeList(&list);
	name[0] = '\0';
	return false;
}

bool ShaderSets_setActive(const char *name)
{
	char path[MAX_PATH];

	if (!name || !name[0])
		return writeActiveName("");

	if (!ShaderSets_rootPath(name, path, sizeof(path)) || !isRegularFile(path))
		return false;

	return writeActiveName(name);
}

bool ShaderSets_advance(const ShaderSetList *list, char *name, size_t name_size)
{
	int next;

	if (!list || !list->names || list->count <= 0 || !name || name_size == 0)
		return false;

	next = (ShaderSets_activeIndex(list) + 1) % list->count;
	if (snprintf(name, name_size, "%s", list->names[next]) >= (int)name_size)
		return false;

	return ShaderSets_setActive(name);
}

bool ShaderSets_rootPath(const char *name, char *path, size_t path_size)
{
	if (!isSafeComponent(name) || !path || path_size == 0)
		return false;

	return snprintf(path, path_size, "%s/%s.cfg", SHADER_SETS_PATH, name) < (int)path_size;
}

bool ShaderSets_overridePath(const char *name, const char *tag, char *path, size_t path_size)
{
	if (!isSafeComponent(name) || !isSafeComponent(tag) || !path || path_size == 0)
		return false;

	return snprintf(path, path_size, "%s/%s/%s.cfg",
		SHADER_SETS_PATH, tag, name) < (int)path_size;
}
