# Instalação

*[In English](INSTALL.md).*

Dois caminhos. O primeiro é um comando só e faz tudo; o segundo são quatro arquivos
copiados na mão, e ele está aqui pra você conseguir conferir o que o primeiro fez.

---

## O comando único

**macOS ou Linux** — enfia o cartão no computador e roda:

```bash
curl -fsSL https://raw.githubusercontent.com/NspxMiguel/SwitchSaveSync/main/install.sh | bash
```

**Windows** — baixa o **[install.bat](https://github.com/NspxMiguel/SwitchSaveSync/releases/latest/download/install.bat)** e **dá dois cliques**. É isso: um arquivo
só, sem administrador, sem instalar nada. Ele busca o resto sozinho. O Windows mostra antes
aquele aviso pequeno de "Open File - Security Warning" — é o que qualquer script sem
assinatura recebe.

Se preferir digitar um comando, o PowerShell faz o mesmo:

```powershell
irm https://raw.githubusercontent.com/NspxMiguel/SwitchSaveSync/main/install.ps1 | iex
```

Ele lista os cartões FAT32/exFAT que enxerga, marca o que tem cara de Switch, e pergunta
qual é. Depois baixa a versão mais nova, copia quatro arquivos e confere se cada um chegou
do tamanho certo. **Não apaga nada** e nunca formata — teus jogos, saves e configurações
ficam onde estão.

Opções, pra quando você não quiser ser perguntado:

| | |
| --- | --- |
| `--card /Volumes/SWITCH` (`-Card E:\`) | usa esse cartão, sem procurar |
| `--app-only` (`-AppOnly`) | só o app, sem o autosync |
| `--zip arquivo.zip` (`-Zip`) | instala de um arquivo que você já baixou, sem internet |
| `--version v0.4.0` (`-Version`) | uma versão antiga em vez da mais nova |
| `--yes` (`-Yes`) | não pergunta nada |
| `--no-hbl` (`-NoHbl`) | não cria o `atmosphere/config/override_config.ini` |
| `--eject` (`-Eject`) | desmonta o cartão no fim |

Os dois scripts são curtos e dá pra ler, e são os mesmos arquivos que o comando lá em cima
baixa. Lê antes se preferir — é pra isso que eles vão como código, e não como programa
fechado.

---

## Antes de começar

- **Console com CFW (Atmosphère) e o menu de homebrew funcionando.** Se ainda não tem isso,
  resolve primeiro — esse tutorial é de outra pessoa, não é este aqui.
- **Firmware:** feito e testado no 18.1.0. O sysmodule do autosync declara o firmware 6.0
  como piso.
- **Espaço livre no cartão**: pelo menos o tamanho do teu maior save, duas vezes. Trazer um
  save de volta baixa pro cartão antes e ainda guarda uma cópia do save antigo.
- **Ultrahand** ([nx-ovlloader + Ultrahand Overlay](https://github.com/ppkantorski/Ultrahand-Overlay)),
  **só se você quiser o autosync.** O app sozinho não precisa.
- **sysMMC ou emuMMC — escolhe um e fica nele.** O que o app lembra mora no cartão, que os
  dois compartilham, enquanto os saves são separados. Sincronizar pelo sysMMC e depois pelo
  emuMMC faz o "já sincronizei esse save" descrever o save do outro sistema.

---

## 1. O app

### Baixar

A [versão mais recente](https://github.com/NspxMiguel/SwitchSaveSync/releases/latest) tem
cinco arquivos. Você precisa do zip **ou** dos arquivos avulsos:

| Arquivo | Onde ele vai, no cartão |
| --- | --- |
| `SwitchSaveSync-x.y.z-sd.zip` | descompacta **na raiz do cartão** — tudo o que está abaixo já vem dentro, nas pastas certas |
| `SwitchSaveSync.nro` | `/switch/SwitchSaveSync.nro` |
| `SwitchSaveSync.ovl` | `/switch/.overlays/SwitchSaveSync.ovl` |
| `exefs.nsp` | `/atmosphere/contents/00FF0000535953FF/exefs.nsp` |
| `toolbox.json` | `/atmosphere/contents/00FF0000535953FF/toolbox.json` |

Só o `.nro` é necessário pro app. Os outros três são o autosync, que é opcional e está na
parte 2.

**Copia e cola o id da pasta** — `00FF0000535953FF`. Digitar na mão é o jeito de acabar com
uma pasta que o console lê como mod de algum jogo em vez de módulo de sistema. E se você
compilou o sysmodule, o arquivo sai como `SwitchSaveSync.nsp` e precisa ser **renomeado pra
`exefs.nsp`**; com o nome errado nada carrega e nada reclama.

### A linha que faz o R funcionar

Pra abrir o homebrew *como aplicação* você segura **R** ao abrir um jogo instalado. Isso só
funciona se o Atmosphère estiver com o `override_any_app` ligado, e **ele não se liga
sozinho**: o pacote traz esse arquivo em `atmosphere/config_templates/` e nunca escreve por
cima do que está em `atmosphere/config/`. Atualizar o Atmosphère também não acrescenta.

Então, no cartão, o `/atmosphere/config/override_config.ini` precisa ter:

```ini
[hbl_config]
override_any_app=true
override_any_app_key=R
override_any_app_address_space=39_bit
path=atmosphere/hbl.nsp
```

Se o arquivo já existir, **mantém as linhas que já estão lá** (`program_id_1`,
`override_key_0` e companhia) e só acrescenta essas. O instalador faz exatamente isso: cria
o arquivo se ele não existe e, se existe, pergunta antes de encostar — e guarda uma cópia do
antigo do lado.

### Abrir

Pelo menu de homebrew, **segurando R em cima de um jogo instalado — não pelo Álbum**.

> Aberto pelo Álbum, homebrew roda em *modo applet*: uns 448 MB de memória e uma rede que às
> vezes simplesmente não sobe. Segurando R na hora de abrir um jogo, o menu de homebrew entra
> no lugar do jogo, com memória inteira e rede de verdade. Se estiver na dúvida de qual modo
> está valendo, o app conta: **Ajustes → Diagnóstico**.

### Entrar na conta

**Google Drive:** a primeira tela mostra um endereço, um código curto e um QR do mesmo
endereço. Abre no celular ou no PC, digita o código, autoriza. O console nunca vê tua senha,
e o app só recebe acesso aos arquivos que ele mesmo cria. O resultado é um token em
`sdmc:/switch/SwitchSaveSync/token.txt`; "sair da conta" apaga esse arquivo.

**Ou sem Google nenhum:** **Ajustes → Nuvem → WebDAV** aponta o app pro teu próprio
servidor — Nextcloud, ownCloud, Synology, QNAP, Box. Detalhes em [NUVENS.md](NUVENS.md).
Teu usuário e tua senha ficam no cartão, em texto puro, em
`sdmc:/switch/SwitchSaveSync/webdav.cfg`.

**Ou sem nuvem nenhuma:** o backup no cartão funciona sem conta e sem rede.

### Credencial própria (google.cfg)

Não é preciso, e a maioria não vai querer. Está aqui pra quem prefere **não
depender da credencial do projeto** — nem da que vem no app, nem do servidor
que a guarda.

Crie um projeto no Google Cloud (as instruções estão em
[core/config.h.example](core/config.h.example), leva uns 10 minutos) e escreva
`sdmc:/switch/SwitchSaveSync/google.cfg`:

```ini
client_id=SEU_ID.apps.googleusercontent.com
client_secret=SEU_SEGREDO
```

A partir daí o app fala direto com o Google com a **sua** chave: nenhum servidor
nosso entra no caminho, e nenhum limite nosso te alcança. Vale mais que tudo:
existindo esse arquivo completo, ele ganha do endpoint e da chave embutida.

Quem só quer sair do nosso servidor, sem criar chave nenhuma, escreve
`endpoint=direto` — aí volta a valer a credencial embutida no app.

> **Por que isso existe.** Em app instalado não há segredo: o
> [RFC 6749 §2.1](https://datatracker.ietf.org/doc/html/rfc6749#section-2.1)
> chama isso de *public client* e assume que a chave é extraível do binário. O
> que protege você é o escopo `drive.file` (o app só enxerga o que ele mesmo
> criou), a tela de consentimento, e — se quiser — usar a sua própria chave.
> Ajustes → Diagnóstico mostra qual das três está valendo agora.

### Conferir que funcionou

**Ajustes → Diagnóstico → Testar conexão** sobe e baixa um arquivinho. Não encosta em save
nenhum, e é o jeito mais rápido de saber se o problema é a conta, a rede, ou o modo em que o
app está rodando.

### Opcional: um ícone no menu do console

Com o [Sphaira](https://github.com/ITotalJustice/sphaira) dá pra fazer o app aparecer como
ícone no menu do próprio console, sem R e sem menu de homebrew.

> **Não mexe o `.nro` de lugar depois disso.** O atalho guarda o caminho do arquivo, e o
> Sphaira tira o title ID de um hash desse caminho. Mudou o arquivo de lugar, o atalho
> aponta pro nada — e refazer cria um *segundo* ícone em vez de consertar o primeiro. Deixa
> em `sdmc:/switch/SwitchSaveSync.nro` e pronto.

---

## 2. Autosync (opcional, e ainda em teste)

Um processo em segundo plano que faz backup do save quando você fecha o jogo e — se você
deixar — traz save da nuvem enquanto o console está parado no menu.

**1.** Instala o [Ultrahand](https://github.com/ppkantorski/Ultrahand-Overlay) primeiro. O
overlay é como você liga e desliga o sysmodule; não existe outro interruptor.

**2.** Os três arquivos da tabela lá de cima precisam estar no cartão.

**3.** Reinicia o console. O sysmodule é lido na hora de ligar; trocar o `exefs.nsp` com o
console aceso não muda nada até reiniciar.

**4.** Abre o menu do Ultrahand → **SwitchSaveSync**. São **dois interruptores, e os dois
importam**:

- **Sysmodule** — liga o processo. Desligado, nada roda.
- **Backup ao fechar jogo** — o autosync em si. **Ele nasce desligado**, de propósito: "não
  configurei nada ainda" não pode virar "pode mexer nos meus saves sozinho".

Depois, **Onde salvar** — *Cartão SD* e *Google Drive*, independentes, os dois ligados por
padrão. Desliga os dois e o autosync não tem onde pôr nada. **Jogos** desliga jogo por jogo.
**Fazer backup do último jogado** faz na hora, e recusa se tiver jogo aberto.

**Ele não sobe junto com o console.** Não existe `boot2.flag` no pacote e não deve existir:
uma coisa que monta savedata não sobe antes de você mandar.

### O que ele faz de verdade

**Subindo:** três segundos depois de o jogo fechar, o save daquele jogo vai pra cima.

**Baixando — lê esta parte.** Com a nuvem ligada e o console parado no menu por 30 segundos,
ele varre a biblioteca e traz save de volta, um jogo por passada, cada jogo no máximo a cada
30 minutos. Baixar **substitui o savedata inteiro** — o conteúdo é apagado e regravado. É o
que sincronizar significa, e é também a parte que pode custar progresso, então só acontece
quando as três coisas valem:

1. **Este console já subiu aquele save antes.** Sem registro de sincronização anterior, não
   baixa.
2. **O save local não mudou desde aquele upload.** Se mudou, o local ganha e a nuvem fica
   quieta.
3. **Uma cópia do save local foi gravada no cartão antes.** Se essa cópia falhar, o download
   é abortado.

Enquanto baixa, uma tela com barra de porcentagem fica por cima do menu, com dois botões:
*"Nao puxar save da nuvem nesse jogo"* e OK. Ela fica de pé por pelo menos cinco segundos.

### Como saber se está funcionando

O overlay mostra a última linha de status. A versão longa é o
`sdmc:/switch/SwitchSaveSync/autosync.log`.

---

## O cartão, com tudo no lugar

```
/switch/SwitchSaveSync.nro                              o app
/switch/.overlays/SwitchSaveSync.ovl                    o overlay do Ultrahand
/switch/SwitchSaveSync/                                 tudo o que o app lembra
    token.txt          login do Google       webdav.cfg     servidor, usuário e senha
    nuvem.cfg          qual nuvem vale       idioma.txt     idioma
    google.cfg         credencial sua (opcional)
    autosync.cfg       autosync lig/desl     destino.cfg    cartão e/ou nuvem
    excluidos.txt      jogos que você tirou  pastas.txt     pasta na nuvem de cada save
    status.txt         último status         autosync.log   a versão longa
    rev-*.txt          marcadores de "já sincronizei"
    backups/           backups no cartão     staging/       área de trabalho
/atmosphere/contents/00FF0000535953FF/exefs.nsp         o sysmodule
/atmosphere/contents/00FF0000535953FF/toolbox.json      fica do lado dele
/atmosphere/config/override_config.ini                  o que faz o R funcionar
```

Apagar `/switch/SwitchSaveSync/` joga fora teu login, teus backups no cartão e os marcadores
de sincronização. Nada na nuvem é tocado, mas a próxima sincronização vai parecer a primeira
de todas.

---

## Se não funcionou

**Seguro o R e o jogo abre normal.** Falta `override_any_app=true` no
`/atmosphere/config/override_config.ini`. Está explicado lá em cima. Nove em cada dez vezes
é isso.

**"Sem conexão", mas o wi-fi está bom.** Você está em modo applet — abriu pelo Álbum em vez
de segurar R num jogo. **Ajustes → Diagnóstico** diz em qual modo você está.

**O overlay não aparece na lista do Ultrahand.** O `.ovl` tem que estar exatamente em
`/switch/.overlays/SwitchSaveSync.ovl`, e o Ultrahand tem que estar instalado.

**O interruptor Sysmodule volta pra desligado sozinho.** O `exefs.nsp` não está onde devia,
ou o id da pasta está errado, ou você não reiniciou depois de copiar. O overlay mostra o
código da falha.

**"Sem conta Google salva — abra o app e faça login".** O sysmodule não tem login próprio:
ele usa o token do app. Abre o app uma vez e entra na conta.

**Diz que preciso entrar de novo.** Ou o token foi revogado — o app apaga sozinho e avisa —
ou a rede caiu, e aí ele fala diferente. Se você compilou com credencial própria e deixou a
tela de consentimento do Google em *Testing*, o Google expira esse token a cada 7 dias.

---

## Perguntas

**Preciso do sysmodule?** Não. O `.nro` é completo sozinho e não depende de nada disso.

**Isso apaga meu save?** Por acidente, não. Subir não encosta no save do console. Trazer de
volta, seja você pedindo ou o autosync, substitui o savedata — e sempre grava uma cópia no
cartão antes, recusa com o jogo aberto, e pergunta quando as duas cópias discordam.

**Funciona com duas contas?** Funciona. O jogo aparece uma vez, com quantos saves houver
contas, e cada um ganha a própria pasta na nuvem — com o apelido do perfil, ou `console` pros
saves que são do aparelho e não de uma pessoa. Renomear o perfil depois não deixa backup
órfão: a pasta fica anotada no `pastas.txt`.

**Dá pra usar sem conta do Google?** Dá — WebDAV, ou o backup no cartão, sem rede nenhuma.

**Meu jogo não aparece na lista.** Só jogo instalado é listado. Save que sobrou de um jogo
que você apagou não aparece.

**Dá pra subir junto com o console?** Não, e é decisão consciente. Quem liga é você, pelo
overlay.

**Como atualizo?** Roda o instalador de novo, ou copia os arquivos por cima. Reinicia se o
`exefs.nsp` mudou. Nada em `/switch/SwitchSaveSync/` é tocado, então você continua logado e
com tuas configurações.

**Como desinstalo?** Apaga `/switch/SwitchSaveSync.nro`,
`/switch/.overlays/SwitchSaveSync.ovl` e `/atmosphere/contents/00FF0000535953FF/`. Apaga
também `/switch/SwitchSaveSync/` se quiser o login e os backups locais fora — o que está na
nuvem continua na nuvem, e isso você apaga pelo site da própria nuvem.
