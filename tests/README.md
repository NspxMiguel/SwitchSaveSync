# Testes que rodam no Mac, sem console

```sh
./tests/run.sh
```

136 testes. Não precisa de Switch, não precisa de conta do Google, não instala
nada — usa o Apache que já vem no macOS pra levantar um servidor WebDAV de
mentira por alguns segundos e o derruba no fim.

Tudo compila com **ASan e UBSan** ligados. Eles pegam estouro de buffer e
leitura de memória não inicializada, que é justamente o erro que no console
vira crash sem explicação nenhuma na tela.

## O que cobre

**`test_sssbox.c` — o formato do arquivo `.ssaves` (31 testes).**
Grava uma árvore de 6 arquivos (de 0 byte a 2 MB, com acento no nome, pasta
dentro de pasta), lê de volta e confere byte a byte. Confere também que o
arquivo não entrega o conteúdo nem os nomes das pastas em texto puro; que
arquivo cortado pela metade é **recusado** em vez de restaurar meio save por
cima do save bom; e que arquivo vazio é apagado em vez de virar um backup
falso.

**`test_webdav.c` — WebDAV e o espelhamento da nuvem (48 testes).**
Contra um servidor WebDAV de verdade. Endereço/usuário/senha, conectar, senha
errada dando 401 com frase legível, servidor fora do ar, criar e achar pasta,
subir, baixar, subir por cima sem duplicar, listar, árvore de 3 níveis.

O que mais importa aqui é a **limpeza** (`cloud_prune_extras`) — a única
operação que apaga da nuvem. Testado que ela apaga o que foi apagado no
console **e não encosta** no que continua lá.

**`test_nomes.c` — respostas do Google e nome de pasta (57 testes).**
O leitor de JSON, que é quem lê todo login e toda listagem do Drive: campos
fora de ordem, barras escapadas `\/`, aspas dentro do nome, listar vários sem
parar no primeiro, pasta vazia, buffer curto. O saneamento de nome de pasta.
E a regra do nome da pasta do save — inclusive a prova de que renomear a conta
do console só muda a pasta quando o jogo tem save de mais de uma conta.

## O que NÃO cobre, de propósito

- **O Google Drive de verdade.** Exigiria entrar numa conta. O que dá pra
  garantir daqui é o pedaço que lê as respostas e o espelhamento, que é código
  comum às duas nuvens.
- **A conexão segura.** O servidor de teste é `http` puro, então a parte do
  `core/http.c` que confere o certificado (`romfs:/cacert.pem`) não passa por
  aqui. No console ela é obrigatória.
- **Montar save de verdade, e a tela.** Precisa do console.

## Os stubs

`stubs/switch.h` é um `<switch.h>` de mentira, só com os tipos (`u8`..`u64`,
`Result`, `AccountUid`) e o relógio. `stubs_console.c` tem as funções que só
existem no console (montar save, listar jogos) devolvendo falso — nenhum teste
encosta nelas, estão ali só pro linker parar de reclamar.

Um detalhe: o `-Wno-format` no `run.sh` é por causa do stub. No Switch `u64` é
`unsigned long`; no Mac é `unsigned long long`. Os `%016lX` do core estão
certos lá e reclamam aqui.
