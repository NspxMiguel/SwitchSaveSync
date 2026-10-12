// server.mjs - serves the Vercel handlers from ../api on a plain Node HTTP
// server, so the Google OAuth client secret can live on a VPS instead of
// inside the apps or CI.
//
//   /sss/{device,token,refresh}    SwitchSaveSync  (SSS_GOOGLE_CLIENT_ID/SECRET)
//   /nxbox/{device,token,refresh}  NXbox           (NXBOX_GOOGLE_CLIENT_ID/SECRET,
//                                                   falls back to SSS_*)
//   /healthz
//
// Listens on 127.0.0.1:8794 (PORT / HOST override). Put a tunnel in front.

import { createServer } from 'node:http'
import { contexto } from '../lib/google.js'
import device from '../api/device.js'
import token from '../api/token.js'
import refresh from '../api/refresh.js'

const env = process.env
const apps = {
  sss: {
    id: env.SSS_GOOGLE_CLIENT_ID,
    segredo: env.SSS_GOOGLE_CLIENT_SECRET,
  },
  nxbox: {
    id: env.NXBOX_GOOGLE_CLIENT_ID || env.SSS_GOOGLE_CLIENT_ID,
    segredo: env.NXBOX_GOOGLE_CLIENT_SECRET || env.SSS_GOOGLE_CLIENT_SECRET,
  },
}
const rotas = { device, token, refresh }

const server = createServer((req, res) => {
  const { pathname } = new URL(req.url, 'http://x')
  if (pathname === '/healthz') {
    res.setHeader('content-type', 'application/json')
    return res.end(JSON.stringify({ ok: true, apps: Object.keys(apps).filter((a) => apps[a].id && apps[a].segredo) }))
  }
  const m = pathname.match(/^\/(sss|nxbox)\/(device|token|refresh)$/)
  const cred = m && apps[m[1]]
  if (!cred || !cred.id || !cred.segredo) {
    res.statusCode = 404
    return res.end('{"error":"not_found"}')
  }
  // Behind a tunnel the real client is in cf-connecting-ip; the rate limiter
  // reads x-forwarded-for / x-real-ip.
  const ip = req.headers['cf-connecting-ip']
  if (ip) req.headers['x-forwarded-for'] = ip
  contexto.run({ id: cred.id, segredo: cred.segredo }, () => rotas[m[2]](req, res))
})

server.listen(Number(env.PORT) || 8794, env.HOST || '127.0.0.1')
