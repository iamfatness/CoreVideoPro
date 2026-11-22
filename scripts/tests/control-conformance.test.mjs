import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const script = resolve(dirname(fileURLToPath(import.meta.url)), '../control-conformance.mjs');

function stub(handler) {
  return new Promise(r => { const s = createServer(handler).listen(0, '127.0.0.1', () => r(s)); });
}
// Async on purpose: the stub servers live in THIS process, so a spawnSync would block the event
// loop that has to answer the child's requests (every request would time out).
const run = (port, extra = []) => new Promise(r => {
  const child = spawn(process.execPath, [script, '--base', `http://127.0.0.1:${port}`, ...extra]);
  let stdout = '', stderr = '';
  child.stdout.on('data', c => stdout += c);
  child.stderr.on('data', c => stderr += c);
  child.on('close', status => r({ status, stdout, stderr }));
});

test('a host that answers like Windows passes every live-safe case', async () => {
  const server = await stub((req, res) => {
    let body = ''; req.on('data', c => body += c); req.on('end', () => {
      res.setHeader('Content-Type', 'application/json');
      if (req.method === 'GET' && req.url === '/snapshot') return res.end(JSON.stringify({ ok: true, available: false, stale: true, staleAfterMs: 2000 }));
      if (req.method === 'GET' && req.url.startsWith('/manifest')) return res.end('{}');
      if (req.method !== 'POST' || req.url !== '/invoke') { res.statusCode = 404; return res.end('{"ok":false,"error":"Not found"}'); }
      res.statusCode = 400;
      if (!body.trim()) return res.end('{"ok":false,"error":"Request body is required."}');
      let j; try { j = JSON.parse(body); } catch { return res.end('{"ok":false,"error":"Invalid JSON: x"}'); }
      if (typeof j.action !== 'string' || !j.action.trim()) return res.end('{"ok":false,"error":"Missing \'action\'."}');
      const known = { 'scene.select': 'sceneId', 'transport.ftb.set': 'enabled' };
      if (!(j.action in known)) return res.end(JSON.stringify({ ok: false, error: `Unknown action '${j.action}'.` }));
      const args = Array.isArray(j.args) ? j.args : [];
      if (args[0] == null) return res.end(JSON.stringify({ ok: false, error: `Action '${j.action}' requires parameter '${known[j.action]}' at position 0.` }));
      return res.end(JSON.stringify({ ok: false, error: `Action '${j.action}' parameter '${known[j.action]}' must be Bool.` }));
    });
  });
  const result = await run(server.address().port, ['--host', 'windows']);
  server.close();
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.match(result.stdout, /PASS unknown-action/);
  assert.doesNotMatch(result.stdout, /bool-from-int|refusal-reply|preview-reply/, 'state-changing cases are never sent');
});

test('one wrong answer fails the run and names the case', async () => {
  const server = await stub((req, res) => { res.statusCode = 400; res.end('{"ok":false,"error":"nope"}'); });
  const result = await run(server.address().port);
  server.close();
  assert.equal(result.status, 1);
  assert.match(result.stdout, /FAIL unknown-action/);
});

test('an unreachable base is a usage/connection error, not a pass', async () => {
  const result = await run(1);
  assert.equal(result.status, 2);
});
