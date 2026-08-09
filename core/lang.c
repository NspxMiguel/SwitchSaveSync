#include "lang.h"

#include "syncstate.h"

#include <switch.h>
#include <stdio.h>
#include <string.h>

#define LANG_PATH SYNC_APP_DIR "/idioma.txt"

// Português por padrão. O app nasceu em português e o dono é brasileiro; e se
// lang_load() nunca for chamado — o sysmodule não chama, o log dele não vai pra
// tela de ninguém — é esse valor que vale.
static LangChoice g_choice = LANG_AUTO;
static bool g_is_pt        = true;

bool lang_is_pt(void) { return g_is_pt; }
LangChoice lang_choice(void) { return g_choice; }

// "O console está em português?"
//
// Vale pt e pt-BR: o português do Brasil só virou idioma separado no firmware
// 10.1.0, então console mais antigo (ou com o europeu escolhido) responde "pt".
// Comparar os dois primeiros caracteres pega os dois casos.
static bool console_is_pt(void)
{
    if (R_FAILED(setInitialize()))
        return true; // sem o serviço, fica no padrão

    u64 code  = 0;
    Result rc = setGetSystemLanguage(&code);
    setExit();

    if (R_FAILED(rc))
        return true;

    // O LanguageCode é uma string curta empacotada dentro de um u64
    // ("en-US", "pt-BR"), sem garantia de terminar em zero se ocupar os 8
    // bytes — por isso a cópia pra um buffer de 9.
    char name[9] = { 0 };
    memcpy(name, &code, sizeof(code));
    return strncmp(name, "pt", 2) == 0;
}

static void apply(void)
{
    if (g_choice == LANG_PT)
        g_is_pt = true;
    else if (g_choice == LANG_EN)
        g_is_pt = false;
    else
        g_is_pt = console_is_pt();
}

void lang_load(void)
{
    FILE *f = fopen(LANG_PATH, "r");
    if (f)
    {
        char buf[16] = { 0 };
        if (fgets(buf, sizeof(buf), f))
        {
            if (strncmp(buf, "pt", 2) == 0)
                g_choice = LANG_PT;
            else if (strncmp(buf, "en", 2) == 0)
                g_choice = LANG_EN;
            else
                g_choice = LANG_AUTO;
        }
        fclose(f);
    }
    apply();
}

bool lang_set(LangChoice choice)
{
    g_choice = choice;
    apply(); // vale já, mesmo que a gravação abaixo falhe

    syncstate_ensure_dirs();

    FILE *f = fopen(LANG_PATH, "w");
    if (!f)
        return false;

    fprintf(f, "%s\n", choice == LANG_PT ? "pt" : choice == LANG_EN ? "en" : "auto");
    return fclose(f) == 0;
}
