// POST /api/token — device_code=... → o app troca o código aprovado por token.
// É a rota que o app bate de 5 em 5 segundos enquanto a pessoa confirma no
// celular, então o balde dela é o mais folgado dos três.
import { monta } from '../lib/rota.js'
import { trocaDeviceCode } from '../lib/google.js'

export default monta('token', (campos) => {
  const codigo = String(campos.device_code || '')
  if (!codigo || codigo.length > 512) {
    return { status: 400, texto: JSON.stringify({ error: 'invalid_request' }) }
  }
  return trocaDeviceCode(codigo)
})
