# Homebrew App Store — o pacote, pronto pra enviar

Isto aqui é o que a loja pede pra listar o SwitchSaveSync. **Ainda não foi
enviado**: enviar é abrir um Pull Request no repositório de outra gente, e isso
é decisão de quem assina o projeto, não minha.

O app se qualifica. A política deles é curta — *"non-destructive apps that
1) serve a purpose, 2) have source code available and 3) aren't just clones"* —
e o repositório é público e GPLv3. Precisar de conta na nuvem não impede nada:
o **microNXSaveSync** (sincroniza save por servidor remoto), o **Switchfin**
(precisa de servidor Jellyfin) e o **NXpotify** (login de Spotify) estão na loja.
Sysmodule e overlay de Ultrahand também são aceitos — `sys-clk`, `emuiibo`,
`MissionControl`, `FPSLocker` e uns dez outros fazem exatamente isso.

## Isto já existe? (olhei antes, e a resposta é: parecido, sim; igual, não)

Vasculhei os **485 pacotes** da loja — 73 falam em save — e os repositórios dos que
chegam perto. O que existe hoje:

| App | Tamanho | O que faz de parecido | O que o separa daqui |
| --- | --- | --- | --- |
| **[Checkpoint](https://github.com/FlagBrew/Checkpoint)** | 3039★, 324 mil downloads | Gerenciador de save com motor de script em C, e vem com script `googledrive` (login por código de dispositivo, um zip por backup) e `webdav` | Exige **projeto próprio no Google Cloud** e o `client_secret.json` no cartão. A nuvem é destino de *backup*, não sincronização. Sem processo em segundo plano. |
| **[JKSV](https://github.com/J-D-K/JKSV)** | 1892★, 293 mil downloads | **Google Drive e WebDAV nativos**, com envio automático depois do backup | Mesmo projeto próprio no Google Cloud (guia de 10–15 min). Tudo parte de um clique seu dentro do app; não existe sysmodule. Faz muito que aqui não tem: criar, redimensionar e apagar savedata, todos os tipos de save, importar `.svi`. |
| **[NX-Save-Sync](https://github.com/Xc987/NX-Save-Sync)** | 38★, 10 mil downloads | Sincroniza save entre console e PC/emulador | Precisa do programa dele rodando no PC, na mesma rede. Não é nuvem. |
| **[uNSS / micro NX Save Sync](https://github.com/prodeveloper0/uNSS)** | 7★, 418 downloads | Sincroniza entre aparelhos | Por um **servidor central que você tem que subir e manter**. |
| **[BackupNX](https://github.com/SegFault42/BackupNX)** | 15★ | Manda arquivo pra nuvem (Dropbox) | Parado desde 2019, e com chave de API sua. |
| Neumann, svitch, Y'allAreNUTs, EdiZon-SE | — | Extrair, injetar e editar save | Local, sem nuvem e sem sincronização. |

Três coisas que **nenhum deles** faz, e que são a razão deste projeto existir:

1. **Ninguém tem sysmodule de save.** Em 485 pacotes não há um único processo em segundo
   plano que faça backup sozinho quando o jogo fecha. O conselho que circula na comunidade
   pra "automatizar" é subir um servidor de FTP no console e deixar o PC puxando com
   WinSCP. Aqui é um sysmodule com overlay pra ligar e desligar.
2. **Todo caminho de nuvem exige um projeto no Google Cloud.** Checkpoint e JKSV mandam a
   pessoa criar projeto, habilitar API, montar tela de consentimento e baixar um JSON —
   10 a 15 minutos antes do primeiro backup. Aqui a credencial vai embutida: você entra
   com a conta e acabou.
3. **Todos tratam a nuvem como lugar de backup.** Sobem cópias datadas e você escolhe qual
   restaurar. Aqui a decisão é de *sincronização*, estilo Steam: compara impressão digital
   dos dois lados, decide o sentido, e quando os dois mudaram ele pergunta em vez de
   escolher por você.

E o que **não** dá pra dizer: isto não substitui Checkpoint nem JKSV. Eles criam,
redimensionam e apagam savedata, editam save, cuidam de todos os tipos, gerenciam trapaça.
Este app faz uma coisa só.

Pro critério da loja — *"aren't just clones without major features of other apps"* — a
diferença que importa é o sysmodule e o não precisar de projeto no Google Cloud. Vale
escrever isso no corpo do Pull Request, com os links acima, em vez de esperar a pergunta.

## O que tem nesta pasta

| Arquivo | O que é |
| --- | --- |
| `pkgbuild.json` | O pacote. É o único arquivo que descreve o que vai pra onde no cartão. |
| `icon.png` | Ícone, **256x150**, exigido. |
| `screen.png` | Banner da página do app, **848x208**, exigido. |
| `icon.svg`, `screen.svg` | As fontes dos dois de cima. |
| `gerar.sh` | Refaz os PNG a partir dos SVG. |

Screenshots (`screen1.png`… `screenN.png`, 1280x720) são opcionais e só entram
por Pull Request — o formulário do site não tem campo pra elas. Vale a pena
quando existirem fotos de tela do app rodando no console.

## Como enviar

Há dois caminhos, e eles **não devem ser usados ao mesmo tempo** — já teve PR
fechado por causa disso.

**1. Pull Request (é o que serve aqui).** Copiar esta pasta pra
`packages/SwitchSaveSync/` num fork de
[fortheusers/switch-hbas-repo](https://github.com/fortheusers/switch-hbas-repo),
mandando só `pkgbuild.json`, `icon.png` e `screen.png` — os `.svg` e este
README não vão junto. É o único caminho onde dá pra declarar **qual arquivo vai
pra qual pasta**, e o nosso pacote tem quatro arquivos em três lugares.

**2. Formulário** em [submit.fortheusers.org](https://submit.fortheusers.org).
Só coleta metadados; quem escreve o `pkgbuild.json` é um mantenedor, adivinhando
o layout. Serve pra app de arquivo único, não pra este.

Quando o PR abre, a integração deles roda o `spinarak` e **comenta no próprio PR
o resultado do build**, com o manifest gerado. Erro de caminho aparece ali,
antes de qualquer humano olhar. Nos últimos PRs de app novo, a espera foi de
algumas horas a cinco dias.

Depois de entrar, **atualização é automática**: com a URL do GitHub no pacote,
um bot acompanha as releases novas e abre o PR de update sozinho. O que ele
compara é o campo `info.version` — release nova sem trocar a versão ali não
chega em ninguém.

## Pra conferir antes de abrir o PR

- [ ] `info.version` bate com a release, e a URL do zip aponta pra essa mesma tag.
- [ ] O zip da release ainda tem os quatro arquivos nos mesmos caminhos. Se um
      nome mudar, o build deles falha com *"is in the manifest, but does not exist"*.
- [ ] `binary` continua sendo `/switch/SwitchSaveSync.nro`. Sem esse campo, eles
      chutam o primeiro `.nro` do manifest e o botão de abrir o app quebra.
- [ ] Nada de configuração do usuário entrou no pacote. O que a loja instala,
      ela apaga na desinstalação — e o token do Google, os backups e o estado
      moram em `sdmc:/switch/SwitchSaveSync/`, que **não** está no pacote. É de
      propósito: desinstalar pela loja não pode levar backup de save junto.
- [ ] **Sem `boot2.flag`.** Outros pacotes de sysmodule incluem esse arquivo pra
      subir junto com o console; este não inclui, e a decisão é do dono do
      projeto. Quem instalar pela loja liga o sysmodule pelo overlay. O texto do
      `details` já explica isso — se um revisor perguntar "o sysmodule não sobe",
      a resposta é essa.

Testar o build na sua máquina antes, se quiser (é o mesmo que a integração deles roda):

```bash
git clone --recursive https://github.com/fortheusers/switch-hbas-repo && cd switch-hbas-repo
rm -rf packages/* && cp -R /caminho/deste/repo/hbas packages/SwitchSaveSync
rm packages/SwitchSaveSync/README.md packages/SwitchSaveSync/*.svg packages/SwitchSaveSync/gerar.sh
pip3 install -r ../spinarak/requirements.txt && (cd packages && python3 ../../spinarak/spinarak.py)
```
