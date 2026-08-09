#!/bin/sh
# Roda os testes do core no Mac, sem console nenhum.
#
# A ideia: o que é lógica pura (formato do arquivo, leitura de JSON, nome de
# pasta, espelhamento da nuvem) não precisa do Switch pra ser testado, e
# esperar o console pra descobrir que o buffer estourou é caro demais. Aqui
# roda com ASan e UBSan ligados, que pegam estouro de buffer e leitura de
# lixo — o tipo de erro que no console vira crash sem explicação.
#
# O que NÃO está aqui, de propósito: qualquer coisa que precise da conta do
# Google de verdade, montar save de verdade, ou a tela. Isso só o console
# testa.
#
#   ./tests/run.sh          roda tudo
#   ./tests/run.sh nomes    roda só um (sssbox | webdav | nomes)

set -e
cd "$(dirname "$0")"

CORE=../core
SAN="-fsanitize=address,undefined"
CFLAGS="-O1 -g $SAN -Wall -Wextra -Wno-unused-parameter -Wno-format -Istubs -I$CORE"
# O -Wno-format é por causa do stub: no Switch u64 é "unsigned long", no Mac é
# "unsigned long long", então todo %016lX do core reclama aqui e está certo lá.

QUAL="${1:-tudo}"
falhou=0

roda() {
    nome=$1; shift
    printf '\n########## %s ##########\n' "$nome"
    rm -rf "build/$nome"
    mkdir -p "build/$nome"
    clang $CFLAGS "$@" -lcurl -lz -o "build/$nome/teste"
    (cd "build/$nome" && ./teste) || falhou=1
}

# ---- 1. o formato .ssaves ----
if [ "$QUAL" = tudo ] || [ "$QUAL" = sssbox ]; then
    roda sssbox test_sssbox.c $CORE/sssbox.c
fi

# ---- 3. JSON, nome de pasta e saneamento ----
# (fora de ordem de propósito: o webdav precisa do servidor no ar, então fica
# por último, e assim os dois que não precisam de nada rodam mesmo sem Apache.)
if [ "$QUAL" = tudo ] || [ "$QUAL" = nomes ]; then
    roda nomes test_nomes.c stubs_console.c \
        $CORE/minijson.c $CORE/syncstate.c $CORE/syncjob.c \
        $CORE/sssbox.c $CORE/cloud.c $CORE/webdav.c $CORE/http.c
fi

# ---- 2. WebDAV, contra um servidor de verdade ----
if [ "$QUAL" = tudo ] || [ "$QUAL" = webdav ]; then
    # O Apache do sistema NÃO consegue ler config dentro de ~/Documents — o SIP
    # do macOS barra, com "Operation not permitted". Por isso o servidor de
    # teste mora numa pasta temporária, fora do repo. O binário do teste
    # continua em tests/build; só o Apache é que sai daqui.
    DAV="${TMPDIR:-/tmp}/switchsavesync-dav-teste"
    SENHA="senha de teste 123"

    # Sobe um Apache com mod_dav só pra este teste. É o Apache que já vem no
    # macOS; não instala nada. Morre no fim, inclusive se o teste falhar.
    rm -rf "$DAV"
    mkdir -p "$DAV/root" "$DAV/logs" "$DAV/conf"
    htpasswd -bc "$DAV/conf/users" miguel "$SENHA" >/dev/null 2>&1

    cat > "$DAV/conf/httpd.conf" <<EOF
ServerName localhost
Listen 127.0.0.1:8088
ServerRoot "$DAV"

LoadModule mpm_prefork_module /usr/libexec/apache2/mod_mpm_prefork.so
LoadModule authn_file_module  /usr/libexec/apache2/mod_authn_file.so
LoadModule authn_core_module  /usr/libexec/apache2/mod_authn_core.so
LoadModule auth_basic_module  /usr/libexec/apache2/mod_auth_basic.so
LoadModule authz_core_module  /usr/libexec/apache2/mod_authz_core.so
LoadModule authz_user_module  /usr/libexec/apache2/mod_authz_user.so
LoadModule authz_host_module  /usr/libexec/apache2/mod_authz_host.so
LoadModule unixd_module       /usr/libexec/apache2/mod_unixd.so
LoadModule log_config_module  /usr/libexec/apache2/mod_log_config.so
LoadModule mime_module        /usr/libexec/apache2/mod_mime.so
LoadModule alias_module       /usr/libexec/apache2/mod_alias.so
LoadModule dav_module         /usr/libexec/apache2/mod_dav.so
LoadModule dav_fs_module      /usr/libexec/apache2/mod_dav_fs.so
LoadModule dir_module         /usr/libexec/apache2/mod_dir.so

PidFile   "logs/httpd.pid"
ErrorLog  "logs/error.log"
CustomLog "logs/access.log" common
DavLockDB "logs/DavLock"

DocumentRoot "$DAV/root"

<Directory "$DAV/root">
    Dav On
    Options Indexes
    AllowOverride None

    AuthType Basic
    AuthName "SwitchSaveSync"
    AuthUserFile "$DAV/conf/users"
    Require valid-user
</Directory>
EOF

    /usr/sbin/httpd -f "$DAV/conf/httpd.conf" -k start
    trap '/usr/sbin/httpd -f "$DAV/conf/httpd.conf" -k stop 2>/dev/null || true' EXIT INT TERM
    sleep 1

    roda webdav test_webdav.c $CORE/webdav.c $CORE/http.c $CORE/cloud.c

    /usr/sbin/httpd -f "$DAV/conf/httpd.conf" -k stop 2>/dev/null || true
    trap - EXIT INT TERM
    sleep 1   # o -k stop volta na hora, mas o processo leva um instante pra sair
fi

printf '\n'
if [ "$falhou" -eq 0 ]; then
    echo "tudo passou"
else
    echo "TEVE FALHA — olha acima"
fi
exit $falhou
