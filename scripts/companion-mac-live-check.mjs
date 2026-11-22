#!/usr/bin/env node
// Drives the Bitfocus Companion module's OWN compiled request code (fetchManifest,
// invokeAction, and the `ws` client the module uses) against a running CoreVideo
// Pro control server, Mac or Windows. RC5 of
// docs/superpowers/plans/2026-11-21-mac-control-audio-phase2.md.
//
//   node scripts/companion-mac-live-check.mjs --module <built module dir> --base http://127.0.0.1:8011
//        [--token <t>] [--allow-take]
//
// <built module dir> is a copy of companion-module-corevideopro with `npm install`
// and `npm run build` done (dist/manifest.js and node_modules/ws must exist).
// Read-only by default: it reads the manifest, the first /ws state frame, and
// invokes one action the Mac refuses (422) so nothing on air changes.
// --allow-take also invokes transport.take (it WILL Take; never on a live show).
// Exit 0 = every check passed.

import { pathToFileURL } from 'node:url'
import path from 'node:path'
import { createRequire } from 'node:module'

const args = process.argv.slice(2)
const opt = (name) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : undefined }
const moduleDir = opt('--module')
const base = opt('--base') ?? 'http://127.0.0.1:8011'
const token = opt('--token') ?? ''
const allowTake = args.includes('--allow-take')
if (!moduleDir) { console.error('usage: --module <built companion module dir> [--base URL] [--token T] [--allow-take]'); process.exit(2) }

const { fetchManifest, invokeAction } = await import(pathToFileURL(path.join(moduleDir, 'dist', 'manifest.js')).href)
const WebSocket = createRequire(path.join(moduleDir, 'package.json'))('ws')

// main.ts headers(): Content-Type on every request, Bearer when configured.
const headers = { 'Content-Type': 'application/json', ...(token ? { Authorization: `Bearer ${token}` } : {}) }
let failed = 0
const check = (ok, what) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what}`); if (!ok) failed += 1 }

const manifest = await fetchManifest(base, headers)
const ids = new Set(manifest.actions.map((a) => a.id))
check(manifest.actions.length > 0, `fetchManifest parsed ${manifest.actions.length} actions`)
for (const id of ['transport.ftb.toggle', 'transport.take', 'transport.record.toggle', 'transport.stream.toggle',
  'transport.engine.toggle', 'graphics.lowerThird.toggle']) {
  check(ids.has(id), `preset action ${id} is in the manifest`)
}

const refused = await invokeAction(base, headers, 'automation.autoAssignInputs.set', [true])
const refusedBody = await refused.json().catch(() => ({}))
check(ids.has('automation.autoAssignInputs.set') ? refused.ok : refused.status === 422,
  `autoassign preset: HTTP ${refused.status} ${refusedBody.error ?? ''}`)

if (allowTake) {
  const take = await invokeAction(base, headers, 'transport.take', [])
  check(take.ok, `transport.take: HTTP ${take.status}`)
}

// main.ts connectWebSocket: ws://host:port/ws[?token=], no options.
const wsUrl = base.replace(/^http/, 'ws') + '/ws' + (token ? `?token=${encodeURIComponent(token)}` : '')
const state = await new Promise((resolve) => {
  const ws = new WebSocket(wsUrl)
  const timer = setTimeout(() => { ws.terminate(); resolve(null) }, 5000)
  ws.on('message', (data) => { clearTimeout(timer); ws.close(); try { resolve(JSON.parse(data.toString())) } catch { resolve(null) } })
  ws.on('error', () => { clearTimeout(timer); resolve(null) })
})
check(state !== null, 'the first /ws frame is a JSON ControlState')
for (const field of ['programBlackout', 'canTake', 'recording', 'streaming', 'engineOn', 'lowerThirdOnAir']) {
  check(typeof state?.[field] === 'boolean', `feedback ${field} is a boolean`)
}

console.log(failed === 0 ? 'companion live check: PASS' : `companion live check: ${failed} FAILED`)
process.exit(failed === 0 ? 0 : 1)
