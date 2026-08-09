// minijson.h — extrator mínimo de campos de JSON, sem parser completo.
//
// Não é um parser JSON de verdade. É busca de texto tolerante o bastante pra
// extrair campos simples ("key": "value" / "key": 123) das respostas planas
// da API do Google (device code, token, Drive files). Não lida com JSON
// arbitrário — só com o formato conhecido dessas respostas.
//
// Motivo de não vendorizar uma lib JSON completa (cJSON etc.): reduzir
// código que eu não consigo compilar/testar aqui. Isso é mais fácil de
// revisar linha a linha e de debugar com printf no console do Switch.
#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Procura "key":"..."  (com aspas) e copia o valor (sem aspas) pra out.
// Faz unescape básico de \" \\ \/ \n \t. Retorna false se não achou a key
// ou se o valor não é uma string.
bool json_get_string(const char *json, const char *key, char *out, size_t outsz);

// Procura "key":123.45 (número, sem aspas) e retorna em *out.
bool json_get_number(const char *json, const char *key, double *out);

// Procura "key": { ... } ou "key": [ ... ] e copia o bloco bruto
// (incluindo os delimitadores { } ou [ ]) pra out, respeitando aninhamento
// e strings com aspas. Útil pra pegar o array "files": [...] da resposta
// de listagem do Drive antes de extrair o primeiro elemento.
bool json_get_block(const char *json, const char *key, char *out, size_t outsz);

// Dado o texto de um array JSON (começando em '[' , terminando em ']'),
// copia o primeiro elemento (objeto ou string) bruto pra out.
// Retorna false se o array estiver vazio ou malformado.
bool json_first_element(const char *arrayJson, char *out, size_t outsz);

// Iteração sobre TODOS os elementos de um array JSON (não só o primeiro) —
// usada pra listar todos os arquivos de uma pasta do Drive, não só um.
//
// Uso:
//   const char *cursor;
//   if (json_array_begin(files_block, &cursor)) {
//       char elem[600];
//       while (json_array_next(&cursor, elem, sizeof(elem))) {
//           // processa elem (objeto ou string bruto)
//       }
//   }
bool json_array_begin(const char *arrayJson, const char **cursor);
bool json_array_next(const char **cursor, char *out, size_t outsz);

#ifdef __cplusplus
}
#endif
