// sssbox.h — "tudo num arquivo só", num formato que é nosso.
//
// conversa privada removida do historico
// conversa privada removida do historico
// conversa privada removida do historico
// abrir com dois cliques no Mac. Não é o que ele quer: ele quer um arquivo que
// não seja igual ao de todo mundo.
//
// Então: container próprio, extensão `.sss`, com o índice no fim e os bytes
// embaralhados. **Isso é disfarce, não cofre** — o código está público no
// conversa privada removida do historico
// o arquivo não é um zip, não abre com duplo clique, e não entrega o save de
// ninguém pra quem só topou com ele. Quem tem que ler é o nosso app.
//
// Por isso o modo padrão continua sendo uma pasta por jogo no Drive — save
// preso num formato que só um programa lê é save que morre junto com o
// programa. Este aqui é a opção, não a regra.
//
// Layout (tudo little-endian):
//
//   [0]    cabeçalho de 40 bytes, EM CLARO (é o que identifica o arquivo)
//   [40]   os dados de cada entrada, um atrás do outro, deflate + embaralhados
//   [...]  o índice, embaralhado, no fim
//
// O índice fica no fim de propósito: assim dá pra escrever entrada por entrada,
// direto do save montado, sem saber de antemão quanto cada uma vai ocupar
// depois de comprimida — e sem precisar de espaço no cartão pra uma cópia
// intermediária.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSSBOX_EXT "sss"

// ---------------------------------------------------------------------------
// escrita
// ---------------------------------------------------------------------------

typedef struct SssBoxWriter SssBoxWriter;

// Cria (ou trunca) o arquivo. NULL se não deu pra abrir pra escrita.
SssBoxWriter *sssbox_open_write(const char *path);

// Enfia a árvore de local_dir dentro do arquivo, com os nomes começando por
// prefix (ex.: "Zelda/"). local_dir pode ser um save montado — "save:/" — e
// nesse caso nada é copiado pro cartão no meio do caminho.
//
// Devolve false na primeira falha; o writer fica marcado como estragado e o
// close vai apagar o arquivo.
bool sssbox_add_dir(SssBoxWriter *b, const char *prefix, const char *local_dir);

// Fecha: escreve o índice, o cabeçalho definitivo, e confere que o fclose deu
// certo (é onde cartão cheio aparece). Em qualquer erro apaga o arquivo — meio
// arquivo é pior que arquivo nenhum, porque parece backup.
// Libera o writer nos dois casos.
bool sssbox_close_write(SssBoxWriter *b);

// Desiste: fecha, apaga o arquivo e libera. Serve pro cancelamento.
void sssbox_abort_write(SssBoxWriter *b);

size_t sssbox_entry_count(const SssBoxWriter *b);
uint64_t sssbox_bytes_written(const SssBoxWriter *b);

// ---------------------------------------------------------------------------
// leitura
// ---------------------------------------------------------------------------

// É um arquivo nosso? (só confere o cabeçalho, não lê o resto)
bool sssbox_is_box(const char *path);

typedef void (*sssbox_list_cb)(const char *name, uint64_t size, void *userdata);

// Chama cb pra cada arquivo lá dentro, com o tamanho original.
bool sssbox_list(const char *path, sssbox_list_cb cb, void *userdata);

// Chama cb uma vez por PASTA de primeiro nível (o "Zelda/" de cima), com a
// soma dos tamanhos. É o que a tela usa pra dizer o que tem dentro do arquivo
// antes de escrever em save nenhum.
bool sssbox_list_folders(const char *path, sssbox_list_cb cb, void *userdata);

// Tira de dentro tudo que começa com prefix, recriando as subpastas em
// dest_dir. prefix "" tira tudo. Nome com "..", com barra no começo ou com
// ":" é recusado — o destino aqui vira save de jogo.
bool sssbox_extract(const char *path, const char *prefix, const char *dest_dir);

#ifdef __cplusplus
}
#endif
