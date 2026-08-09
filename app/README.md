# SwitchSaveSync — spike de teste

Isso **não é o produto final**. É o "passo 1" do roadmap em
[`../ANALISE.md`](../ANALISE.md): um homebrew simples (`.nro`), rodando via
Homebrew Menu, que só prova que a cadeia inteira funciona — login no Google
via Device Flow, upload e download de um arquivo no Drive, direto do
Switch. Sem sysmodule, sem `fs.mitm`, sem tocar em save de jogo nenhum
ainda. Risco zero de travar o console: se der erro, o pior caso é o
homebrew fechar.

Eu (Claude) escrevi este código sem conseguir compilar nem testar num
Switch de verdade — não tenho toolchain devkitPro nem hardware aqui. Espere
precisar corrigir 1-2 coisas no primeiro build. Me manda o erro do `make`
que eu ajusto.

**Aviso extra sobre `titles.c` e `savemount.c`:** esses dois arquivos são
significativamente mais arriscados que o resto. `http.c`/`oauth.c`/`drive.c`
só falam com a rede (fácil de auditar, formato de request/response bem
documentado pelo Google). Já `titles.c`/`savemount.c` dependem do layout
exato de duas structs internas da libnx que eu não consegui conferir sem o
SDK instalado — `FsSaveDataInfo` (em `switch/services/fs.h`) e
`NacpStruct`/`NsApplicationControlData` (em `switch/services/ns.h`). Se o
build quebrar nesses dois arquivos, ou se ele compilar mas mostrar nomes de
jogo errados / falhar ao montar o save, abra esses headers no seu
`$DEVKITPRO/libnx/include/` e compara os nomes de campo com o que
`titles.c` usa (especialmente `save_data_system_id` como "application_id" —
esse é o ponto mais incerto do arquivo inteiro).

## Pré-requisitos

1. [devkitPro](https://devkitpro.org/wiki/Getting_Started) instalado, com o
   pacote `switch-dev`.
2. Portlibs de rede/TLS:
   ```
   sudo dkp-pacman -S switch-curl switch-mbedtls switch-zlib
   ```
3. Um Switch com **CFW (Atmosphère) e Homebrew Menu já funcionando** — isso
   aqui assume que essa parte você já tem, é pré-requisito de qualquer
   homebrew.

## Configurar o login do Google (grátis, é seu)

O app precisa de um "OAuth client" seu no Google Cloud — é de graça e leva
uns 5 minutos. Sem isso ele não sabe com quem falar.

1. [console.cloud.google.com](https://console.cloud.google.com) → criar um
   projeto novo (qualquer nome, ex: "switch-save-sync").
2. **APIs e serviços → Biblioteca** → ativar **Google Drive API**.
3. **APIs e serviços → Tela de consentimento OAuth**:
   - Tipo: **Externo**.
   - Preenche nome do app e seu e-mail.
   - Em **Usuários de teste**, adiciona seu próprio e-mail do Google.
   - Não precisa publicar — modo "Teste" já deixa você logar.
4. **APIs e serviços → Credenciais → Criar credenciais → ID do cliente
   OAuth**:
   - Tipo de aplicativo: **TVs e dispositivos de entrada limitada**.
5. Copia o **Client ID** e o **Client Secret** gerados.
6. Copia `source/config.h.example` pra `source/config.h` e cola os dois
   valores lá dentro.

`config.h` está no `.gitignore` — não sobe pro Git.

## Build

```
cd app
make
```

Se der certo, gera `SwitchSaveSync.nro` na pasta.

Erros mais prováveis no primeiro build, e o que provavelmente significam:
- `switch_rules: No such file` → `$DEVKITPRO` não está setado no ambiente.
- `cannot find -lcurl` / `-lmbedtls` → faltou instalar os portlibs do passo 2.
- erro dentro de `http.c` sobre `CURLOPT_*` desconhecido → a versão do
  `switch-curl` instalada pode ser mais antiga/nova que a API que assumi;
  me manda o erro exato.

## Instalar no Switch

Copia `SwitchSaveSync.nro` pro cartão SD, em:

```
sd:/switch/SwitchSaveSync/SwitchSaveSync.nro
```

Abre pelo **Homebrew Menu** (Álbum, se você usa o applet takeover padrão do
Atmosphère).

## Testar

1. **A** → login. Mostra um código e uma URL (tipo `google.com/device`).
   Abre essa URL **no celular ou PC**, loga com a sua conta Google, digita
   o código. Volta pro Switch — se der certo, ele salva o token em
   `sd:/switch/SwitchSaveSync/token.txt` e não precisa logar de novo.
2. **X** → sobe um arquivo de teste (`switchsavesync_test.txt`) pra uma
   pasta `SwitchSaveSync` criada automaticamente no seu Google Drive.
3. **Y** → baixa esse mesmo arquivo de volta e mostra o conteúdo na tela.
   Se aparecer o texto certinho, a cadeia inteira funcionou:
   Switch → TLS → Google OAuth → Google Drive → Switch.
4. **L** → backup de verdade: lista os jogos que têm save no console (nome
   lido do NACP), você escolhe um, ele monta o save, copia pra uma pasta
   local de staging, e sobe tudo pro Drive dentro de
   `Nintendo Switch Saves/<Nome do Jogo>/` (cria a subpasta automaticamente
   se for a primeira vez).
5. **R** → restore de verdade: mesma lista, mas baixa
   `Nintendo Switch Saves/<Nome do Jogo>/` do Drive e escreve por cima do
   save local (pede confirmação com A antes, porque **sobrescreve**).
6. **B** → logout (apaga o token salvo). **+** → sair.

⚠️ **L/R mexem no save de verdade dos seus jogos.** Testa primeiro com um
jogo que não faça diferença perder o progresso (ou faz um backup manual do
save antes, por fora, tipo com o Checkpoint), até confiar que tá
funcionando direitinho.

Se o passo 1 (login) travar em "Aguardando confirmação" pra sempre, o mais
provável é:
- Wi-Fi do Switch sem internet de verdade (captive portal, etc).
- Relógio do Switch errado (TLS pode falhar com RTC zoado — checa
  Configurações do Sistema → Data e Hora).
- `client_id` errado/com espaço sobrando no `config.h`.

## O que vem depois

O passo (1) do roadmap original (ler/escrever save de verdade, não só
arquivo de teste) já está aqui — é o L/R. Os próximos passos (ver
`ANALISE.md`) são: (2) partir pro sysmodule com upload automático na saída
do jogo (sem precisar abrir esse `.nro` manualmente); (3) por último o hook
de download bloqueante via `fs.mitm`, com o diálogo de conflito estilo
Steam quando save local e save da nuvem divergirem. Este `.nro` aqui não
vira o sysmodule — a lógica de OAuth/Drive/save
(`oauth.c`/`drive.c`/`http.c`/`titles.c`/`savemount.c`) é que vai ser
reaproveitada lá, adaptada pro orçamento de memória de sysmodule (bem mais
apertado que um homebrew normal).
