#include "ma_internal.h"
#include "ma_options.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int Option_getValueIndex(Option* item, const char* value) {
	if (!value || !item || !item->values) return 0;
	int i = 0;
	while (item->values[i]) {
		if (!strcmp(item->values[i], value)) return i;
		i++;
	}
	return 0;
}
void Option_setValue(Option* item, const char* value) {
	// TODO: store previous value?
	item->value = Option_getValueIndex(item, value);
}

// TODO: does this also need to be applied to OptionList_vars()?
static const char* option_key_name[] = {
	"pcsx_rearmed_analog_combo", "DualShock Toggle Combo",
	NULL
};
static const char* getOptionNameFromKey(const char* key, const char* name) {
	char* _key = NULL;
	for (int i=0; (_key = (char*)option_key_name[i]); i+=2) {
		if (exactMatch((char*)key,_key)) return option_key_name[i+1];
	}
	return name;
}

// the following 3 functions always touch config.core, the rest can operate on arbitrary OptionLists
void OptionList_init(const struct retro_core_option_definition *defs) {
	LOG_info("OptionList_init\n");
	int count;
	for (count=0; defs[count].key; count++);

	// LOG_info("count: %i\n", count);

	// TODO: add frontend options to this? so the can use the same override method? eg. minarch_*

	config.core.count = count;
	config.core.categories = NULL; // There is no categories in v1 definition
	if (count) {
		config.core.options = calloc(count+1, sizeof(Option));

		for (int i=0; i<config.core.count; i++) {
			int len;
			const struct retro_core_option_definition *def = &defs[i];
			Option* item = &config.core.options[i];
			len = strlen(def->key) + 1;

			item->key = calloc(len, sizeof(char));
			strcpy(item->key, def->key);

			len = strlen(def->desc) + 1;
			item->name = calloc(len, sizeof(char));
			strcpy(item->name, getOptionNameFromKey(def->key,def->desc));

			if (def->info) {
				len = strlen(def->info) + 1;
				item->desc = calloc(len, sizeof(char));
				strncpy(item->desc, def->info, len);

				item->full = calloc(len, sizeof(char));
				strncpy(item->full, item->desc, len);
				// item->desc[len-1] = '\0';

				GFX_wrapText(font.tiny, item->desc, DEVICE_WIDTH - SCALE1(2*PADDING), 2);
				GFX_wrapText(font.medium, item->full, DEVICE_WIDTH - SCALE1(2*PADDING), 16);
			}

			for (count=0; def->values[count].value; count++);

			item->count = count;
			item->values = calloc(count+1, sizeof(char*));
			item->labels = calloc(count+1, sizeof(char*));

			for (int j=0; j<count; j++) {
				const char* value = def->values[j].value;
				const char* label = def->values[j].label;

				len = strlen(value) + 1;
				item->values[j] = calloc(len, sizeof(char));
				strcpy(item->values[j], value);

				if (label) {
					len = strlen(label) + 1;
					item->labels[j] = calloc(len, sizeof(char));
					strcpy(item->labels[j], label);
				}
				else {
					item->labels[j] = item->values[j];
				}
				// printf("\t%s\n", item->labels[j]);
			}

			item->value = Option_getValueIndex(item, def->default_value);
			item->default_value = item->value;

			// LOG_info("\tINIT %s (%s) TO %s (%s)\n", item->name, item->key, item->labels[item->value], item->values[item->value]);
		}
	}
	// fflush(stdout);
}

void OptionList_v2_init(const struct retro_core_options_v2 *opt_defs) {
	LOG_info("OptionList_v2_init\n");
	struct retro_core_option_v2_category   *cats = opt_defs->categories;
	struct retro_core_option_v2_definition *defs = opt_defs->definitions;

	int cat_count = 0;
	while (cats[cat_count].key) cat_count++;

	int count = 0;
	while (defs[count].key) count++;

	// LOG_info("%i categories, %i options\n", cat_count, count);

	// TODO: add frontend options to this? so the can use the same override method? eg. minarch_*

	if (cat_count) {
		config.core.categories = calloc(cat_count + 1, sizeof(OptionCategory));

		for (int i=0; i<cat_count; i++) {
			const struct retro_core_option_v2_category *cat = &cats[i];
			OptionCategory* item = &config.core.categories[i];

			item->key  = strdup(cat->key);
			item->desc = strdup(cat->desc);
			item->info = cat->info ? strdup(cat->info) : NULL;
			printf("CATEGORY %s\n", item->key);
		}
	}
	else {
		config.core.categories = NULL;
	}

	config.core.count = count;
	if (count) {
		config.core.options = calloc(count+1, sizeof(Option));

		for (int i=0; i<config.core.count; i++) {
			const struct retro_core_option_v2_definition *def = &defs[i];
			Option* item = &config.core.options[i];

			item->key = strdup(def->key);
			item->name = strdup(getOptionNameFromKey(def->key, def->desc_categorized ? def->desc_categorized : def->desc));
			item->category = def->category_key ? strdup(def->category_key) : NULL;

			if (def->info) {
				item->desc = strdup(def->info);
				item->full = strdup(item->desc);

				GFX_wrapText(font.tiny, item->desc, DEVICE_WIDTH - SCALE1(2*PADDING), 2);
				GFX_wrapText(font.medium, item->full, DEVICE_WIDTH - SCALE1(2*PADDING), 16);
			}

			for (count=0; def->values[count].value; count++);

			item->count = count;
			item->values = calloc(count+1, sizeof(char*));
			item->labels = calloc(count+1, sizeof(char*));

			for (int j=0; j<count; j++) {
				const char* value = def->values[j].value;
				const char* label = def->values[j].label;

				item->values[j] = strdup(value);

				if (label) {
					item->labels[j] = strdup(label);
				}
				else {
					item->labels[j] = item->values[j];
				}
				// printf("\t%s\n", item->labels[j]);
			}

			item->value = Option_getValueIndex(item, def->default_value);
			item->default_value = item->value;

			// LOG_info("\tINIT %s (%s) TO %s (%s)\n", item->name, item->key, item->labels[item->value], item->values[item->value]);
		}
	}
	// fflush(stdout);
}

void OptionList_vars(const struct retro_variable *vars) {
	LOG_info("OptionList_vars\n");
	int count;
	for (count=0; vars[count].key; count++);

	config.core.count = count;
	if (count) {
		config.core.options = calloc(count+1, sizeof(Option));

		for (int i=0; i<config.core.count; i++) {
			int len;
			const struct retro_variable *var = &vars[i];
			Option* item = &config.core.options[i];

			len = strlen(var->key) + 1;
			item->key = calloc(len, sizeof(char));
			strcpy(item->key, var->key);

			len = strlen(var->value) + 1;
			item->var = calloc(len, sizeof(char));
			strcpy(item->var, var->value);

			char* tmp = strchr(item->var, ';');
			if (tmp && *(tmp+1)==' ') {
				*tmp = '\0';
				item->name = item->var;
				tmp += 2;
			}

			char* opt = tmp;
			for (count=0; (tmp=strchr(tmp, '|')); tmp++, count++);
			count += 1; // last entry after final '|'

			item->count = count;
			item->values = calloc(count+1, sizeof(char*));
			item->labels = calloc(count+1, sizeof(char*));

			tmp = opt;
			int j;
			for (j=0; (tmp=strchr(tmp, '|')); j++) {
				item->values[j] = opt;
				item->labels[j] = opt;
				*tmp = '\0';
				tmp += 1;
				opt = tmp;
			}
			item->values[j] = opt;
			item->labels[j] = opt;

			// no native default_value support for retro vars
			item->value = 0;
			item->default_value = item->value;
			// printf("SET %s to %s (%i)\n", item->key, default_value, item->value); fflush(stdout);
		}
	}
	// fflush(stdout);
}
void OptionList_reset(void) {
	if (!config.core.count) return;

	for (int i=0; i<config.core.count; i++) {
		Option* item = &config.core.options[i];
		if (item->var) {
			// values/labels are all points to var
			// so no need to free individually
			free(item->var);
		}
		else {
			if (item->desc) free(item->desc);
			if (item->full) free(item->full);
			for (int j=0; j<item->count; j++) {
				char* value = item->values[j];
				char* label = item->labels[j];
				if (label!=value) free(label);
				free(value);
			}
		}
		free(item->values);
		free(item->labels);
		free(item->key);
		free(item->name);
	}
	if (config.core.enabled_options) free(config.core.enabled_options);
	config.core.enabled_count = 0;
	free(config.core.options);
}

Option* OptionList_getOption(OptionList* list, const char* key) {
	for (int i=0; i<list->count; i++) {
		Option* item = &list->options[i];
		if (!strcmp(item->key, key)) return item;
	}
	return NULL;
}
char* OptionList_getOptionValue(OptionList* list, const char* key) {
	Option* item = OptionList_getOption(list, key);
	// if (item) LOG_info("\tGET %s (%s) = %s (%s)\n", item->name, item->key, item->labels[item->value], item->values[item->value]);
	if (item) {
		int count = 0;
		while ( item->values && item->values[count]) count++;
		if (item->value >= 0 && item->value < count) {
			return item->values[item->value];
		}
	}
	// else LOG_warn("unknown option %s \n", key);
	return NULL;
}
void OptionList_setOptionRawValue(OptionList* list, const char* key, int value) {
	Option* item = OptionList_getOption(list, key);
	if (item) {
		item->value = value;
		list->changed = 1;
		// LOG_info("\tRAW SET %s (%s) TO %s (%s)\n", item->name, item->key, item->labels[item->value], item->values[item->value]);
		// if (list->on_set) list->on_set(list, key);

		if (exactMatch((char*)core.tag, "GB") && containsString(item->key, "palette")) Special_updatedDMGPalette(3); // from options
	}
	else LOG_info("unknown option %s \n", key);
}
void OptionList_setOptionValue(OptionList* list, const char* key, const char* value) {
	Option* item = OptionList_getOption(list, key);
	if (item) {
		Option_setValue(item, value);
		list->changed = 1;
		// LOG_info("\tSET %s (%s) TO %s (%s)\n", item->name, item->key, item->labels[item->value], item->values[item->value]);
		// if (list->on_set) list->on_set(list, key);

		if (exactMatch((char*)core.tag, "GB") && containsString(item->key, "palette")) Special_updatedDMGPalette(2); // from core
	}
	else LOG_info("unknown option %s \n", key);
}
void OptionList_setOptionVisibility(OptionList* list, const char* key, int visible) {
	Option* item = OptionList_getOption(list, key);
	if (item) item->hidden = !visible;
	else printf("unknown option %s \n", key); fflush(stdout);
}
