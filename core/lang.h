// lang.h — idioma do app: português ou inglês.
//
// conversa privada removida do historico
//
// Sem arquivo de tradução e sem tabela de chaves, de propósito. A borealis tem
// um i18n próprio (resources/i18n/<locale>/*.json), mas ele lê só o idioma do
// CONSOLE — não dá pra escolher dentro do app, que é justamente o que ele
// pediu. E tabela de chaves troca erro de compilação por erro em tempo de
// execução: chave que ninguém traduziu aparece na tela como "menu/games/title",
// e só se descobre abrindo aquela tela.
//
// Aqui as duas versões ficam lado a lado no ponto de uso:
//
//     TR("Enviar o save pro Drive", "Upload save to Drive")
//
// Quem esquecer a segunda não compila.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LANG_AUTO = 0, // segue o idioma do console
    LANG_PT   = 1,
    LANG_EN   = 2,
} LangChoice;

// Lê a escolha guardada no cartão e resolve LANG_AUTO perguntando ao console.
// Chame uma vez, no começo do main. Sem essa chamada vale o padrão: português.
void lang_load(void);

LangChoice lang_choice(void);     // o que está guardado (pode ser LANG_AUTO)
bool lang_set(LangChoice choice); // grava no cartão e passa a valer na hora
bool lang_is_pt(void);            // o idioma que está valendo agora

// TR e não T: `T` é o nome que meio mundo usa pra parâmetro de template, e um
// macro chamado T dentro de um projeto que inclui a borealis é pedir encrenca.
#define TR(pt, en) (lang_is_pt() ? (pt) : (en))

#ifdef __cplusplus
}
#endif
