// teste.mjs — o freio é a única parte deste servidor que tem lógica de verdade,
// e é a que ninguém consegue conferir olhando: erro aqui só aparece no dia do
// ataque, que é o pior dia pra descobrir. Então ele é testado com o relógio na
// mão (todas as funções aceitam o instante como parâmetro).
//
//     node servidor/teste.mjs

import assert from 'node:assert/strict'
import { REGRAS, ATAQUE, verifica, entrou, saiu, zera, ipDe, emAtaque } from './lib/limite.js'
import { ESCOPO_FIXO, pedeDeviceCode } from './lib/google.js'
import { monta } from './lib/rota.js'

let passou = 0
const casos = []
const teste = (nome, fn) => casos.push([nome, fn])

const T0 = 1_800_000_000_000 // instante fixo: teste não pode depender de relógio

// ---------------------------------------------------------------- balde por IP

teste('o balde deixa passar a capacidade e freia a proxima', () => {
  zera()
  for (let i = 0; i < REGRAS.device.capacidade; i++) {
    assert.equal(verifica('1.1.1.1', 'device', T0).ok, true, `chamada ${i + 1} devia passar`)
  }
  const r = verifica('1.1.1.1', 'device', T0)
  assert.equal(r.ok, false)
  assert.equal(r.motivo, 'rate_limited')
  assert.ok(r.esperaS >= 1, 'tem que dizer quantos segundos esperar')
})

teste('esperar recarrega, e nao precisa virar o minuto inteiro', () => {
  zera()
  for (let i = 0; i < REGRAS.device.capacidade + 5; i++) verifica('2.2.2.2', 'device', T0)
  assert.equal(verifica('2.2.2.2', 'device', T0).ok, false)
  // recarga de 2/min = uma ficha a cada 30s
  assert.equal(verifica('2.2.2.2', 'device', T0 + 31_000).ok, true)
})

teste('um IP freado nao freia o vizinho', () => {
  zera()
  for (let i = 0; i < REGRAS.device.capacidade + 1; i++) verifica('3.3.3.3', 'device', T0)
  assert.equal(verifica('3.3.3.3', 'device', T0).ok, false)
  assert.equal(verifica('4.4.4.4', 'device', T0).ok, true)
})

teste('cada rota tem balde proprio', () => {
  zera()
  for (let i = 0; i < REGRAS.device.capacidade + 1; i++) verifica('5.5.5.5', 'device', T0)
  assert.equal(verifica('5.5.5.5', 'device', T0).ok, false)
  assert.equal(verifica('5.5.5.5', 'token', T0).ok, true, 'login travado nao pode travar a renovacao')
})

teste('o poll do login de 5 em 5 segundos cabe no balde por meia hora', () => {
  zera()
  // 30 minutos de poll: 12 por minuto. Se isto freia, o login normal quebra.
  for (let s = 0; s < 1800; s += 5) {
    const r = verifica('6.6.6.6', 'token', T0 + s * 1000)
    assert.equal(r.ok, true, `o poll foi freado aos ${s}s`)
  }
})

// -------------------------------------------------------------- modo de ataque

teste('rajada de muitos enderecos liga o modo ataque pra todo mundo', () => {
  zera()
  for (let i = 0; i <= ATAQUE.naJanela; i++) verifica(`10.0.${i >> 8}.${i & 255}`, 'token', T0)
  const novato = verifica('9.9.9.9', 'token', T0)
  assert.equal(novato.ok, false)
  assert.equal(novato.motivo, 'under_attack', 'quem chega limpo tambem tem que ver "under_attack"')
})

teste('muitas chamadas abertas ao mesmo tempo tambem ligam', () => {
  zera()
  for (let i = 0; i <= ATAQUE.simultaneas + 1; i++) entrou()
  const r = verifica('8.8.8.8', 'device', T0)
  assert.equal(r.motivo, 'under_attack')
  for (let i = 0; i <= ATAQUE.simultaneas + 1; i++) saiu()
})

teste('o modo ataque passa sozinho depois do tempo', () => {
  zera()
  for (let i = 0; i <= ATAQUE.naJanela; i++) verifica(`11.0.${i >> 8}.${i & 255}`, 'token', T0)
  assert.equal(emAtaque(T0), true)
  const depois = T0 + ATAQUE.duracaoMs + 1000
  assert.equal(emAtaque(depois), false)
  assert.equal(verifica('7.7.7.7', 'token', depois).ok, true)
})

teste('a janela e curta: uso espalhado no dia nao liga o modo ataque', () => {
  zera()
  for (let i = 0; i < ATAQUE.naJanela * 3; i++) {
    verifica(`12.0.${i >> 8}.${i & 255}`, 'token', T0 + i * 1000) // 1 por segundo
  }
  assert.equal(emAtaque(T0 + ATAQUE.naJanela * 3000), false)
})

// -------------------------------------------------------------------- endereco

teste('x-forwarded-for com cadeia: vale o primeiro', () => {
  assert.equal(ipDe({ headers: { 'x-forwarded-for': '203.0.113.9, 70.41.3.18' } }), '203.0.113.9')
})

teste('sem x-forwarded-for, cai no x-real-ip', () => {
  assert.equal(ipDe({ headers: { 'x-real-ip': '198.51.100.4' } }), '198.51.100.4')
})

// ------------------------------------------------------------- escopo fixado

teste('o escopo e drive.file, e o que o cliente mandar e ignorado', async () => {
  process.env.GOOGLE_CLIENT_ID = 'id-de-teste'
  process.env.GOOGLE_CLIENT_SECRET = 'segredo-de-teste'

  let corpoVisto = null
  const fetchOriginal = globalThis.fetch
  globalThis.fetch = async (_url, opcoes) => {
    corpoVisto = opcoes.body.toString()
    return { status: 200, text: async () => '{}' }
  }
  try {
    await pedeDeviceCode({ scope: 'https://www.googleapis.com/auth/drive' })
  } finally {
    globalThis.fetch = fetchOriginal
  }

  const campos = new URLSearchParams(corpoVisto)
  assert.equal(campos.get('scope'), ESCOPO_FIXO)
  assert.equal(ESCOPO_FIXO, 'https://www.googleapis.com/auth/drive.file')
  assert.equal(campos.get('client_secret'), 'segredo-de-teste', 'o segredo tem que sair daqui, nao do app')
  assert.ok(!corpoVisto.includes('auth/drive&') && !corpoVisto.endsWith('auth/drive'),
            'escopo largo do cliente nao pode passar')
})

// ------------------------------------------------------------------ as rotas

function respostaFalsa() {
  return {
    statusCode: 0, corpo: '', cabecalhos: {},
    setHeader(k, v) { this.cabecalhos[k.toLowerCase()] = v },
    end(t) { this.corpo = t ?? '' },
  }
}

teste('GET e recusado: estas rotas so existem em POST', async () => {
  zera()
  const res = respostaFalsa()
  await monta('device', async () => ({ status: 200, texto: '{}' }))({ method: 'GET', headers: {} }, res)
  assert.equal(res.statusCode, 405)
})

teste('POST sem corpo nenhum funciona: /device nao manda campo algum', async () => {
  zera()
  const res = respostaFalsa()
  const h = monta('device', async () => ({ status: 200, texto: '{"device_code":"abc"}' }))
  await h({ method: 'POST', headers: { 'x-real-ip': '17.17.17.17' } }, res) // sem body, sem iterador
  assert.equal(res.statusCode, 200)
  assert.equal(res.corpo, '{"device_code":"abc"}')
})

teste('corpo em texto tambem e lido', async () => {
  zera()
  let visto = null
  const res = respostaFalsa()
  const h = monta('token', async (campos) => { visto = campos; return { status: 200, texto: '{}' } })
  await h({ method: 'POST', headers: { 'x-real-ip': '18.18.18.18' }, body: 'device_code=xyz' }, res)
  assert.equal(visto.device_code, 'xyz')
})

teste('freada responde 429, com o motivo e o retry-after', async () => {
  zera()
  const req = { method: 'POST', headers: { 'x-real-ip': '13.13.13.13' } }
  const h = monta('device', async () => ({ status: 200, texto: '{}' }))
  for (let i = 0; i < REGRAS.device.capacidade; i++) await h(req, respostaFalsa())

  const res = respostaFalsa()
  await h(req, res)
  assert.equal(res.statusCode, 429)
  const json = JSON.parse(res.corpo)
  assert.equal(json.error, 'rate_limited')
  assert.ok(res.cabecalhos['retry-after'], 'faltou o retry-after')
})

teste('erro falando com o Google nao vaza detalhe pro cliente', async () => {
  zera()
  const res = respostaFalsa()
  const h = monta('token', async () => { throw new Error('device_code=SEGREDO-QUE-NAO-PODE-SAIR') })
  await h({ method: 'POST', headers: { 'x-real-ip': '14.14.14.14' }, body: {} }, res)
  assert.equal(res.statusCode, 502)
  assert.equal(res.corpo.includes('SEGREDO'), false)
})

teste('a resposta do Google passa inteira, com o status dele', async () => {
  zera()
  const res = respostaFalsa()
  const corpoDoGoogle = '{"error":"authorization_pending"}'
  const h = monta('token', async () => ({ status: 428, texto: corpoDoGoogle }))
  await h({ method: 'POST', headers: { 'x-real-ip': '15.15.15.15' }, body: { device_code: 'x' } }, res)
  assert.equal(res.statusCode, 428)
  assert.equal(res.corpo, corpoDoGoogle)
  assert.equal(res.cabecalhos['cache-control'], 'no-store')
})

teste('nao existe cabecalho de CORS: o cliente e um Switch, nao um navegador', async () => {
  zera()
  const res = respostaFalsa()
  const h = monta('device', async () => ({ status: 200, texto: '{}' }))
  await h({ method: 'POST', headers: { 'x-real-ip': '16.16.16.16' }, body: {} }, res)
  assert.equal(res.cabecalhos['access-control-allow-origin'], undefined)
})

// ----------------------------------------------------------------------------

const falhas = []
for (const [nome, fn] of casos) {
  try {
    await fn()
    passou++
    console.log(`  ok   ${nome}`)
  } catch (e) {
    falhas.push([nome, e])
    console.log(`  FALHOU  ${nome}\n         ${e.message}`)
  }
}

console.log(`\n${passou}/${casos.length} passaram`)
process.exit(falhas.length ? 1 : 0)
