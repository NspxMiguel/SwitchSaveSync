// POST /api/device — começa o login. Sem corpo: o escopo é fixado no servidor.
import { monta } from '../lib/rota.js'
import { pedeDeviceCode } from '../lib/google.js'

export default monta('device', () => pedeDeviceCode())
