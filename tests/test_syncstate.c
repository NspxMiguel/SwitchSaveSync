// Teste do core/syncstate.c — os arquivos que os TRES programas dividem.
//
// O app (.nro), o sysmodule e o overlay não conversam por nenhuma API: eles
// conversam por arquivo no cartão. status.txt, autosync.cfg, destino.cfg,
// excluidos.txt, pedido.txt e autosync.log são o protocolo inteiro entre os
// três. Quando um deles lê diferente do que o outro escreveu, o que aparece é
// o overlay dizendo uma coisa e o app dizendo outra — e isso já aconteceu
// mais de uma vez neste projeto.
//
// Este arquivo pega a parte que nenhum teste tocava: o log (e o corte dele),
// o status, a chave de liga/desliga, o destino e o pedido de backup.
//
// A pasta do app é "sdmc:/switch/SwitchSaveSync" — no Mac isso vira uma pasta
// com esse nome literal dentro do diretório do teste, do mesmo jeito que os
// testes do FTP e do savemount já fazem.

#include "syncstate.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// O relógio do console. Aqui é fixo: o que este teste olha é o conteúdo das
// linhas, não a hora delas.
Result timeGetCurrentTime(TimeType type, u64 *out)
{
    (void)type;
    *out = 1723600000ULL;
    return 0;
}

// ------------------------------------------------------------------ placar

static int falhas = 0, testes = 0;

static void ok(int cond, const char *msg)
{
    testes++;
    if (cond) { printf("  ok      %s\n", msg); }
    else      { printf("  FALHOU  %s\n", msg); falhas++; }
}

// --------------------------------------------------------------- ajudantes

static long tamanho(const char *caminho)
{
    struct stat st;
    return stat(caminho, &st) == 0 ? (long)st.st_size : -1;
}

static bool existe(const char *caminho) { return tamanho(caminho) >= 0; }

// O arquivo inteiro na memória. O log de teste passa de 64 KB, então o buffer
// tem que ser maior que isso.
static char *le_tudo(const char *caminho)
{
    static char buf[256 * 1024];
    buf[0] = '\0';
    FILE *f = fopen(caminho, "rb");
    if (!f) return buf;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static bool contem(const char *palheiro, const char *agulha)
{
    return strstr(palheiro, agulha) != NULL;
}

static void limpa(void)
{
    system("rm -rf 'sdmc:'");
}

// No console, "sdmc:" e o dispositivo do cartao: ele existe sempre, e o
// syncstate_ensure_dirs conta com isso (ele cria "sdmc:/switch", nao o
// "sdmc:"). Aqui a raiz e uma pasta comum, entao quem cria e o teste.
static void cartao_na_maquina(void)
{
    limpa();
    mkdir("sdmc:", 0777);
}

// -----------------------------------------------------------------------

int main(void)
{
    cartao_na_maquina();

    printf("\n-- a pasta do app --\n");
    syncstate_ensure_dirs();
    struct stat st;
    ok(stat(SYNC_APP_DIR, &st) == 0 && S_ISDIR(st.st_mode), "criou a pasta do app");
    syncstate_ensure_dirs();
    ok(stat(SYNC_APP_DIR, &st) == 0, "e chamar de novo nao estraga nada");

    printf("\n-- autosync: o padrao e DESLIGADO --\n");
    // Isto não é detalhe: escrever em save é irreversível, e "não configurei
    // nada ainda" não pode virar "pode mexer sozinho nos meus saves".
    ok(!syncstate_autosync_enabled(), "sem arquivo nenhum, o autosync esta desligado");
    syncstate_set_autosync_enabled(true);
    ok(syncstate_autosync_enabled(), "ligou");
    syncstate_set_autosync_enabled(false);
    ok(!syncstate_autosync_enabled(), "desligou");

    printf("\n-- destino: sem arquivo, os dois ligados --\n");
    ok(syncstate_dest_local() && syncstate_dest_cloud(),
       "sem destino.cfg, cartao e nuvem contam como ligados");
    syncstate_set_dest(true, false);
    ok(syncstate_dest_local() && !syncstate_dest_cloud(), "so o cartao");
    syncstate_set_dest(false, true);
    ok(!syncstate_dest_local() && syncstate_dest_cloud(), "so a nuvem");
    syncstate_set_dest(false, false);
    ok(!syncstate_dest_local() && !syncstate_dest_cloud(),
       "os dois desligados (o autosync fica sem ter o que fazer, e tudo bem)");

    printf("\n-- status: a linha que o overlay le --\n");
    char buf[256];
    ok(!syncstate_get_status(buf, sizeof(buf)), "sem arquivo, nao inventa status");
    syncstate_set_status("Sincronizando %s...", "Zelda");
    ok(syncstate_get_status(buf, sizeof(buf)) && strcmp(buf, "Sincronizando Zelda...") == 0,
       "leu de volta a mesma linha");
    syncstate_set_status("Ativo");
    ok(syncstate_get_status(buf, sizeof(buf)) && strcmp(buf, "Ativo") == 0,
       "o status novo SUBSTITUI o antigo (nao empilha)");
    ok(tamanho(SYNC_STATUS_PATH) == 6, "e o arquivo tem so a linha nova dentro");

    printf("\n-- pedido de backup: o app pede, o sysmodule pega --\n");
    u64 id = 0;
    ok(!syncstate_take_request(&id), "sem pedido, nao ha o que pegar");
    syncstate_request_backup(0x0100000000010000ULL);
    ok(existe(SYNC_REQUEST_PATH), "o pedido virou arquivo");
    ok(syncstate_take_request(&id) && id == 0x0100000000010000ULL, "pegou o id certo");
    ok(!existe(SYNC_REQUEST_PATH), "e o pedido sumiu do cartao");
    ok(!syncstate_take_request(&id), "pegar duas vezes nao repete o trabalho");

    // Pedido ilegível não pode ficar preso: se ficasse, o sysmodule tentaria
    // pegar o mesmo lixo a cada volta do laço, pra sempre.
    FILE *f = fopen(SYNC_REQUEST_PATH, "w");
    ok(f != NULL, "consegui escrever um pedido ilegivel pra testar");
    if (f) { fprintf(f, "nao e um id\n"); fclose(f); }
    ok(!syncstate_take_request(&id), "pedido ilegivel devolve false");
    ok(!existe(SYNC_REQUEST_PATH), "e some do cartao em vez de ficar preso pra sempre");

    printf("\n-- excluidos --\n");
    ok(!syncstate_is_excluded(0x0100000000020000ULL), "nada excluido no comeco");
    ok(syncstate_set_excluded(0x0100000000020000ULL, true), "excluiu um jogo");
    ok(syncstate_is_excluded(0x0100000000020000ULL), "e ele conta como excluido");
    ok(syncstate_set_excluded(0x0100000000030000ULL, true), "excluiu outro");
    u64 lista[8];
    size_t n = syncstate_list_excluded(lista, 8);
    ok(n == 2, "a lista tem os dois");
    ok(syncstate_set_excluded(0x0100000000020000ULL, false), "tirou o primeiro");
    n = syncstate_list_excluded(lista, 8);
    ok(n == 1 && lista[0] == 0x0100000000030000ULL, "e so o segundo ficou");
    ok(!existe(SYNC_EXCLUDE_PATH ".parcial"), "sem sobra de arquivo pela metade");

    printf("\n-- log: escreve e carimba a hora --\n");
    remove(SYNC_LOG_PATH);
    syncstate_log("primeira linha");
    syncstate_log("save de %s subiu", "Zelda");
    char *log = le_tudo(SYNC_LOG_PATH);
    ok(contem(log, "primeira linha"), "gravou a primeira");
    ok(contem(log, "save de Zelda subiu"), "gravou a segunda, com formatacao");
    ok(contem(log, "["), "e carimbou a hora");
    ok(strstr(log, "primeira linha") < strstr(log, "save de Zelda subiu"),
       "na ordem em que aconteceram");

    // O caso que interessa. Antes daqui, passar de 64 KB APAGAVA o log inteiro
    // — e o log só chega perto disso numa sessão comprida, que é justamente a
    // sessão em que alguma coisa deu errado.
    printf("\n-- log grande: fica a metade NOVA, nao o arquivo vazio --\n");
    remove(SYNC_LOG_PATH);
    for (int i = 0; i < 3000; i++)
        syncstate_log("linha numero %d com bastante texto pra engordar o arquivo depressa", i);

    long depois = tamanho(SYNC_LOG_PATH);
    ok(depois > 0, "o log continua existindo");
    ok(depois < 64 * 1024 + 4096, "e ficou abaixo do teto");

    log = le_tudo(SYNC_LOG_PATH);
    ok(contem(log, "linha numero 2999"), "a linha MAIS NOVA esta la");
    ok(!contem(log, "linha numero 0 com"), "a mais velha foi cortada");
    ok(contem(log, "o comeco do log foi cortado"),
       "e o arquivo diz que foi cortado, em vez de parecer que a historia comeca ali");
    ok(log[0] == '[', "a primeira linha comeca inteira (nao no meio de uma frase)");
    ok(!existe(SYNC_LOG_TEMP_PATH), "sem sobra do arquivo temporario");

    // Depois do corte o log continua sendo log: as linhas seguintes entram
    // normalmente, no fim.
    syncstate_log("depois do corte");
    log = le_tudo(SYNC_LOG_PATH);
    ok(contem(log, "depois do corte"), "e continua aceitando linha nova depois do corte");
    ok(contem(log, "linha numero 2999"), "sem perder o que ja estava la");

    printf("\n-- log pequeno nao e mexido --\n");
    remove(SYNC_LOG_PATH);
    syncstate_log("so uma linha");
    long antes = tamanho(SYNC_LOG_PATH);
    syncstate_log("duas linhas");
    ok(tamanho(SYNC_LOG_PATH) > antes, "log pequeno so cresce, ninguem corta nada");
    ok(contem(le_tudo(SYNC_LOG_PATH), "so uma linha"), "e a primeira linha continua la");

    limpa();

    printf("\n%d/%d passaram\n", testes - falhas, testes);
    return falhas ? 1 : 0;
}
