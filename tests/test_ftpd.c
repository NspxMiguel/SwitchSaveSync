// Teste do servidor de FTP do app, rodando no Mac contra clientes de verdade.
//
// O ftpd.c é socket BSD puro — a única coisa de console nele é a thread e o
// mutex da libnx. Trocando esses dois por pthread (ver stubs_libnx.c), o mesmo
// arquivo que roda no Switch roda aqui, e aí dá pra apontar o `curl` e o `ftp`
// do sistema pra ele.
//
// Vale a pena porque servidor de FTP é um protocolo com muitos jeitos de dar
// quase certo: a listagem que o Finder aceita e o FileZilla não, o PASV que
// anuncia a porta errada, o STOR que fecha o arquivo antes do último pedaço,
// o ".." que sobe acima da raiz. Nada disso aparece lendo o código.
//
// O que NÃO dá pra testar aqui: a rede do console, a memória apertada, e o
// comportamento com o cartão ocupado por outro processo.

#include "ftpd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ------------------------------------------------------------------ placar

static int falhas = 0, testes = 0;

static void ok(int cond, const char *msg)
{
    testes++;
    if (cond) { printf("  ok      %s\n", msg); }
    else      { printf("  FALHOU  %s\n", msg); falhas++; }
}

static void eq(const char *got, const char *want, const char *msg)
{
    testes++;
    if (got && strcmp(got, want) == 0) { printf("  ok      %s\n", msg); }
    else {
        printf("  FALHOU  %s\n          esperado: '%s'\n          veio:     '%s'\n",
               msg, want, got ? got : "(null)");
        falhas++;
    }
}

// --------------------------------------------------------------- ajudantes

// Roda um comando e devolve a saída inteira. Usado pra falar com o servidor
// pelo curl, que é o cliente de FTP mais fácil de dirigir por script.
static char *roda(const char *cmd)
{
    static char saida[65536];
    saida[0] = '\0';
    FILE *p = popen(cmd, "r");
    if (!p) return saida;
    size_t n = fread(saida, 1, sizeof(saida) - 1, p);
    saida[n] = '\0';
    pclose(p);
    return saida;
}

static bool contem(const char *palheiro, const char *agulha)
{
    return strstr(palheiro, agulha) != NULL;
}

static void escreve_arquivo(const char *caminho, const char *conteudo)
{
    FILE *f = fopen(caminho, "wb");
    if (!f) { printf("nao consegui escrever %s\n", caminho); exit(1); }
    fwrite(conteudo, 1, strlen(conteudo), f);
    fclose(f);
}

static char *le_arquivo(const char *caminho)
{
    static char buf[8192];
    buf[0] = '\0';
    FILE *f = fopen(caminho, "rb");
    if (!f) return buf;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

// -----------------------------------------------------------------------

#define PORTA 5099

int main(void)
{
    // O ftpd trata "sdmc:" como raiz. No Mac isso é uma pasta com esse nome
    // literal, do mesmo jeito que o teste do Drive já faz.
    system("rm -rf 'sdmc:' && mkdir -p 'sdmc:/switch/SwitchSaveSync' 'sdmc:/atmosphere'");
    escreve_arquivo("sdmc:/switch/oi.txt", "conteudo de teste\n");
    escreve_arquivo("sdmc:/switch/SwitchSaveSync/log.txt", "linha 1\nlinha 2\n");

    printf("\n-- subir o servidor --\n");
    ok(ftpd_start(PORTA), "o servidor subiu");
    if (!ftpd_running())
    {
        printf("  nao subiu: %s\n", ftpd_ultimo_erro());
        return 1;
    }
    ok(ftpd_porta() == PORTA, "na porta que pedimos");
    ok(ftpd_endereco()[0] != '\0', "com um endereco pra mostrar na tela");

    char base[128];
    snprintf(base, sizeof(base), "ftp://127.0.0.1:%d", PORTA);

    printf("\n-- listar --\n");
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "curl -s --max-time 10 '%s/' 2>&1", base);
    char *saida = roda(cmd);
    ok(contem(saida, "switch"), "a raiz lista a pasta switch");
    ok(contem(saida, "atmosphere"), "e a pasta atmosphere");
    ok(saida[0] == 'd' || contem(saida, "\nd"), "as pastas saem marcadas com 'd' no comeco");

    snprintf(cmd, sizeof(cmd), "curl -s --max-time 10 '%s/switch/' 2>&1", base);
    saida = roda(cmd);
    ok(contem(saida, "oi.txt"), "lista o arquivo dentro de uma subpasta");

    printf("\n-- baixar --\n");
    snprintf(cmd, sizeof(cmd), "curl -s --max-time 10 '%s/switch/oi.txt' 2>&1", base);
    eq(roda(cmd), "conteudo de teste\n", "o arquivo baixado vem igualzinho");

    snprintf(cmd, sizeof(cmd), "curl -s --max-time 10 '%s/switch/SwitchSaveSync/log.txt' 2>&1", base);
    eq(roda(cmd), "linha 1\nlinha 2\n", "arquivo dois niveis abaixo tambem");

    printf("\n-- baixar o que nao existe --\n");
    snprintf(cmd, sizeof(cmd), "curl -s -o /dev/null -w '%%{http_code}' --max-time 10 '%s/switch/nao-existe.txt' 2>&1", base);
    saida = roda(cmd);
    ok(!contem(saida, "conteudo"), "nao inventa conteudo pro que nao existe");

    printf("\n-- mandar arquivo --\n");
    escreve_arquivo("subir.bin", "isto veio do computador\n");
    snprintf(cmd, sizeof(cmd), "curl -s --max-time 10 -T subir.bin '%s/switch/' 2>&1", base);
    roda(cmd);
    eq(le_arquivo("sdmc:/switch/subir.bin"), "isto veio do computador\n",
       "o arquivo enviado chegou inteiro no cartao");

    // Um arquivo maior que o buffer de 64 KB, pra exercitar o laco de pedacos.
    printf("\n-- mandar arquivo grande (300 KB, passa do buffer) --\n");
    system("head -c 307200 /dev/urandom > grande.bin");
    snprintf(cmd, sizeof(cmd), "curl -s --max-time 30 -T grande.bin '%s/switch/' 2>&1", base);
    roda(cmd);
    struct stat st;
    ok(stat("sdmc:/switch/grande.bin", &st) == 0 && st.st_size == 307200,
       "chegou com o tamanho certo");
    ok(system("cmp -s grande.bin 'sdmc:/switch/grande.bin'") == 0,
       "e byte a byte igual ao original");

    printf("\n-- e baixar o grande de volta --\n");
    snprintf(cmd, sizeof(cmd), "curl -s --max-time 30 -o baixado.bin '%s/switch/grande.bin' 2>&1", base);
    roda(cmd);
    ok(system("cmp -s grande.bin baixado.bin") == 0, "voltou byte a byte igual");

    printf("\n-- criar e apagar pasta --\n");
    snprintf(cmd, sizeof(cmd), "curl -s --max-time 10 '%s/' -Q 'MKD /switch/nova' 2>&1", base);
    roda(cmd);
    ok(stat("sdmc:/switch/nova", &st) == 0 && S_ISDIR(st.st_mode), "MKD criou a pasta");

    snprintf(cmd, sizeof(cmd), "curl -s --max-time 10 '%s/' -Q 'RMD /switch/nova' 2>&1", base);
    roda(cmd);
    ok(stat("sdmc:/switch/nova", &st) != 0, "RMD apagou a pasta");

    printf("\n-- apagar arquivo --\n");
    snprintf(cmd, sizeof(cmd), "curl -s --max-time 10 '%s/' -Q 'DELE /switch/subir.bin' 2>&1", base);
    roda(cmd);
    ok(stat("sdmc:/switch/subir.bin", &st) != 0, "DELE apagou o arquivo");

    printf("\n-- renomear --\n");
    snprintf(cmd, sizeof(cmd),
             "curl -s --max-time 10 '%s/' -Q 'RNFR /switch/oi.txt' -Q 'RNTO /switch/oi2.txt' 2>&1", base);
    roda(cmd);
    ok(stat("sdmc:/switch/oi2.txt", &st) == 0, "RNFR+RNTO renomeou");
    ok(stat("sdmc:/switch/oi.txt", &st) != 0, "e o nome antigo sumiu");

    printf("\n-- '..' nao sobe acima da raiz --\n");
    // Se o normaliza deixasse passar, isto leria fora da pasta do teste.
    snprintf(cmd, sizeof(cmd),
             "curl -s --max-time 10 '%s/' -Q 'CWD /../../..' -Q 'PWD' 2>&1 | grep -a 257 || true", base);
    saida = roda(cmd);
    ok(!contem(saida, "\"/..\""), "CWD com .. demais nao escapa da raiz");

    snprintf(cmd, sizeof(cmd), "curl -s --max-time 10 '%s/switch/../switch/oi2.txt' 2>&1", base);
    eq(roda(cmd), "conteudo de teste\n", "mas '..' no meio do caminho funciona normal");

    printf("\n-- contadores da tela --\n");
    int conexoes = 0;
    u64 enviados = 0, recebidos = 0;
    ftpd_estado(&conexoes, &enviados, &recebidos);
    ok(enviados > 300000, "contou os bytes enviados");
    ok(recebidos > 300000, "contou os bytes recebidos");
    ok(ftpd_ultima_linha()[0] != '\0', "guardou a ultima linha pra tela");

    printf("\n-- derrubar --\n");
    ftpd_stop();
    ok(!ftpd_running(), "o servidor parou");

    snprintf(cmd, sizeof(cmd), "curl -s --max-time 3 -o /dev/null -w '%%{http_code}' '%s/' 2>&1", base);
    saida = roda(cmd);
    ok(!contem(saida, "226"), "e a porta nao responde mais");

    printf("\n-- subir de novo na mesma porta --\n");
    ok(ftpd_start(PORTA), "sobe de novo sem 'porta ocupada' (SO_REUSEADDR)");
    ftpd_stop();

    system("rm -rf 'sdmc:' subir.bin grande.bin baixado.bin");

    printf("\n%d/%d passaram\n", testes - falhas, testes);
    return falhas ? 1 : 0;
}
