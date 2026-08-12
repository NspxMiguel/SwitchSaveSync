# O servidor de autenticação

Três funções na Vercel que guardam a credencial do Google fora do `.nro`,
**fixam o escopo em `drive.file`** e freiam abuso. Plano gratuito, sem banco de
dados, sem nada pra pagar.

**Ele é opcional.** Sem `SSS_AUTH_ENDPOINT` preenchido no `config.h`, o app fala
direto com o Google como sempre falou, e nada disto aqui existe pra ele.

## O que ele resolve — e o que não resolve

Vale ser honesto sobre isso, porque a intuição erra aqui.

**Não resolve** o que parece resolver. Esconder o `client_secret` não impede
ninguém de usá-lo: o endpoint precisa ser público e sem autenticação (o cliente
é um Switch, não tem como provar que é ele), então quem quiser monta a mesma
tela de consentimento chamando *este servidor*. O `client_id`, que é o que
permite a impersonação, é público por definição — aparece na URL que o console
mostra pro usuário. Isto é [RFC 6749 §2.1](https://datatracker.ietf.org/doc/html/rfc6749#section-2.1):
app instalado é *public client*, e o padrão já assume que ele não guarda segredo.

**Resolve** três coisas concretas:

1. **Fixa o escopo.** É o ganho de verdade, e o único que protege o usuário
   final. Com a chave dentro do `.nro`, quem a extrai pode pedir consentimento
   com escopo `drive` inteiro — a vítima leria *"SwitchSaveSync quer acesso a
   todo o seu Google Drive"*, com o nome certo, e aprovaria. Com a chave aqui,
   `lib/google.js` ignora o que o cliente manda e sempre pede `drive.file`. O
   pior caso cai de "o Drive todo" pra "as pastas que o app criou".
2. **Trocar a chave sem release.** Chave queimada hoje significa todo mundo
   baixar `.nro` novo. Com o servidor, troca a variável de ambiente e pronto.
3. **Freio e botão de desligar.** Limite por IP, detector de rajada, e a
   possibilidade de derrubar o endpoint se algo sair do controle.

## Subir

```bash
cd servidor && npx vercel deploy --prod
```

Depois, no painel do projeto → **Settings → Environment Variables**:

| Nome | Valor |
| --- | --- |
| `GOOGLE_CLIENT_ID` | o client id do projeto no Google Cloud |
| `GOOGLE_CLIENT_SECRET` | o client secret |

E no `core/config.h` do app:

```c
#define SSS_AUTH_ENDPOINT "https://SEU-PROJETO.vercel.app/api"
```

Recompilar. O app passa a bater aqui em vez de bater no Google.

> A conta do Google Cloud deve ser **separada da pessoal** e **sem faturamento
> habilitado**. Assim o pior caso de um abuso é o projeto ser suspenso — nunca
> uma conta pra pagar, nunca o Gmail e o Drive de quem mantém o projeto.

## As rotas

Todas são `POST`, respondem o JSON do Google **sem tocar**, e não têm CORS de
propósito (o cliente é um console, não um navegador).

| Rota | Corpo | O que faz |
| --- | --- | --- |
| `/api/device` | nenhum | começa o login. O escopo é do servidor, não do cliente. |
| `/api/token` | `device_code=…` | troca o código aprovado por tokens. |
| `/api/refresh` | `refresh_token=…` | renova o access token. |

Freada devolve **429** com `{"error":"rate_limited"}` ou
`{"error":"under_attack"}` e um `retry-after`. O app distingue os dois: o
primeiro vira *"muitas tentativas deste endereço"*, o segundo vira *"foi
detectado um ataque nos servidores do SwitchSaveSync, favor tente novamente
mais tarde"*.

## O freio

Duas defesas, porque são dois problemas (`lib/limite.js`):

**Um endereço insistindo** → balde de fichas por IP e por rota. Os números
saem do uso real: o login faz poll de 5 em 5 segundos por até meia hora, então
a rota `/token` tem que aguentar 12 chamadas por minuto sem reclamar — tem um
teste só pra isso.

| Rota | Capacidade | Recarga |
| --- | --- | --- |
| `/device` | 10 | 2/min |
| `/token` | 90 | 15/min |
| `/refresh` | 60 | 10/min |

**Muitos ao mesmo tempo** → modo ataque, ligado por qualquer um dos três:
400 chamadas em 10 segundos, 60 chamadas abertas no mesmo instante, ou 3000
endereços diferentes vivos na memória. Dura 10 minutos e vale pra todo mundo,
inclusive pra quem acabou de chegar — é o que faz o app mostrar "não é você".

### O que este freio não faz

- **O estado é de memória.** Não tem banco de dados porque banco de dados
  significa conta em outro serviço e conta pra pagar. O preço: o contador zera
  quando a Vercel recicla a instância, e duas instâncias contam separado. Pra
  rajada — curta e concentrada — funciona. Pra alguém batendo devagar o dia
  inteiro, não pega. Se um dia isso importar, o lugar de trocar por Redis é
  `lib/limite.js`, sem mexer no app.
- **CGNAT.** Muita gente sai pelo mesmo IP. Por isso os baldes são folgados: é
  melhor deixar passar abuso do que travar o login de um bairro inteiro.

### Privacidade

Nenhum IP é guardado em texto — a chave do mapa é um hash. Nenhum log leva
token, `refresh_token`, `device_code` ou endereço; erro ao falar com o Google
sai como `[token] falhou: TypeError`, sem o que veio no corpo. O que este
servidor não escreve, ele não vaza depois.

## Testes

```bash
node servidor/teste.mjs
```

Ou junto com o resto: `./tests/run.sh servidor` (e `tudo` já inclui). São 19
casos, todos com o instante passado na mão — teste de freio não pode depender
do relógio.

## Quem não quiser depender disto

Qualquer pessoa pode passar por cima com um `google.cfg` no cartão, sem
recompilar nada — inclusive pra não depender deste servidor nunca mais. Está em
[INSTALACAO.md](../INSTALACAO.md#credencial-própria-google-cfg).
