// titles.c — enumera saves de conta no console e resolve o nome de cada
// jogo pelo NACP.
//
// Structs conferidas contra a libnx instalada:
//   /opt/devkitpro/libnx/include/switch/services/fs.h  -> FsSaveDataInfo
//     (campo do title id se chama literalmente application_id)
//   /opt/devkitpro/libnx/include/switch/services/ns.h  -> NsApplicationControlData
//   /opt/devkitpro/libnx/include/switch/nacp.h         -> NacpStruct.lang[16]

#include "titles.h"

#include <switch.h>
#include <string.h>
#include <stdio.h>

#define SCAN_BATCH_SIZE 32

// "Esse jogo esta instalado no console?"
//
// conversa privada removida do historico
// conversa privada removida do historico
// ficou na NAND) e o caso comum — o console guarda o save mesmo depois de
// desinstalar o jogo, entao a lista vinha cheia de coisa que ele nao pode usar.
//
// nsIsAnyApplicationEntityInstalled e a pergunta literal, respondida pelo ns.
// Nao da pra usar o nome como pista: nsGetApplicationControlData com
// NsApplicationControlSource_Storage responde do CACHE quando o jogo nao esta
// mais instalado, entao um save orfao ainda devolve o nome bonito do jogo.
//
// Em erro, o padrao e EXCLUIR: se eu nao consegui confirmar que esta instalado,
// mostrar assim mesmo e trazer de volta exatamente a lista que mandaram tirar.
static bool is_installed(uint64_t application_id) {
    bool installed = false;
    Result rc = nsIsAnyApplicationEntityInstalled(application_id, &installed);
    if (R_FAILED(rc)) return false;
    return installed;
}

static bool resolve_title_name(uint64_t application_id, char *out, size_t outsz) {
    // NsApplicationControlData tem um icon[0x20000] embutido — é grande
    // demais pra stack do Switch (crash certo), por isso aqui embaixo tem
    // que ser static (BSS) e não variável local.
    static NsApplicationControlData ctrl;
    u64 actual_size = 0; // a libnx pede u64*, não size_t*

    Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage,
                                             application_id, &ctrl, sizeof(ctrl), &actual_size);
    if (R_FAILED(rc)) return false;

    for (int i = 0; i < 16; i++) {
        if (ctrl.nacp.lang[i].name[0] != '\0') {
            strncpy(out, ctrl.nacp.lang[i].name, outsz - 1);
            out[outsz - 1] = '\0';
            return true;
        }
    }
    return false;
}

bool titles_get_icon(u64 application_id, u8 *out, size_t outsz, size_t *out_len) {
    if (!out || !out_len) return false;

    // mesma história do resolve_title_name: essa struct tem 128 KB de ícone
    // dentro, não pode ser variável local.
    static NsApplicationControlData ctrl;
    u64 actual_size = 0;

    bool ns_owned = R_SUCCEEDED(nsInitialize());
    Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage,
                                             application_id, &ctrl, sizeof(ctrl), &actual_size);
    if (ns_owned) nsExit();
    if (R_FAILED(rc)) return false;

    // actual_size cobre o NACP + o ícone; o que sobra depois do NACP é o
    // JPEG. Se vier só o NACP, esse jogo não tem ícone disponível.
    if (actual_size <= sizeof(ctrl.nacp)) return false;
    size_t icon_len = (size_t)actual_size - sizeof(ctrl.nacp);
    if (icon_len > sizeof(ctrl.icon)) icon_len = sizeof(ctrl.icon);
    if (icon_len > outsz) return false;

    memcpy(out, ctrl.icon, icon_len);
    *out_len = icon_len;
    return true;
}

// Ordena a lista do mais recente jogado pro mais antigo, usando o serviço
// pdm (o mesmo registro de uso que o próprio menu do console usa pra montar
// a fileira de jogos recentes).
//
// pdmqryQueryLastPlayTime devolve, pra cada application_id que ele conhece,
// o timestamp da última vez que o jogo foi aberto. Nem todo jogo aparece
// (save de jogo nunca aberto nessa instalação, log de uso desativado, jogo
// de outro perfil), então quem não tem registro fica com timestamp 0 e cai
// pro fim da lista, mantendo a ordem original entre eles.
static void sort_by_last_played(TitleEntry *entries, size_t count) {
    if (count < 2) return;

    static u64 ids[TITLES_MAX_SORTED];
    static PdmLastPlayTime times[TITLES_MAX_SORTED];
    static u32 keys[TITLES_MAX_SORTED];

    if (count > TITLES_MAX_SORTED) return; // não deveria acontecer; sem ordenação é melhor que estourar buffer

    for (size_t i = 0; i < count; i++) {
        ids[i] = entries[i].application_id;
        keys[i] = 0;
    }

    if (R_FAILED(pdmqryInitialize())) return; // sem pdm, fica na ordem que o FS devolveu

    s32 total_out = 0;
    Result rc = pdmqryQueryLastPlayTime(false, times, ids, (s32)count, &total_out);
    pdmqryExit();
    if (R_FAILED(rc) || total_out <= 0) return;

    // O retorno pode vir filtrado (total_out < count), então casamos por
    // application_id em vez de confiar na posição do array.
    for (s32 i = 0; i < total_out && i < (s32)count; i++) {
        for (size_t j = 0; j < count; j++) {
            if (entries[j].application_id == times[i].application_id) {
                keys[j] = times[i].timestamp_user;
                break;
            }
        }
    }

    // insertion sort estável, decrescente por timestamp. count aqui é a
    // quantidade de jogos com save no console — dezenas, não milhares.
    for (size_t i = 1; i < count; i++) {
        TitleEntry entry = entries[i];
        u32 key = keys[i];
        size_t j = i;
        while (j > 0 && keys[j - 1] < key) {
            entries[j] = entries[j - 1];
            keys[j] = keys[j - 1];
            j--;
        }
        entries[j] = entry;
        keys[j] = key;
    }
}

size_t titles_list_with_savedata(TitleEntry *out, size_t max_entries) {
    if (!out || max_entries == 0) return 0;

    bool fs_owned = false, ns_owned = false;
    if (R_SUCCEEDED(fsInitialize())) {
        fs_owned = true;
    }
    // se fsInitialize já tinha sido chamado em outro lugar do app, essa
    // chamada falha com erro "já inicializado" — não é fatal, seguimos
    // tentando o resto (o serviço já está disponível de qualquer forma).
    if (R_SUCCEEDED(nsInitialize())) {
        ns_owned = true;
    }

    size_t count = 0;

    FsSaveDataInfoReader reader;
    Result rc = fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User);
    if (R_FAILED(rc)) {
        goto cleanup;
    }

    {
        // fsSaveDataInfoReaderRead é um iterador: cada chamada devolve um
        // lote e avança a posição interna do reader. Uma chamada só (com
        // buffer fixo) perderia silenciosamente qualquer entrada além do
        // tamanho do lote se o console tiver muita save data (de sistema +
        // jogos, não só jogos) — por isso o loop até vir 0 de volta.
        FsSaveDataInfo entries[SCAN_BATCH_SIZE];
        for (;;) {
            s64 total_read = 0;
            rc = fsSaveDataInfoReaderRead(&reader, entries, SCAN_BATCH_SIZE, &total_read);
            if (R_FAILED(rc) || total_read == 0) break;

            for (s64 i = 0; i < total_read && count < max_entries; i++) {
                FsSaveDataInfo *info = &entries[i];
                if (info->save_data_type != FsSaveDataType_Account) continue;

                uint64_t application_id = info->application_id;
                if (application_id == 0) continue;

                // So jogo instalado. Ver is_installed() logo acima.
                if (!is_installed(application_id)) continue;

                // pula duplicata (mesmo jogo pode ter mais de uma entrada de
                // save, ex: perfis de usuário diferentes — por ora pegamos só
                // a primeira que achar)
                bool dup = false;
                for (size_t j = 0; j < count; j++) {
                    if (out[j].application_id == application_id) { dup = true; break; }
                }
                if (dup) continue;

                out[count].application_id = application_id;
                out[count].save_data_id = info->save_data_id;
                out[count].uid = info->uid;

                if (!resolve_title_name(application_id, out[count].name, sizeof(out[count].name))) {
                    snprintf(out[count].name, sizeof(out[count].name),
                             "Titulo desconhecido (%016lX)", application_id);
                }
                count++;
            }

            if (count >= max_entries) break;
            if ((size_t)total_read < SCAN_BATCH_SIZE) break; // lote parcial = acabou
        }
        fsSaveDataInfoReaderClose(&reader);
    }

cleanup:
    if (ns_owned) nsExit();
    if (fs_owned) fsExit();
    sort_by_last_played(out, count);
    return count;
}
