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
    AccountUid uid;       // conta dona do save — necessária pra montar (zerado em device save)
    char name[0x201];     // nome do jogo (do NACP) ou "Titulo desconhecido (<id hex>)"
    char account[0x21];   // apelido da conta dona do save ("" se não deu pra ler / device save)

    // Esse save é do CONSOLE, não de uma conta.
    //
    // O Switch tem mais de um tipo de save data. O comum é o de conta, mas
    // existe o "device save": um só por console, compartilhado por todos os
    // perfis. É onde o Animal Crossing guarda a ilha e onde o Pokémon
    // Sword/Shield guarda parte das coisas. Enquanto isto não existia, esses
    // jogos nem apareciam na lista — e no caso do Animal Crossing dava pra
    // fazer "backup" do save de conta (quase vazio) achando que a ilha tinha
    // ido junto.
    bool device_save;

    // Esse jogo tem MAIS DE UM save neste console — seja de contas
    // diferentes, seja um de conta e um do console.
    //
    // A lista aqui tem uma linha por SAVE, não por jogo. Sem este marcador,
    // dois saves do mesmo jogo viravam duas linhas idênticas na tela e —
    // pior — a mesma pasta no Drive, uma sobrescrevendo a outra. Só quando ele
    // é true é que o dono do save (conta ou "console") entra na tela e no nome
    // da pasta: assim jogo de save único continua com exatamente o nome de
    // pasta de sempre, e nada que já está no Drive fica órfão.
    bool shared_game;
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
