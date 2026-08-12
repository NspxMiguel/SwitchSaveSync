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
