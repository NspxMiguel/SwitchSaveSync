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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
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

    // ------------------------------------------------------------------
    // Os tres abaixo sao os defeitos que a revisao achou. Cada um estava a um
    // cliente de distancia de acontecer na casa dele.
    // ------------------------------------------------------------------

    printf("\n-- caminho comprido no PWD (o %%s de 766 bytes) --\n");
    // O responde() usava o retorno do vsnprintf como indice e escrevia o \r\n
    // depois do fim de um buffer de 512 na pilha. O PWD e' justamente quem
    // devolve o caminho inteiro. Sob o sanitizador, se voltar, isto aborta.
    {
        char fundo[8192];
        strcpy(fundo, "sdmc:");
        // 8 niveis de 80 caracteres = ~730, dentro do que o FAT aceita por
        // componente (255) e acima dos 509 que cabiam na resposta.
        for (int i = 0; i < 8; i++)
        {
            strcat(fundo, "/");
            for (int j = 0; j < 80; j++) strcat(fundo, "a");
            mkdir(fundo, 0777);
        }
        const char *dentro = fundo + 5; // sem o "sdmc:"
        snprintf(cmd, sizeof(cmd),
                 "curl -s --max-time 10 '%s/' -Q 'CWD %s' -Q 'PWD' 2>&1 | head -c 200", base, dentro);
        roda(cmd);
        ok(ftpd_running(), "o servidor continua de pe depois de um PWD comprido");
    }

    printf("\n-- renomear pra ele mesmo nao pode sumir com o arquivo --\n");
    escreve_arquivo("sdmc:/switch/eu.sav", "progresso de dois anos\n");
    snprintf(cmd, sizeof(cmd),
             "curl -s --max-time 10 '%s/' -Q 'RNFR /switch/eu.sav' -Q 'RNTO /switch/eu.sav' 2>&1", base);
    roda(cmd);
    ok(stat("sdmc:/switch/eu.sav", &st) == 0, "o arquivo continua existindo");
    eq(le_arquivo("sdmc:/switch/eu.sav"), "progresso de dois anos\n",
       "e com o conteudo intacto");

    printf("\n-- cliente que para de ler nao pode prender o ftpd_stop --\n");
    // Este e o que travava o console: o envio girava no EAGAIN sem olhar o
    // g_parar, entao sair da tela (que faz join na thread) nunca voltava.
    {
        system("head -c 4194304 /dev/urandom > 'sdmc:/switch/pesado.bin'");

        int ctrl = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = { 0 };
        a.sin_family = AF_INET;
        a.sin_port   = htons(PORTA);
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        ok(connect(ctrl, (struct sockaddr *)&a, sizeof(a)) == 0, "conectei no controle");

        char resp[512];
        recv(ctrl, resp, sizeof(resp), 0); // 220

        send(ctrl, "PASV\r\n", 6, 0);
        ssize_t rn = recv(ctrl, resp, sizeof(resp) - 1, 0);
        resp[rn > 0 ? rn : 0] = '\0';

        int p1 = 0, p2 = 0;
        char *par = strrchr(resp, '(');
        int achou = par && sscanf(par, "(%*d,%*d,%*d,%*d,%d,%d)", &p1, &p2) == 2;
        ok(achou, "li a porta do PASV");

        if (achou)
        {
            int dados = socket(AF_INET, SOCK_STREAM, 0);
            int pequeno = 2048;
            setsockopt(dados, SOL_SOCKET, SO_RCVBUF, &pequeno, sizeof(pequeno));
            a.sin_port = htons((u16)(p1 * 256 + p2));
            ok(connect(dados, (struct sockaddr *)&a, sizeof(a)) == 0, "abri a conexao de dados");

            send(ctrl, "RETR /switch/pesado.bin\r\n", 26, 0);

            // Deixa o servidor encher a rede e NAO le nada: e o "pausei o
            // download no FileZilla" / "o notebook dormiu".
            struct timespec espera = { 2, 0 };
            nanosleep(&espera, NULL);

            struct timeval t0, t1;
            gettimeofday(&t0, NULL);
            ftpd_stop();
            gettimeofday(&t1, NULL);

            double levou = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
            printf("          (o ftpd_stop levou %.2f s)\n", levou);
            ok(levou < 3.0, "o ftpd_stop voltou depressa em vez de travar pra sempre");
            ok(!ftpd_running(), "e o servidor parou de verdade");

            close(dados);
        }
        close(ctrl);
        remove("sdmc:/switch/pesado.bin");

        ok(ftpd_start(PORTA), "sobe de novo pro resto do teste");
    }

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
