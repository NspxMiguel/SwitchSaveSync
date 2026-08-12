#!/usr/bin/env python3
"""Todo servico que o sysmodule abre esta liberado no NPDM dele?

Esta e a familia de defeito que mais custou tempo neste projeto, e ela tem
sempre a mesma cara: o app funciona, o sysmodule nao, e o erro que aparece na
tela fala de outra coisa. Tres vezes ate agora:

  * `set` faltando  -> setGetSystemLanguage falhava -> o sysmodule escrevia
    tudo em ingles com o resto do app em portugues;
  * `acc:u1` pedido em vez de `acc:su` (os nomes da libnx enganam:
    AccountServiceType_System abre acc:u1) -> nenhum apelido de conta era
    lido -> no Drive a pasta do dono virava "conta-10012EE7DE4B4164";
  * `pdm:qry` faltando -> a lista nunca saia ordenada por ultimo jogado.

Nenhuma das tres da erro na compilacao, nenhuma aparece em teste de logica, e
todas falham CALADAS em tempo de execucao: o Initialize devolve erro, o codigo
segue com o caminho degradado, e quem descobre e o usuario.

O que este script faz e burro de proposito: le as chamadas *Initialize() do
codigo que entra no sysmodule, traduz cada uma pro nome de servico que a libnx
pede ao sm, e confere contra o service_access do NPDM.

    python3 tests/servicos.py
"""

import json
import os
import re
import sys

RAIZ = os.path.dirname(os.path.abspath(__file__))
PROJETO = os.path.dirname(RAIZ)

NPDM = os.path.join(PROJETO, 'sysmodule', 'SwitchSaveSync.json')

# O sysmodule compila source/ + core/ inteiros (ver o SOURCES do Makefile dele),
# entao servico aberto dentro do core conta como servico aberto pelo sysmodule.
FONTES = [
    os.path.join(PROJETO, 'sysmodule', 'source'),
    os.path.join(PROJETO, 'core'),
]

# funcao da libnx -> servico que ela pede ao sm.
#
# Quando o servico depende do argumento, o valor é um dicionário do argumento
# para o serviço. É onde mora a pegadinha do account: os nomes do enum não são
# os nomes dos serviços.
TABELA = {
    'fsInitialize':      'fsp-srv',
    'nsInitialize':      'ns:am2',
    'hidInitialize':     'hid',
    'pmdmntInitialize':  'pm:dmnt',
    'pminfoInitialize':  'pm:info',
    'pdmqryInitialize':  'pdm:qry',
    'setInitialize':     'set',
    'setsysInitialize':  'set:sys',
    'capssuInitialize':  'caps:su',
    'accountInitialize': {
        'AccountServiceType_Application':   'acc:u0',
        'AccountServiceType_System':        'acc:u1',
        'AccountServiceType_Administrator': 'acc:su',
    },
    'viInitialize': {
        'ViServiceType_Application': 'vi:u',
        'ViServiceType_System':      'vi:s',
        'ViServiceType_Manager':     'vi:m',
    },
    'plInitialize': {
        'PlServiceType_User':   'pl:u',
        'PlServiceType_System': 'pl:s',
    },
}

# Servicos que nao saem de uma chamada com nome fixo, e por que eles precisam
# estar la assim mesmo.
SEMPRE = {
    'sfdnsres': 'resolver DNS — sem isso nenhum nome de servidor vira endereco',
    'csrng':    'aleatoriedade do TLS',
    'nvdrv:s':  'o framebuffer da tela de "puxando save da nuvem"',
}

# timeInitialize e socketInitialize aceitam mais de um servico; basta um.
QUALQUER_UM = [
    (['time:u', 'time:s'], 'timeInitialize', 'relogio — o TLS precisa da data pra validar o certificado'),
    (['bsd:s', 'bsd:u'],   'socketInitialize', 'os sockets'),
]

CHAMADA = re.compile(r'\b([a-zA-Z_][a-zA-Z0-9_]*Initialize)\s*\(')


def argumentos(texto, abre):
    """O que esta entre os parenteses da chamada que comeca em 'abre'.

    Nao da pra pegar isso com expressao regular: o argumento pode ter
    parenteses dentro. O plInitialize do sysmodule e literalmente
    `plInitialize(hosversionAtLeast(16, 0, 0) ? PlServiceType_User :
    PlServiceType_System)` — e as DUAS pontas do ternario abrem servico
    dependendo do firmware, entao as duas contam.
    """
    nivel = 0
    for i in range(abre, len(texto)):
        if texto[i] == '(':
            nivel += 1
        elif texto[i] == ')':
            nivel -= 1
            if nivel == 0:
                return texto[abre + 1:i]
    return ''


def fontes():
    for pasta in FONTES:
        for nome in sorted(os.listdir(pasta)):
            if nome.endswith(('.c', '.cpp')):
                yield os.path.join(pasta, nome)


def main():
    permitidos = set(json.load(open(NPDM))['service_access'])

    usados = {}   # servico -> [(arquivo, chamada)]
    chamadas_vistas = set()
    desconhecidas = []

    for caminho in fontes():
        texto = open(caminho, encoding='utf-8').read()
        # Comentário não é código. As explicações que este projeto tem sobre
        # serviço faltando citam o nome da chamada dentro do comentário, e sem
        # tirar isso o script "acha" serviço que ninguém abre.
        texto = re.sub(r'/\*.*?\*/', ' ', texto, flags=re.S)
        texto = re.sub(r'//[^\n]*', ' ', texto)

        for m in CHAMADA.finditer(texto):
            funcao = m.group(1)
            chamadas_vistas.add(funcao)
            if funcao in ('smInitialize', 'socketInitialize', 'timeInitialize'):
                continue
            regra = TABELA.get(funcao)
            if regra is None:
                desconhecidas.append((os.path.relpath(caminho, PROJETO), funcao))
                continue

            arg = argumentos(texto, m.end() - 1)
            rel = os.path.relpath(caminho, PROJETO)

            if isinstance(regra, str):
                usados.setdefault(regra, []).append((rel, f'{funcao}()'))
                continue

            # Todo valor do enum que aparecer dentro dos parenteses conta.
            achados = [srv for chave, srv in regra.items() if re.search(r'\b' + chave + r'\b', arg)]
            if not achados:
                desconhecidas.append((rel, f'{funcao}({arg.strip()})'))
                continue
            for srv in achados:
                usados.setdefault(srv, []).append((rel, f'{funcao}(...)'))

    falhas = []
    print('-- servicos abertos pelo codigo do sysmodule --')
    for servico in sorted(usados):
        ok = servico in permitidos
        if not ok:
            falhas.append(f'{servico} e aberto por {usados[servico][0][1]} em '
                          f'{usados[servico][0][0]}, mas NAO esta no service_access do NPDM')
        print(f'  {"ok  " if ok else "FALTA"} {servico:<10} {usados[servico][0][0]}')

    print('\n-- servicos que nao aparecem numa chamada, e precisam estar la --')
    for servico, porque in sorted(SEMPRE.items()):
        ok = servico in permitidos
        if not ok:
            falhas.append(f'{servico} nao esta no NPDM ({porque})')
        print(f'  {"ok  " if ok else "FALTA"} {servico:<10} {porque}')

    print('\n-- pelo menos um de cada grupo --')
    for grupo, funcao, porque in QUALQUER_UM:
        ok = any(s in permitidos for s in grupo)
        if not ok:
            falhas.append(f'nenhum de {grupo} esta no NPDM, e {funcao} precisa ({porque})')
        print(f'  {"ok  " if ok else "FALTA"} {" ou ".join(grupo):<16} {porque}')

    if desconhecidas:
        # Não é falha: é o script avisando que ficou cego. Chamada nova que a
        # TABELA não conhece some da conferência em silêncio, e silêncio aqui é
        # exatamente o problema que este arquivo existe pra evitar.
        print('\n-- AVISO: chamadas que este script nao sabe traduzir --')
        for arquivo, chamada in sorted(set(desconhecidas)):
            print(f'  ?    {chamada} em {arquivo} — acrescente na TABELA')

    sobrando = sorted(permitidos - set(usados) - set(SEMPRE)
                      - {s for g, _, _ in QUALQUER_UM for s in g})
    if sobrando:
        # Também não é falha. Permissão a mais não quebra nada; só vale saber,
        # porque NPDM enxuto é NPDM que um revisor consegue ler.
        print('\n-- no NPDM e aparentemente sem uso --')
        print('  ' + ', '.join(sobrando))

    print()
    if falhas:
        for f in falhas:
            print(f'  FALHOU: {f}')
        print(f'\n{len(falhas)} falha(s)')
        return 1

    print(f'{len(usados)} servico(s) conferido(s), nenhum faltando')
    return 0


if __name__ == '__main__':
    sys.exit(main())
