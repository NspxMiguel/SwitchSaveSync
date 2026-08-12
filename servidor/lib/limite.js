// limite.js — freio por IP e detector de rajada.
//
// Duas defesas diferentes, porque são dois problemas diferentes:
//
//   1. UM endereço insistindo  → balde por IP. Cada rota tem o seu, com
//      capacidade e velocidade de recarga pensadas pro uso normal daquela
//      rota (o poll do login bate de 5 em 5 segundos; renovar token não).
//   2. MUITOS ao mesmo tempo   → modo ataque. Conta o que chega no endpoint
//      inteiro numa janela curta, e conta quantas chamadas estão abertas no
//      mesmo instante. Passou, todo mundo leva 429 com "under_attack" por
//      alguns minutos, e o app mostra a mensagem certa em vez de acusar a
//      internet do usuário.
//
// O estado é de memória, de propósito: banco de dados aqui significaria conta
// em outro serviço e conta pra pagar, e o projeto não tem nem um nem outro. O
// preço é que o contador zera quando a Vercel recicla a instância, e que duas
// instâncias contam separado. Pra rajada — que é curta e concentrada — isso
// funciona; pra alguém batendo devagar durante o dia inteiro, não pega. Se um
// dia pegar mal, o lugar de trocar é aqui, com Redis, sem mexer no app.
//
// Nenhum IP é guardado em texto: a chave do mapa é um hash. Um despejo de
// memória deste processo não entrega de onde ninguém entrou.

import { createHash } from 'node:crypto'

const MINUTO = 60_000

// capacidade = quantas seguidas aguenta sem reclamar (a rajada normal)
// recarga    = quantas voltam por minuto (o ritmo sustentado)
export const REGRAS = {
  device:  { capacidade: 10, recarga: 2 },
  token:   { capacidade: 90, recarga: 15 },
  refresh: { capacidade: 60, recarga: 10 },
}

// Sinais de que o endpoint está apanhando, não sendo usado.
export const ATAQUE = {
  janelaMs: 10_000,   // a janela curta onde a rajada aparece
  naJanela: 400,      // chamadas em 10s vindas de todo mundo
  simultaneas: 60,    // chamadas abertas no mesmo instante
  ipsDistintos: 3000, // endereços diferentes vivos na memória ao mesmo tempo
  duracaoMs: 10 * MINUTO,
}

const baldes = new Map()
let recentes = []          // instantes das chamadas na janela curta
let abertas = 0
let ataqueAte = 0

const agora = () => Date.now()

function chave(ip, rota) {
  return createHash('sha256').update(`${rota}\0${ip}`).digest('base64').slice(0, 22)
}

// x-forwarded-for pode vir com a cadeia inteira; o primeiro é o cliente. Quem
// chega sem nenhum dos dois cabeçalhos não existe na Vercel, mas existe em
// teste — e aí vira um IP só, o que é o comportamento seguro (freia mais).
export function ipDe(req) {
  const encaminhado = req.headers?.['x-forwarded-for']
  if (typeof encaminhado === 'string' && encaminhado.length) {
    return encaminhado.split(',')[0].trim()
  }
  return req.headers?.['x-real-ip'] || 'desconhecido'
}

function limpaVelhos(t) {
  recentes = recentes.filter((quando) => t - quando < ATAQUE.janelaMs)
  if (baldes.size <= ATAQUE.ipsDistintos) return
  // Mapa grande demais é, ele mesmo, sinal de ataque distribuído — e é também
  // o caminho pra estourar a memória da instância. Joga fora o que já está
  // cheio de novo (quem parou de bater) antes de deixar crescer.
  for (const [k, b] of baldes) {
    if (t - b.visto > 5 * MINUTO) baldes.delete(k)
  }
}

export function emAtaque(t = agora()) {
  return t < ataqueAte
}

function marcaAtaque(t) {
  ataqueAte = t + ATAQUE.duracaoMs
}

// Chamada uma vez por requisição, antes de falar com o Google.
// Devolve { ok } ou { ok:false, motivo:'rate_limited'|'under_attack', esperaS }.
export function verifica(ip, rota, t = agora()) {
  const regra = REGRAS[rota]
  if (!regra) throw new Error(`rota sem regra: ${rota}`)

  limpaVelhos(t)
  recentes.push(t)

  if (emAtaque(t)) {
    return { ok: false, motivo: 'under_attack', esperaS: Math.ceil((ataqueAte - t) / 1000) }
  }

  if (recentes.length > ATAQUE.naJanela ||
      abertas > ATAQUE.simultaneas ||
      baldes.size > ATAQUE.ipsDistintos) {
    marcaAtaque(t)
    return { ok: false, motivo: 'under_attack', esperaS: Math.ceil(ATAQUE.duracaoMs / 1000) }
  }

  const k = chave(ip, rota)
  let b = baldes.get(k)
  if (!b) {
    b = { fichas: regra.capacidade, visto: t }
    baldes.set(k, b)
  }

  // Recarga contínua: quem espera dez segundos recupera um pedaço, em vez de
  // ficar preso até virar o minuto.
  const decorrido = t - b.visto
  b.fichas = Math.min(regra.capacidade, b.fichas + (decorrido / MINUTO) * regra.recarga)
  b.visto = t

  if (b.fichas < 1) {
    const faltam = (1 - b.fichas) / regra.recarga
    return { ok: false, motivo: 'rate_limited', esperaS: Math.max(1, Math.ceil(faltam * 60)) }
  }

  b.fichas -= 1
  return { ok: true }
}

export function entrou() { abertas += 1 }
export function saiu() { abertas = Math.max(0, abertas - 1) }

// Só pro teste: devolve tudo ao estado inicial.
export function zera() {
  baldes.clear()
  recentes = []
  abertas = 0
  ataqueAte = 0
}
