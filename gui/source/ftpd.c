#include "ftpd.h"

#include <arpa/inet.h>
#include <stdarg.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Limites
//
// Números pequenos de propósito: isto roda ao lado da interface gráfica, que
// já come a memória do console. Quatro conexões dão conta do Finder (que abre
// duas ou três) e sobra.
// ---------------------------------------------------------------------------

#define MAX_SESSOES   4
#define BUF_TRANSF    (64 * 1024)
#define RAIZ          "sdmc:"
#define CAMINHO_MAX   768
// Caminho do console = caminho FTP + o prefixo "sdmc:". Ter um nome proprio
// pro tamanho evita o snprintf truncando calado na conversao.
#define DISCO_MAX     (CAMINHO_MAX + 16)

typedef enum {
    PARADO = 0,
    ESPERANDO_DATA, // PASV feito, cliente ainda não conectou
    ENVIANDO,
    RECEBENDO,
    LISTANDO,
} Transferencia;

typedef struct {
    bool  usada;
    int   ctrl;          // socket de controle
    int   escuta_data;   // socket que espera a conexão de dados (PASV)
    int   data;          // socket de dados já conectado

    char  entrada[1024]; // comandos que chegaram e ainda não viraram linha
    size_t entrada_n;

    char  cwd[CAMINHO_MAX];   // caminho FTP, sempre começando com '/'
    char  renomear[DISCO_MAX];   // caminho do console, guardado entre RNFR e RNTO

    Transferencia estado;
    FILE *arquivo;
    char *lista;       // texto do LIST, montado de uma vez
    size_t lista_n, lista_enviado;
} Sessao;

static Thread   g_thread;
static bool     g_rodando  = false;
static bool     g_parar    = false;
static int      g_escuta   = -1;
static u16      g_porta    = 0;
static char     g_ip[32]   = {0};
static char     g_erro[160] = {0};
static char     g_ultima[256] = {0};
static Sessao   g_sessoes[MAX_SESSOES];
static u64      g_enviados = 0, g_recebidos = 0;
static Mutex    g_trava;

// ---------------------------------------------------------------------------
// Caminhos
//
// O cliente fala em caminho unix ("/switch/x.nro"); o console fala em devoptab
// ("sdmc:/switch/x.nro"). A conversão é literal, e o cuidado que importa é não
// deixar ".." subir acima da raiz — senão "cd ../../.." vira um passeio por
// "sdmc:/../..", que no newlib não dá em lugar nenhum bom.
// ---------------------------------------------------------------------------

static void normaliza(const char *entra, char *sai, size_t saisz)
{
    char pilha[CAMINHO_MAX];
    size_t n = 0;
    pilha[0] = '\0';

    const char *p = entra;
    while (*p)
    {
        while (*p == '/') p++;
        if (!*p) break;

        const char *ini = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - ini);

        if (len == 1 && ini[0] == '.')
            continue;

        if (len == 2 && ini[0] == '.' && ini[1] == '.')
        {
            char *barra = strrchr(pilha, '/');
            if (barra) { *barra = '\0'; n = strlen(pilha); }
            else       { pilha[0] = '\0'; n = 0; }
            continue;
        }

        if (n + len + 2 >= sizeof(pilha)) break;
        pilha[n++] = '/';
        memcpy(pilha + n, ini, len);
        n += len;
        pilha[n] = '\0';
    }

    snprintf(sai, saisz, "%s", pilha[0] ? pilha : "/");
}

// Caminho FTP (relativo ou absoluto) -> caminho do console.
static void resolve(const Sessao *s, const char *arg, char *ftp_out, size_t ftpsz,
                    char *disco_out, size_t discosz)
{
    char junto[CAMINHO_MAX * 2];
    if (arg && arg[0] == '/')
        snprintf(junto, sizeof(junto), "%s", arg);
    else
        snprintf(junto, sizeof(junto), "%s/%s", s->cwd, arg ? arg : "");

    normaliza(junto, ftp_out, ftpsz);
    snprintf(disco_out, discosz, "%s%s", RAIZ, ftp_out);
}

// ---------------------------------------------------------------------------
// Falar com o cliente
// ---------------------------------------------------------------------------

static void anota(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void anota(const char *fmt, ...)
{
    char linha[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(linha, sizeof(linha), fmt, ap);
    va_end(ap);

    mutexLock(&g_trava);
    snprintf(g_ultima, sizeof(g_ultima), "%s", linha);
    mutexUnlock(&g_trava);
}

static void responde(Sessao *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void responde(Sessao *s, const char *fmt, ...)
{
    char linha[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(linha, sizeof(linha) - 3, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    linha[n++] = '\r';
    linha[n++] = '\n';
    send(s->ctrl, linha, (size_t)n, 0);
}

static void fecha_data(Sessao *s)
{
    if (s->data >= 0)        { close(s->data); s->data = -1; }
    if (s->escuta_data >= 0) { close(s->escuta_data); s->escuta_data = -1; }
    if (s->arquivo)          { fclose(s->arquivo); s->arquivo = NULL; }
    free(s->lista);
    s->lista = NULL;
    s->lista_n = s->lista_enviado = 0;
    s->estado = PARADO;
}

static void fecha_sessao(Sessao *s)
{
    fecha_data(s);
    if (s->ctrl >= 0) { close(s->ctrl); s->ctrl = -1; }
    s->usada = false;
    s->entrada_n = 0;
}

// ---------------------------------------------------------------------------
// PASV: abrir uma porta e dizer qual é
// ---------------------------------------------------------------------------

static bool abre_pasv(Sessao *s)
{
    fecha_data(s);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = 0; // que o sistema escolha

    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 1) < 0)
    {
        close(fd);
        return false;
    }

    socklen_t tam = sizeof(a);
    if (getsockname(fd, (struct sockaddr *)&a, &tam) < 0)
    {
        close(fd);
        return false;
    }

    fcntl(fd, F_SETFL, O_NONBLOCK);
    s->escuta_data = fd;
    s->estado = ESPERANDO_DATA;

    u16 porta = ntohs(a.sin_port);
    unsigned ip[4] = {0};
    sscanf(g_ip, "%u.%u.%u.%u", &ip[0], &ip[1], &ip[2], &ip[3]);

    responde(s, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
             ip[0], ip[1], ip[2], ip[3], porta >> 8, porta & 0xFF);
    return true;
}

// ---------------------------------------------------------------------------
// LIST
// ---------------------------------------------------------------------------

static void junta(char **buf, size_t *n, size_t *cap, const char *texto)
{
    size_t len = strlen(texto);
    if (*n + len + 1 > *cap)
    {
        size_t novo = (*cap ? *cap * 2 : 4096);
        while (novo < *n + len + 1) novo *= 2;
        char *p = realloc(*buf, novo);
        if (!p) return;
        *buf = p;
        *cap = novo;
    }
    memcpy(*buf + *n, texto, len);
    *n += len;
    (*buf)[*n] = '\0';
}

static void monta_lista(Sessao *s, const char *disco, bool so_nomes)
{
    size_t cap = 0;
    s->lista = NULL;
    s->lista_n = 0;
    s->lista_enviado = 0;

    DIR *d = opendir(disco);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        char linha[CAMINHO_MAX + 128];
        if (so_nomes)
        {
            snprintf(linha, sizeof(linha), "%s\r\n", e->d_name);
            junta(&s->lista, &s->lista_n, &cap, linha);
            continue;
        }

        char completo[CAMINHO_MAX * 2];
        snprintf(completo, sizeof(completo), "%s/%s", disco, e->d_name);

        struct stat st;
        bool tem = stat(completo, &st) == 0;
        bool dir = tem && S_ISDIR(st.st_mode);
        long long tamanho = tem ? (long long)st.st_size : 0;

        // Data: o FAT do cartão guarda mtime, e cliente nenhum liga muito pra
        // isso. Formato clássico do ls, que é o que todo cliente sabe ler.
        char data[32] = "Jan  1  2020";
        if (tem)
        {
            struct tm tmv;
            time_t t = st.st_mtime;
            if (localtime_r(&t, &tmv))
                strftime(data, sizeof(data), "%b %e %H:%M", &tmv);
        }

        snprintf(linha, sizeof(linha), "%crw-rw-rw-   1 switch switch %12lld %s %s\r\n",
                 dir ? 'd' : '-', tamanho, data, e->d_name);
        junta(&s->lista, &s->lista_n, &cap, linha);
    }
    closedir(d);

    if (!s->lista) // pasta vazia: o cliente espera corpo vazio, não erro
    {
        s->lista = strdup("");
        s->lista_n = 0;
    }
}

// ---------------------------------------------------------------------------
// Os comandos
// ---------------------------------------------------------------------------

static void comando(Sessao *s, char *linha)
{
    char *arg = strchr(linha, ' ');
    if (arg) { *arg = '\0'; arg++; while (*arg == ' ') arg++; }

    for (char *p = linha; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;

    // Nada aqui registra conteúdo de arquivo nem senha (não existe senha).
    if (strcmp(linha, "PASS") != 0)
        anota("%s%s%s", linha, arg ? " " : "", arg ? arg : "");

    if (!strcmp(linha, "USER")) { responde(s, "230 Sem senha, entra"); return; }
    if (!strcmp(linha, "PASS")) { responde(s, "230 Sem senha, entra"); return; }
    if (!strcmp(linha, "SYST")) { responde(s, "215 UNIX Type: L8"); return; }
    if (!strcmp(linha, "NOOP")) { responde(s, "200 ok"); return; }
    if (!strcmp(linha, "TYPE")) { responde(s, "200 ok"); return; }
    if (!strcmp(linha, "OPTS"))
    {
        // "OPTS UTF8 ON" — o Finder manda logo de cara. Tudo aqui já é UTF-8.
        responde(s, "200 ok");
        return;
    }
    if (!strcmp(linha, "FEAT"))
    {
        responde(s, "211-Extensoes");
        responde(s, " UTF8");
        responde(s, " SIZE");
        responde(s, " PASV");
        responde(s, "211 fim");
        return;
    }
    if (!strcmp(linha, "QUIT"))
    {
        responde(s, "221 tchau");
        fecha_sessao(s);
        return;
    }
    if (!strcmp(linha, "PWD"))
    {
        responde(s, "257 \"%s\"", s->cwd);
        return;
    }
    if (!strcmp(linha, "CWD") || !strcmp(linha, "CDUP"))
    {
        char ftp[CAMINHO_MAX], disco[DISCO_MAX];
        resolve(s, !strcmp(linha, "CDUP") ? ".." : (arg ? arg : "/"), ftp, sizeof(ftp), disco, sizeof(disco));

        struct stat st;
        if (stat(disco, &st) != 0 || !S_ISDIR(st.st_mode))
        {
            responde(s, "550 nao existe");
            return;
        }
        snprintf(s->cwd, sizeof(s->cwd), "%s", ftp);
        responde(s, "250 ok");
        return;
    }
    if (!strcmp(linha, "PASV"))
    {
        if (!abre_pasv(s)) responde(s, "425 nao consegui abrir a porta de dados");
        return;
    }
    if (!strcmp(linha, "SIZE"))
    {
        char ftp[CAMINHO_MAX], disco[DISCO_MAX];
        resolve(s, arg, ftp, sizeof(ftp), disco, sizeof(disco));
        struct stat st;
        if (stat(disco, &st) == 0 && S_ISREG(st.st_mode))
            responde(s, "213 %lld", (long long)st.st_size);
        else
            responde(s, "550 nao existe");
        return;
    }
    if (!strcmp(linha, "DELE"))
    {
        char ftp[CAMINHO_MAX], disco[DISCO_MAX];
        resolve(s, arg, ftp, sizeof(ftp), disco, sizeof(disco));
        responde(s, remove(disco) == 0 ? "250 apagado" : "550 nao consegui apagar");
        return;
    }
    if (!strcmp(linha, "MKD"))
    {
        char ftp[CAMINHO_MAX], disco[DISCO_MAX];
        resolve(s, arg, ftp, sizeof(ftp), disco, sizeof(disco));
        if (mkdir(disco, 0777) == 0) responde(s, "257 \"%s\" criada", ftp);
        else                          responde(s, "550 nao consegui criar");
        return;
    }
    if (!strcmp(linha, "RMD"))
    {
        char ftp[CAMINHO_MAX], disco[DISCO_MAX];
        resolve(s, arg, ftp, sizeof(ftp), disco, sizeof(disco));
        responde(s, rmdir(disco) == 0 ? "250 apagada" : "550 nao consegui apagar");
        return;
    }
    if (!strcmp(linha, "RNFR"))
    {
        char ftp[CAMINHO_MAX], disco[DISCO_MAX];
        resolve(s, arg, ftp, sizeof(ftp), disco, sizeof(disco));
        struct stat st;
        if (stat(disco, &st) != 0) { responde(s, "550 nao existe"); return; }
        snprintf(s->renomear, sizeof(s->renomear), "%s", disco);
        responde(s, "350 e pra qual nome?");
        return;
    }
    if (!strcmp(linha, "RNTO"))
    {
        if (!s->renomear[0]) { responde(s, "503 faltou o RNFR"); return; }
        char ftp[CAMINHO_MAX], disco[DISCO_MAX];
        resolve(s, arg, ftp, sizeof(ftp), disco, sizeof(disco));
        // No FAT o rename não sobrescreve; tirar o de destino antes é o que
        // faz "mandar por cima" funcionar, que é o uso normal aqui.
        remove(disco);
        responde(s, rename(s->renomear, disco) == 0 ? "250 renomeado" : "550 nao consegui");
        s->renomear[0] = '\0';
        return;
    }
    if (!strcmp(linha, "LIST") || !strcmp(linha, "NLST"))
    {
        if (s->estado != ESPERANDO_DATA) { responde(s, "425 faltou o PASV"); return; }
        char ftp[CAMINHO_MAX], disco[DISCO_MAX];
        // O Finder manda "LIST -a"; a flag não é caminho.
        const char *alvo = (arg && arg[0] == '-') ? NULL : arg;
        resolve(s, alvo ? alvo : ".", ftp, sizeof(ftp), disco, sizeof(disco));
        monta_lista(s, disco, !strcmp(linha, "NLST"));
        responde(s, "150 mandando a lista");
        s->estado = LISTANDO;
        return;
    }
    if (!strcmp(linha, "RETR"))
    {
        if (s->estado != ESPERANDO_DATA) { responde(s, "425 faltou o PASV"); return; }
        char ftp[CAMINHO_MAX], disco[DISCO_MAX];
        resolve(s, arg, ftp, sizeof(ftp), disco, sizeof(disco));
        s->arquivo = fopen(disco, "rb");
        if (!s->arquivo) { responde(s, "550 nao consegui abrir"); fecha_data(s); return; }
        responde(s, "150 mandando o arquivo");
        s->estado = ENVIANDO;
        return;
    }
    if (!strcmp(linha, "STOR"))
    {
        if (s->estado != ESPERANDO_DATA) { responde(s, "425 faltou o PASV"); return; }
        char ftp[CAMINHO_MAX], disco[DISCO_MAX];
        resolve(s, arg, ftp, sizeof(ftp), disco, sizeof(disco));
        s->arquivo = fopen(disco, "wb");
        if (!s->arquivo) { responde(s, "550 nao consegui escrever"); fecha_data(s); return; }
        responde(s, "150 pode mandar");
        s->estado = RECEBENDO;
        return;
    }

    responde(s, "502 nao conheco esse comando");
}

// ---------------------------------------------------------------------------
// Bombear a transferência: um pedaço por volta do poll
// ---------------------------------------------------------------------------

static void bombeia(Sessao *s)
{
    if (s->data < 0) return;

    if (s->estado == LISTANDO)
    {
        if (s->lista_enviado >= s->lista_n)
        {
            fecha_data(s);
            responde(s, "226 fim da lista");
            return;
        }
        ssize_t n = send(s->data, s->lista + s->lista_enviado,
                         s->lista_n - s->lista_enviado, 0);
        if (n <= 0)
        {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            fecha_data(s);
            responde(s, "426 a conexao caiu");
            return;
        }
        s->lista_enviado += (size_t)n;
        return;
    }

    if (s->estado == ENVIANDO)
    {
        static char buf[BUF_TRANSF]; // uma transferência por vez, uma thread só
        size_t lidos = fread(buf, 1, sizeof(buf), s->arquivo);
        if (lidos == 0)
        {
            fecha_data(s);
            responde(s, "226 acabou");
            return;
        }
        size_t mandados = 0;
        while (mandados < lidos)
        {
            ssize_t n = send(s->data, buf + mandados, lidos - mandados, 0);
            if (n <= 0)
            {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { svcSleepThread(1000000ULL); continue; }
                fecha_data(s);
                responde(s, "426 a conexao caiu");
                return;
            }
            mandados += (size_t)n;
        }
        mutexLock(&g_trava);
        g_enviados += lidos;
        mutexUnlock(&g_trava);
        return;
    }

    if (s->estado == RECEBENDO)
    {
        static char buf[BUF_TRANSF];
        ssize_t n = recv(s->data, buf, sizeof(buf), 0);
        if (n == 0) // o cliente fechou: fim do arquivo
        {
            fecha_data(s);
            responde(s, "226 recebido");
            return;
        }
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            fecha_data(s);
            responde(s, "426 a conexao caiu");
            return;
        }
        if (fwrite(buf, 1, (size_t)n, s->arquivo) != (size_t)n)
        {
            fecha_data(s);
            responde(s, "552 nao coube no cartao");
            return;
        }
        mutexLock(&g_trava);
        g_recebidos += (u64)n;
        mutexUnlock(&g_trava);
        return;
    }
}

// ---------------------------------------------------------------------------
// O laço
// ---------------------------------------------------------------------------

static void trata_entrada(Sessao *s)
{
    char buf[512];
    ssize_t n = recv(s->ctrl, buf, sizeof(buf), 0);
    if (n <= 0)
    {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        fecha_sessao(s);
        return;
    }

    for (ssize_t i = 0; i < n; i++)
    {
        char c = buf[i];
        if (c == '\n')
        {
            while (s->entrada_n && (s->entrada[s->entrada_n - 1] == '\r' ||
                                    s->entrada[s->entrada_n - 1] == ' '))
                s->entrada_n--;
            s->entrada[s->entrada_n] = '\0';
            if (s->entrada_n) comando(s, s->entrada);
            s->entrada_n = 0;
            if (!s->usada) return; // QUIT fechou a sessão
        }
        else if (s->entrada_n < sizeof(s->entrada) - 1)
        {
            s->entrada[s->entrada_n++] = c;
        }
    }
}

static void laco(void *_)
{
    (void)_;

    while (!g_parar)
    {
        struct pollfd fds[1 + MAX_SESSOES * 2];
        int mapa[1 + MAX_SESSOES * 2];
        int tipo[1 + MAX_SESSOES * 2]; // 0=escuta, 1=ctrl, 2=data
        int n = 0;

        fds[n].fd = g_escuta; fds[n].events = POLLIN; mapa[n] = -1; tipo[n] = 0; n++;

        for (int i = 0; i < MAX_SESSOES; i++)
        {
            Sessao *s = &g_sessoes[i];
            if (!s->usada) continue;

            fds[n].fd = s->ctrl; fds[n].events = POLLIN; mapa[n] = i; tipo[n] = 1; n++;

            if (s->escuta_data >= 0)
            {
                fds[n].fd = s->escuta_data; fds[n].events = POLLIN; mapa[n] = i; tipo[n] = 2; n++;
            }
            else if (s->data >= 0)
            {
                fds[n].fd = s->data;
                fds[n].events = (s->estado == RECEBENDO) ? POLLIN : POLLOUT;
                mapa[n] = i; tipo[n] = 2; n++;
            }
        }

        int pronto = poll(fds, (nfds_t)n, 200);
        if (pronto <= 0) continue;

        for (int k = 0; k < n; k++)
        {
            if (!(fds[k].revents & (POLLIN | POLLOUT | POLLHUP | POLLERR))) continue;

            if (tipo[k] == 0)
            {
                int novo = accept(g_escuta, NULL, NULL);
                if (novo < 0) continue;

                int livre = -1;
                for (int i = 0; i < MAX_SESSOES; i++)
                    if (!g_sessoes[i].usada) { livre = i; break; }

                if (livre < 0)
                {
                    const char *cheio = "421 muitas conexoes\r\n";
                    send(novo, cheio, strlen(cheio), 0);
                    close(novo);
                    continue;
                }

                Sessao *s = &g_sessoes[livre];
                memset(s, 0, sizeof(*s));
                s->usada = true;
                s->ctrl = novo;
                s->escuta_data = s->data = -1;
                snprintf(s->cwd, sizeof(s->cwd), "/");
                fcntl(novo, F_SETFL, O_NONBLOCK);
                responde(s, "220 SwitchSaveSync FTP (desenvolvimento)");
                anota("conectou");
                continue;
            }

            Sessao *s = &g_sessoes[mapa[k]];
            if (!s->usada) continue;

            if (tipo[k] == 1)
            {
                trata_entrada(s);
                continue;
            }

            // dados
            if (s->escuta_data >= 0)
            {
                int novo = accept(s->escuta_data, NULL, NULL);
                if (novo < 0) continue;
                close(s->escuta_data);
                s->escuta_data = -1;
                s->data = novo;
                fcntl(novo, F_SETFL, O_NONBLOCK);
                continue;
            }

            bombeia(s);
        }
    }

    for (int i = 0; i < MAX_SESSOES; i++)
        if (g_sessoes[i].usada) fecha_sessao(&g_sessoes[i]);

    if (g_escuta >= 0) { close(g_escuta); g_escuta = -1; }
}

// ---------------------------------------------------------------------------

static void descobre_ip(void)
{
    g_ip[0] = '\0';
    u32 ip = gethostid();
    if (ip == 0 || ip == INADDR_LOOPBACK) return;
    struct in_addr a = { .s_addr = ip };
    snprintf(g_ip, sizeof(g_ip), "%s", inet_ntoa(a));
}

bool ftpd_start(u16 porta)
{
    if (g_rodando) return true;

    g_erro[0] = '\0';
    g_ultima[0] = '\0';
    g_enviados = g_recebidos = 0;
    memset(g_sessoes, 0, sizeof(g_sessoes));
    for (int i = 0; i < MAX_SESSOES; i++)
    {
        g_sessoes[i].ctrl = g_sessoes[i].data = g_sessoes[i].escuta_data = -1;
    }

    descobre_ip();
    if (!g_ip[0])
    {
        snprintf(g_erro, sizeof(g_erro), "sem endereco de rede — o app esta em modo applet ou o wi-fi caiu");
        return false;
    }

    g_escuta = socket(AF_INET, SOCK_STREAM, 0);
    if (g_escuta < 0)
    {
        snprintf(g_erro, sizeof(g_erro), "socket() falhou (%d)", errno);
        return false;
    }

    int um = 1;
    setsockopt(g_escuta, SOL_SOCKET, SO_REUSEADDR, &um, sizeof(um));

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(porta);

    if (bind(g_escuta, (struct sockaddr *)&a, sizeof(a)) < 0)
    {
        snprintf(g_erro, sizeof(g_erro), "a porta %u ja esta ocupada (o DBI usa a 5000)", porta);
        close(g_escuta); g_escuta = -1;
        return false;
    }
    if (listen(g_escuta, MAX_SESSOES) < 0)
    {
        snprintf(g_erro, sizeof(g_erro), "listen() falhou (%d)", errno);
        close(g_escuta); g_escuta = -1;
        return false;
    }
    fcntl(g_escuta, F_SETFL, O_NONBLOCK);

    g_porta = porta;
    g_parar = false;

    // Prioridade abaixo da thread principal: a interface gráfica não pode
    // engasgar por causa de uma cópia de arquivo.
    if (R_FAILED(threadCreate(&g_thread, laco, NULL, NULL, 0x8000, 0x2D, -2)) ||
        R_FAILED(threadStart(&g_thread)))
    {
        snprintf(g_erro, sizeof(g_erro), "nao consegui criar a thread do servidor");
        close(g_escuta); g_escuta = -1;
        return false;
    }

    g_rodando = true;
    return true;
}

void ftpd_stop(void)
{
    if (!g_rodando) return;
    g_parar = true;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    g_rodando = false;
}

bool ftpd_running(void) { return g_rodando; }
const char *ftpd_endereco(void) { return g_ip; }
u16 ftpd_porta(void) { return g_porta; }
const char *ftpd_ultimo_erro(void) { return g_erro; }

const char *ftpd_ultima_linha(void)
{
    static char copia[256];
    mutexLock(&g_trava);
    snprintf(copia, sizeof(copia), "%s", g_ultima);
    mutexUnlock(&g_trava);
    return copia;
}

void ftpd_estado(int *conexoes, u64 *enviados, u64 *recebidos)
{
    int n = 0;
    for (int i = 0; i < MAX_SESSOES; i++)
        if (g_sessoes[i].usada) n++;

    mutexLock(&g_trava);
    if (conexoes)  *conexoes = n;
    if (enviados)  *enviados = g_enviados;
    if (recebidos) *recebidos = g_recebidos;
    mutexUnlock(&g_trava);
}
