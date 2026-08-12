#!/bin/bash
# Manda o que foi compilado aqui pro console, por FTP.
#
# Sao tres coisas que vao pra tres lugares diferentes, e errar o lugar de uma
# delas e passar meia hora achando que o build quebrou:
#
#   gui/SwitchSaveSync.nro       -> /switch/SwitchSaveSync.nro
#   sysmodule/SwitchSaveSync.nsp -> /atmosphere/contents/00FF0000535953FF/exefs.nsp
#   sysmodule/toolbox.json       -> /atmosphere/contents/00FF0000535953FF/toolbox.json
#   overlay/SwitchSaveSync.ovl   -> /switch/.overlays/SwitchSaveSync.ovl
#
# conversa privada removida do historico
# pro GitHub. Passa na hora de rodar:
#
#   ./enviar-pro-switch.sh 10.0.0.x
#   SWITCH_IP=10.0.0.x ./enviar-pro-switch.sh
#
# O FTP e o do DBI (porta 5000 por padrao; POR=... muda). Abra o FTP no DBI
# antes de rodar. Nada aqui apaga arquivo do cartao: so sobrescreve os quatro
# de cima.
#
#   --so-app     manda so o .nro (quando mexeu so no app)
#   --so-auto    manda so o sysmodule + overlay (quando mexeu so no autosync)

set -u

IP="${1:-${SWITCH_IP:-}}"
[ "${IP#--}" != "$IP" ] && IP="${SWITCH_IP:-}"   # o primeiro argumento era uma opcao
POR="${POR:-5000}"
RAIZ="$(cd "$(dirname "$0")" && pwd)"

QUE="tudo"
for a in "$@"; do
    [ "$a" = "--so-app" ]  && QUE="app"
    [ "$a" = "--so-auto" ] && QUE="auto"
done

if [ -z "$IP" ]; then
    echo "falta o endereco do console:"
    echo "  $0 10.0.0.x"
    echo "  SWITCH_IP=10.0.0.x $0"
    exit 1
fi

# Sem isto, o curl fica pendurado esperando um FTP que nao esta aberto e
# parece que o script travou.
if ! nc -z -G 3 -w 3 "$IP" "$POR" 2>/dev/null; then
    echo "o FTP do console nao respondeu em $IP:$POR."
    echo "abra o DBI e ligue o FTP (MTP/FTP -> FTP), depois roda de novo."
    exit 1
fi

BASE="ftp://$IP:$POR/sdmc:"
falhou=0

# O --ftp-create-dirs cria a pasta de destino quando ela ainda nao existe, que
# e o caso da primeira vez que o sysmodule vai pro console.
manda() {
    local de="$1" para="$2"
    if [ ! -f "$de" ]; then
        echo "  !! nao existe: $de   (compilou?)"
        falhou=1
        return
    fi
    printf '  %-34s -> %s\n' "$(basename "$de")" "$para"
    if ! curl -sS --ftp-create-dirs -T "$de" "$BASE$para"; then
        echo "  !! nao subiu"
        falhou=1
    fi
}

echo "############ mandando pro $IP:$POR ############"

if [ "$QUE" != auto ]; then
    manda "$RAIZ/gui/SwitchSaveSync.nro" "/switch/SwitchSaveSync.nro"
fi

if [ "$QUE" != app ]; then
    TID=/atmosphere/contents/00FF0000535953FF
    manda "$RAIZ/sysmodule/SwitchSaveSync.nsp" "$TID/exefs.nsp"
    manda "$RAIZ/sysmodule/toolbox.json"       "$TID/toolbox.json"
    manda "$RAIZ/overlay/SwitchSaveSync.ovl"   "/switch/.overlays/SwitchSaveSync.ovl"
fi

echo
if [ "$falhou" -eq 0 ]; then
    echo "foi tudo."
    [ "$QUE" != app ] && echo "o sysmodule so troca de verdade depois de REINICIAR o console."
else
    echo "TEVE FALHA — olha acima."
fi
exit $falhou
