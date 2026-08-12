// google.js — o que este servidor faz com o Google, e o que ele se recusa a fazer.
//
// A regra é uma só: **nada que o cliente manda vira parâmetro do Google sem
// passar por uma lista fechada.** Repassar o corpo inteiro seria transformar
// este endpoint numa chave mestra — em particular no `scope`, que é o campo
// que decide o que a tela de consentimento vai pedir pro usuário.

const ESCOPO = 'https://www.googleapis.com/auth/drive.file'

const DEVICE_URL = 'https://oauth2.googleapis.com/device/code'
const TOKEN_URL = 'https://oauth2.googleapis.com/token'

export function credenciais() {
  const id = process.env.GOOGLE_CLIENT_ID
  const segredo = process.env.GOOGLE_CLIENT_SECRET
  if (!id || !segredo) {
    throw new Error('GOOGLE_CLIENT_ID/GOOGLE_CLIENT_SECRET nao configurados no ambiente')
  }
  return { id, segredo }
}

async function paraGoogle(url, campos) {
  const { id, segredo } = credenciais()
  const corpo = new URLSearchParams({ ...campos, client_id: id, client_secret: segredo })

  const r = await fetch(url, {
    method: 'POST',
    headers: { 'content-type': 'application/x-www-form-urlencoded' },
    body: corpo,
  })
  // Repassa o JSON do Google sem tocar: o app já sabe ler todos os erros dele
  // ("authorization_pending", "slow_down", "invalid_grant"...), e reescrever
  // isso aqui só criaria uma segunda gramática de erro pra manter.
  return { status: r.status, texto: await r.text() }
}

// O escopo é FIXADO aqui, e o que o cliente mandar é ignorado.
//
// É a diferença que este servidor faz de verdade. Com a chave dentro do .nro,
// quem a extrai pode pedir a tela de consentimento com escopo `drive` inteiro
// — o usuário leria "SwitchSaveSync quer acesso a todo o seu Google Drive",
// com o nome e a marca certos, e aprovaria. Com a chave aqui, o pior que
// alguém consegue pedir em nome deste projeto é drive.file: os arquivos que o
// próprio app criou. O estrago máximo cai de "o Drive todo" pra "as pastas de
// save".
export function pedeDeviceCode() {
  return paraGoogle(DEVICE_URL, { scope: ESCOPO })
}

export function trocaDeviceCode(deviceCode) {
  return paraGoogle(TOKEN_URL, {
    device_code: deviceCode,
    grant_type: 'urn:ietf:params:oauth:grant-type:device_code',
  })
}

export function renovaToken(refreshToken) {
  return paraGoogle(TOKEN_URL, {
    refresh_token: refreshToken,
    grant_type: 'refresh_token',
  })
}

export const ESCOPO_FIXO = ESCOPO
