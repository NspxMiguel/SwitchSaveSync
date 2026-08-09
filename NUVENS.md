# Adicionar uma nuvem nova

Este guia é pra quem quer usar o SwitchSaveSync com uma nuvem que ele ainda não fala —
OneDrive, Dropbox, pCloud, Backblaze B2, o que for — e está disposto a escrever o código.
Não precisa pedir pra mim: o app foi montado pra isso ser um arquivo novo, não uma cirurgia.

> Se você só quer usar um servidor seu (Nextcloud, NAS da Synology ou da QNAP, ownCloud,
> Box), **não precisa programar nada**: já tem WebDAV pronto, em Ajustes → Onde salvar.
> Este guia é pra nuvem que fala uma API própria.

*In English: [CLOUDS.md](CLOUDS.md).*

---

## O tamanho da tarefa

Um arquivo novo em `core/`, **doze funções**, e três linhas de registro. Nada mais no app
muda — nem a tela dos jogos, nem a sincronização, nem o conflito, nem o `.nxsaves`.

Isso é possível porque a parte difícil já está escrita e não é de nuvem nenhuma. Subir uma
pasta inteira recursivamente, espelhar, apagar o que sumiu do console, baixar a árvore de
volta — tudo isso mora em [`core/cloud.c`](core/cloud.c) e é escrito **uma vez só**, em
cima de sete primitivas. Era metade do `drive.c` original e não tinha uma linha de Google
dentro.

Você implementa as sete primitivas. O resto você ganha de graça.

---

## Passo 1 — copie o vizinho mais parecido

Não comece do zero. Escolha pelo formato do **id**:

| Sua nuvem identifica arquivo por... | Copie | Por quê |
| --- | --- | --- |
| **caminho** (`/SwitchSaveSync/Zelda/save.dat`) | [`core/webdav.c`](core/webdav.c) | Já trata id-como-caminho, e é o menor dos dois |
| **id opaco** (`1a2B3c4D...`) | [`core/drive.c`](core/drive.c) | Já trata id opaco, paginação e OAuth |

Dropbox e OneDrive aceitam os dois estilos. Recomendo **id opaco** nos dois: renomear uma
pasta pelo site da nuvem não quebra nada, porque o id não muda.

Chame o arquivo de `core/onedrive.c` e `core/onedrive.h`, no mesmo molde dos vizinhos.

> **Você não precisa mexer em Makefile.** Os três (`gui`, `app`, `sysmodule`) listam
> `SOURCES` como *diretório*, não como lista de arquivos. Um `.c` novo em `core/` entra na
> compilação sozinho.

---

## Passo 2 — as doze funções

A tabela abaixo é o contrato. Está declarado em
[`core/cloud_backend.h`](core/cloud_backend.h) — leia lá também, os comentários dizem mais.

### As quatro de identidade e estado

| Campo | O que devolver |
| --- | --- |
| `key` | A palavrinha que vai gravada em `nuvem.cfg`. Minúscula, sem espaço: `"onedrive"`. Nunca mude depois de lançar, senão quem já escolheu essa nuvem volta pro Drive. |
| `name()` | O nome na tela: `"OneDrive"`. É **função** e não string fixa porque passa pelo `TR()`, e campo de struct estático não aceita escolha feita em tempo de execução. |
| `is_ready()` | `true` se dá pra usar agora (tem login gravado, tem servidor configurado). A tela mostra as nuvens prontas diferente das que faltam configurar. |
| `setup_hint()` | Uma frase curta dizendo o que falta quando `is_ready()` é `false`: `"Falta entrar na conta"`. Aparece embaixo do nome. |
| `logout()` | Apaga o login gravado no cartão. Tem que deixar `is_ready()` falso. |

### A da sessão

| Campo | O que devolver |
| --- | --- |
| `begin(auth, authsz)` | Prepara uma sessão e escreve em `auth` o que as outras funções vão precisar. No Drive é o `access_token`; no WebDAV é o header `Basic` já montado. O buffer tem `CLOUD_AUTH_MAX` (2048) — o token do Google passa de 1 KB, então não aperte isso. É aqui que você renova token vencido. |

### As sete primitivas

Todas recebem o `auth` do `begin()` como primeiro argumento.

| Campo | Contrato |
| --- | --- |
| `root(auth, id_out, outsz)` | O id da pasta-raiz do app, **criando se não existir**. É a única que pode criar sem pedir. |
| `find_child(auth, parent_id, name, want_folder, id_out, outsz)` | Acha um filho direto de `parent_id` chamado `name`. `want_folder` separa pasta de arquivo — sem isso, um jogo chamado igual a um arquivo confundiria os dois. **Não achar não é erro**: devolve `false` e pronto. |
| `make_folder(auth, parent_id, name, id_out, outsz)` | Cria a pasta e devolve o id. |
| `put_file(auth, parent_id, name, local_path, mime)` | Sobe o arquivo local. **Se já existe um com esse nome lá, substitui** — não duplica. Isso é obrigatório: sem isso, cada sincronização deixaria uma cópia nova. |
| `get_file(auth, id, local_path)` | Baixa pro caminho local. |
| `remove(auth, id)` | Tira da nuvem. **Prefira lixeira a apagar de vez**, se a sua nuvem tiver: isto aqui mexe em backup de save, e engano tem que ter volta. |
| `list(auth, folder_id, cb, userdata)` | Chama `cb` uma vez por filho direto, com `(id, name, is_folder, userdata)`. **Trate a paginação** — quase toda API devolve os primeiros N e um cursor pro resto. Uma pasta com 300 saves listada pela metade vira save apagado no `prune`. |

Junte tudo numa instância de `CloudBackend` e devolva o ponteiro, igual ao final do
`webdav.c`:

```c
static const CloudBackend g_onedrive_backend = {
    .key         = "onedrive",
    .name        = onedrive_be_name,
    .is_ready    = onedrive_be_is_ready,
    .setup_hint  = onedrive_be_setup_hint,
    .logout      = onedrive_be_logout,
    .begin       = onedrive_be_begin,
    .root        = onedrive_be_root,
    .find_child  = onedrive_be_find_child,
    .make_folder = onedrive_be_make_folder,
    .put_file    = onedrive_be_put_file,
    .get_file    = onedrive_be_get_file,
    .remove      = onedrive_be_remove,
    .list        = onedrive_be_list,
};

const CloudBackend *onedrive_backend(void) { return &g_onedrive_backend; }
```

---

## Passo 3 — registrar, em três linhas

**1.** Em [`core/cloud.h`](core/cloud.h), no enum `CloudKind`, **antes** do `CLOUD_COUNT`:

```c
typedef enum
{
    CLOUD_DRIVE = 0,
    CLOUD_WEBDAV,
    CLOUD_ONEDRIVE,   // <-- aqui
    CLOUD_COUNT,
} CloudKind;
```

> A ordem importa e não pode ser bagunçada depois: quem já tem `nuvem.cfg` gravado é
> reencontrado pela `key`, não pelo número — mas **acrescente sempre no fim**, antes do
> `CLOUD_COUNT`. Enfiar no meio muda o valor dos que vêm depois.

**2.** Em [`core/cloud_backend.h`](core/cloud_backend.h), a declaração:

```c
const CloudBackend *onedrive_backend(void);
```

**3.** Em [`core/cloud.c`](core/cloud.c), no `backend_of()`:

```c
case CLOUD_ONEDRIVE: return onedrive_backend();
```

Pronto. `cloud_name()`, `cloud_is_ready()`, `cloud_set_current()` e a gravação do
`nuvem.cfg` já percorrem o enum inteiro — elas passam a conhecer a sua nuvem sem você
tocar nelas.

---

## Passo 4 — a tela

Em `gui/source/main.cpp`, na aba **Ajustes**, grupo "Onde salvar", tem uma linha por
nuvem. Copie o padrão do `g_webdav_item`: uma linha que abre a tela de configuração
daquela nuvem.

Se a sua nuvem usa login por código (device flow), a tela de login do Google já é um molde
pronto — mostra o código, o link e o QR Code, e fica esperando. Se usa usuário e senha, o
molde é a tela do WebDAV.

---

## Passo 5 — testar sem console

Esta é a parte que economiza mais tempo, e é fácil de não perceber que existe.

**O `core/` inteiro roda no Mac ou no Linux.** O truque está em dois `#define`:
`SYNC_APP_DIR`, em [`core/syncstate.h`](core/syncstate.h), é `"sdmc:/switch/SwitchSaveSync"`;
e `CA_BUNDLE_PATH`, em [`core/http.c`](core/http.c), é `"romfs:/cacert.pem"`. Nenhum dos
dois começa com `/` — então no computador eles são **caminhos relativos**. Criando pastas
com esses nomes literais (`sdmc:` e `romfs:`, dois-pontos e tudo), o código do core roda
sem modificação nenhuma, inclusive a verificação de certificado TLS.

Veja o bloco `drive` no [`tests/run.sh`](tests/run.sh): ele monta essas pastas, aponta o
`romfs:/cacert.pem` pro certificado do próprio projeto, e roda a bateria contra a nuvem de
verdade. Copie esse bloco trocando `test_drive.c` pelo seu.

```sh
./tests/run.sh            # tudo que não precisa de conta nem de internet
./tests/run.sh drive      # fala com o Google Drive de verdade
```

Tudo compila com **ASan e UBSan ligados**. Estouro de buffer que no console vira crash sem
explicação, aqui vira uma mensagem com o número da linha.

> **Uma regra, e não é negociável:** teste apagar coisa **só** dentro de uma pasta de teste
> isolada. `cloud_prune_extras` e o `remove` do backend apagam de verdade. Nunca aponte um
> teste destrutivo pra pasta de save de alguém.

---

# OneDrive, concreto

O OneDrive dá certo e é o mais parecido com o que já existe: **mesmo device flow** do
Google, e ainda por cima sem segredo de cliente.

## Registrar o app (grátis)

1. [portal.azure.com](https://portal.azure.com) → **Microsoft Entra ID** → **Registros de
   aplicativo** → **Novo registro**.
2. Tipo de conta: **"Contas em qualquer diretório organizacional e contas Microsoft
   pessoais"** — sem isso, conta pessoal (que é a que tem OneDrive comum) não entra.
3. Em **Autenticação**, ligue **"Permitir fluxos de cliente público"**. É essa chave que
   libera o device flow.
4. Em **Permissões de API** → Microsoft Graph → **Permissões delegadas**, adicione
   `Files.ReadWrite.AppFolder` e `offline_access`.
5. Anote o **ID do aplicativo (cliente)**. **Não precisa de segredo** — cliente público não
   usa, e isso é uma vantagem real sobre o Google aqui.

> Use `Files.ReadWrite.AppFolder`, e não `Files.ReadWrite`. Ele é o equivalente do
> `drive.file` do Google: o app enxerga só a pasta dele, e nada do resto do OneDrive da
> pessoa. Pedir mais do que precisa é o que faz um app de save virar um app que lê tudo.

## O login (device flow)

Pedir o código — `{tenant}` é `consumers` pra conta pessoal, `common` se você quiser
aceitar conta de trabalho também:

```
POST https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode
Content-Type: application/x-www-form-urlencoded

client_id=SEU_ID&scope=Files.ReadWrite.AppFolder%20offline_access
```

Volta `user_code` (o que a pessoa digita), `verification_uri` (onde digita), `device_code`
(o que você guarda), `expires_in` e `interval`.

Aí você fica perguntando, esperando `interval` segundos entre uma pergunta e outra:

```
POST https://login.microsoftonline.com/consumers/oauth2/v2.0/token
Content-Type: application/x-www-form-urlencoded

grant_type=urn:ietf:params:oauth:grant-type:device_code&client_id=SEU_ID&device_code=...
```

Enquanto a pessoa não terminou, vem erro — e cada um quer uma reação diferente:

| Erro | O que fazer |
| --- | --- |
| `authorization_pending` | Normal. Espera e pergunta de novo. |
| `authorization_declined` | A pessoa recusou. Para de perguntar. |
| `expired_token` | Passou do `expires_in`. Para, e oferece começar de novo. |
| `bad_verification_code` | Você mandou o `device_code` errado. É bug seu. |

Deu certo, vem `access_token` (vale ~1 hora) e `refresh_token` (o que você grava no
cartão, porque é o que sobrevive). O `refresh_token` só vem se você pediu `offline_access`
lá no scope.

O [`core/oauth.c`](core/oauth.c) já faz exatamente essa dança pro Google, incluindo a
espera, o cancelamento pelo B e a renovação do token. É o arquivo pra copiar.

## As sete primitivas em Microsoft Graph

Base: `https://graph.microsoft.com/v1.0`, com `Authorization: Bearer <access_token>`.

| Primitiva | Chamada |
| --- | --- |
| `root` | `GET /me/drive/special/approot` → o campo `id`. A pasta nasce na primeira vez que você chama. |
| `find_child` | `GET /me/drive/items/{parent}:/{nome}` — 404 quer dizer "não tem", que não é erro. Pra saber se é pasta, veja se o objeto tem a faceta `folder`. |
| `make_folder` | `POST /me/drive/items/{parent}/children` com `{"name":"X","folder":{},"@microsoft.graph.conflictBehavior":"fail"}` |
| `put_file` | `PUT /me/drive/items/{parent}:/{nome}:/content` — **veja o aviso do tamanho abaixo** |
| `get_file` | `GET /me/drive/items/{id}/content` — responde **302** pra uma URL de download temporária; o curl tem que seguir redirecionamento (`CURLOPT_FOLLOWLOCATION`). |
| `remove` | `DELETE /me/drive/items/{id}` — vai pra lixeira do OneDrive, que é o comportamento que a gente quer. |
| `list` | `GET /me/drive/items/{id}/children` — **pagina**. Enquanto vier `@odata.nextLink` na resposta, busque a próxima página. |

### Duas armadilhas que vão te pegar

**1. O PUT direto só vai até 4 MB.** Acima disso o Graph recusa, e save de Switch passa
disso sem esforço — Zelda, Animal Crossing, qualquer jogo grande. Pra arquivo maior você
precisa de sessão de upload: `POST /me/drive/items/{parent}:/{nome}:/createUploadSession`,
e aí manda em pedaços com `PUT` na `uploadUrl` que voltou, usando `Content-Range` em cada
pedaço. Use múltiplos de 320 KiB nos pedaços — é o que a Microsoft pede.

Não caia na tentação de testar só com save pequeno e achar que funcionou.

**2. `Content-Type` é obrigatório no PUT.** Sem ele, o Graph responde 400 — ou pior, aceita
e grava o arquivo corrompido. Use `application/octet-stream`. O `put_file` já recebe um
`mime_type` de parâmetro; passe adiante.

---

# Dropbox, concreto

Também dá, e é o mais simples dos três: a API `/2/files` trabalha por **caminho**, não por
id, então o `find_child` vira concatenação de string e o `root` é uma constante.

1. Crie um app em [dropbox.com/developers](https://www.dropbox.com/developers) →
   **Scoped access** → **App folder** (não "Full Dropbox" — mesma ideia do
   `Files.ReadWrite.AppFolder`: o app só enxerga a pasta dele).
2. Permissões: `files.content.write` e `files.content.read`.
3. O device flow do Dropbox é mais limitado que o da Microsoft; o caminho normal é PKCE. Se
   você não quiser tela de navegador no console, dá pra gerar um token de longa duração no
   painel e deixar a pessoa colar — feio, mas honesto, e o molde de digitação já existe na
   tela do WebDAV.

Endpoints: `https://api.dropboxapi.com/2/files/*` pras operações de metadado (criar pasta,
listar, apagar) e `https://content.dropboxapi.com/2/files/*` pra subir e baixar. Aqui
também tem paginação: `list_folder` devolve um `cursor`, e você chama `list_folder/continue`
enquanto `has_more` for verdadeiro.

---

# iCloud Drive: não dá

E não é falta de vontade nem de tempo.

A Apple não tem API pública de terceiro pro iCloud Drive. O que existe é o **CloudKit**, que
só responde pra um app com identidade da Apple, e o **CloudKit Web Services** exige uma
conta paga do **Apple Developer Program**. Isso bateria de frente com a única regra
inegociável do projeto, que é **custo zero pra quem usa**.

Se algum dia isso mudar, o lugar de mexer é o mesmo dos outros: um `core/icloud.c` e três
linhas de registro.

---

## Antes de mandar o PR

- `./tests/run.sh` passando (é o mínimo, e não precisa de conta nenhuma pra rodar).
- Um teste seu no molde do `tests/test_drive.c`, apontado **só** pra uma pasta de teste.
- `make -C gui` sem aviso novo.
- Se a sua nuvem precisa de credencial, ela vai no `core/config.h` — que é gitignored.
  Coloque o passo a passo de conseguir a sua em `core/config.h.example`, do mesmo jeito
  que está lá pro Google.
- **Nunca commite credencial**, nem "só pra testar". Se escapar uma, considere ela queimada
  e gere outra: bot varrendo commit de GitHub atrás de chave é coisa de minutos, não de
  dias.

Abre uma [issue](https://github.com/NspxMiguel/SwitchSaveSync/issues) se travar em algum
ponto — de preferência antes de escrever tudo, que é mais fácil ajustar rumo no começo.
