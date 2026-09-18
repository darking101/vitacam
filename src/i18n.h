#ifndef I18N_H
#define I18N_H

#include <string.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>

typedef enum {
    LANG_EN = 0,
    LANG_ES = 1
} AppLanguage;

extern AppLanguage g_lang;

static inline void i18n_init(void) {
    SceAppUtilInitParam initParam;
    SceAppUtilBootParam bootParam;
    memset(&initParam, 0, sizeof(initParam));
    memset(&bootParam, 0, sizeof(bootParam));
    if (sceAppUtilInit(&initParam, &bootParam) >= 0) {
        int sys_lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
        if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &sys_lang) >= 0) {
            if (sys_lang == SCE_SYSTEM_PARAM_LANG_SPANISH) {
                g_lang = LANG_ES;
            } else {
                g_lang = LANG_EN;
            }
        }
    }
}

#define LOC(es, en) (g_lang == LANG_ES ? (es) : (en))

#endif
