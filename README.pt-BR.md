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

[English](README.md)

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

## Antes de mais nada

Olha... isso aqui foi feito por **uma pessoa só**. Uma. Sem equipe de testes, sem QA, sem
ninguém pra olhar por cima do meu ombro e dizer *"tem certeza disso?"*. Foi testado no meu
console, com os meus jogos, do meu jeito.

Então sim, pode ter bug. Provavelmente tem, e eu ainda não faço ideia de qual — só vou
descobrir do pior jeito possível, que é alguém me contando que quebrou.

**Se você achar algum, me conta?** Por favor mesmo. Não precisa ser bonito nem técnico: um
*"travou quando eu fiz tal coisa"* já me ajuda mais do que você imagina. É logo ali nas
[Issues](https://github.com/NspxMiguel/SwitchSaveSync/issues).

Eu conserto. Choro um pouquinho antes, mas conserto.

E antes que dê ruim: **guarda uma cópia dos saves que você não pode perder.** Não porque eu
ache que vai dar errado — mas porque save é save, e eu ia dormir bem melhor sabendo que
você tem uma.

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
- **Tudo num arquivo só** — opcional: junta todos os saves num `.nxsaves`, um formato nosso que
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

**Switch V2 com firmware 18.1.0, em Atmosphère.** É a configuração em que ele foi escrito e
é onde ele roda todo dia — o resto é honestidade sobre o que ninguém tentou ainda.

Nada aqui depende de versão: o app não tem tabela de offsets, não faz patch em nada e não
lê estrutura interna de save. Ele usa as chamadas de montar save da libnx, que são as
mesmas desde o firmware 1.0, e a API do Google Drive, que é HTTPS. Então a chance de
quebrar numa versão diferente é baixa — mas *baixa* não é *testada*, e é isso que este
parágrafo está dizendo.

Se rodar (ou não rodar) em outra versão, [abra uma
issue](https://github.com/NspxMiguel/SwitchSaveSync/issues) contando qual — dá pra
transformar isto numa lista de verdade.

## Instalar

Precisa de um console com CFW (**Atmosphère**) e o menu de homebrew funcionando. Se isso
ainda não está de pé, resolve primeiro — é assunto de outro tutorial, não deste.

### 1. Um comando, ou quatro arquivos

Enfia o cartão no computador e roda, no **macOS ou Linux**:

```bash
curl -fsSL https://raw.githubusercontent.com/NspxMiguel/SwitchSaveSync/main/install.sh | bash
```

No **Windows**, no PowerShell — ou baixa o [`install.bat`](install.bat) e dá dois cliques:

```powershell
irm https://raw.githubusercontent.com/NspxMiguel/SwitchSaveSync/main/install.ps1 | iex
```

Ele mostra os cartões que enxerga, você escolhe um, e ele baixa a versão mais nova e põe
cada arquivo no lugar certo. Não apaga nada e nunca formata.

Na mão, em vez disso: o `SwitchSaveSync.nro` da
**[última versão](https://github.com/NspxMiguel/SwitchSaveSync/releases/latest)** vai pra
`sdmc:/switch/SwitchSaveSync.nro`. Esse arquivo sozinho é o app inteiro. A release traz
também o autosync — mais três arquivos — e um zip com a árvore do cartão já montada.

**[O passo a passo completo está no INSTALACAO.md](INSTALACAO.md)** *([in English](INSTALL.md))*:
pré-requisitos, autosync, a árvore do cartão, o que fazer quando não funciona, e as
perguntas que as pessoas fazem de verdade.

### 2. Abrir

Pelo menu de homebrew — mas **segurando R num jogo, não pelo Álbum**.

> Aberto pelo Álbum, o homebrew roda em *modo applet*: ganha só ~448 MB de memória e a
> pilha de rede às vezes nem sobe. Segurando **R** ao abrir um jogo instalado, o menu de
> homebrew abre no lugar do jogo e roda como aplicação, com a memória e a rede inteiras. Se
> tiver dúvida de em qual modo você está, o próprio app diz: aba **Ajustes**, em
> Diagnóstico.
>
> Isso deixa de ser necessário depois que você instala o atalho, mais abaixo.
>
> E o **R só funciona se o Atmosphère estiver deixando**: o
> `/atmosphere/config/override_config.ini` precisa ter `override_any_app=true`. O Atmosphère
> nunca escreve essa linha sozinho, e atualizar ele também não acrescenta — o instalador lá
> em cima cria o arquivo quando ele não existe.
> [O bloco exato está no INSTALACAO.md](INSTALACAO.md#a-linha-que-faz-o-r-funcionar).

### 4. Entrar na sua conta

Na primeira vez, o app mostra um **código** e um endereço (e um QR Code, se preferir a
câmera do celular). Você abre esse endereço no celular ou no PC, digita o código e autoriza.

**O console nunca pede a sua senha.** Quem faz login é você, na página do próprio Google.

O acesso pedido é o **`drive.file`**: o app só enxerga os arquivos que ele mesmo criou. O
resto do seu Drive fica invisível pra ele — não é promessa minha, é o Google que não deixa.

O login fica guardado só no cartão, em `/switch/SwitchSaveSync/token.txt`, e sai de vez no
**Sair da conta**.

> **De quem é a credencial?** É minha — o app vem com ela embutida, pra você não precisar
> criar projeto no Google Cloud pra usar um homebrew de save. A **conta é sua**, o **Drive
> é seu** e os **arquivos são seus**: eu não tenho acesso a nada disso, e o `drive.file`
> impede até o app de olhar o resto do seu Drive. Se ainda assim você preferir usar uma
> credencial sua, é só [compilar](#compilar-com-a-sua-própria-credencial) — o caminho
> continua aberto.

### 4. Pronto — e opcionalmente, com cara de jogo

Já dá pra usar: abre, aperta **A** num jogo, e ele resolve o resto.

### Deixar com cara de jogo, na tela inicial

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

### Autosync — opcional, e novo

O app aí de cima é o produto inteiro; esta parte é a mais. É um **sysmodule**: roda de
fundo e, quando você fecha um jogo, faz o backup do save daquele jogo sozinho. Você nunca
abre ele — você olha o que ele fez por um **overlay do
[Ultrahand](https://github.com/ppkantorski/Ultrahand-Overlay)**.

Três arquivos, e nenhum deles substitui o `.nro`:

| Do release | Onde vai no cartão |
| --- | --- |
| `exefs.nsp` | `/atmosphere/contents/00FF0000535953FF/exefs.nsp` |
| `toolbox.json` | `/atmosphere/contents/00FF0000535953FF/toolbox.json` |
| `SwitchSaveSync.ovl` | `/switch/.overlays/SwitchSaveSync.ovl` |

Depois reinicia, abre o overlay do Ultrahand e liga ele por lá.

> **Ele não sobe junto com o console, de propósito.** Não existe `boot2.flag` nessa pasta e
> não deve existir: uma coisa que monta savedata não sobe antes de você mandar. Você liga na
> mão, pelo overlay, e desliga quando quiser.

Ele usa o **mesmo login do app** — entra na conta por lá primeiro, senão a metade da nuvem
não tem com quem falar, e o overlay vai dizer isso.

**Onde isso está de verdade:** a metade do cartão roda no console hoje. A metade da nuvem
ainda está em teste, então trate como a parte que está sendo feita, e não como a parte em
que você confia. O `.nro` não depende de nada disso.

## Onde os saves vão parar

Numa pasta `Nintendo Switch Saves/`, na raiz do seu Drive (ou do seu WebDAV), uma pasta por
**jogo** e, dentro dela, uma pasta por **conta**:

```
Nintendo Switch Saves/
  Rayman Legends_ Definitive Edition/
    Jogador 1/       ← os arquivos, soltos
    Jogador 2/
  The Legend of Zelda_ Breath of the Wild/
    Jogador 1/
```

A pasta da conta leva o **apelido do perfil no console** — `Jogador 1` aqui é só exemplo —
ou `console`, pros saves que são do aparelho e não de uma pessoa.

A pasta da conta existe sempre, mesmo quando o jogo tem um save só: save solto na pasta do
jogo é save sem dono, e isso só continua verdade até outra pessoa do console abrir o mesmo
jogo.

Nada de formato fechado: dá pra abrir pelo site da nuvem e baixar um arquivo na mão quando
quiser. A única exceção é opcional — um `<Jogo>.nxsaves` do lado das pastas de conta, que é
o save daquele jogo, de todas as contas, num arquivo só pra levar de uma vez.

Nome é só nome. Quem identifica o backup é o par jogo + conta, anotado em
`/switch/SwitchSaveSync/pastas.txt` — por isso você pode renomear a conta do console à
vontade que o app não perde ele de vista.

## Compilar com a sua própria credencial

Nada disso é necessário pra usar o app — é pra quem prefere não passar pela minha
credencial, ou pra quem vai mexer no código.

Precisa do [devkitPro](https://devkitpro.org/wiki/Getting_Started) com o grupo
`switch-dev`, mais `switch-curl`, `switch-mbedtls`, `switch-zlib`, `switch-glfw`,
`switch-mesa` e `switch-glm`.

```bash
cp core/config.h.example core/config.h
```

O `config.h.example` tem o passo a passo com as telas: criar um projeto no
[console.cloud.google.com](https://console.cloud.google.com), ativar a **Google Drive API**,
montar a tela de consentimento e gerar um ID de cliente do tipo **"TVs e dispositivos de
entrada limitada"** — é esse tipo, e não outro: é o que libera o login por código, sem
teclado.

Um detalhe que economiza uma dor de cabeça: na tela de consentimento, deixe o status como
**"Em produção"**. Em *"Testing"*, o Google expira o login a cada **7 dias**. Como o
`drive.file` é um escopo não-sensível, publicar é imediato — não passa por verificação
nenhuma.

Cole o ID e o segredo no `config.h`; ele está no `.gitignore` e nunca entra em commit.

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=$DEVKITPRO/devkitA64
export PATH=$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH
make -C gui
```

Sai um `gui/SwitchSaveSync.nro` — daí é só copiar pro cartão como acima.

O autosync compila do mesmo jeito, e precisa das duas metades: `make -C sysmodule` e
`make -C overlay`. O `make -C app` compila a primeira versão, em modo texto, que fica de
histórico e não de uso.

## O que ele não faz

- **Não mexe em save de jogo aberto.** O jogo tem que estar fechado; o app avisa quando não
  consegue montar.
- **Não interpreta save.** Não edita, não converte, não "conserta" save.
- **O app não sobe nada sozinho.** No app, sincronizar é sempre um clique seu. Automático é
  outra coisa, que você instala e liga por conta — o sysmodule do autosync, ali em cima — e
  mesmo ligado ele não escreve por cima de save que mudou desde o último upload.
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
| `sysmodule/` | Autosync rodando de fundo | **Novo, em teste** |
| `overlay/` | O overlay do Ultrahand que liga ele | **Novo, em teste** |

O autosync faz o backup quando você **fecha** o jogo e — com a metade da nuvem ligada —
traz save de volta com o console parado no menu, atrás de três travas que impedem ele de
escrever por cima de coisa mais nova.
[Como instalar](INSTALACAO.md#2-autosync-opcional-e-ainda-em-teste).

A tela com barra de porcentagem já existe, mas ela aparece **no menu**, com o console
parado. O que falta é a outra ponta: a mesma tela na **abertura** do jogo, pro caso de você
abrir o jogo antes de o download terminar.

## Leitura

- [`ANALISE.md`](ANALISE.md) — a análise de viabilidade que começou o projeto: o que já
  existia pronto, o que precisava ser construído, e onde estava o risco.
- [`SAVES.md`](SAVES.md) — como save de Switch funciona de verdade, e o que isso obriga o
  app a fazer.
- [`NUVENS.md`](NUVENS.md) — **como adicionar uma nuvem nova você mesmo** (OneDrive,
  Dropbox, o que for): as doze funções que faltam escrever, onde registrar, como testar sem
  console, e os endpoints do OneDrive já mastigados. Não precisa pedir pra mim — é um
  arquivo novo, não uma cirurgia.

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
