// rota.js — o que toda rota faz igual: método, corpo, freio e resposta.
//
// Nenhum log leva token, refresh_token, device_code ou IP. O que este servidor
// não escreve, ele não vaza depois.

import { ipDe, verifica, entrou, saiu } from './limite.js'

const CORPO_MAX = 4096 // um refresh_token do Google não passa de ~512 bytes

async function leCorpo(req) {
  if (req.body && typeof req.body === 'object') return req.body // a Vercel já parseou
  if (typeof req.body === 'string') return Object.fromEntries(new URLSearchParams(req.body))
  // /device é POST sem corpo nenhum — e requisição sem corpo não é sempre
  // iterável. Tratar isso como erro dava 502 num pedido perfeitamente válido.
  if (typeof req?.[Symbol.asyncIterator] !== 'function') return {}

  let bruto = ''
  for await (const pedaco of req) {
    bruto += pedaco
    if (bruto.length > CORPO_MAX) throw new Error('corpo grande demais')
  }
  return Object.fromEntries(new URLSearchParams(bruto))
}

function responde(res, status, objeto, extras = {}) {
  res.statusCode = status
  res.setHeader('content-type', 'application/json; charset=utf-8')
  res.setHeader('cache-control', 'no-store')
  // Sem CORS de propósito: o cliente é um Switch, não um navegador. Liberar
  // origem daria a qualquer página web um caminho pronto pra bater aqui.
  for (const [k, v] of Object.entries(extras)) res.setHeader(k, v)
  res.end(typeof objeto === 'string' ? objeto : JSON.stringify(objeto))
}

// rota: 'device' | 'token' | 'refresh'
// trabalho(campos) -> { status, texto }
export function monta(rota, trabalho) {
  return async function handler(req, res) {
    if (req.method !== 'POST') {
      return responde(res, 405, { error: 'method_not_allowed' })
    }

    const freio = verifica(ipDe(req), rota)
    if (!freio.ok) {
      return responde(res, 429, { error: freio.motivo, retry_after: freio.esperaS },
                      { 'retry-after': String(freio.esperaS) })
    }

    entrou()
    try {
      const campos = await leCorpo(req)
      const { status, texto } = await trabalho(campos)
      res.statusCode = status
      res.setHeader('content-type', 'application/json; charset=utf-8')
      res.setHeader('cache-control', 'no-store')
      return res.end(texto)
    } catch (e) {
      // A mensagem do erro pode conter o que veio no corpo. Não vai pra fora.
      console.error(`[${rota}] falhou: ${e?.constructor?.name || 'erro'}`)
      return responde(res, 502, { error: 'upstream_failed' })
    } finally {
      saiu()
    }
  }
}

export { responde }
