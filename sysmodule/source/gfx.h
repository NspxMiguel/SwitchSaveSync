// gfx.h — a tela de "Puxando save da nuvem".
//
// Uma mensagem no estilo da interface do próprio console: "Puxando save da
// nuvem", a porcentagem embaixo, e o jogo só abre quando termina. A tela fica
// de pé por pelo menos 5 segundos, com dois botões — "Nao puxar save da nuvem
// nesse jogo" (que depois dá pra desmarcar no Ultrahand) e um OK.
//
// Quem desenha isso é o sysmodule, não o jogo — então não dá pra usar a tela do
// jogo. O jeito de um processo de sistema aparecer por cima de tudo é criar uma
// camada própria no compositor (vi:m, "managed layer"), que é exatamente o que
// o Tesla/Ultrahand faz pra desenhar o overlay dele. A diferença é que aqui a
// camada é a tela inteira e o jogo está congelado atrás dela.
//
// A fonte é a do próprio console (pl:u), rasterizada com stb_truetype.
#pragma once
#include <switch.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sobe a camada. Devolve false se o compositor recusou — nesse caso o pull
// continua, só que sem tela (melhor puxar o save calado do que não puxar).
bool gfx_init(void);
void gfx_exit(void);

// Botões da tela.
#define GFX_BTN_NEVER 0 // "Nao puxar save da nuvem nesse jogo"
#define GFX_BTN_OK    1

// Desenha um quadro inteiro.
//   game     nome do jogo (linha de baixo do título)
//   pct      0..100, ou -1 quando ainda não dá pra saber o total
//   line     texto miúdo de status ("Baixando da nuvem...")
//   selected GFX_BTN_*
//   done     true = acabou de baixar (o OK acende)
void gfx_draw_pull(const char *game, int pct, const char *line, int selected, bool done);

// Lê o controle. Devolve os botões que ACABARAM de ser apertados
// (HidNpadButton_*). Precisa ser chamado num laço pra funcionar.
u64 gfx_keys_down(void);

#ifdef __cplusplus
}
#endif
