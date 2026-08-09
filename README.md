<div align="center">

<img src="assets/icon.png" alt="SwitchSaveSync" width="128">

# SwitchSaveSync

**Save de Switch no Google Drive, num clique só.**
Abre o app, aperta A no jogo, pronto — ele decide sozinho se sobe ou se desce.

[![Plataforma](https://img.shields.io/badge/plataforma-Nintendo%20Switch-e60012)](https://switchbrew.org/)
[![CFW](https://img.shields.io/badge/CFW-Atmosph%C3%A8re-5865f2)](https://github.com/Atmosphere-NX/Atmosphere)
[![Compilador](https://img.shields.io/badge/build-devkitPro-1f9c4b)](https://devkitpro.org/)
[![Idiomas](https://img.shields.io/badge/idiomas-PT%20%7C%20EN-blue)](#tamb%C3%A9m-tem)
[![Testado em](https://img.shields.io/badge/testado%20em-firmware%2018.1.0-f59f00)](#onde-foi-testado)
[![Licença](https://img.shields.io/badge/licen%C3%A7a-GPLv3-663366)](LICENSE)

[English](README.en.md)

</div>

---

## O que é

Um homebrew que guarda os saves dos seus jogos no **seu** Google Drive e traz de volta
quando você precisa — do jeito que o Steam Cloud faz, mas sem servidor, sem assinatura e
sem custo nenhum. Não existe backend: o console fala direto com a API do Google Drive,
usando uma conta que é sua.

Ele **nunca interpreta o conteúdo do save**. Copia a árvore de arquivos byte a byte nos
dois sentidos. Por isso funciona em jogo que nunca foi testado — inclusive jogo que ainda
nem saiu — sem precisar de lista de jogos suportados.

## O clique único

O botão **A** faz a coisa certa sozinho, comparando uma impressão digital do save dos dois
lados com a da última sincronização:

| Situação | O que ele faz |
| --- | --- |
| Só o console mudou | Sobe pro Drive |
| Só o Drive mudou | Baixa pro console |
| Nenhum dos dois mudou | Não encosta em nada |
| O console não tem save ainda | Baixa do Drive |
| **Os dois mudaram** | **Para e pergunta** |

O último caso é o importante: escolher sozinho ali apagaria progresso de verdade. O app
mostra os dois lados (quantos arquivos, quantos bytes) e deixa a decisão com você. Nada é
escrito enquanto você não apertar.

O botão **Y** abre o menu com as ações separadas, pra quando você quiser mandar em vez de
deixar ele decidir: subir, baixar, backup no cartão, restaurar do cartão.

## O que ele faz que os outros não fazem

**Save separado por conta.** O mesmo jogo jogado por dois perfis do console tem dois saves
diferentes, e eles não se misturam. O jogo aparece uma vez na lista, com `2 saves` do lado,
e a escolha de quem é o save acontece no clique.

**Save de console, não só de conta.** Existem dois tipos de save no Switch: o de conta
(`FsSaveDataType_Account`) e o de console (`FsSaveDataType_Device`), que é do aparelho e não
de um perfil. Animal Crossing é o caso feio: **a ilha é save de console**, e o save de conta
existe e é quase vazio. Ferramenta que só lê save de conta faz "backup do Animal Crossing"
e não leva a ilha junto. Pokémon Sword/Shield usa os dois tipos. Aqui os dois são lidos.

**Espelho de verdade.** Arquivo que o jogo apagou do save também sai da nuvem — senão ele
voltaria vivo no próximo restore. Some pra lixeira do Drive, não de vez: você tem 30 dias
pra desfazer.

**Backup no próprio cartão.** Sem internet e sem conta Google, dá pra guardar uma cópia em
`sdmc:/switch/SwitchSaveSync/backups` e restaurar de lá. É a rede de segurança pra quando a
nuvem não é uma opção.

## Também tem

- **Sincronizar tudo de uma vez** — uma linha no topo da lista passa por todos os saves,
  decidindo pra que lado cada um vai. Quando os dois lados mudaram ele **não** escolhe:
  aquele jogo fica de fora e sai listado no fim, pra você resolver um a um.
- **Tudo num arquivo só** — opcional: junta todos os saves num `.sss`, um formato nosso que
  não é zip e que só este app lê. Serve pra levar tudo de uma vez. *É disfarce, não
  cadeado* — o código é aberto, então quem quiser de verdade lê. Por isso o modo normal
  (uma pasta por jogo no Drive) continua sendo o recomendado: save preso num formato que só
  um programa lê é save que morre junto com o programa.
- **Login por QR code** — aponta o celular, digita o código, pronto. Sem teclado de tela.
- **Controle parental** — senha de 4 a 8 dígitos na abertura do app, guardada como hash.
  Trocar ou tirar a senha exige a atual.
- **Lista por último jogado**, com o ícone e o nome de verdade de cada jogo, lidos do
  console.
- **Português e inglês**, com o idioma seguindo o console ou escolhido na mão.

## Onde foi testado

**Switch OLED com firmware 18.1.0, em Atmosphère.** É o console em que ele foi escrito e é
onde ele roda todo dia — o resto é honestidade sobre o que ninguém tentou ainda.

Nada aqui depende de versão: o app não tem tabela de offsets, não faz patch em nada e não
lê estrutura interna de save. Ele usa as chamadas de montar save da libnx, que são as
mesmas desde o firmware 1.0, e a API do Google Drive, que é HTTPS. Então a chance de
quebrar numa versão diferente é baixa — mas *baixa* não é *testada*, e é isso que este
parágrafo está dizendo.

Se rodar (ou não rodar) em outra versão, [abra uma
issue](https://github.com/NspxMiguel/SwitchSaveSync/issues) contando qual — dá pra
transformar isto numa lista de verdade.

## Instalar

1. Console com CFW (**Atmosphère**) e o menu de homebrew funcionando.
2. Copie `SwitchSaveSync.nro` pra `sdmc:/switch/`.
3. Abra pelo menu de homebrew.

> **Abra segurando R num jogo, não pelo Álbum.** Aberto pelo Álbum, o homebrew roda em
> *modo applet*: ganha ~448 MB de memória e a pilha de rede às vezes não sobe — o app avisa
> isso na aba Conta, em Diagnóstico. Segurando **R** ao abrir um jogo instalado, ele roda
> como aplicação, com a memória e a rede inteiras.

### Com cara de jogo, na tela inicial

Dá pra ter um ícone do app na tela inicial do console, do lado dos jogos, e abrir dali. O
[Sphaira](https://github.com/ITotalJustice/sphaira) faz isso sozinho, **no próprio
console** — não precisa de PC, nem de `hacbrewpack`, nem da sua `prod.keys`: ele deriva a
chave direto do console.

1. Abra o Sphaira e ache o `SwitchSaveSync` na lista de homebrew.
2. Abra as opções e escolha **Install Forwarder**.
3. Instalar vem desligado de fábrica no Sphaira; ele pergunta se pode ligar — responda que
   sim.

O atalho nasce com o nome e o ícone que estão dentro do `.nro`, então aparece como
**SwitchSaveSync**, de *Miguel*, com o mesmo ícone daqui de cima.

E resolve o parágrafo anterior de quebra: o atalho é instalado como *aplicação*, então
abrir por ele já dá a memória e a rede inteiras. O truque do **R** deixa de ser necessário.

> **Não mude o `.nro` de lugar depois.** O atalho guarda o caminho do arquivo, e o Sphaira
> tira o ID do título de um hash desse caminho. Movido o arquivo, o atalho aponta pro vazio
> — e refazer o atalho a partir do caminho novo cria um segundo ícone em vez de consertar o
> primeiro. Deixe em `sdmc:/switch/SwitchSaveSync.nro` e pronto.

## Configurar o Google Drive

O app não vem com credencial embutida — cada pessoa usa um projeto Google Cloud seu, de
graça. É o que mantém o custo em zero e o acesso restrito a você.

```bash
cp core/config.h.example core/config.h
```

O `config.h.example` tem o passo a passo completo (criar o projeto, ativar a Drive API,
gerar um ID de cliente do tipo *TVs e dispositivos de entrada limitada*). O `config.h` está
no `.gitignore` e nunca entra em commit.

O escopo pedido é **`drive.file`**: o app só enxerga os arquivos que ele mesmo criou. O resto
do seu Drive fica invisível pra ele — não é uma promessa nossa, é o Google que não deixa.

Os saves vão pra uma pasta `Nintendo Switch Saves/`, com uma subpasta por jogo.

## Compilar

Precisa do [devkitPro](https://devkitpro.org/wiki/Getting_Started) com o grupo
`switch-dev`, mais `switch-curl`, `switch-mbedtls` e `switch-zlib`.

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=$DEVKITPRO/devkitA64
export PATH=$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH
make -C gui
```

Sai um `gui/SwitchSaveSync.nro`.

## O que ele não faz

- **Não mexe em save de jogo aberto.** O jogo tem que estar fechado; o app avisa quando não
  consegue montar.
- **Não interpreta save.** Não edita, não converte, não "conserta" save.
- **Não sobe nada sozinho.** Sincronização é sempre um clique seu. O modo automático está
  planejado, mas não existe ainda.
- **Não se instala no boot.** Nada de `boot2.flag` — decisão consciente: homebrew que sobe
  junto com o console é homebrew que pode deixar o console sem subir.

## Estado do projeto

O **app gráfico** (`gui/`) é o que está pronto e em uso. As outras pastas são caminhos que
foram abertos e estão parados de propósito:

| Pasta | O que é | Estado |
| --- | --- | --- |
| `gui/` | O app, em [borealis](https://github.com/natinusala/borealis) | **Em uso** |
| `core/` | O motor: Drive, OAuth, montagem de save, sincronização | **Em uso** |
| `app/` | Primeira versão, em modo texto | Histórico |
| `sysmodule/` | Autosync rodando de fundo | Pausado |
| `overlay/` | Menu no Ultrahand/Tesla | Pausado |

O autosync vai voltar como uma tela na **abertura** do jogo — "sincronizando, aguarde", com
barra de porcentagem e um botão **Pular** pra quem não quer esperar. Ainda não está feito.

## Leitura

- [`ANALISE.md`](ANALISE.md) — a análise de viabilidade que começou o projeto: o que já
  existia pronto, o que precisava ser construído, e onde estava o risco.
- [`SAVES.md`](SAVES.md) — como save de Switch funciona de verdade, e o que isso obriga o
  app a fazer.

## Créditos

[borealis](https://github.com/natinusala/borealis) pela interface,
[libnx](https://github.com/switchbrew/libnx) e [devkitPro](https://devkitpro.org/) pelo
resto, [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) por existir,
[qrcodegen](https://github.com/nayuki/QR-Code-generator) pelo QR do login.

O caminho de save de console veio de olhar onde o [JKSV](https://github.com/J-D-K/JKSV) e o
[Checkpoint](https://github.com/FlagBrew/Checkpoint) tropeçam — os dois têm issue aberta
sobre isso.

## Licença

[GPLv3](LICENSE) — a mesma do Atmosphère, do JKSV e do Checkpoint. Use, estude, modifique e
distribua à vontade; quem distribuir uma versão modificada tem que abrir o código dela
também.
