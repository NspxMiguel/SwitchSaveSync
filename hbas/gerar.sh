#!/usr/bin/env bash
# Refaz icon.png (256x150) e screen.png (848x208) a partir dos SVG.
#
# A loja corta qualquer imagem maior que essas medidas, entao os SVG ja nascem
# no tamanho exato e o rasterizador so' transcreve.
#
# No Mac o sips faz isso sozinho. Em Linux, rsvg-convert.
set -eu
cd "$(dirname "$0")"

for nome in icon screen; do
    if command -v sips >/dev/null 2>&1; then
        sips -s format png "$nome.svg" --out "$nome.png" >/dev/null
    elif command -v rsvg-convert >/dev/null 2>&1; then
        rsvg-convert "$nome.svg" -o "$nome.png"
    else
        echo "preciso do sips (macOS) ou do rsvg-convert" >&2
        exit 1
    fi
    python3 - "$nome.png" <<'FIM'
import struct, sys
d = open(sys.argv[1], 'rb').read()
print('  %s  %dx%d' % ((sys.argv[1],) + struct.unpack('>II', d[16:24])))
FIM
done
