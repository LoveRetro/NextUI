#ifndef SHADER_SETS_H
#define SHADER_SETS_H

#include <stdbool.h>
#include <stddef.h>

#ifndef SHADER_SETS_PATH
#define SHADER_SETS_PATH SDCARD_PATH "/Shaders/sets"
#endif
#ifndef SHADER_SET_STATE_PATH
#define SHADER_SET_STATE_PATH SHARED_USERDATA_PATH "/shader-set.txt"
#endif
#define SHADER_SET_DISABLED_LABEL "Disabled"

typedef struct ShaderSetList {
	char **names;
	int count;
} ShaderSetList;

bool ShaderSets_list(ShaderSetList *list);
void ShaderSets_freeList(ShaderSetList *list);

const char *ShaderSets_displayName(const char *name);
int ShaderSets_activeIndex(const ShaderSetList *list);
bool ShaderSets_getActive(char *name, size_t name_size);
bool ShaderSets_setActive(const char *name);
bool ShaderSets_advance(const ShaderSetList *list, char *name, size_t name_size);

bool ShaderSets_rootPath(const char *name, char *path, size_t path_size);
bool ShaderSets_overridePath(const char *name, const char *tag, char *path, size_t path_size);

#endif
