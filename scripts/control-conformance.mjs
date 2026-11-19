#!/usr/bin/env node
// control-conformance.mjs — runs the LIVE-SAFE subset of contracts/control.fixtures.json against a
// running control API (Windows or macOS), so the two hosts can be checked against one file on a
// real build, not only in their unit suites.
//
//   node scripts/control-conformance.mjs --base http://127.0.0.1:8011 [--token T] [--host mac|windows] [--json out.json]
//
// Live-safe means the request cannot change a show:
//   - every http case whose request is not POST /invoke (404s, /manifest routing);
//   - the POST /invoke cases that are refused at binding, before any surface call (400s);
//   - with --host mac, invoke-unavailable-on-mac (a 422 the Mac answers before the surface).
// A case expecting 200 or a 422 from a surface reply is NEVER sent. GET /snapshot is checked for
// shape only. Requests carry no Origin header and POSTs carry Content-Type: application/json (the
// Mac refuses anything else with 415, by design).
//
// Exit: 0 every selected case passed, 1 any failure, 2 usage or connection error.
// Plan: docs/superpowers/plans/2026-11-16-mac-remote-control-phase1.md (Task 6).

import { readFileSync, writeFileSync } from 'node:fs';
import { request } from 'node:http';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { isDeepStrictEqual } from 'node:util';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');

/** POST /invoke cases refused before any surface call, safe against a live show. */
export const LIVE_SAFE_INVOKE = [
  'empty-body', 'whitespace-body', 'invalid-json', 'missing-action', 'non-string-action', 'blank-action',
  'unknown-action', 'retired-magic', 'missing-required', 'args-not-array', 'null-required', 'bad-bool',
];
const MAC_ONLY_SAFE = ['invoke-unavailable-on-mac'];

export function selectCases(fixtures, host) {
  return fixtures.http.filter(test => {
    if (test.hosts && !(host && test.hosts.includes(host))) return false;
    const invoke = test.request.method === 'POST' && test.request.path.split('?')[0].replace(/\/+$/, '') === '/invoke';
    if (!invoke) return true;
    return LIVE_SAFE_INVOKE.includes(test.name) || (host === 'mac' && MAC_ONLY_SAFE.includes(test.name));
  });
}

function usage(message) {
  console.error(`${message}\nusage: node scripts/control-conformance.mjs --base http://127.0.0.1:8011 [--token T] [--host mac|windows] [--json out.json]`);
  process.exit(2);
}

function parseArgs(argv) {
  const options = {};
  for (let i = 0; i < argv.length; i += 1) {
    const flag = argv[i];
    const value = argv[i + 1];
    if (!['--base', '--token', '--host', '--json'].includes(flag)) usage(`unknown argument ${flag}`);
    if (value === undefined) usage(`${flag} needs a value`);
    options[flag.slice(2)] = value;
    i += 1;
  }
  if (!options.base) usage('--base is required');
  if (options.host && !['mac', 'windows'].includes(options.host)) usage('--host must be mac or windows');
  return options;
}

class ConnectionError extends Error {}

function send(base, token, method, path, body) {
  const url = new URL(path, base);
  const headers = {};
  if (token) headers.Authorization = `Bearer ${token}`;
  const payload = body === undefined ? undefined : Buffer.from(body, 'utf8');
  if (method === 'POST') {
    headers['Content-Type'] = 'application/json';
    headers['Content-Length'] = String(payload ? payload.length : 0);
  }
  return new Promise((resolvePromise, reject) => {
    const req = request({ hostname: url.hostname, port: url.port, path: url.pathname + url.search, method, headers, timeout: 10000 }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => resolvePromise({ status: res.statusCode, text: Buffer.concat(chunks).toString('utf8') }));
    });
    req.on('timeout', () => req.destroy(new ConnectionError(`timed out: ${method} ${path}`)));
    req.on('error', error => reject(error instanceof ConnectionError ? error : new ConnectionError(`${method} ${path}: ${error.message}`)));
    if (payload) req.write(payload);
    req.end();
  });
}

function judge(test, response) {
  const expect = test.expect;
  if (response.status !== expect.status) return `status ${response.status}, expected ${expect.status}`;
  if (expect.body === undefined && expect.errorPrefix === undefined) return null;
  let body;
  try { body = JSON.parse(response.text); } catch { return `body is not JSON: ${response.text.slice(0, 120)}`; }
  if (expect.errorPrefix !== undefined) {
    if (typeof body.error !== 'string' || !body.error.startsWith(expect.errorPrefix)) {
      return `error ${JSON.stringify(body.error)} does not start with ${JSON.stringify(expect.errorPrefix)}`;
    }
    delete body.error;
  }
  if (expect.body !== undefined && !isDeepStrictEqual(body, expect.body)) {
    return `body ${JSON.stringify(body)}, expected ${JSON.stringify(expect.body)}`;
  }
  return null;
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  const fixtures = JSON.parse(readFileSync(resolve(root, 'contracts/control.fixtures.json'), 'utf8'));
  const results = [];
  try {
    for (const test of selectCases(fixtures, options.host)) {
      const response = await send(options.base, options.token, test.request.method, test.request.path, test.request.body);
      results.push({ name: test.name, why: judge(test, response) });
    }
    const snapshot = await send(options.base, options.token, 'GET', '/snapshot');
    let why = snapshot.status === 200 ? null : `status ${snapshot.status}`;
    if (!why) {
      try {
        const body = JSON.parse(snapshot.text);
        if (body.ok !== true) why = 'ok is not true';
        else if (typeof body.available !== 'boolean') why = 'available is not a boolean';
        else if (typeof body.stale !== 'boolean') why = 'stale is not a boolean';
        else if (body.staleAfterMs !== 2000) why = `staleAfterMs ${body.staleAfterMs}, expected 2000`;
      } catch { why = 'body is not JSON'; }
    }
    results.push({ name: 'snapshot-shape', why });
  } catch (error) {
    if (error instanceof ConnectionError) {
      console.error(`connection error: ${error.message}`);
      process.exit(2);
    }
    throw error;
  }
  for (const r of results) console.log(r.why ? `FAIL ${r.name}: ${r.why}` : `PASS ${r.name}`);
  const failed = results.filter(r => r.why).length;
  console.log(`${results.length - failed}/${results.length} passed against ${options.base}${options.host ? ` (host ${options.host})` : ''}`);
  if (options.json) {
    writeFileSync(options.json, JSON.stringify({ base: options.base, host: options.host ?? null, results }, null, 2) + '\n');
  }
  process.exit(failed ? 1 : 0);
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch(error => { console.error(error); process.exit(2); });
}
