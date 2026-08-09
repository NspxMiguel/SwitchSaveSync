// titles.h — descobre quais jogos tem save data no console e resolve o
// nome de exibição de cada um (via NACP), pra montar a lista que aparece
// no menu "escolher jogo pra sincronizar".
//
// As structs usadas aqui (FsSaveDataInfo, NacpStruct) foram conferidas
// contra a libnx instalada em /opt/devkitpro — não são mais chute.
#pragma once
#include <switch.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Teto de jogos que a ordenação por "último jogado" consegue tratar de uma
// vez (os buffers da consulta ao pdm são estáticos). Quem chama não precisa
// respeitar isso — acima disso a lista só volta sem ordenar.
#define TITLES_MAX_SORTED 128

typedef struct {
    u64 application_id;   // title id do jogo
    u64 save_data_id;     // id do save (informativo; o mount é por application_id+uid)
    AccountUid uid;       // conta dona do save — necessária pra montar
    char name[0x201];     // nome do jogo (do NACP) ou "Titulo desconhecido (<id hex>)"
} TitleEntry;

// Enumera até max_entries jogos com save data de conta de usuário presente
// no console, preenchendo out[]. Devolve quantos achou (0 em caso de erro
// ou se não houver nenhum).
//
// A lista vem ordenada do jogo mais recentemente jogado pro mais antigo
// (registro de uso do serviço pdm). Jogo sem registro de uso vai pro fim.
size_t titles_list_with_savedata(TitleEntry *out, size_t max_entries);

// Copia o ícone do jogo (JPEG, o mesmo que aparece no menu do console) pra
// out, escrevendo o tamanho real em out_len. Precisa de um buffer grande:
// o ícone vem dentro de NsApplicationControlData e o campo tem 0x20000
// bytes. Devolve false se o jogo não tiver ícone acessível.
//
// É uma chamada por jogo, sob demanda, de propósito: guardar o ícone de
// todos os jogos de uma vez seriam vários MB parados na memória.
bool titles_get_icon(u64 application_id, u8 *out, size_t outsz, size_t *out_len);

#ifdef __cplusplus
}
#endif
