#pragma once

#include "ma_internal.h"

// Option value helpers
int   Option_getValueIndex(Option *item, const char *value);
void  Option_setValue(Option *item, const char *value);

// OptionList initialization (called from environment_callback)
void OptionList_init(const struct retro_core_option_definition *defs);
void OptionList_v2_init(const struct retro_core_options_v2 *opt_defs);
void OptionList_vars(const struct retro_variable *vars);
void OptionList_reset(void);

// OptionList accessors (called from Config_* and frontend)
Option *OptionList_getOption(OptionList *list, const char *key);
char   *OptionList_getOptionValue(OptionList *list, const char *key);
void    OptionList_setOptionValue(OptionList *list, const char *key, const char *value);
void    OptionList_setOptionRawValue(OptionList *list, const char *key, int value);
void    OptionList_setOptionVisibility(OptionList *list, const char *key, int visible);
