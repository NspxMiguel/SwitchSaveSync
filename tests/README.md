# Testes que rodam no Mac, sem console

```sh
./tests/run.sh
```

136 testes. Não precisa de Switch, não precisa de conta do Google, não instala
nada — usa o Apache que já vem no macOS pra levantar um servidor WebDAV de
mentira por alguns segundos e o derruba no fim.

Tem mais 42 contra o Google Drive de verdade, que ficam de fora deste comando
porque precisam de conta. Estão logo abaixo.

Tudo compila com **ASan e UBSan** ligados. Eles pegam estouro de buffer e
leitura de memória não inicializada, que é justamente o erro que no console
vira crash sem explicação nenhuma na tela.

## O que cobre

**`test_nxsaves.c` — o formato do arquivo `.nxsaves` (31 testes).**
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

## O Google Drive de verdade — fora do `tudo`

```sh
./tests/run.sh drive
```

42 testes contra o Drive de uma conta de verdade. Está fora do `tudo` porque
precisa de conta e de internet, e porque fala com a nuvem de alguém.

O login é **por código**: o teste mostra um código, você aprova no seu
navegador, na página do próprio Google. Senha nenhuma passa pelo teste. O token
fica numa pasta temporária **fora do repo** e é reaproveitado nas rodadas
seguintes — a mensagem no fim diz onde, pra você apagar quando terminar.

Tudo acontece dentro de uma pasta chamada `_teste do Mac (pode apagar)`, criada
na hora e apagada no fim. O `cloud_prune_extras`, que é a **única** operação
que apaga da nuvem, só roda lá dentro. Nenhum backup de verdade é tocado, e o
teste imprime a listagem antes e depois pra provar. Precisa do `core/config.h`,
que tem o client id/secret do Google e não vai pro git.

Este é também o único teste que exercita a **conexão segura**: o servidor
WebDAV de teste é `http` puro, mas o Drive é `https`, e o `core/http.c` roda
aqui sem alteração nenhuma, conferindo o certificado contra o `cacert.pem` do
próprio projeto.

## O que NÃO cobre

- **Montar save de verdade, e a tela.** Precisa do console.

## Os stubs

`stubs/switch.h` é um `<switch.h>` de mentira, só com os tipos (`u8`..`u64`,
`Result`, `AccountUid`) e o relógio. `stubs_console.c` tem as funções que só
existem no console (montar save, listar jogos) devolvendo falso — nenhum teste
encosta nelas, estão ali só pro linker parar de reclamar.

Um detalhe: o `-Wno-format` no `run.sh` é por causa do stub. No Switch `u64` é
`unsigned long`; no Mac é `unsigned long long`. Os `%016lX` do core estão
certos lá e reclamam aqui.

## `pastas` — o caminho da pasta na nuvem

Testa a regra `<Jogo>/<Dono>`, que substituiu a pasta achatada `Jogo (Dono)`.

Existe porque essa regra errou **três vezes no mesmo dia em que foi escrita**, e
nenhuma das três precisava de console pra ser pega:

1. o ramo de primeira sync entregou o caminho de dois níveis pra uma função que
   cria um nome só — no Drive nasceu uma pasta cujo *nome* tinha uma barra, o
   upload dizia que deu certo e nenhum leitor achava aquilo nunca mais;
2. o fallback do layout antigo passou a resolver pra pasta-container do jogo, e
   restaurar aquilo escreveria as pastas das outras contas e o `.nxsaves` de
   29 MB **dentro do savedata**;
3. a contagem da barra de progresso usava `ensure` em vez de `find` e criava
   pasta vazia no Drive de quem só olhou uma tela.

O `nomes` continua testando a regra **achatada**, que não morreu: ela ainda vale
pro backup no cartão e pro nome de pasta dentro do arquivo `.nxsaves`.

### Sobre a mutação

Este arquivo passou verde de primeira, o que é motivo pra desconfiar — teste que
não sabe falhar não vale nada. Rodando mutação (quebrar o código de propósito e
conferir que o teste acusa), **o teste da truncagem não pegou**: o apelido usado
era "Miguel", de 6 letras, e 512 + 1 + 6 ainda cabia no buffer de 544. O pior
caso de verdade é o apelido no tamanho que o console entrega — `account[0x21]`,
32 caracteres — onde 512 + 1 + 32 estoura por um byte e quem perde o fim é o
dono. Com dois apelidos de 32 que diferem só no último caractere, os dois donos
colidem no mesmo caminho e um escreve por cima do save do outro.

Corrigido, as três mutações são pegas:

| mutação | testes que acusam |
| --- | --- |
| tirar a reserva de espaço da truncagem | 3 |
| voltar a deixar save único solto na raiz do jogo | 6 |
| voltar a obedecer registro achatado do `pastas.txt` | 2 |

## `sync` — a decisão de subir, baixar ou recusar

O `syncjob_sync_title` decide **sozinho** se sobe o save, se baixa, se não mexe
ou se recusa por conflito. Errar essa decisão apaga progresso de jogo, e até
agora a única forma de exercitar isso era no console do Miguel, com o save de
verdade dele — o pior lugar do mundo pra descobrir um erro.

O `fake_cloud.c` põe uma nuvem e um savedata falsos em cima de pastas de
verdade no disco. O `syncjob.c` que roda é o de produção, sem uma linha
alterada. Os oito ramos da decisão são cobertos, mais o jogo aberto e o save com
subpasta.

Dois detalhes do falso que valem saber:

- **A raiz da nuvem falsa já é a pasta do app.** Quem cria o nível
  `Nintendo Switch Saves` no Drive de verdade é o `drive.c`, que está
  substituído. O que interessa testar é o que vem depois dele.
- **O commit é observado.** `savemount_unmount(true)` num mount read-only marca
  o contador com um número absurdo, pra ficar impossível de passar batido — é
  exatamente esse engano que corrompeu dois jogos.

### Mutação

| mutação | testes que acusam |
| --- | --- |
| primeira sync volta a usar `ensure_subfolder` com o caminho de dois níveis | **10** |
| "mudou dos dois lados" vira download em vez de conflito | 4 |
| "sem marcador" vira upload em vez de conflito | 2 |

A primeira é o bug de verdade que passou hoje. O teste que o pega é o de **ida e
volta**: subir e sincronizar de novo. Se o caminho que escreve e o que lê não
forem o mesmo, a segunda chamada não acha nada, cai em "primeira sync" outra vez
e sobe pra sempre — foi isso que a pasta com barra no nome causou.
