# Como save de Switch funciona (e o que isso exige do app)

conversa privada removida do historico
conversa privada removida do historico

## 1. Save no Switch nao e' um arquivo. E' um sistema de arquivos.

Cada save e' um **container** que o console monta como se fosse um cartao: tem
raiz, tem pasta, tem subpasta, tem quantos arquivos o jogo quiser. O que vai
dentro e' decisao exclusiva do jogo — nao existe formato padrao, nao existe
"o arquivo de save".

Consequencia pro app, e e' a decisao de projeto mais importante daqui:
**nunca interpretar o conteudo.** O app copia a arvore byte a byte nos dois
sentidos. Assim ele funciona em jogo que nunca foi testado, inclusive jogo que
ainda nao saiu, sem precisar de uma lista de jogos suportados.

## 2. O caso do Zelda (dados reais dele, conferidos no Drive)

`The Legend of Zelda: Breath of the Wild` subiu assim:

```
The Legend of Zelda_ Breath of the Wild/
├── 0/  game_data.sav (1.027.216 B) + caption.sav (1.528 B)
├── 1/  idem
├── 2/  idem
├── 3/  idem
├── 4/  idem
├── 5/  idem
├── tracker/  trackblock00.sav (65.536 B)
└── option.sav (344 B)
```

Nao sao 6 pedacos de um save. Sao **6 saves**: no modo normal o BotW guarda
1 save manual + 5 automaticos (com Master Mode chega a 9). Cada pasta numerada
e' um slot inteiro.

- `game_data.sav` — o mundo daquele slot (~1 MB). E' o save de verdade.
- `caption.sav` — a plaquinha da tela de carregar ("Planalto, dia 40").
- `option.sav` — as opcoes (camera invertida, sensibilidade). Uma so' pra todos
  os slots, por isso fica na raiz.
- `tracker/trackblock*.sav` — o Caminho do Heroi, o risco vermelho no mapa com
  as ultimas 200 horas. Cresce conforme joga, entao o numero de trackblock
  aumenta com o tempo.

Fonte da estrutura: [ZeldaMods — Save Files](https://zeldamods.org/w_botw/index.php?title=Save_Files).

**Isso ja' funciona.** A copia e' recursiva nas quatro pernas — `save:` ->
staging (`savemount_copy_tree`), staging -> Drive criando subpasta de verdade
(`drive_upload_tree`), Drive -> staging (`drive_download_tree`) e staging ->
`save:`. A impressao digital que compara os dois lados (`fingerprint_dir`)
tambem desce nas subpastas, entao "mudou dos dois lados" enxerga arquivo
aninhado. Conferido no Drive de quem usa: as 8 pastas e todos os arquivos chegaram.

## 3. Os tipos de save data — e o buraco que existia aqui

O `FsSaveDataType` da libnx tem sete valores. O que o app faz com cada um:

| Tipo | O que e' | App |
|---|---|---|
| `Account` | Save de uma conta de usuario. O caso comum. | **sincroniza** |
| `Device` | Save do CONSOLE, sem dono, compartilhado por todos os perfis. | **sincroniza** (desde) |
| `Cache` | Dado derivado que o jogo remonta sozinho se sumir. | ignora |
| `Bcat` / `SystemBcat` | Conteudo que o servidor da Nintendo empurra (evento, brinde). Nao e' progresso. | ignora |
| `Temporary` | Morre sozinho. | ignora |
| `System` | Do sistema, nao de jogo. | ignora |

**O buraco:** ate' o app so' olhava `Account`. Jogo que guarda
progresso em `Device` simplesmente **nao aparecia na lista**.

Quem usa `Device`:

- **Animal Crossing: New Horizons** — a ilha e' do console, uma por Switch, nao
  da conta. Esse era o pior caso: o save de conta existe e e' quase vazio,
  entao dava pra "fazer backup do Animal Crossing" e **nao levar a ilha junto**.
- **Pokemon Sword/Shield** — usa os dois tipos ao mesmo tempo.
- **1-2-Switch** e outros de dado compartilhado.

O JKSV tem o mesmo furo, com
[issue aberta](https://github.com/J-D-K/JKSV/issues/9) — e o Checkpoint
[tambem](https://github.com/BernardoGiordano/Checkpoint/issues/290). Nao e' um
caso exotico, e' um canto que os save managers costumam deixar de fora.

### Como ficou

- `titles.c` aceita `Device` junto com `Account`, e a chave de duplicata virou
  **(jogo, tipo, conta)** — senao o save de conta e o do console do mesmo jogo
  se anulariam.
- `savemount_mount_typed()` monta com `fsdevMountDeviceSaveData` quando e'
  device save (esse nao leva conta: nao tem dono).
- Nome da pasta ganha ` (console)` **so' quando o jogo tem mais de um save**.
  Jogo de save unico continua com o nome de pasta de sempre — mudar isso
  deixaria orfao tudo que ja' esta' no Drive de quem usa.

### Ressalva honesta

A libnx **nao tem** versao somente-leitura do mount de device save (so' existe
`fsdevMountDeviceSaveData`). No save de conta o backup monta read-only e o
sistema impede escrita; no device save a unica protecao e' o codigo nao
escrever. Esta' anotado no `savemount.h`.

E: nao da' pra testar device save sem um jogo que use. Se ele nao tiver Animal
Crossing nem Pokemon no console, esse caminho vai continuar sem prova real —
o que da' pra garantir e' que o caminho de save de conta nao mudou.

## 4. O que ainda pode morder

- **Cache storage** — alguns jogos poem coisa util la'. E' remontavel por
  definicao, mas se aparecer jogo que perde algo visivel, vale reavaliar.
  Precisa do `save_data_index`, entao nao e' so' ligar um `if`.
- **Save grande** — o app le' o conteudo inteiro pra calcular a impressao
  digital. Com save de 1 MB (Zelda) e' instantaneo; se aparecer save de
  centenas de MB, isso vira espera. Ai a saida e' hash so' de nome+tamanho
  quando passar de um limite.
- **Caminho longo** — os buffers de caminho sao de 600-700 bytes e o nome de
  jogo pode ter ate' 512. Com nome gigante + sufixo de conta + subpasta funda,
  o `snprintf` trunca (nao estoura, mas erra o caminho). Nenhum nome real chega
  perto; fica anotado.
- **Nome de arquivo esquisito** — `syncstate_sanitize_name` so' limpa o nome da
  PASTA do jogo. Nome de arquivo dentro do save vai como esta' pro Drive, que
  aceita quase tudo. Voltando pro console e' o mesmo nome, entao fecha.

## Fontes

- [ZeldaMods — Save Files (Breath of the Wild)](https://zeldamods.org/w_botw/index.php?title=Save_Files)
- [libnx — fs.h (FsSaveDataType)](https://switchbrew.github.io/libnx/fs_8h_source.html)
- [JKSV — issue de SaveCommon/DeviceSaveData](https://github.com/J-D-K/JKSV/issues/9)
- [Checkpoint — mesma issue](https://github.com/BernardoGiordano/Checkpoint/issues/290)
- [Nintendo — save compartilhado no Animal Crossing: New Horizons](https://en-americas-support.nintendo.com/app/answers/detail/a_id/49983/~/how-to-back-up-and-restore-island-save-data-(animal-crossing:-new-horizons))
