#!/bin/bash
# Traz do console tudo que serve pra descobrir o que deu errado.
#
# Toda rodada de teste com o console na mao comecava do mesmo jeito: "olha os
# logs, olha as prints". Isso e um comando so.
#
# O que ele traz:
#
#   /switch/SwitchSaveSync/*.txt e *.log  -> o que o app e o sysmodule escreveram
#   /atmosphere/crash_reports/            -> o processo abortou? qual e onde
#   as prints mais novas do album           -> o que apareceu na tela
#
# O RELATORIO DE CRASH E O MAIS IMPORTANTE DOS TRES. Sysmodule que aborta (por
# servico faltando no NPDM, por diagAbortWithResult dentro da libnx, por acesso
# invalido) some sem deixar recado no log do app — o unico rastro fica ali.
#
# O QUE ELE NAO TRAZ, DE PROPOSITO:
#
#   token.txt  -> e a credencial da conta Google do dono do console. Nao ha
#                 motivo nenhum pra ela sair do cartao, e ja teve um dia em que
#                 ela saiu pra um diretorio temporario e precisou ser apagada
#                 na mao. Nao repetir.
#
# O IP NAO fica escrito aqui: e endereco de rede de casa e este arquivo vai pro
# GitHub. Passa na hora de rodar:
#
#   ./pegar-do-switch.sh 10.0.0.x
#   SWITCH_IP=10.0.0.x ./pegar-do-switch.sh
#
# O FTP e o do DBI (porta 5000 por padrao; POR=... muda). Abra o FTP no DBI
# antes de rodar. Nada aqui escreve nem apaga no cartao: so le.
#
#   --so-log      so os arquivos do app
#   --so-crash    so os relatorios de crash
#   --prints N    quantas prints trazer (padrao 6, 0 desliga)

set -u

IP="${1:-${SWITCH_IP:-}}"
[ "${IP#--}" != "$IP" ] && IP="${SWITCH_IP:-}"   # o primeiro argumento era uma opcao
POR="${POR:-5000}"
RAIZ="$(cd "$(dirname "$0")" && pwd)"

QUE="tudo"
PRINTS=6
espera_prints=0
for a in "$@"; do
    if [ "$espera_prints" = 1 ]; then PRINTS="$a"; espera_prints=0; continue; fi
    [ "$a" = "--so-log" ]   && QUE="log"
    [ "$a" = "--so-crash" ] && QUE="crash"
    [ "$a" = "--prints" ]   && espera_prints=1
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

# Uma pasta por rodada, com a data no nome: comparar a rodada de hoje com a de
# ontem e metade do trabalho de achar o que quebrou.
QUANDO="$(date +%Y-%m-%d_%H%M%S)"
DESTINO="$RAIZ/diagnostico-do-switch/$QUANDO"
mkdir -p "$DESTINO"

BASE="ftp://$IP:$POR"
CURL="curl -s --max-time 30"

# O DBI expoe raizes virtuais: sdmc:, album_sd:, album_nand:, install:, games:.
SD="$BASE/sdmc:"
ALBUM="$BASE/album_sd:"

listar() { $CURL --list-only "$1/" 2>/dev/null; }

baixar() {
    origem="$1"; destino="$2"
    mkdir -p "$(dirname "$destino")"
    if $CURL -o "$destino" "$origem" && [ -s "$destino" ]; then
        printf '  %s\n' "${destino#$DESTINO/}"
    else
        rm -f "$destino"
    fi
}

# --- os arquivos do app ----------------------------------------------------
if [ "$QUE" = tudo ] || [ "$QUE" = log ]; then
    echo "arquivos do app:"
    for nome in $(listar "$SD/switch/SwitchSaveSync"); do
        case "$nome" in
            token.txt) continue ;;             # credencial: fica no cartao
            *.txt|*.log|*.cfg) ;;
            *) continue ;;                     # pastas (staging, backups) ficam
        esac
        baixar "$SD/switch/SwitchSaveSync/$nome" "$DESTINO/app/$nome"
    done
fi

# --- relatorio de crash ----------------------------------------------------
if [ "$QUE" = tudo ] || [ "$QUE" = crash ]; then
    echo "relatorios de crash:"
    achou_crash=0
    for nome in $(listar "$SD/atmosphere/crash_reports"); do
        case "$nome" in
            *.log|*.bin) ;;
            *) continue ;;
        esac
        baixar "$SD/atmosphere/crash_reports/$nome" "$DESTINO/crash/$nome"
        achou_crash=1
    done
    [ "$achou_crash" = 0 ] && echo "  (nenhum — nenhum processo abortou desde a ultima limpeza)"
fi

# --- as prints mais novas --------------------------------------------------
if { [ "$QUE" = tudo ] || [ "$QUE" = log ]; } && [ "$PRINTS" != 0 ]; then
    echo "prints (as $PRINTS mais novas):"
    # O album e /ANO/MES/DIA/arquivo. Desce sempre pelo ultimo de cada nivel.
    ano="$(listar "$ALBUM" | sort | tail -1)"
    mes="$(listar "$ALBUM/$ano" | sort | tail -1)"
    dia="$(listar "$ALBUM/$ano/$mes" | sort | tail -1)"
    if [ -n "$ano" ] && [ -n "$mes" ] && [ -n "$dia" ]; then
        for nome in $(listar "$ALBUM/$ano/$mes/$dia" | sort | tail -"$PRINTS"); do
            baixar "$ALBUM/$ano/$mes/$dia/$nome" "$DESTINO/prints/$nome"
        done
    else
        echo "  (album vazio)"
    fi
fi

echo
echo "tudo em: $DESTINO"
