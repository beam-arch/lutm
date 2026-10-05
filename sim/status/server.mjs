import {createClient, Sandbox} from '@depot/sandbox'
import {createServer} from 'node:http'
import {readFile, writeFile, mkdir} from 'node:fs/promises'
import {homedir} from 'node:os'
import {fileURLToPath} from 'node:url'
import path from 'node:path'

const root = fileURLToPath(new URL('../../', import.meta.url))
const assets = fileURLToPath(new URL('.', import.meta.url))
const stateFile = process.env.BUILD_STATUS_BUILDER_FILE || path.join(root, '.hoplite/runtime/depot-arm64-dual/builder.json')
const cacheFile = path.join(root, '.hoplite/runtime/build-status.json')
const interval = 45000
let snapshot = {status: 'connecting', stage: 'Connecting to Depot', layouts: [], recent: []}
let busy = false
let installedId
try { snapshot = JSON.parse(await readFile(cacheFile, 'utf8')) } catch {}

async function refresh() {
  if (busy) return
  busy = true
  try {
    const state = JSON.parse(await readFile(stateFile, 'utf8'))
    const token = process.env.DEPOT_TOKEN || (await readFile(process.env.DEPOT_TOKEN_FILE || path.join(homedir(), '.config/hoplite-depot/token'), 'utf8')).trim()
    const builder = await Sandbox.get(createClient({token}), state.sandboxId)
    if (builder.status === 'running') {
      if (installedId !== builder.sandboxId) {
        await builder.fs().writeFile('/home/runner/lutm-build-status.py', await readFile(path.join(assets, 'collect.py')))
        installedId = builder.sandboxId
      }
      const command = await builder.runCommand({cmd: '/usr/bin/timeout', args: ['20', 'python3', '/home/runner/lutm-build-status.py']})
      const finished = await command.wait()
      if (finished.exitCode !== 0) throw new Error('Telemetry command failed')
      snapshot = JSON.parse(await command.stdout())
    } else {
      snapshot = {...snapshot, sampledAt: new Date().toISOString(), stage: snapshot.status === 'complete' ? 'Complete' : `Builder ${builder.status}`, status: snapshot.status === 'complete' ? 'complete' : 'stopped'}
    }
    snapshot = {...snapshot, createdAt: builder.createdAt, resources: builder.resources, connection: 'live'}
    await mkdir(path.dirname(cacheFile), {recursive: true})
    await writeFile(cacheFile, JSON.stringify(snapshot), {mode: 0o600})
  } catch {
    snapshot = {...snapshot, connection: 'unavailable', error: 'Depot could not be reached. Showing the last successful update; retrying automatically.'}
  } finally { busy = false }
}

const files = {'/': ['index.html', 'text/html'], '/style.css': ['style.css', 'text/css'], '/app.js': ['app.js', 'text/javascript']}
const server = createServer(async (request, response) => {
  response.setHeader('Cache-Control', 'no-store')
  response.setHeader('X-Content-Type-Options', 'nosniff')
  response.setHeader('Content-Security-Policy', "default-src 'self'; style-src 'self'; script-src 'self'; connect-src 'self'")
  if (request.method !== 'GET') { response.writeHead(405).end(); return }
  const route = new URL(request.url, 'http://localhost').pathname
  if (route === '/api/status') {
    response.writeHead(200, {'Content-Type': 'application/json'})
    response.end(JSON.stringify({...snapshot, refreshSeconds: interval / 1000}))
    return
  }
  const file = files[route]
  if (!file) { response.writeHead(404).end('Not found'); return }
  try {
    response.writeHead(200, {'Content-Type': `${file[1]}; charset=utf-8`})
    response.end(await readFile(path.join(assets, file[0])))
  } catch { response.writeHead(500).end('Page unavailable') }
})
server.listen(Number(process.env.PORT || 3000), '0.0.0.0', () => console.log('Build status page listening on port', process.env.PORT || 3000))
refresh()
setInterval(refresh, interval)
