#include "minijson.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Acha o começo do valor associado a "key": no json, pulando espaços.
// Continua procurando se achar "key" mas não vier seguido de ':' (evita
// casar com uma ocorrência de texto que não é a chave de verdade).
static const char *find_value_start(const char *json, const char *key) {
    char needle[128];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= (int)sizeof(needle)) return NULL;

    const char *p = json;
    while ((p = strstr(p, needle)) != NULL) {
        const char *c = p + n;
        while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') c++;
        if (*c == ':') {
            c++;
            while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') c++;
            return c;
        }
        p = p + n;
    }
    return NULL;
}

bool json_get_string(const char *json, const char *key, char *out, size_t outsz) {
    if (!json || !key || !out || outsz == 0) return false;
    const char *v = find_value_start(json, key);
    if (!v || *v != '"') return false;
    v++;
    size_t oi = 0;
    while (*v && *v != '"' && oi + 1 < outsz) {
        if (*v == '\\' && *(v + 1)) {
            v++;
            switch (*v) {
                case 'n': out[oi++] = '\n'; break;
                case 't': out[oi++] = '\t'; break;
                case 'r': out[oi++] = '\r'; break;
                case '"': out[oi++] = '"'; break;
                case '\\': out[oi++] = '\\'; break;
                case '/': out[oi++] = '/'; break;
                default: out[oi++] = *v; break;
            }
            v++;
        } else {
            out[oi++] = *v++;
        }
    }
    out[oi] = '\0';
    return true;
}

bool json_get_number(const char *json, const char *key, double *out) {
    if (!json || !key || !out) return false;
    const char *v = find_value_start(json, key);
    if (!v) return false;
    char *end = NULL;
    double val = strtod(v, &end);
    if (end == v) return false;
    *out = val;
    return true;
}

// Copia um bloco delimitado por open/close (objeto {} ou array []),
// respeitando aninhamento e strings com aspas (pra não contar chave/colchete
// dentro de um valor string como se fosse estrutura). Devolve em *end um
// ponteiro pra logo depois do delimitador de fechamento, pra quem chamou
// poder continuar escaneando dali (usado pela iteração de array).
static bool copy_balanced_block_ex(const char *start, char open, char close,
                                    char *out, size_t outsz, const char **end) {
    if (*start != open) return false;
    int depth = 0;
    bool in_string = false;
    size_t oi = 0;
    const char *p = start;
    while (*p) {
        char c = *p;
        if (oi + 1 >= outsz) return false; // buffer pequeno demais pro bloco
        out[oi++] = c;
        if (in_string) {
            if (c == '\\' && *(p + 1)) {
                p++;
                if (oi + 1 >= outsz) return false;
                out[oi++] = *p;
                p++;
                continue;
            }
            if (c == '"') in_string = false;
        } else {
            if (c == '"') in_string = true;
            else if (c == open) depth++;
            else if (c == close) {
                depth--;
                if (depth == 0) {
                    out[oi] = '\0';
                    if (end) *end = p + 1;
                    return true;
                }
            }
        }
        p++;
    }
    return false; // chegou no fim da string sem fechar o bloco
}

static bool copy_balanced_block(const char *start, char open, char close, char *out, size_t outsz) {
    return copy_balanced_block_ex(start, open, close, out, outsz, NULL);
}

bool json_get_block(const char *json, const char *key, char *out, size_t outsz) {
    const char *v = find_value_start(json, key);
    if (!v) return false;
    if (*v == '{') return copy_balanced_block(v, '{', '}', out, outsz);
    if (*v == '[') return copy_balanced_block(v, '[', ']', out, outsz);
    return false;
}

bool json_first_element(const char *arrayJson, char *out, size_t outsz) {
    if (!arrayJson) return false;
    const char *p = arrayJson;
    while (*p && *p != '[') p++;
    if (*p != '[') return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
    if (*p == ']') return false; // array vazio
    if (*p == '{') return copy_balanced_block(p, '{', '}', out, outsz);
    if (*p == '"') {
        p++;
        size_t oi = 0;
        while (*p && *p != '"' && oi + 1 < outsz) out[oi++] = *p++;
        out[oi] = '\0';
        return true;
    }
    return false;
}

bool json_array_begin(const char *arrayJson, const char **cursor) {
    if (!arrayJson || !cursor) return false;
    const char *p = arrayJson;
    while (*p && *p != '[') p++;
    if (*p != '[') return false;
    *cursor = p + 1;
    return true;
}

bool json_array_next(const char **cursor, char *out, size_t outsz) {
    if (!cursor || !*cursor) return false;
    const char *p = *cursor;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
    if (*p == '\0' || *p == ']') {
        *cursor = p;
        return false;
    }

    if (*p == '{') {
        const char *end = NULL;
        if (!copy_balanced_block_ex(p, '{', '}', out, outsz, &end)) return false;
        *cursor = end;
        return true;
    }
    if (*p == '[') {
        const char *end = NULL;
        if (!copy_balanced_block_ex(p, '[', ']', out, outsz, &end)) return false;
        *cursor = end;
        return true;
    }
    if (*p == '"') {
        const char *q = p + 1;
        size_t oi = 0;
        while (*q && *q != '"' && oi + 1 < outsz) {
            if (*q == '\\' && *(q + 1)) q++;
            out[oi++] = *q++;
        }
        out[oi] = '\0';
        if (*q == '"') q++;
        *cursor = q;
        return true;
    }
    // número/literal cru (não usado nas respostas que tratamos, mas não
    // trava a iteração se aparecer)
    const char *q = p;
    size_t oi = 0;
    while (*q && *q != ',' && *q != ']' && oi + 1 < outsz) out[oi++] = *q++;
    out[oi] = '\0';
    *cursor = q;
    return true;
}
