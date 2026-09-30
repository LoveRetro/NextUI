#ifndef __I18N_H__
#define __I18N_H__

#ifdef __cplusplus
extern "C" {
#endif

#define I18N_LANG_CODE_MAX 8
#define I18N_LANG_DIR      "/.system/res/lang"

void        I18N_init(const char *lang_code);
void        I18N_quit(void);
int         I18N_reload(const char *lang_code);
char       *I18N_t(const char *key);
const char *I18N_active_code(void);

// Merges an additional .lang file into the table already loaded by
// I18N_init()/I18N_reload(), without clearing it first -- lets a pak add
// its own keys (or a community translation override another pak's) on top
// of the OS vocabulary. Safe to call more than once; later files win on
// key collisions. No-op (returns 0) if called before I18N_init().
int I18N_load_extra(const char *path);

#define T(k) I18N_t(k)

#ifdef __cplusplus
}
#endif

#endif
