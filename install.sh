#!/usr/bin/env bash
# SwitchSaveSync — puts the app on the SD card for you. macOS and Linux.
#
#   curl -fsSL https://raw.githubusercontent.com/NspxMiguel/SwitchSaveSync/main/install.sh | bash
#
# It finds the card, downloads the latest release and copies the files where
# they belong. It NEVER deletes anything and it never formats: the only files
# it writes are the four that belong to this app.
#
#   switch/SwitchSaveSync.nro                              the app
#   switch/.overlays/SwitchSaveSync.ovl                    Ultrahand overlay
#   atmosphere/contents/00FF0000535953FF/exefs.nsp         autosync sysmodule
#   atmosphere/contents/00FF0000535953FF/toolbox.json      goes beside it
#
# Options, for when you don't want to be asked:
#
#   --card /Volumes/SWITCH   use this card, don't look for one
#   --app-only               only the app, skip autosync
#   --zip file.zip           install from a file you already downloaded
#   --version v0.4.0         a specific release instead of the newest
#   --yes                    don't ask anything (needs --card, or one clear card)
#   --no-hbl                 don't touch atmosphere/config/override_config.ini
#   --eject                  unmount the card at the end
#
# Speaks Portuguese when the system does. Read it before running it — this is
# the whole point of shipping the installer as source.

set -u

REPO="NspxMiguel/SwitchSaveSync"
# SSS_API existe pra um so' proposito: a integracao continua aponta ele pro
# vazio e prova que o plano B (sem API) acha a release sozinho.
API="${SSS_API:-https://api.github.com/repos/$REPO/releases}"
PAGINA="https://github.com/$REPO/releases"

# Os quatro arquivos, e onde cada um cai. Errar o lugar de um deles e passar a
# tarde achando que o app esta quebrado.
ARQUIVOS_APP="switch/SwitchSaveSync.nro"
ARQUIVOS_AUTO="switch/.overlays/SwitchSaveSync.ovl
atmosphere/contents/00FF0000535953FF/exefs.nsp
atmosphere/contents/00FF0000535953FF/toolbox.json"

CARTAO=""
ZIP_LOCAL=""
VERSAO=""
SO_APP=0
SEM_PERGUNTA=0
EJETAR=0
MEXE_HBL=1

# O Mac cria ._arquivo em cartao FAT se deixar. O Switch nao gosta.
export COPYFILE_DISABLE=1

# --------------------------------------------------------------- conversa

PT=0
case "${LC_ALL:-}${LANG:-}" in pt*|*pt_BR*|*pt_PT*) PT=1 ;; esac

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    NEGRITO=$(printf '\033[1m'); APAGA=$(printf '\033[0m')
    VERDE=$(printf '\033[32m'); VERMELHO=$(printf '\033[31m')
    AMARELO=$(printf '\033[33m')
else
    NEGRITO=""; APAGA=""; VERDE=""; VERMELHO=""; AMARELO=""
fi

diz() { if [ "$PT" = 1 ]; then printf '%s\n' "$1"; else printf '%s\n' "$2"; fi; }
t()   { if [ "$PT" = 1 ]; then printf '%s'   "$1"; else printf '%s'   "$2"; fi; }

morre() {
    printf '%s%s%s\n' "$VERMELHO" "$(t "  ✗ $1" "  ✗ $2")" "$APAGA" >&2
    exit 1
}

# Perguntar funciona mesmo com o script vindo de um cano (curl | bash): nesse
# caso a entrada padrao e o cano, e quem responde e o terminal.
TEM_TECLADO=0
[ -r /dev/tty ] && TEM_TECLADO=1

pergunta() {
    [ "$TEM_TECLADO" = 1 ] || return 1
    printf '%s' "$1"
    IFS= read -r RESPOSTA < /dev/tty || return 1
    return 0
}

# ------------------------------------------------------------- argumentos

while [ $# -gt 0 ]; do
    case "$1" in
        --card|--cartao) CARTAO="${2:-}"; shift 2 || morre "--card precisa do caminho" "--card needs a path" ;;
        --zip)           ZIP_LOCAL="${2:-}"; shift 2 || morre "--zip precisa do arquivo" "--zip needs a file" ;;
        --version|--versao) VERSAO="${2:-}"; shift 2 || morre "--version precisa da tag" "--version needs a tag" ;;
        --app-only|--so-app) SO_APP=1; shift ;;
        --yes|--sim)     SEM_PERGUNTA=1; shift ;;
        --no-hbl|--sem-hbl) MEXE_HBL=0; shift ;;
        --eject|--ejetar) EJETAR=1; shift ;;
        -h|--help|--ajuda)
            sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) morre "opcao que eu nao conheco: $1" "option I don't know: $1" ;;
    esac
done

# ------------------------------------------------------- achar o cartao

sistema=$(uname -s)

sistema_de_arquivos() {
    case "$sistema" in
        Darwin)
            linha=$(mount | grep -F " on $1 (" | head -1)
            printf '%s' "$linha" | sed -n 's/.* (\([^,)]*\).*/\1/p'
            ;;
        *)
            if command -v findmnt >/dev/null 2>&1; then
                findmnt -no FSTYPE -- "$1" 2>/dev/null
            else
                stat -f -c %T -- "$1" 2>/dev/null
            fi
            ;;
    esac
}

# Quanto mais disto o volume tem, mais ele parece o cartao de um Switch com
# CFW. Nenhum item sozinho decide: um cartao recem-formatado so tem Nintendo/.
parece_switch() {
    ponto=0
    [ -d "$1/atmosphere" ]  && ponto=$((ponto + 3))
    [ -d "$1/bootloader" ]  && ponto=$((ponto + 2))
    [ -d "$1/switch" ]      && ponto=$((ponto + 2))
    [ -d "$1/Nintendo" ]    && ponto=$((ponto + 1))
    [ -f "$1/payload.bin" ] && ponto=$((ponto + 1))
    [ -f "$1/boot.dat" ]    && ponto=$((ponto + 1))
    printf '%s' "$ponto"
}

# Um cartao de Switch e' FAT32 ou exFAT, sempre. Volume de outro tipo nao entra
# na lista: e o jeito de o disco do sistema nunca aparecer como opcao.
volume_serve() {
    case "$(sistema_de_arquivos "$1")" in
        msdos|exfat|vfat|fat|fat32|fuseblk) return 0 ;;
        *) return 1 ;;
    esac
}

lista_candidatos() {
    case "$sistema" in
        Darwin) locais="/Volumes/*" ;;
        *)      locais="/media/$USER/* /run/media/$USER/* /media/* /mnt/*" ;;
    esac
    # shellcheck disable=SC2086
    for v in $locais; do
        [ -L "$v" ] && continue   # /Volumes/Macintosh HD e' link pra / — nunca e cartao
        [ -d "$v" ] || continue
        [ -w "$v" ] || continue
        volume_serve "$v" || continue
        printf '%s\t%s\n' "$(parece_switch "$v")" "$v"
    done | sort -rn
}

escolhe_cartao() {
    candidatos=$(lista_candidatos)
    if [ -z "$candidatos" ]; then
        diz "Nao achei nenhum cartao FAT32/exFAT montado." \
            "No FAT32/exFAT card is mounted."
        diz "Enfia o cartao do Switch no computador e roda de novo — ou passa o caminho com --card." \
            "Put the Switch card in the computer and run again — or pass the path with --card."
        exit 1
    fi

    quantos=$(printf '%s\n' "$candidatos" | wc -l | tr -d ' ')
    melhor_nota=$(printf '%s\n' "$candidatos" | head -1 | cut -f1)
    melhor=$(printf '%s\n' "$candidatos" | head -1 | cut -f2-)

    # Um candidato so, com cara de Switch, e ninguem pra perguntar: segue.
    if [ "$quantos" = 1 ] && [ "$melhor_nota" -ge 3 ] && { [ "$SEM_PERGUNTA" = 1 ] || [ "$TEM_TECLADO" = 0 ]; }; then
        CARTAO="$melhor"
        return 0
    fi

    if [ "$TEM_TECLADO" = 0 ]; then
        morre "tem mais de um cartao possivel e ninguem pra escolher: use --card" \
              "more than one possible card and nobody to choose: use --card"
    fi

    printf '\n%s\n' "$(t "Cartoes que eu achei:" "Cards I found:")"
    i=0
    printf '%s\n' "$candidatos" | while IFS="$(printf '\t')" read -r nota caminho; do
        i=$((i + 1))
        if [ "$nota" -ge 3 ]; then
            marca="$VERDE$(t "parece um Switch com CFW" "looks like a Switch on CFW")$APAGA"
        elif [ "$nota" -ge 1 ]; then
            marca="$AMARELO$(t "talvez" "maybe")$APAGA"
        else
            marca="$(t "vazio, nao da pra saber" "empty, no way to tell")"
        fi
        printf '  %d) %s%s%s  — %s\n' "$i" "$NEGRITO" "$caminho" "$APAGA" "$marca"
    done

    total=$quantos
    while :; do
        pergunta "$(t "
Qual? [1-$total, ou Enter pra cancelar] " "
Which one? [1-$total, or Enter to cancel] ")" || morre "sem resposta" "no answer"
        [ -z "$RESPOSTA" ] && { diz "Cancelado, nada foi escrito." "Cancelled, nothing was written."; exit 0; }
        case "$RESPOSTA" in
            ''|*[!0-9]*) continue ;;
        esac
        [ "$RESPOSTA" -ge 1 ] 2>/dev/null && [ "$RESPOSTA" -le "$total" ] 2>/dev/null || continue
        CARTAO=$(printf '%s\n' "$candidatos" | sed -n "${RESPOSTA}p" | cut -f2-)
        break
    done
}

# ------------------------------------------- o R que faz o app abrir direito

# Segurar R em cima de um jogo so' entra no homebrew se o Atmosphere estiver
# com override_any_app ligado, e ele NAO liga sozinho: o pacote traz o arquivo
# em config_templates/ e nunca escreve por cima do que esta em config/. Sem
# isso, a pessoa segura R, o jogo abre normal, e ela conclui que o app nao
# presta. E o tropeco numero um, e ele nao esta escrito em lugar nenhum.
#
# Regra aqui: se o arquivo NAO existe, eu crio. Se existe, eu nao reescrevo a
# configuracao de ninguem — no maximo pergunto se posso acrescentar, guardando
# uma copia antes.
BLOCO_HBL='[hbl_config]
program_id_1=010000000000100d
override_key_0=R

; Segurar R ao abrir um jogo instalado entra no homebrew em MODO APLICACAO,
; que e o que da memoria e rede de jogo de verdade. Abrir o jogo sem segurar
; nada continua abrindo o jogo.
override_any_app=true
override_any_app_key=R
override_any_app_address_space=39_bit
path=atmosphere/hbl.nsp'

cuida_do_hbl() {
    [ "$MEXE_HBL" = 1 ] || return 0
    ini="$CARTAO/atmosphere/config/override_config.ini"

    if [ ! -f "$ini" ]; then
        mkdir -p "$(dirname "$ini")" || return 0
        printf '%s\n' "$BLOCO_HBL" > "$ini" || return 0
        printf '  %s %s\n' "${VERDE}✓${APAGA}" \
            "$(t "atmosphere/config/override_config.ini criado (e o que faz o R funcionar)" \
                 "atmosphere/config/override_config.ini created (this is what makes R work)")"
        return 0
    fi

    if grep -qi '^[[:space:]]*override_any_app[[:space:]]*=[[:space:]]*true' "$ini"; then
        return 0
    fi

    printf '\n%s\n' "${AMARELO}$(t \
        "O teu override_config.ini nao tem override_any_app=true — sem isso, segurar R nao faz nada." \
        "Your override_config.ini has no override_any_app=true — without it, holding R does nothing.")${APAGA}"

    if [ "$SEM_PERGUNTA" = 1 ] || [ "$TEM_TECLADO" = 0 ]; then
        diz "Nao mexo num arquivo de configuracao que ja existe. Acrescenta a mao, no fim de $ini:" \
            "I don't rewrite a config file that already exists. Add this yourself, at the end of $ini:"
        printf '\n    override_any_app=true\n    override_any_app_key=R\n    override_any_app_address_space=39_bit\n    path=atmosphere/hbl.nsp\n\n'
        return 0
    fi

    pergunta "$(t "Posso acrescentar essas linhas no fim dele? Guardo uma copia antes. [s/N] " \
                  "May I append those lines to it? I keep a copy first. [y/N] ")" || return 0
    case "$RESPOSTA" in
        s|S|y|Y|sim|SIM|yes|YES) : ;;
        *) diz "Ok, nao mexi nele." "Fine, I left it alone."; return 0 ;;
    esac

    cp "$ini" "$ini.antes-do-SwitchSaveSync" 2>/dev/null
    {
        printf '\n; --- SwitchSaveSync ---\n'
        printf 'override_any_app=true\noverride_any_app_key=R\noverride_any_app_address_space=39_bit\npath=atmosphere/hbl.nsp\n'
    } >> "$ini" && printf '  %s %s\n' "${VERDE}✓${APAGA}" \
        "$(t "acrescentado (copia do antigo em override_config.ini.antes-do-SwitchSaveSync)" \
             "appended (the old one is at override_config.ini.antes-do-SwitchSaveSync)")"
}

# ------------------------------------------------------------- baixar

acha_url_do_zip() {
    if [ -n "$VERSAO" ]; then
        alvo="$API/tags/$VERSAO"
    else
        alvo="$API/latest"
    fi

    url=$(curl -fsSL -H 'Accept: application/vnd.github+json' "$alvo" 2>/dev/null \
        | tr ',' '\n' | grep -o '"browser_download_url": *"[^"]*-sd\.zip"' \
        | head -1 | sed 's/.*"\(https[^"]*\)"/\1/')

    # A API do GitHub da 60 chamadas por hora por endereco de IP, e num
    # provedor grande esse limite ja chega gasto. O plano B nao usa a API.
    #
    # A pagina da release NAO tem o link do anexo: ela carrega essa parte
    # depois, de /releases/expanded_assets/<tag>. Entao sao dois passos —
    # descobrir a tag no HTML da pagina, e ler o pedaco que tem os anexos.
    if [ -z "$url" ]; then
        if [ -n "$VERSAO" ]; then
            pedaco="releases/expanded_assets/$VERSAO"
        else
            pedaco=$(curl -fsSL "$PAGINA/latest" 2>/dev/null \
                | grep -o 'releases/expanded_assets/[A-Za-z0-9._-]*' | head -1)
        fi
        if [ -n "$pedaco" ]; then
            url=$(curl -fsSL "https://github.com/$REPO/$pedaco" 2>/dev/null \
                | grep -o '/'"$REPO"'/releases/download/[^"]*-sd\.zip' | head -1)
            [ -n "$url" ] && url="https://github.com$url"
        fi
    fi

    printf '%s' "$url"
}

# ------------------------------------------------------------- instalar

tamanho_de() {
    if [ "$sistema" = Darwin ]; then stat -f %z "$1" 2>/dev/null; else stat -c %s "$1" 2>/dev/null; fi
}

humano() {
    n="${1:-0}"
    if [ "$n" -ge 1048576 ]; then printf '%s.%s MB' "$((n / 1048576))" "$(((n % 1048576) * 10 / 1048576))"
    elif [ "$n" -ge 1024 ]; then printf '%s KB' "$((n / 1024))"
    else printf '%s B' "$n"; fi
}

principal() {
    printf '%s%s%s\n' "$NEGRITO" "SwitchSaveSync" "$APAGA"
    diz "Instalador de cartao — nao apaga nada do que ja esta la." \
        "SD card installer — it deletes nothing that is already there."

    command -v curl >/dev/null 2>&1 || morre "preciso do curl" "I need curl"

    [ -n "$CARTAO" ] || escolhe_cartao
    [ -d "$CARTAO" ] || morre "isso nao e uma pasta: $CARTAO" "that is not a folder: $CARTAO"
    [ -w "$CARTAO" ] || morre "nao consigo escrever em $CARTAO" "I can't write to $CARTAO"

    # Cartao que nao tem cara de Switch nenhum: pergunta antes, sempre.
    if [ "$(parece_switch "$CARTAO")" -lt 3 ] && [ "$SEM_PERGUNTA" = 0 ] && [ "$TEM_TECLADO" = 1 ]; then
        printf '%s\n' "$AMARELO$(t \
            "Esse volume nao tem atmosphere/ nem switch/ — pode nao ser o cartao do Switch." \
            "That volume has no atmosphere/ or switch/ — it may not be the Switch card.")$APAGA"
        pergunta "$(t "Escrever em $CARTAO mesmo assim? [s/N] " "Write to $CARTAO anyway? [y/N] ")" \
            || morre "sem resposta" "no answer"
        case "$RESPOSTA" in
            s|S|y|Y|sim|SIM|yes|YES) : ;;
            *) diz "Cancelado, nada foi escrito." "Cancelled, nothing was written."; exit 0 ;;
        esac
    fi

    temporaria=$(mktemp -d 2>/dev/null || mktemp -d -t sss)
    [ -d "$temporaria" ] || morre "nao consegui criar pasta temporaria" "couldn't create a temp folder"
    trap 'rm -rf "$temporaria"' EXIT INT TERM

    zip="$temporaria/sd.zip"
    if [ -n "$ZIP_LOCAL" ]; then
        [ -f "$ZIP_LOCAL" ] || morre "nao achei o arquivo $ZIP_LOCAL" "no such file: $ZIP_LOCAL"
        cp "$ZIP_LOCAL" "$zip" || morre "nao consegui ler $ZIP_LOCAL" "couldn't read $ZIP_LOCAL"
        printf '  %s %s\n' "$(t "arquivo:" "file:")" "$ZIP_LOCAL"
    else
        if [ -n "$VERSAO" ]; then
            printf '  %s' "$(t "procurando a versao $VERSAO... " "looking for release $VERSAO... ")"
        else
            printf '  %s' "$(t "procurando a versao mais nova... " "looking for the newest release... ")"
        fi
        url=$(acha_url_do_zip)
        if [ -z "$url" ] && [ -n "$VERSAO" ]; then
            morre "a versao $VERSAO nao tem o zip do cartao (ele existe da v0.4.0 em diante)" \
                  "release $VERSAO has no SD-card zip (those start at v0.4.0)"
        fi
        [ -n "$url" ] || morre \
            "nao achei o zip da release. Sem internet? Baixa a mao em $PAGINA/latest e roda com --zip" \
            "couldn't find the release zip. No internet? Download it from $PAGINA/latest and use --zip"
        printf '%s\n' "$(basename "$url")"
        printf '  %s' "$(t "baixando... " "downloading... ")"
        if [ -t 2 ]; then barra="--progress-bar"; else barra="-sS"; fi
        curl -fL $barra -o "$zip" "$url" || morre "o download falhou" "the download failed"
        printf '%s\n' "$(humano "$(tamanho_de "$zip")")"
    fi

    # Descompactar num canto isolado, conferir, e so entao encostar no cartao.
    saco="$temporaria/aberto"
    mkdir -p "$saco"
    if command -v unzip >/dev/null 2>&1; then
        unzip -q -o "$zip" -d "$saco" || morre "o zip veio quebrado" "the zip came broken"
    elif command -v bsdtar >/dev/null 2>&1; then
        bsdtar -xf "$zip" -C "$saco" || morre "o zip veio quebrado" "the zip came broken"
    elif tar -xf "$zip" -C "$saco" 2>/dev/null; then
        :
    elif command -v python3 >/dev/null 2>&1; then
        python3 - "$zip" "$saco" <<'FIM' || morre "o zip veio quebrado" "the zip came broken"
import sys, zipfile, os
zf, destino = sys.argv[1], os.path.realpath(sys.argv[2])
with zipfile.ZipFile(zf) as z:
    for nome in z.namelist():
        # zip que tenta escrever fora da pasta de destino nao passa daqui
        alvo = os.path.realpath(os.path.join(destino, nome))
        if not (alvo == destino or alvo.startswith(destino + os.sep)):
            raise SystemExit(f'caminho suspeito no zip: {nome}')
    z.extractall(destino)
FIM
    else
        morre "preciso do unzip (ou tar, ou python3)" "I need unzip (or tar, or python3)"
    fi

    # Um zip pode trazer link simbolico, inclusive apontando pra fora do cartao,
    # e o cp seguiria o link e copiaria o alvo. O nosso zip nao tem nenhum;
    # apagar antes de olhar custa nada e fecha a porta.
    find "$saco" -type l -exec rm -f {} + 2>/dev/null

    [ -f "$saco/$ARQUIVOS_APP" ] || morre \
        "o zip nao tem o app dentro — versao errada?" \
        "that zip has no app inside — wrong release?"

    if [ "$SO_APP" = 1 ]; then
        lista="$ARQUIVOS_APP"
    else
        lista="$ARQUIVOS_APP
$ARQUIVOS_AUTO"
    fi

    precisa=0
    for rel in $lista; do
        [ -f "$saco/$rel" ] || continue
        precisa=$((precisa + $(tamanho_de "$saco/$rel")))
    done
    livre_kb=$(df -Pk "$CARTAO" 2>/dev/null | awk 'NR==2 {print $4}')
    if [ -n "${livre_kb:-}" ] && [ "$((livre_kb * 1024))" -lt "$precisa" ]; then
        morre "o cartao nao tem espaco: precisa de $(humano $precisa)" \
              "the card is full: it needs $(humano $precisa)"
    fi

    printf '\n%s\n' "$(t "Copiando pra $CARTAO:" "Copying to $CARTAO:")"
    for rel in $lista; do
        origem="$saco/$rel"
        [ -f "$origem" ] || { printf '  %s %s\n' "${AMARELO}-${APAGA}" "$rel $(t "(nao veio no zip)" "(not in the zip)")"; continue; }
        destino="$CARTAO/$rel"
        mkdir -p "$(dirname "$destino")" || morre "nao consegui criar $(dirname "$rel")" "couldn't create $(dirname "$rel")"
        antes=""
        [ -f "$destino" ] && antes=" $(t "(estava la, atualizei)" "(was there, updated)")"
        cp "$origem" "$destino" || morre "nao consegui escrever $rel" "couldn't write $rel"
        printf '  %s %s  %s%s\n' "${VERDE}✓${APAGA}" "$rel" "$(humano "$(tamanho_de "$destino")")" "$antes"
    done

    # Conferir byte a byte o tamanho: cartao cheio ou mal encaixado mente na
    # hora da copia e so aparece quando o console recusa o arquivo.
    for rel in $lista; do
        [ -f "$saco/$rel" ] || continue
        [ "$(tamanho_de "$saco/$rel")" = "$(tamanho_de "$CARTAO/$rel")" ] || morre \
            "$rel saiu do tamanho errado — tira o cartao e poe de novo, depois roda outra vez" \
            "$rel came out the wrong size — reseat the card and run this again"
    done

    cuida_do_hbl

    # Restos que o Mac deixa em cartao FAT e o console as vezes reclama.
    command -v dot_clean >/dev/null 2>&1 && dot_clean -m "$CARTAO" 2>/dev/null
    sync 2>/dev/null

    printf '\n%s%s%s\n' "$VERDE$NEGRITO" "$(t "Pronto." "Done.")" "$APAGA"
    if [ "$SO_APP" = 1 ]; then
        diz "So o app foi instalado (--app-only)." "Only the app was installed (--app-only)."
    fi
    diz "Agora, no console:" "Now, on the console:"
    diz "  1. Poe o cartao de volta e liga o console no CFW." \
        "  1. Put the card back and boot the console into CFW."
    diz "  2. Abre o menu de homebrew SEGURANDO R em cima de um jogo instalado — nao pelo Album." \
        "  2. Open the homebrew menu HOLDING R on an installed game — not from the Album."
    diz "  3. Abre o SwitchSaveSync e entra na tua conta do Google na primeira tela." \
        "  3. Open SwitchSaveSync and sign in to your Google account on the first screen."
    if [ "$SO_APP" = 0 ]; then
        diz "  4. O autosync nao liga sozinho, de proposito: liga ele no overlay do Ultrahand quando quiser." \
            "  4. Autosync does not start on its own, on purpose: turn it on from the Ultrahand overlay."
    fi

    # Ejetar importa: FAT nao tem journal e a escrita fica em cache. Se a
    # ejecao falhar (algum programa com o cartao aberto), dizer "pode tirar"
    # seria mentira que corrompe cartao.
    if [ "$EJETAR" = 1 ]; then
        if [ "$sistema" = Darwin ]; then
            diskutil eject "$CARTAO" >/dev/null 2>&1
        elif command -v udisksctl >/dev/null 2>&1; then
            udisksctl unmount -b "$(df -P "$CARTAO" | awk 'NR==2{print $1}')" >/dev/null 2>&1
        else
            umount "$CARTAO" >/dev/null 2>&1
        fi
        if [ $? -eq 0 ]; then
            diz "Cartao ejetado, pode tirar." "Card ejected, you can pull it."
        else
            printf '%s\n' "${AMARELO}$(t \
                "Nao consegui ejetar (algum programa esta com o cartao aberto). Ejeta pelo sistema ANTES de tirar." \
                "Couldn't eject (something still has the card open). Eject it from the system BEFORE pulling it.")${APAGA}"
        fi
    fi
}

principal "$@"
