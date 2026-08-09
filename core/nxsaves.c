#include "nxsaves.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zlib.h>

// "NXSAVES" + 0x1A. O 0x1A é o EOF do DOS: com ele, um `type` no Windows ou um
// `cat` distraído para na primeira linha em vez de despejar binário no
// terminal. A versão não entra aqui — ela tem lugar próprio, em cab+8.
static const uint8_t MAGIC[8] = { 'N', 'X', 'S', 'A', 'V', 'E', 'S', 0x1A };

#define CABECALHO 40
#define VERSAO    1

#define FLAG_EMBARALHADO 0x1

#define METODO_CRU     0
#define METODO_DEFLATE 1

// 32 bytes fixos por entrada no índice, mais o nome.
#define ENTRADA_FIXA 32

#define PEDACO   16384
#define MAX_NOME 1000

// A chave do embaralhamento. Não é segredo — o código é público. Ela existe
// pra que o arquivo não seja um deflate cru que qualquer ferramenta abre por
// conversa privada removida do historico
#define CHAVE 0x5357495443484E58ull // "SWITCHNX"

typedef struct
{
    char *nome;
    uint64_t offset;    // onde os dados começam no arquivo
    uint64_t gravado;   // quantos bytes ocupam gravados
    uint64_t original;  // quantos bytes tinha antes de comprimir
    uint32_t crc;       // crc32 do conteúdo original
    uint8_t metodo;
} Entrada;

struct NxSavesWriter
{
    FILE *f;
    char path[512];
    Entrada *entradas;
    size_t n, cap;
    uint64_t pos;   // onde estamos no arquivo
    bool estragado; // deu erro em algum ponto; o close apaga tudo
};

// ---------------------------------------------------------------------------
// bytes
// ---------------------------------------------------------------------------

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)((v >> (i * 8)) & 0xff);
}

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)((v >> (i * 8)) & 0xff);
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t get32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= (uint32_t)p[i] << (i * 8);
    return v;
}

static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (i * 8);
    return v;
}

// ---------------------------------------------------------------------------
// embaralhamento
// ---------------------------------------------------------------------------

// splitmix64. Serve pra virar uma posição do arquivo em 8 bytes de lixo
// determinístico.
static uint64_t mistura(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// XOR com um fluxo derivado da POSIÇÃO no arquivo, não da ordem de escrita.
// É o que deixa a leitura poder pular direto pro meio do arquivo (o extract de
// um jogo só não precisa passar pelos outros) e o que faz a mesma função servir
// pra embaralhar e desembaralhar.
static void embaralha(uint8_t *buf, size_t n, uint64_t pos)
{
    for (size_t i = 0; i < n; i++)
    {
        uint64_t p = pos + i;
        uint64_t k = mistura(CHAVE ^ (p >> 3));
        buf[i] ^= (uint8_t)(k >> ((p & 7) * 8));
    }
}

// ---------------------------------------------------------------------------
// escrita
// ---------------------------------------------------------------------------

NxSavesWriter *nxsaves_open_write(const char *path)
{
    NxSavesWriter *b = calloc(1, sizeof(NxSavesWriter));
    if (!b)
        return NULL;

    b->f = fopen(path, "wb");
    if (!b->f)
    {
        free(b);
        return NULL;
    }

    snprintf(b->path, sizeof(b->path), "%s", path);

    // O cabeçalho de verdade só dá pra escrever no fim (ele aponta pro índice).
    // Aqui vai o espaço dele, zerado.
    uint8_t vazio[CABECALHO] = { 0 };
    if (fwrite(vazio, 1, sizeof(vazio), b->f) != sizeof(vazio))
    {
        fclose(b->f);
        remove(path);
        free(b);
        return NULL;
    }

    b->pos = CABECALHO;
    return b;
}

size_t nxsaves_entry_count(const NxSavesWriter *b) { return b ? b->n : 0; }
uint64_t nxsaves_bytes_written(const NxSavesWriter *b) { return b ? b->pos : 0; }

static bool guarda_entrada(NxSavesWriter *b, const Entrada *e)
{
    if (b->n == b->cap)
    {
        size_t nova    = b->cap ? b->cap * 2 : 32;
        Entrada *maior = realloc(b->entradas, nova * sizeof(Entrada));
        if (!maior)
            return false;
        b->entradas = maior;
        b->cap      = nova;
    }

    b->entradas[b->n++] = *e;
    return true;
}

// Escreve n bytes já embaralhados a partir da posição atual.
static bool escreve(NxSavesWriter *b, const uint8_t *dados, size_t n)
{
    uint8_t tmp[PEDACO];
    size_t feito = 0;

    while (feito < n)
    {
        size_t quanto = n - feito;
        if (quanto > sizeof(tmp))
            quanto = sizeof(tmp);

        memcpy(tmp, dados + feito, quanto);
        embaralha(tmp, quanto, b->pos);

        if (fwrite(tmp, 1, quanto, b->f) != quanto)
            return false;

        b->pos += quanto;
        feito += quanto;
    }
    return true;
}

// Um arquivo do cartão (ou do save montado) -> uma entrada.
//
// Comprime em fluxo, sem nunca ter o arquivo inteiro na memória: o save do
// Zelda tem ~1 MB por slot e no sysmodule a heap é apertada.
static bool poe_arquivo(NxSavesWriter *b, const char *nome, const char *local)
{
    if (strlen(nome) > MAX_NOME)
        return false;

    FILE *in = fopen(local, "rb");
    if (!in)
        return false;

    Entrada e = { 0 };
    e.offset  = b->pos;
    e.metodo  = METODO_DEFLATE;

    z_stream s = { 0 };
    if (deflateInit2(&s, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    {
        fclose(in);
        return false;
    }

    uint8_t entrada[PEDACO], saida[PEDACO];
    uint32_t crc = crc32(0L, Z_NULL, 0);
    bool ok      = true;
    int fim      = Z_NO_FLUSH;

    do
    {
        size_t n = fread(entrada, 1, sizeof(entrada), in);
        if (ferror(in))
        {
            ok = false;
            break;
        }

        e.original += n;
        crc        = crc32(crc, entrada, (uInt)n);
        s.next_in  = entrada;
        s.avail_in = (uInt)n;
        fim        = feof(in) ? Z_FINISH : Z_NO_FLUSH;

        do
        {
            s.next_out  = saida;
            s.avail_out = sizeof(saida);

            if (deflate(&s, fim) == Z_STREAM_ERROR)
            {
                ok = false;
                break;
            }

            size_t saiu = sizeof(saida) - s.avail_out;
            if (saiu && !escreve(b, saida, saiu))
            {
                ok = false;
                break;
            }

            e.gravado += saiu;
        } while (s.avail_out == 0 && ok);
    } while (ok && fim != Z_FINISH);

    deflateEnd(&s);
    fclose(in);

    if (!ok)
        return false;

    e.crc  = crc;
    e.nome = strdup(nome);
    if (!e.nome)
        return false;

    if (!guarda_entrada(b, &e))
    {
        free(e.nome);
        return false;
    }
    return true;
}

bool nxsaves_add_dir(NxSavesWriter *b, const char *prefix, const char *local_dir)
{
    if (!b || b->estragado)
        return false;

    DIR *d = opendir(local_dir);
    if (!d)
        return false;

    bool ok = true;
    struct dirent *ent;

    while (ok && (ent = readdir(d)) != NULL)
    {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;

        // A barra só entra se já não houver uma. O save montado é lido direto
        // de "save:/" — sem esse cuidado o caminho sairia "save://arquivo".
        size_t dlen    = strlen(local_dir);
        bool tem_barra = (dlen && local_dir[dlen - 1] == '/');

        char caminho[1024];
        snprintf(caminho, sizeof(caminho), tem_barra ? "%s%s" : "%s/%s", local_dir, ent->d_name);

        char nome[1024];
        snprintf(nome, sizeof(nome), "%s%s", prefix, ent->d_name);

        struct stat st;
        if (stat(caminho, &st) != 0)
        {
            ok = false;
            break;
        }

        if (S_ISDIR(st.st_mode))
        {
            // Corta aqui em vez de deixar o snprintf truncar: um prefixo
            // truncado no meio jogaria os arquivos dessa subpasta na pasta de
            // cima, e o restore os devolveria no lugar errado do save.
            char sub[1024];
            if (strlen(nome) + 2 > sizeof(sub))
            {
                ok = false;
                break;
            }
            snprintf(sub, sizeof(sub), "%s/", nome);
            ok = nxsaves_add_dir(b, sub, caminho);
        }
        else
        {
            ok = poe_arquivo(b, nome, caminho);
        }
    }

    closedir(d);

    if (!ok)
        b->estragado = true;
    return ok;
}

static void solta(NxSavesWriter *b)
{
    for (size_t i = 0; i < b->n; i++)
        free(b->entradas[i].nome);
    free(b->entradas);
    free(b);
}

void nxsaves_abort_write(NxSavesWriter *b)
{
    if (!b)
        return;

    if (b->f)
        fclose(b->f);
    remove(b->path);
    solta(b);
}

bool nxsaves_close_write(NxSavesWriter *b)
{
    if (!b)
        return false;

    if (b->estragado || b->n == 0)
    {
        nxsaves_abort_write(b);
        return false;
    }

    uint64_t off_indice = b->pos;
    uint64_t tam_indice = 0;
    uint32_t crc_indice = crc32(0L, Z_NULL, 0);
    bool ok             = true;

    for (size_t i = 0; ok && i < b->n; i++)
    {
        const Entrada *e = &b->entradas[i];
        size_t tam_nome  = strlen(e->nome);

        uint8_t reg[ENTRADA_FIXA + MAX_NOME];
        put16(reg + 0, (uint16_t)tam_nome);
        reg[2] = e->metodo;
        reg[3] = 0; // reservado
        put32(reg + 4, e->crc);
        put64(reg + 8, e->offset);
        put64(reg + 16, e->gravado);
        put64(reg + 24, e->original);
        memcpy(reg + ENTRADA_FIXA, e->nome, tam_nome);

        size_t total = ENTRADA_FIXA + tam_nome;

        // O crc do índice é do índice EM CLARO: assim ele também serve de
        // conferência de que o desembaralhamento saiu certo na leitura.
        crc_indice = crc32(crc_indice, reg, (uInt)total);

        ok = escreve(b, reg, total);
        tam_indice += total;
    }

    // Agora sim o cabeçalho, que só existia zerado. Fica em claro: é o que
    // permite reconhecer o arquivo sem ter que desembaralhar nada.
    if (ok)
    {
        uint8_t cab[CABECALHO] = { 0 };
        memcpy(cab, MAGIC, sizeof(MAGIC));
        put32(cab + 8, VERSAO);
        put32(cab + 12, FLAG_EMBARALHADO);
        put32(cab + 16, (uint32_t)b->n);
        put32(cab + 20, crc_indice);
        put64(cab + 24, off_indice);
        put64(cab + 32, tam_indice);

        ok = (fseek(b->f, 0, SEEK_SET) == 0)
            && fwrite(cab, 1, sizeof(cab), b->f) == sizeof(cab);
    }

    // fclose é onde cartão cheio aparece: o fwrite acima pode ter ido só pro
    // buffer. Sem conferir aqui, um arquivo truncado passaria por bom.
    if (fclose(b->f) != 0)
        ok = false;
    b->f = NULL;

    if (!ok)
    {
        remove(b->path);
        solta(b);
        return false;
    }

    solta(b);
    return true;
}

// ---------------------------------------------------------------------------
// leitura
// ---------------------------------------------------------------------------

typedef struct
{
    FILE *f;
    uint8_t *indice;    // o índice inteiro, já em claro
    uint64_t tam;
    uint32_t entradas;
} Leitor;

// Lê n bytes de `pos` e desembaralha.
static bool le_em(FILE *f, uint64_t pos, uint8_t *out, size_t n)
{
    if (fseek(f, (long)pos, SEEK_SET) != 0 || fread(out, 1, n, f) != n)
        return false;
    embaralha(out, n, pos);
    return true;
}

static bool abre(const char *path, Leitor *l)
{
    memset(l, 0, sizeof(*l));

    l->f = fopen(path, "rb");
    if (!l->f)
        return false;

    uint8_t cab[CABECALHO];
    if (fread(cab, 1, sizeof(cab), l->f) != sizeof(cab) || memcmp(cab, MAGIC, sizeof(MAGIC)) != 0
        || get32(cab + 8) != VERSAO)
    {
        fclose(l->f);
        return false;
    }

    l->entradas       = get32(cab + 16);
    uint32_t crc_diz  = get32(cab + 20);
    uint64_t off      = get64(cab + 24);
    l->tam            = get64(cab + 32);

    // Um índice absurdo aqui é arquivo corrompido, e o malloc abaixo levaria
    // junto. 64 MB dá pra centenas de milhares de arquivos.
    if (l->tam == 0 || l->tam > 64ull * 1024 * 1024 || off < CABECALHO)
    {
        fclose(l->f);
        return false;
    }

    l->indice = malloc((size_t)l->tam);
    if (!l->indice)
    {
        fclose(l->f);
        return false;
    }

    if (!le_em(l->f, off, l->indice, (size_t)l->tam)
        || crc32(crc32(0L, Z_NULL, 0), l->indice, (uInt)l->tam) != crc_diz)
    {
        free(l->indice);
        fclose(l->f);
        return false;
    }

    return true;
}

static void fecha(Leitor *l)
{
    free(l->indice);
    if (l->f)
        fclose(l->f);
}

// Percorre o índice. cb devolve false pra parar.
typedef bool (*cada_cb)(const char *nome, size_t tam_nome, uint8_t metodo, uint32_t crc,
    uint64_t offset, uint64_t gravado, uint64_t original, void *userdata);

static bool percorre(Leitor *l, cada_cb cb, void *userdata)
{
    uint64_t p = 0;
    for (uint32_t i = 0; i < l->entradas; i++)
    {
        if (p + ENTRADA_FIXA > l->tam)
            return false;

        const uint8_t *reg = l->indice + p;
        size_t tam_nome    = get16(reg + 0);

        if (p + ENTRADA_FIXA + tam_nome > l->tam || tam_nome == 0)
            return false;

        if (!cb((const char *)(reg + ENTRADA_FIXA), tam_nome, reg[2], get32(reg + 4),
                get64(reg + 8), get64(reg + 16), get64(reg + 24), userdata))
            return true;

        p += ENTRADA_FIXA + tam_nome;
    }
    return true;
}

bool nxsaves_is_box(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    uint8_t magia[sizeof(MAGIC)];
    bool ok = fread(magia, 1, sizeof(magia), f) == sizeof(magia)
        && memcmp(magia, MAGIC, sizeof(MAGIC)) == 0;
    fclose(f);
    return ok;
}

typedef struct
{
    nxsaves_list_cb cb;
    void *userdata;
} ListaCtx;

static bool na_lista(const char *nome, size_t tam_nome, uint8_t metodo, uint32_t crc,
    uint64_t offset, uint64_t gravado, uint64_t original, void *userdata)
{
    (void)metodo;
    (void)crc;
    (void)offset;
    (void)gravado;

    ListaCtx *c = userdata;
    char buf[MAX_NOME + 1];
    if (tam_nome > MAX_NOME)
        tam_nome = MAX_NOME;
    memcpy(buf, nome, tam_nome);
    buf[tam_nome] = '\0';

    c->cb(buf, original, c->userdata);
    return true;
}

bool nxsaves_list(const char *path, nxsaves_list_cb cb, void *userdata)
{
    Leitor l;
    if (!abre(path, &l))
        return false;

    ListaCtx c = { cb, userdata };
    bool ok    = percorre(&l, na_lista, &c);
    fecha(&l);
    return ok;
}

// ---- listagem por pasta de primeiro nível ----

#define MAX_PASTAS 256

typedef struct
{
    char nomes[MAX_PASTAS][0x201];
    uint64_t bytes[MAX_PASTAS];
    size_t n;
} Pastas;

static bool na_pasta(const char *nome, size_t tam_nome, uint8_t metodo, uint32_t crc,
    uint64_t offset, uint64_t gravado, uint64_t original, void *userdata)
{
    (void)metodo;
    (void)crc;
    (void)offset;
    (void)gravado;

    Pastas *p = userdata;

    // Só o pedaço até a primeira barra. Arquivo solto na raiz do container
    // entra com o próprio nome.
    const char *barra = memchr(nome, '/', tam_nome);
    size_t corte      = barra ? (size_t)(barra - nome) : tam_nome;
    if (corte >= sizeof(p->nomes[0]))
        corte = sizeof(p->nomes[0]) - 1;

    for (size_t i = 0; i < p->n; i++)
        if (strlen(p->nomes[i]) == corte && memcmp(p->nomes[i], nome, corte) == 0)
        {
            p->bytes[i] += original;
            return true;
        }

    if (p->n >= MAX_PASTAS)
        return true; // para de acumular, mas não invalida o resto

    memcpy(p->nomes[p->n], nome, corte);
    p->nomes[p->n][corte] = '\0';
    p->bytes[p->n]        = original;
    p->n++;
    return true;
}

bool nxsaves_list_folders(const char *path, nxsaves_list_cb cb, void *userdata)
{
    Leitor l;
    if (!abre(path, &l))
        return false;

    // Estático: são ~130 KB, e alocar isso na pilha de uma thread da borealis
    // não cabe. Só uma thread lê container por vez.
    static Pastas p;
    p.n = 0;

    bool ok = percorre(&l, na_pasta, &p);
    fecha(&l);

    if (ok)
        for (size_t i = 0; i < p.n; i++)
            cb(p.nomes[i], p.bytes[i], userdata);

    return ok;
}

// ---- extração ----

// O destino aqui vira save de jogo. Nome que sobe de pasta, nome absoluto ou
// nome com ":" (que no devoptab do Switch troca de dispositivo) é recusado.
static bool nome_perigoso(const char *nome)
{
    if (nome[0] == '/' || nome[0] == '\\' || strchr(nome, ':'))
        return true;

    for (const char *p = nome; *p; p++)
        if (p[0] == '.' && p[1] == '.' && (p == nome || p[-1] == '/') && (p[2] == '/' || p[2] == '\0'))
            return true;

    return false;
}

// Cria as pastas de cima do arquivo, uma de cada vez. Não dá pra usar um
// mkdir -p pronto: no devoptab do save montado a raiz ("save:/") já existe e
// tentar criá-la dá erro.
static bool cria_pastas(const char *caminho)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", caminho);

    char *p = strchr(buf, ':');
    p       = p ? p + 1 : buf;
    if (*p == '/')
        p++;

    for (; *p; p++)
    {
        if (*p != '/')
            continue;

        *p = '\0';
        mkdir(buf, 0777); // já existir é o caso comum, não é erro
        *p = '/';
    }
    return true;
}

// Descomprime uma entrada pro caminho de destino, conferindo o crc no fim.
static bool tira_uma(FILE *f, uint64_t offset, uint64_t gravado, uint64_t original,
    uint32_t crc_diz, uint8_t metodo, const char *destino)
{
    if (metodo != METODO_DEFLATE && metodo != METODO_CRU)
        return false;

    cria_pastas(destino);

    FILE *out = fopen(destino, "wb");
    if (!out)
        return false;

    z_stream s = { 0 };
    if (metodo == METODO_DEFLATE && inflateInit2(&s, -15) != Z_OK)
    {
        fclose(out);
        return false;
    }

    uint8_t entrada[PEDACO], saida[PEDACO];
    uint32_t crc   = crc32(0L, Z_NULL, 0);
    uint64_t resta = gravado;
    uint64_t saiu  = 0;
    bool ok        = true;

    while (ok && resta > 0)
    {
        size_t quanto = resta > sizeof(entrada) ? sizeof(entrada) : (size_t)resta;

        if (!le_em(f, offset + (gravado - resta), entrada, quanto))
        {
            ok = false;
            break;
        }
        resta -= quanto;

        if (metodo == METODO_CRU)
        {
            crc = crc32(crc, entrada, (uInt)quanto);
            saiu += quanto;
            ok = fwrite(entrada, 1, quanto, out) == quanto;
            continue;
        }

        s.next_in  = entrada;
        s.avail_in = (uInt)quanto;

        do
        {
            s.next_out  = saida;
            s.avail_out = sizeof(saida);

            int r = inflate(&s, Z_NO_FLUSH);
            if (r == Z_NEED_DICT || r == Z_DATA_ERROR || r == Z_MEM_ERROR || r == Z_STREAM_ERROR)
            {
                ok = false;
                break;
            }

            size_t n = sizeof(saida) - s.avail_out;
            if (n)
            {
                crc = crc32(crc, saida, (uInt)n);
                saiu += n;
                if (fwrite(saida, 1, n, out) != n)
                {
                    ok = false;
                    break;
                }
            }

            if (r == Z_STREAM_END)
                break;
        } while (s.avail_out == 0);
    }

    if (metodo == METODO_DEFLATE)
        inflateEnd(&s);

    // fclose de novo: é onde cartão cheio aparece.
    if (fclose(out) != 0)
        ok = false;

    // Tamanho e crc conferidos: é o que separa "restaurei o save" de
    // "escrevi lixo por cima do save".
    if (ok && (saiu != original || crc != crc_diz))
        ok = false;

    if (!ok)
        remove(destino);
    return ok;
}

typedef struct
{
    FILE *f;
    const char *prefix;
    size_t tam_prefix;
    const char *dest;
    bool ok;
    size_t tirados;
} ExtraiCtx;

static bool na_extracao(const char *nome, size_t tam_nome, uint8_t metodo, uint32_t crc,
    uint64_t offset, uint64_t gravado, uint64_t original, void *userdata)
{
    ExtraiCtx *c = userdata;

    if (tam_nome > MAX_NOME)
    {
        c->ok = false;
        return false;
    }

    char buf[MAX_NOME + 1];
    memcpy(buf, nome, tam_nome);
    buf[tam_nome] = '\0';

    if (c->tam_prefix)
    {
        if (strncmp(buf, c->prefix, c->tam_prefix) != 0)
            return true; // não é desse jogo
    }

    const char *relativo = buf + c->tam_prefix;
    if (*relativo == '\0')
        return true;

    if (nome_perigoso(buf))
    {
        c->ok = false;
        return false;
    }

    char destino[1024];
    size_t dlen    = strlen(c->dest);
    bool tem_barra = (dlen && c->dest[dlen - 1] == '/');
    snprintf(destino, sizeof(destino), tem_barra ? "%s%s" : "%s/%s", c->dest, relativo);

    if (!tira_uma(c->f, offset, gravado, original, crc, metodo, destino))
    {
        c->ok = false;
        return false;
    }

    c->tirados++;
    return true;
}

bool nxsaves_extract(const char *path, const char *prefix, const char *dest_dir)
{
    Leitor l;
    if (!abre(path, &l))
        return false;

    ExtraiCtx c = { l.f, prefix, prefix ? strlen(prefix) : 0, dest_dir, true, 0 };
    bool andou  = percorre(&l, na_extracao, &c);
    fecha(&l);

    // Zero arquivos não é sucesso: quem chama vai escrever isso por cima de um
    // save, e "restaurei nada" com cara de "restaurei" é o pior fim possível.
    return andou && c.ok && c.tirados > 0;
}
