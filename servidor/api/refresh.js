// POST /api/refresh — refresh_token=... → access_token novo.
import { monta } from '../lib/rota.js'
import { renovaToken } from '../lib/google.js'

export default monta('refresh', (campos) => {
  const token = String(campos.refresh_token || '')
  if (!token || token.length > 1024) {
    return { status: 400, texto: JSON.stringify({ error: 'invalid_request' }) }
  }
  return renovaToken(token)
})
