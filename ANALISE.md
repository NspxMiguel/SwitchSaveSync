# Auto save sync no Switch (CFW) estilo Steam Cloud — análise de viabilidade

Data:

## Veredito curto

**Viável, com uma ressalva grande.** As duas metades difíceis do problema — ler/escrever
save de qualquer jogo, e falar HTTPS com a API do Google Drive — já estão resolvidas e
rodando hoje em homebrew público. O que **não** existe pronto é o gatilho automático:
"ao entrar no jogo, baixa o save de lá". Essa é a parte que precisa ser construída, e é
onde mora todo o risco técnico.

Ou seja: 80% do trabalho é integração de peças conhecidas; 20% é um problema de
arquitetura de sistema que tem duas soluções possíveis, uma fácil-mas-imperfeita e uma
elegante-mas-perigosa.

---

## O que já está resolvido (não precisa inventar)

### Acesso ao save de outros jogos
`fsOpenSaveDataFileSystem` da libnx, com `FsSaveDataSpaceId_User` + o application ID e o
user ID (conta do console). Homebrew rodando em **title takeover** (substituindo um jogo
instalado, não via applet do Álbum) herda permissões suficientes no `fsp-srv` pra abrir
save de qualquer título. Depois de escrever é obrigatório dar `fsdevCommitDevice()` —
sem commit, o journal não é aplicado e você perde tudo.

Checkpoint, JKSV e EdiZon fazem exatamente isso há anos. É terreno batido.

### HTTPS e Google Drive a partir do console
libnx tem sockets; devkitPro tem curl + mbedTLS nos portlibs. Você embute seu próprio
`cacert.pem` no romfs (o console não te dá um trust store utilizável).

E o mais importante: o **JKSV já sobe save pro Google Drive**, inclusive usando o fluxo
de OAuth *Limited Input* (device flow) — você pega um código na tela do Switch e faz o
login pelo celular, sem depender do browser interno do console. Também tem WebDAV.
Isso mata duas dúvidas de uma vez: dá pra autenticar sem browser, e o escopo de Drive
funciona no device flow do Google na prática.

**Consequência prática: o projeto pode nascer como um fork do JKSV**, não do zero. Ou,
mais limpo, como um serviço separado que reusa a mesma abordagem de auth.

---

## O problema de verdade: o gatilho

Steam Cloud funciona porque o Steam **é** o launcher — ele baixa o save antes do
processo do jogo existir. No Switch, o menu HOME é da Nintendo e não te dá hook.

Três arquiteturas possíveis, da mais segura pra mais ambiciosa:

### Opção A — Launcher próprio (só spike de validação — ver decisão abaixo)
Um homebrew que lista seus jogos, e ao selecionar um: baixa o save do Drive → grava →
commita → lança o título via `ns` (`nsam` / `LaunchApplication`). Na volta pro homebrew,
sobe o save.

- **Prós:** ordem de execução garantida, zero risco de corromper save, tudo em user-mode,
  fácil de debugar.
- **Contras:** você tem que entrar no launcher todo santo dia em vez de clicar o jogo no
  menu HOME. Se esquecer uma vez, dessincroniza. Não é "Steam-like", é "GOG Galaxy-like".

### Opção B — Sysmodule em background (meio-termo)
Um sysmodule do Atmosphère (mesma categoria de `sys-clk`, `sys-botbase`, `MissionControl`)
que fica de olho no processo de aplicação via `pm:dmnt` e dispara sync **no boot** e **na
saída do jogo**. Um overlay Ultrahand dá botão de "sync agora" sem fechar o jogo.

- **Prós:** funciona lançando pelo menu HOME normal. Upload automático é confiável.
- **Contras:** o download automático não é. Quando o sysmodule detecta o jogo, o jogo já
  abriu o save. Então na prática você tem "upload automático + download no boot", que
  cobre o caso de um console só, mas falha no cenário de dois consoles alternando.
  Sysmodule também tem orçamento de RAM apertado — TLS + JSON + zip nesse espaço exige
  cuidado (dá pra fazer, o `MissionControl` roda pilha Bluetooth inteira ali).

### Opção C — `fs.mitm` bloqueante (o "de verdade")
Interceptar `OpenSaveDataFileSystem` no `fsp-srv` via mitm do Atmosphère e **segurar a
chamada** até o download terminar. Isso é literalmente o comportamento Steam: o jogo não
enxerga o save antigo, porque a abertura só retorna depois do sync.

- **Prós:** é a solução correta. Funciona com qualquer jogo, lançado de qualquer lugar.
- **Contras:** segurar um IPC do FS por 5–30 segundos é ótimo jeito de tomar crash ou
  timeout dentro do jogo; se o Wi-Fi cair no meio, você travou o console. Precisa de
  timeout agressivo com fallback pro save local, e de uma tela de "sincronizando" que o
  jogo não desenha (você teria que desenhar por overlay). Alto risco, alta recompensa.

**Caminho sensato (revisado):** ver decisão de arquitetura logo abaixo, depois da
referência do Fizeau — a Opção A deixou de ser o MVP-alvo e virou só um spike interno de
validação.

### Referência: como o Fizeau resolve isso

Boa referência de arquitetura. O [Fizeau](https://github.com/averne/Fizeau) (correção de
cor via CMU do Tegra X1) é essencialmente a Opção B já madura e testada em produção há
anos, e é um ótimo modelo pra copiar a esqueleto:

- **Três componentes, cada um com um trabalho:** sysmodule (serviço de background),
  aplicativo NRO (config/GUI) e overlay Ultrahand (ajuste rápido sem sair do jogo). Save sync
  mapeia 1:1 nisso: sysmodule cuida do sync, um NRO cuida de login OAuth e escolher quais
  jogos sincronizar, overlay dá um "sync agora" / mostra status sem fechar o jogo.
- **Não fica pollando.** Pra economizar RAM/CPU, o Fizeau não fica lendo config o tempo
  todo — aplica valores só quando um evento dispara (troca de perfil/título). Isso é
  exatamente o padrão certo pra um sysmodule de save: ficar dormindo e só acordar em
  eventos de processo, não em polling contínuo.
- **IPC enxuto.** Abandonaram libstratosphere e foram pro IPC minimalista do `sys-clk`
  pra manter o executável pequeno — sysmodule roda com orçamento de memória apertado, e
  isso é ainda mais crítico pro save sync porque ele também precisa de TLS + JSON, que já
  comem RAM sozinhos.

**Onde a analogia quebra — e é a parte que importa:** o "gatilho" do Fizeau é *aplicar
valores de cor quando o título muda*, e isso é uma ação **não-bloqueante e sem risco**: se
a cor demorar um frame a mais pra atualizar, ninguém nota. O gatilho do save sync é
*garantir que o save certo já esteja no disco antes do jogo abrir o arquivo*, que é uma
ação que **precisa bloquear** — se o download atrasa e o jogo já abriu o save antigo, o
sync automático não serviu pra nada (ou pior, cria conflito).

Ou seja: dá pra copiar a esqueleto inteira do Fizeau (sysmodule + NRO + overlay, detecção
de lançamento de processo via `pm:info`/notificações do `ns`) pro **upload automático na
saída do jogo**, que não tem essa exigência de bloqueio. Mas pro **download antes de
abrir**, copiar só a esqueleto não basta — é por isso que a Opção C (interceptar
especificamente a chamada `OpenSaveDataFileSystem` via `fs.mitm`, não o `fsp-srv` inteiro)
existe. A boa notícia é que dá pra restringir o mitm a **só esse comando específico**, com
timeout curto e fallback pro save local se o Drive não responder — não precisa
interceptar todo o tráfego do sistema de arquivos, só essa chamada pontual no boot do
jogo.

**Design proposto, juntando os dois:**

1. Sysmodule (base Fizeau/sys-clk de IPC) detecta lançamento e saída de título via
   `pm:info`.
2. Na **saída**: upload em background, sem pressa, sem risco — igual ao padrão do Fizeau
   de "reagir a evento, não pollar".
3. No **lançamento**: hook pontual em `OpenSaveDataFileSystem` via `fs.mitm`, com timeout
   (2–3s pra checar se há versão nova; se passar disso, deixa abrir o save local e avisa
   por overlay que não sincronizou). Isso limita o "perigo" do mitm a uma janela curta e
   a um único comando, em vez de segurar o FS inteiro.
4. Overlay Ultrahand mostra status (`sincronizado` / `desatualizado, abrindo local` /
   `sincronizando…`) sem precisar abrir o NRO.

Isso é mais trabalho que a Opção B pura, mas é bem menos arriscado que mitm-in-tudo da
Opção C original, e reaproveita quase toda a esqueleto que o Fizeau já validou.

### Decisão de arquitetura: sysmodule é o alvo, não o launcher

Ponto correto e muda a prioridade do projeto: a Opção A (launcher) tem um defeito que eu
descrevi mas subestimei — ela exige um passo manual extra (abrir o launcher, escolher o
jogo na lista) toda vez, em vez de simplesmente clicar o jogo no menu HOME como sempre.
Isso não é "Steam-like", é fricção adicional, e é exatamente o tipo de coisa que faz um
projeto de "conveniência" não ser usado na prática.

O sysmodule (B+C combinados, como desenhado acima) não tem esse problema: ele fica
residente, detecta *qualquer* título sendo lançado via `pm:info` — não precisa de uma
lista curada, não precisa de um NRO ou entrada por jogo, não precisa que você "cadastre"
o jogo em lugar nenhum. Lança o jogo do jeito que sempre lançou (menu HOME, álbum,
o que for) e o hook de `fs.mitm` intercepta a chamada de save independente de quem
chamou. Isso é o que dá o "funciona em qualquer jogo e GG" — é uma propriedade real da
arquitetura B+C, não só uma esperança.

**Isso muda o roadmap:** a Opção A deixa de ser "o MVP que os usuários usam" e vira só um
**spike de validação interna** — código descartável pra provar a cadeia FS+TLS+Drive
antes de meter a mão no sysmodule, que é mais difícil de depurar (crash de sysmodule
geralmente é tela preta/reboot, não um stacktrace amigável). Ninguém usa a Opção A como
produto final.

**Sobre o overlay:** correto, hoje em dia o [Ultrahand
Overlay](https://github.com/ppkantorski/Ultrahand-Overlay) é o padrão — é um drop-in
replacement pro Tesla Menu (construído sobre `libultrahand`, um fork evoluído da
`libtesla`), acessível por qualquer jogo via hotkey, sem precisar suspender o jogo pra
abrir. E o próprio Fizeau, na versão referenciada acima, já usa `libultrahand` no overlay
dele — então a base de código a copiar já está no padrão certo, não precisa portar nada
de Tesla pra Ultrahand. Corrigido no resto deste documento: onde eu tinha escrito "overlay
Tesla", leia-se Ultrahand.

### Diálogo de conflito (estilo Steam)

É a peça que faltava e melhora o design: em vez de resolver conflito sozinho
(last-write-wins silencioso, como eu tinha proposto acima), fazer igual ao Steam —
só interromper o usuário **quando há conflito de verdade**, e deixar ele escolher.

**Quando o Steam mostra a tela:** só quando save local e save da nuvem divergem e
nenhum dos dois é claramente descendente do outro (ex: jogou offline em um PC E depois
em outro sem sincronizar entre os dois). Se a nuvem é estritamente mais nova que o
local (ou idêntica), ele sincroniza direto, sem perguntar nada. É esse o comportamento
a copiar — a tela de escolha é exceção, não regra, senão vira irritante rápido.

**Detecção do conflito:** dá pra fazer sem servidor próprio, só com o metadata JSON que
já tinha proposto (hash + timestamp do console + contador monotônico por save):
- hash da nuvem == hash do último save que *este* console mandou → sem conflito, nuvem é
  descendente conhecida, sincroniza direto.
- hash da nuvem é diferente E o contador da nuvem é maior que o último que este console
  viu → outro console escreveu depois → baixa direto, sem perguntar (é o caso normal de
  "joguei no Switch 2 ontem, hoje pego o Switch 1").
- hash local mudou desde o último sync E hash da nuvem também mudou desde o ponto comum
  → **conflito de verdade** → mostra a tela.

**Onde a tela mora, por arquitetura:**
- **Opção A (launcher):** trivial. É uma NRO com GUI completa, antes do jogo abrir — dá
  pra desenhar duas opções lado a lado (local: "última jogada em [data]" / nuvem: "última
  jogada em [data], enviada por [nome do console/perfil]"), esperar input, e só então
  lançar o jogo com o save escolhido. Isso é essencialmente de graça na Opção A.
- **Opção B/C (sysmodule + fs.mitm):** bem mais delicado. O hook do `fs.mitm` dispara
  muito cedo no boot do jogo, antes do próprio jogo ter desenhado a primeira tela — pra
  mostrar um diálogo interativo aí, precisa do overlay Ultrahand renderizando por cima nesse
  instante exato e capturando input, o que é mais complexo de acertar em termos de timing
  que o "sync silencioso com timeout" que descrevi antes. E diferente do timeout de 2-3s
  do plano original, aqui você está esperando o usuário decidir — **isso não pode ser um
  bloqueio indefinido**: precisa de um timeout longo (ex: 15s) com uma escolha padrão
  segura (ex: "usa o local, não mexe em nada") se ninguém responder, senão um diálogo que
  falha em desenhar trava o boot do jogo pra sempre.

Consequência pro roadmap: o diálogo de conflito é fácil na Opção A e é o motivo extra
pra deixar a Opção A rodando por mais tempo antes de partir pra B/C — ele testa a lógica
de detecção de conflito num ambiente onde, se der problema, o pior caso é "o launcher
trava", não "o jogo trava".

### Achievements

Você mesmo já desconfiou certo: **isso é inviável dentro deste projeto**, não por
dificuldade de engenharia pontual, mas porque é um problema de escopo completamente
diferente.

Save sync funciona porque save data tem uma API oficial e uniforme
(`fsOpenSaveDataFileSystem`) — o mesmo código lê o save de qualquer jogo. Não existe
equivalente pra "o jogador derrotou o chefe X" ou "achou os 100 itens". O Switch não tem
sistema de conquistas nativo (ao contrário de Xbox/PSN) exposto por API alguma, oficial
ou de homebrew. A única forma de saber que uma condição de jogo aconteceu é **inspecionar
memória ou save data específico daquele jogo** — e cada jogo guarda isso de um jeito
diferente, sem padrão, sem documentação.

O paralelo real é o **RetroAchievements**: ele funciona porque cada jogo (de emulador)
tem um conjunto de "achievement definitions" — endereços de memória específicos —
criados manualmente pela comunidade, jogo por jogo, ao longo de anos. Ferramentas como
`sys-botbase`/EdiZon já dão a capacidade técnica de ler memória de um processo no Switch
via CFW (é como cheat engines funcionam), então o bloqueio não é "não dá pra ler
memória" — é que **cada jogo comercial de Switch precisaria do próprio mapeamento**,
feito por engenharia reversa individual, um projeto de comunidade do tamanho do próprio
RetroAchievements, não uma feature de um app de save sync.

**Veredito:** fora de escopo. Se algum dia isso virar realidade, nasce como projeto
irmão independente (banco de definições por jogo + client que lê memória via
sys-botbase), não como módulo do save sync. Não vou detalhar mais que isso — não vale o
investimento de análise agora.

---

## Riscos e detalhes que mordem

| Item | Situação |
|---|---|
| **Ban da Nintendo** | O maior risco não-técnico. Console com CFW conectado à internet é candidato a ban. Mitigação padrão: emuMMC + bloquear os domínios da Nintendo (90DNS) — o Google Drive continua acessível normalmente, só os servidores da Nintendo ficam inalcançáveis. Isso precisa estar escrito em letra garrafal no README. |
| **Escrever save com jogo aberto** | Não faça. O save já está montado pelo jogo; escrever por baixo é corrupção quase garantida. Toda escrita só com o jogo fechado. |
| **Commit** | Esquecer `fsdevCommitDevice` = save que "sumiu". Erro clássico. |
| **Conflito** | Sem servidor próprio não existe timestamp confiável dos dois lados. Estratégia: metadata JSON junto do backup (hash + timestamp do console + contador monotônico), last-write-wins com aviso, e manter as últimas N revisões no Drive pra poder voltar. Nunca sobrescrever destrutivamente. |
| **Relógio** | TLS quebra com RTC errado, e o Switch pode acordar com data zoada. Checar clock antes de tentar handshake e falhar com mensagem clara. |
| **Tamanho** | Maioria dos saves é KB–poucos MB; alguns (Animal Crossing, Smash) chegam a dezenas de MB. Zip + upload em Wi-Fi do Switch (realista: 10–40 Mbit) dá poucos segundos. Não precisa de delta sync. 15 GB grátis do Drive sobra. |
| **Refresh token** | Guardado em texto no SD. Escopo `drive.file` (só enxerga o que o próprio app criou) limita o estrago se o SD vazar. Não use `drive` completo. |
| **Multi-usuário** | Save é por conta do console. Se você tem mais de um perfil, o mapeamento perfil↔conta Google precisa ser explícito. |

## Legal

Homebrew com libnx/devkitPro, mexendo nos seus próprios saves, no seu próprio console:
sem problema. O que não pode entrar no repo são chaves (`prod.keys`) ou qualquer conteúdo
da Nintendo. O projeto não toca em nada disso.

---

## Roadmap sugerido (revisado — sysmodule é o produto)

1. **Spike de 1 dia (descartável, nunca vira produto):** app homebrew simples que lê o
   save de *um* jogo hardcoded, zipa e faz upload pro Drive com token colado à mão num
   arquivo do SD. Só valida a cadeia FS + TLS + API com o mínimo de código, fora do
   ambiente mais difícil de depurar do sysmodule.
2. **MVP de verdade — sysmodule com upload automático:** base de IPC estilo
   Fizeau/sys-clk, detecção de saída de título via `pm:info`, upload em background sem
   bloquear nada. Zero risco de travar jogo, porque não há hook bloqueante ainda — só
   escreve na nuvem depois que o jogo fechou. Já cobre "esqueci de fazer backup" sozinho.

   * conversa privada removida do historico
conversa privada removida do historico
conversa privada removida do historico
conversa privada removida do historico
conversa privada removida do historico
   "depois a gente vê", é não vai ter. O sysmodule vai pro SD mas não sobe no
   boot: lança na mão pelo overlay Ultrahand. Se ele crashar, é só não lançar, e
   o console boota normal. O raciocínio dele está certo no ponto que importa:
   sem auto-boot, uma atualização de firmware que quebre o sysmodule não tem
   como travar o boot, porque nada sobe sozinho. (Mesmo com auto-boot não
   bricaria: Atmosphère inteiro mora no SD, a firmware da NAND não é tocada, e a
   recuperação seria tirar o SD e apagar `/atmosphere/contents/<TID>/`. Mas o
   custo de não ter auto-boot, no uso dele, é realmente ~zero: o console fica em
   standby, então o sysmodule lançado uma vez continua vivo.)
3. **v1.1 — metadata e conflito:** hash + timestamp + contador monotônico por save,
   histórico de revisões no Drive, e a lógica de detectar "conflito de verdade" descrita
   acima (mesmo sem UI de escolha ainda — pode só logar/avisar por overlay Ultrahand).
4. **v2 — download bloqueante + tela "puxando save da nuvem":** ~~hook pontual em
   `OpenSaveDataFileSystem` via `fs.mitm`~~ — **feito, mas por outro
   caminho.** O `fs.mitm` está bloqueado: a Atmosphère já registra o mitm de
   `fsp-srv` no `ams.mitm.fs`, e o `sm` aceita **um** mitm por serviço, então um
   sysmodule de terceiro não consegue registrar sem forkar a Atmosphère.
   O que funciona no lugar: o sysmodule vê o jogo virar processo (poll de 100 ms
   em `pm:dmnt`) e o congela com `svcDebugActiveProcess` antes de ele ter lido o
   save — anexar como depurador suspende o processo, e fechar o handle o solta.
   Com o jogo parado, baixa da nuvem, escreve no save e só então solta.
   Fecha o ciclo "estilo Steam" — sync automático nos dois sentidos, sem launcher.

   * conversa privada removida do historico
   * conversa privada removida do historico
conversa privada removida do historico
conversa privada removida do historico
conversa privada removida do historico
   - texto "Puxando save da nuvem..." + percentual do download;
   - o jogo só abre quando terminar (é o hook bloqueante que garante isso);
   - fica no mínimo 5 segundos na tela, mesmo se o download for instantâneo;
   - botão **"Não puxar save da nuvem nesse jogo"** → grava o application_id numa
     lista de exclusão, editável depois pelo overlay Ultrahand;
   - botão **OK**.

   Nota de arquitetura importante: **essa tela não tem como sair do NRO**. Um
   `.nro` é app de userland, roda só quando o Miguel abre pelo homebrew menu, e
   não é notificado quando um jogo é lançado. Quem segura o jogo é o sysmodule —
   por isso esse pedido é deste passo do roadmap, e não do app gráfico.
   Como ficou (`sysmodule/source/gfx.c`): camada própria no compositor via
   `vi:m` (o mesmo caminho do overlay do Ultrahand, só que em tela cheia e por
   cima do jogo congelado), fonte do console vinda do `pl:u` rasterizada com
   stb_truetype, framebuffer 640x360 RGBA4444 esticado pra 1280x720 — em
   RGBA8888 na resolução cheia seriam 3,6 MB por buffer, e a heap do sysmodule
   ainda precisa caber o curl e o mbedTLS baixando ao mesmo tempo.
   A % é real: antes de baixar, ele conta quantos arquivos o backup do jogo tem
   no Drive. O botão "Não puxar save da nuvem nesse jogo" cancela o download em
   andamento (seguro: o download vai pro staging no cartão, e o save do jogo só
   seria tocado no fim) e marca o jogo como excluído. O OK só responde depois
   dos 5 s, e a tela sai sozinha em 60 s pra nunca deixar o jogo preso.
5. **v3 — diálogo de conflito:** overlay Ultrahand interativo só quando o mitm detecta
   conflito de verdade (não a cada boot), timeout longo (~15s) com fallback seguro pro
   save local se ninguém responder.

Antes do passo 1, vale meia hora lendo o código de rede/Drive do JKSV
([J-D-K/JKSV](https://github.com/J-D-K/JKSV)) — se ele já cobre 90% do teu caso de uso,
a pergunta real vira "fork do JKSV com automação" em vez de "projeto novo", e isso corta
semanas.

## Status atual do spike

O passo 1 do roadmap (`app/`) já cobre mais do que "um jogo hardcoded": além do
login OAuth Device Flow e upload/download de teste, agora lista **todos** os
jogos com save data (nome de verdade via NACP, não title ID em hex) e faz
backup/restore de save real pra dentro de `Nintendo Switch Saves/<Nome do
Jogo>/` no Drive — a pasta e a estrutura ficam navegáveis pela UI normal do
Drive, sem precisar de ferramenta nenhuma pra ver o que tem lá.

Isso adianta bastante o "v1.1 — metadata e conflito" citado acima em termos
de mecânica de leitura/escrita de save (`titles.c`/`savemount.c`), mas **não**
substitui o passo — ainda falta hash/contador/detecção de conflito de
verdade; hoje backup e restore são manuais e destrutivos (restore sobrescreve
sem comparar versão).

Ponto de risco técnico real, não hipotético: `titles.c` e `savemount.c`
dependem de layout de struct da libnx (`FsSaveDataInfo`, `NacpStruct`) que
não pôde ser conferido contra o SDK de verdade (sem devkitPro instalado
onde o código foi escrito). É o primeiro lugar a olhar se o build falhar ou
se nome de jogo/mount de save se comportar errado — ver aviso em
`app/README.md`.

Fontes: [JKSV no GitHub](https://github.com/J-D-K/JKSV) ·
[JKSV — instruções de remote/Drive](https://github.com/J-D-K/JKSV/blob/master/REMOTE_INSTRUCTIONS.MD) ·
[NH Switch Guide — save management](https://switch.hacks.guide/homebrew/jksv) ·
[OAuth 2.0 for TV and Limited-Input Devices](https://developers.google.com/identity/protocols/oauth2/limited-input-device)

## Status atual (, fim do dia)

Passos 1 e 2 do roadmap estão feitos e no cartão:

| o quê | onde compila | vai pro cartão em |
|---|---|---|
| app gráfico (borealis) | `gui/` | `/switch/SwitchSaveSync.nro` |
| sysmodule de autosync | `sysmodule/` | `/atmosphere/contents/00FF0000535953FF/exefs.nsp` |
| overlay do Ultrahand | `overlay/` | `/switch/.overlays/SwitchSaveSync.ovl` |

Os três compartilham `core/`. O que os liga é `core/syncstate.c`: um punhado de
arquivos-texto no SD (`autosync.cfg`, `excluidos.txt`, `status.txt`,
`pedido.txt`, `autosync.log`) em vez de um serviço IPC próprio — a justificativa
está no comentário do header.

O passo 4 (puxar o save da nuvem com o jogo segurado, + a tela) está escrito e
compilando, por freeze de processo em vez de `fs.mitm` — ver o passo 4 do
roadmap acima. **Nada disso rodou no console ainda**: sysmodule, overlay, o
freeze, a tela e o restore são todos código não testado em hardware.
