#!/usr/bin/env python3
"""Icone do SwitchSaveSync, desenhado por codigo.

A maquina nao tem PIL nem ImageMagick, entao isso aqui rasteriza na mao:
cada forma e uma funcao de distancia com sinal (SDF), a cobertura vem de um
smoothstep na borda (antialiasing de verdade, nao serrilhado) e ainda por
cima roda com supersampling 3x. Escreve um PNG cru (zlib + struct); o sips
converte pro JPEG 256x256 que o nacptool embute no .nro.

Composicao: anel de sync partido em dois — metade azul e metade vermelha,
as cores dos Joy-Con — girando em volta de uma nuvem branca com sombra,
sobre um fundo escuro com vinheta.

    python3 make_icon.py icon.png && sips -s format jpeg icon.png --out icon.jpg
"""
import math, struct, zlib, sys

S, SS = 256, 3
W = S * SS
PX = 1.0 / W                      # um pixel em coordenada normalizada

JOY_BLUE   = (0x00, 0xC3, 0xE3)   # azul do Joy-Con esquerdo
JOY_RED    = (0xFF, 0x3C, 0x28)   # vermelho do Joy-Con direito
BG_TOP     = (0x2A, 0x33, 0x3F)
BG_BOTTOM  = (0x0B, 0x0F, 0x14)
CLOUD      = (0xFF, 0xFF, 0xFF)
CLOUD_DARK = (0xD6, 0xDF, 0xE8)

CX = CY = 0.5
RING_R, RING_HALF = 0.355, 0.049
GAP = 26.0                        # graus de vao de cada lado do anel


def mix(c1, c2, t):
    t = max(0.0, min(1.0, t))
    return tuple(c1[i] + (c2[i] - c1[i]) * t for i in range(3))


def cov(d, soft=1.6):
    """Cobertura [0,1] a partir da distancia com sinal (dentro = negativo)."""
    e = soft * PX
    if d <= -e: return 1.0
    if d >= e:  return 0.0
    t = (e - d) / (2 * e)
    return t * t * (3 - 2 * t)    # smoothstep


def over(dst, src, a):
    if a <= 0: return dst
    return tuple(dst[i] + (src[i] - dst[i]) * a for i in range(3))


def sd_circle(px, py, cx, cy, r):
    return math.hypot(px - cx, py - cy) - r


def sd_round_rect(px, py, cx, cy, hw, hh, r):
    dx, dy = abs(px - cx) - (hw - r), abs(py - cy) - (hh - r)
    return math.hypot(max(dx, 0.0), max(dy, 0.0)) + min(max(dx, dy), 0.0) - r


def cloud_sd(px, py, grow=0.0):
    d = sd_circle(px, py, 0.395, 0.470, 0.108 + grow)
    d = min(d, sd_circle(px, py, 0.500, 0.428, 0.140 + grow))
    d = min(d, sd_circle(px, py, 0.612, 0.478, 0.100 + grow))
    d = min(d, sd_round_rect(px, py, 0.502, 0.545, 0.163 + grow, 0.058 + grow, 0.055))
    return d


def arrow_head(px, py, ang_deg, size):
    """Triangulo na ponta do anel, apontando no sentido do giro."""
    a = math.radians(ang_deg)
    tipx = CX + RING_R * math.cos(a)
    tipy = CY + RING_R * math.sin(a)
    rot = a + math.pi / 2          # tangente = sentido do movimento
    dx, dy = px - tipx, py - tipy
    c, s = math.cos(-rot), math.sin(-rot)
    lx, ly = dx * c - dy * s, dx * s + dy * c
    if lx > 0.0 or lx < -size:
        return 1.0
    return abs(ly) - (size + lx) * 0.78


def ring_sd(px, py, a_start, a_end):
    """Arco grosso entre dois angulos (graus, sentido anti-horario)."""
    d = abs(math.hypot(px - CX, py - CY) - RING_R) - RING_HALF
    a = math.degrees(math.atan2(py - CY, px - CX)) % 360
    span = (a - a_start) % 360
    if span > (a_end - a_start) % 360:
        return 1.0
    return d


def shade(px, py):
    # fundo: degrade vertical + vinheta radial, pra nao ficar chapado
    col = mix(BG_TOP, BG_BOTTOM, py * 0.95)
    vig = math.hypot(px - 0.5, py - 0.45) / 0.72
    col = mix(col, BG_BOTTOM, max(0.0, vig - 0.35) * 1.25)

    # brilho suave no alto, como se viesse luz de cima
    glow = max(0.0, 1.0 - math.hypot(px - 0.5, py - 0.18) / 0.62)
    col = over(col, (0x4A, 0x57, 0x68), glow * 0.30)

    # sombra do anel, deslocada pra baixo
    for a0, a1 in ((90 + GAP, 270 - GAP), (270 + GAP, 90 - GAP)):
        sh = ring_sd(px, py - 0.016, a0, a1)
        col = over(col, (0, 0, 0), cov(sh, 6.0) * 0.38)

    # metade esquerda azul, metade direita vermelha, com as pontas de seta
    left = min(ring_sd(px, py, 90 + GAP, 270 - GAP), arrow_head(px, py, 270 - GAP, 0.105))
    right = min(ring_sd(px, py, 270 + GAP, 90 - GAP), arrow_head(px, py, 90 - GAP, 0.105))
    col = over(col, JOY_BLUE, cov(left))
    col = over(col, JOY_RED, cov(right))

    # sombra da nuvem
    col = over(col, (0, 0, 0), cov(cloud_sd(px, py - 0.020, 0.012), 7.0) * 0.45)

    # nuvem, com um degrade leve de cima pra baixo pra dar volume
    c = cov(cloud_sd(px, py))
    if c > 0:
        body = mix(CLOUD, CLOUD_DARK, max(0.0, (py - 0.40) / 0.24))
        col = over(col, body, c)

    return col


acc = [[0.0, 0.0, 0.0] for _ in range(S * S)]
for yy in range(W):
    py = (yy + 0.5) / W
    row = yy // SS
    for xx in range(W):
        r, g, b = shade((xx + 0.5) / W, py)
        i = row * S + (xx // SS)
        acc[i][0] += r; acc[i][1] += g; acc[i][2] += b
    if yy % 96 == 0:
        print(f"  linha {yy}/{W}", file=sys.stderr)

n = SS * SS
raw = bytearray()
for y in range(S):
    raw.append(0)                                    # filtro None
    for x in range(S):
        p = acc[y * S + x]
        raw += bytes(int(max(0, min(255, round(c / n)))) for c in p)


def chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", S, S, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
       + chunk(b"IEND", b""))

open(sys.argv[1], "wb").write(png)
print("ok:", sys.argv[1], len(png), "bytes", file=sys.stderr)
