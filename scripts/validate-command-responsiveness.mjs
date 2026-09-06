/** Real stdin/engine-IPC regression using only a stub core and synthetic engine.
 * Build: cmake --build native/build --config Release --target corevideo-native corevideo-zoom-engine-fake
 * Run: node scripts/validate-command-responsiveness.mjs [--build-dir native/build]
 * Never changes installed engines or opens a real meeting/camera.
 */
import { spawn, execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { once } from 'node:events';
import { readFile, writeFile, mkdtemp, rm } from 'node:fs/promises';
import { resolve, join, dirname, basename } from 'node:path';
import { tmpdir } from 'node:os';
import { performance } from 'node:perf_hooks';
import { createHash } from 'node:crypto';

const exec = promisify(execFile);
const arg = process.argv.indexOf('--build-dir');
const buildDir = resolve(arg >= 0 ? process.argv[arg + 1] : 'native/build');
const suffix = process.platform === 'win32' ? '.exe' : '';
const core = join(buildDir, `corevideo-native${suffix}`);
const fake = join(buildDir, `corevideo-zoom-engine-fake${suffix}`);
const cache = await readFile(join(buildDir, 'CMakeCache.txt'), 'utf8');
if (!/^COREVIDEO_STUB:BOOL=ON$/m.test(cache)) throw new Error('This test requires COREVIDEO_STUB=ON; refusing a hardware build.');
if (/^COREVIDEO_WITH_(UVC|WASAPI_CAPTURE|AVF_CAPTURE|COREAUDIO|DECKLINK|AJA|SCK|WGC):BOOL=ON$/m.test(cache))
  throw new Error('Capture adapters must be OFF for this synthetic test.');

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const percentile = (values, p) => [...values].sort((a, b) => a - b)[Math.ceil(values.length * p) - 1];
async function memoryBytes(pid) {
  if (process.platform === 'win32') {
    const { stdout } = await exec('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command',
      `(Get-Process -Id ${pid}).PrivateMemorySize64`], { windowsHide: true });
    return Number(stdout.trim());
  }
  const status = await readFile(`/proc/${pid}/status`, 'utf8');
  return Number(status.match(/^VmRSS:\s+(\d+)/m)?.[1]) * 1024;
}

async function scenario(stage) {
  const temp = await mkdtemp(join(tmpdir(), 'corevideo-command-proof-'));
  const engineLog = join(temp, 'fake.log');
  const child = spawn(core, [], { cwd: buildDir, windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'], env: {
    ...process.env, COREVIDEO_ZOOM_ENGINE_PATH: fake, COREVIDEO_FAKE_ENGINE_LOG: engineLog,
    COREVIDEO_ZOOM_SDK_JWT: '', COREVIDEO_ZOOM_PUBLIC_APP_KEY: 'synthetic',
    COREVIDEO_ZOOM_USER_ZAK: '', COREVIDEO_ZOOM_ON_BEHALF_TOKEN: '', COREVIDEO_ZOOM_APP_PRIVILEGE_TOKEN: '',
    COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE: '0', COREVIDEO_FAKE_NO_VIDEO: '1', COREVIDEO_FAKE_NO_CHURN: '1',
    COREVIDEO_FAKE_AUTH_DELAY_MS: stage === 'auth' ? '30000' : '0',
    COREVIDEO_FAKE_JOIN_DELAY_MS: stage === 'join' ? '30000' : '0', COREVIDEO_ZOOM_JOIN_WAIT_MS: '60000',
  }});
  let buffer = '', nextId = 0, handshake, stderr = '';
  const pending = new Map();
  const exited = once(child, 'exit');
  child.stderr.on('data', chunk => { stderr = (stderr + chunk).slice(-16000); });
  child.stdout.on('data', chunk => {
    buffer += chunk;
    let end;
    while ((end = buffer.indexOf('\n')) >= 0) {
      const line = buffer.slice(0, end); buffer = buffer.slice(end + 1);
      if (!line.trim()) continue;
      const message = JSON.parse(line);
      if (message.type === 'handshake') handshake = message;
      const item = pending.get(message.id);
      if (item) {
        pending.delete(message.id); clearTimeout(item.timer);
        item.resolve({ message, latencyMs: performance.now() - item.started });
      }
    }
  });
  function send(type, extra = {}) {
    const id = `synthetic-${++nextId}`;
    const reply = new Promise((resolve, reject) => {
      const timer = setTimeout(() => { pending.delete(id); reject(new Error(`${type} timed out`)); }, type === 'zoom-join' ? 60000 : 10000);
      pending.set(id, { resolve, reject, timer, started: performance.now() });
    });
    const writable = child.stdin.write(JSON.stringify({ id, type, ...extra }) + '\n');
    return { id, reply, drained: writable ? Promise.resolve() : once(child.stdin, 'drain') };
  }
  const ping = [], stop = [], memory = [];
  let overload = 0;
  try {
    for (let i = 0; !handshake && i < 100; i++) await sleep(50);
    if (!handshake) throw new Error('Core did not handshake');
    const joining = send('zoom-join', { payload: { meetingNumber: '1234567890', displayName: 'Synthetic command proof' } });
    // Attach rejection immediately while deliberately leaving the join pending.
    joining.reply.catch(() => {});
    let entered = false;
    for (let i = 0; i < 100; i++) {
      const log = await readFile(engineLog, 'utf8').catch(() => '');
      if (log.includes(`synthetic-${stage}-delay=30000`)) { entered = true; break; }
      await sleep(50);
    }
    if (!entered) throw new Error(`Fake engine did not enter delayed ${stage}`);
    memory.push(await memoryBytes(child.pid));
    for (let i = 0; i < 40; i++) {
      const p = await send('ping').reply;
      const s = await send('media-core-sync', { commands: [{ type: 'stop-recording-session', reason: 'synthetic-test' }] }).reply;
      if (!p.message.ok || !s.message.ok) throw new Error('Ping/Stop was rejected');
      ping.push(p.latencyMs); stop.push(s.latencyMs);
    }
    const rejectedLeave = await send('zoom-leave', { protocolVersion: { major: 999, minor: 0 } }).reply;
    if (rejectedLeave.message.ok || !JSON.stringify(rejectedLeave.message).includes('incompatible-protocol'))
      throw new Error('Unsupported protocol was not rejected');
    await sleep(100);
    if (!pending.has(joining.id)) throw new Error('Rejected Leave cancelled a valid in-flight join');
    // 128 MiB of wire input, continuously drained stdout, bounded sender batches.
    // Unit tests establish exact mailbox limits; these samples detect process growth.
    const padding = 'x'.repeat(65536);
    for (let wave = 0; wave < 8; wave++) {
      const replies = [];
      for (let i = 0; i < 256; i++) {
        const request = send('ping', { padding });
        replies.push(request.reply); await request.drained;
      }
      for (const result of await Promise.all(replies)) {
        if (!result.message.ok) {
          if (JSON.stringify(result.message).includes('control-overloaded')) overload++;
          else throw new Error(`Unexpected burst rejection: ${JSON.stringify(result.message)}`);
        }
      }
      memory.push(await memoryBytes(child.pid));
    }
    if (!pending.has(joining.id)) throw new Error('Join finished before cancellation; delay was not maintained');
    const leave = await send('zoom-leave').reply;
    const cancelled = await joining.reply;
    if (!leave.message.ok || !JSON.stringify(cancelled.message).includes('operation-cancelled'))
      throw new Error('Leave failed to cancel the original join');
    const snapshot = await send('zoom-snapshot').reply;
    if (snapshot.message.snapshot?.meetingState === 'in_meeting') throw new Error('Cancelled join became live');
    const stats = values => ({ samples: values.length, p95Ms: percentile(values, .95), maxMs: Math.max(...values) });
    const result = { stage, injectedDelayMs: 30000, ping: stats(ping), stop: stats(stop), leaveMs: leave.latencyMs,
      inputMiB: 128, overloadRejections: overload, memoryMetric: process.platform === 'win32' ? 'privateBytes' : 'rssBytes',
      memoryBaselineBytes: memory[0], memoryPeakBytes: Math.max(...memory), memoryFinalBytes: memory.at(-1),
      processEpoch: handshake.processEpoch };
    if (result.ping.p95Ms > 250 || result.stop.p95Ms > 250 || result.ping.maxMs >= 1000 || result.stop.maxMs >= 1000)
      throw new Error(`Synthetic command latency exceeded target: ${JSON.stringify(result)}`);
    if (result.memoryPeakBytes - result.memoryBaselineBytes > 128 * 1024 * 1024)
      throw new Error(`Core memory grew beyond synthetic 128 MiB allowance: ${JSON.stringify(result)}`);
    return result;
  } catch (error) {
    error.message += `\nCore stderr tail:\n${stderr}`;
    throw error;
  } finally {
    for (const item of pending.values()) clearTimeout(item.timer);
    child.stdin.end();
    await Promise.race([exited, sleep(5000)]);
    if (child.exitCode === null) { child.kill(); await exited; }
    const cleanupPath = resolve(temp);
    if (dirname(cleanupPath) !== resolve(tmpdir()) || !basename(cleanupPath).startsWith('corevideo-command-proof-'))
      throw new Error('Refusing cleanup outside the synthetic test temporary directory');
    await rm(cleanupPath, { recursive: true, force: true });
  }
}

const evidence = { scope: 'Synthetic stub core and fake Zoom subprocess; no real meeting, camera, GPU or hardware acceptance.',
  createdAt: new Date().toISOString(), core, fake,
  coreSha256: createHash('sha256').update(await readFile(core)).digest('hex'),
  fakeSha256: createHash('sha256').update(await readFile(fake)).digest('hex'), scenarios: [] };
for (const stage of ['auth', 'join']) {
  const result = await scenario(stage);
  evidence.scenarios.push(result);
  console.log(JSON.stringify(result));
}
await writeFile(join(buildDir, 'command-responsiveness-evidence.json'), JSON.stringify(evidence, null, 2) + '\n');
console.log('Synthetic command responsiveness: PASS');
