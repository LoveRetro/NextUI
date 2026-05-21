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

#define T(k) I18N_t(k)

#ifdef __cplusplus
}
#endif

#endif
