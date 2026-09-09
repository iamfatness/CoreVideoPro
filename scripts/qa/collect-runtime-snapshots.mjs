/** Runtime sessionState collector — the producer for
 * `scripts/qa/production-qualification.mjs --runtime-snapshots`.
 *
 * The two Wave 0 judges are genuinely fail-closed and nothing fed them. This
 * samples a real media core's `sessionState` on a DECLARED schedule and writes
 * the envelope `runtime-snapshot-qualification.mjs` consumes:
 *
 *   { expectedWorkers[], recordingExpected, policy{}, samples[
 *       { processGeneration, collectedAtMs, snapshot{ realtimeEvidence,
 *         programBuffer, encoderEvidence } } ] }
 *
 * Usage:
 *   node scripts/qa/collect-runtime-snapshots.mjs --out capture.json [options]
 *     --core PATH        native core executable (default native/build-dev)
 *     --seconds N        capture duration (default 30)
 *     --interval-ms N    sample period, >= 50 (default 250) — DECLARED in the
 *                        envelope, because the judge reasons about staleness
 *     --load N           synthesize N 1080p60 Zoom feeds via the fake engine
 *     --recording        drive a recording session for the window and set
 *                        recordingExpected:true (the judge then requires
 *                        encoderEvidence in every sample)
 *     --expect-workers   comma list of render,audio,videoOutput (default all)
 *     --policy JSON      override staleWorkerMs/encoderQueueAgeMs/
 *                        operationAgeMs/finalizeAgeMs; recorded in the verdict
 *
 * DESIGN NOTES, each one a constraint rather than a preference:
 *
 * - HOW IT REACHES THE CORE. The same stdio JSON-RPC wire the shell and every
 *   other headless probe uses (`scripts/mac-show-drill.py`,
 *   `scripts/validate-recording-finalization.mjs`). There is no second
 *   mechanism: the shell's HTTP control API serves a shaped `ControlState`,
 *   which does not carry `realtimeEvidence`/`programBuffer`/`encoderEvidence`,
 *   so it cannot answer the judge's questions. A core already owned by a
 *   running shell owns its own pipes and cannot be sampled from here; this
 *   collector therefore runs its own core, which is what a G2 evidence run
 *   wants anyway (an exactly known binary and process generation).
 *
 * - PERTURBATION. The request is bare `{"type":"snapshot"}` — the minimum-work
 *   read path in `JsonRpcServer::handle`: no command mutation, no synthetic
 *   render tick, just `MediaCore::sessionState()`. Honest caveat: every request
 *   path in the core serialises on `coreMutex`, so no read is free; there is no
 *   lock-free snapshot surface today. What this does instead is stay far below
 *   the traffic the product itself generates — the shell polls at 2 Hz, the
 *   default here is 4 Hz — and never touch `audioOutputMutex_`, the encoder or
 *   the engine. The interval floor of 50 ms exists so this cannot quietly turn
 *   into load.
 *
 * - `nativeNowMs` IS DELIBERATELY NOT EMITTED. The judge will use it to age a
 *   stop queued behind another writer call, but only against the core's own
 *   monotonic clock (`AsyncEncoderSink`'s steady_clock). This collector's clock
 *   is unrelated, and the judge explicitly forbids substituting it. The core
 *   publishes no monotonic "now", so an outstanding queued stop is reported
 *   unverified rather than guessed at. Closing that needs one field in
 *   `MediaCore::sessionState`, not a fabricated number here.
 *
 * - EVIDENCE SURVIVES A BAD RUN. The envelope is written in a `finally`
 *   (the `validate-recording-finalization.mjs` pattern), so a crashed or aborted
 *   run still yields what it captured. A run that captured NOTHING is written
 *   too, but marked `collectorFailed` with a populated `errors[]` and exits
 *   non-zero — never a quiet empty-but-well-formed envelope.
 */
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createHash } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';
import { DEFAULT_RUNTIME_POLICY } from './runtime-snapshot-qualification.mjs';

const REPO = resolve(fileURLToPath(new URL('../..', import.meta.url)));
const EXE = process.platform === 'win32' ? '.exe' : '';
const BUILD_DIR = process.env.COREVIDEO_BUILD_DIR
  || join(REPO, 'native', process.platform === 'win32' ? 'build-dev' : 'build-metal');
const WORKERS = ['render', 'audio', 'videoOutput'];

const { values } = parseArgs({ options: {
  core: { type: 'string' }, out: { type: 'string' }, seconds: { type: 'string' },
  'interval-ms': { type: 'string' }, load: { type: 'string' }, recording: { type: 'boolean' },
  'expect-workers': { type: 'string' }, policy: { type: 'string' },
} });

if (!values.out) throw new Error('--out FILE is required; the envelope is the whole point of this script');
const outPath = resolve(values.out);
const core = resolve(values.core || join(BUILD_DIR, `corevideo-native${EXE}`));
const seconds = Number(values.seconds ?? 30);
const intervalMs = Number(values['interval-ms'] ?? 250);
const load = Number(values.load ?? 0);
if (!Number.isFinite(seconds) || seconds <= 0) throw new Error('--seconds must be a positive number');
if (!Number.isFinite(intervalMs) || intervalMs < 50) throw new Error('--interval-ms must be >= 50; a faster poll becomes the load it is measuring');
if (!Number.isInteger(load) || load < 0) throw new Error('--load must be a non-negative integer');
const expectedWorkers = (values['expect-workers'] ?? WORKERS.join(',')).split(',').map(x => x.trim()).filter(Boolean);
for (const name of expectedWorkers) if (!WORKERS.includes(name)) throw new Error(`--expect-workers accepts only ${WORKERS.join(',')}`);
if (new Set(expectedWorkers).size !== expectedWorkers.length) throw new Error('--expect-workers must be unique');
const policy = { ...DEFAULT_RUNTIME_POLICY, ...(values.policy ? JSON.parse(values.policy) : {}) };
const fakeEngine = process.env.COREVIDEO_FAKE_ENGINE_PATH || join(BUILD_DIR, `corevideo-zoom-engine-fake${EXE}`);

const startedNs = process.hrtime.bigint();
const monotonicMs = () => Number(process.hrtime.bigint() - startedNs) / 1e6;
const sleep = ms => new Promise(r => setTimeout(r, ms));

const capture = {
  version: 'runtime-snapshot-capture-v1',
  collector: {
    script: 'scripts/qa/collect-runtime-snapshots.mjs',
    request: 'snapshot',
    core,
    coreSha256: createHash('sha256').update(await readFile(core)).digest('hex'),
    startedUtc: new Date().toISOString(),
    requestedSeconds: seconds,
    load,
    // Not a native clock: monotonic milliseconds since this collector started.
    // Never offered to the judge as nativeNowMs.
    clock: 'process.hrtime.bigint monotonic ms since collector start',
    platform: `${process.platform}-${process.arch}`,
  },
  // DECLARED, not implied. The judge reasons about worker staleness against
  // progressAgeMs, and a reader has to know how coarsely we looked.
  sampleIntervalMs: intervalMs,
  expectedWorkers,
  recordingExpected: !!values.recording,
  policy,
  errors: [],
  samples: [],
};

const env = { ...process.env };
if (load > 0) Object.assign(env, {
  COREVIDEO_ZOOM_ENGINE_PATH: fakeEngine,
  COREVIDEO_FAKE_ENGINE_PARTICIPANTS: String(load),
  COREVIDEO_FAKE_ENGINE_RES: process.env.COREVIDEO_FAKE_ENGINE_RES ?? '2',
  COREVIDEO_FAKE_ENGINE_FPS: process.env.COREVIDEO_FAKE_ENGINE_FPS ?? '60',
  COREVIDEO_FAKE_NO_CHURN: '1',
});

const child = spawn(core, [], { cwd: dirname(core), windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'], env });
const exited = once(child, 'exit');
let buffer = '', stderr = '', handshake, next = 0, childExit = null;
const pending = new Map();
child.stderr.on('data', chunk => { stderr = (stderr + chunk).slice(-400000); });
child.stdout.on('data', chunk => {
  buffer += chunk;
  let end;
  while ((end = buffer.indexOf('\n')) >= 0) {
    const line = buffer.slice(0, end); buffer = buffer.slice(end + 1);
    if (!line.trim()) continue;
    let message; try { message = JSON.parse(line); } catch { continue; }
    if (message.type === 'handshake') handshake = message;
    const item = pending.get(message.id);
    if (item) { pending.delete(message.id); clearTimeout(item.timer); item.resolve(message); }
  }
});
child.once('exit', code => {
  childExit = code;
  for (const item of pending.values()) { clearTimeout(item.timer); item.reject(new Error(`Core exited ${code}`)); }
  pending.clear();
});

function send(type, extra = {}, timeoutMs = 10000) {
  if (childExit !== null) return Promise.reject(new Error(`Core is not running (exit ${childExit})`));
  const id = `collect-${++next}`;
  return new Promise((resolveSend, rejectSend) => {
    const timer = setTimeout(() => { pending.delete(id); rejectSend(new Error(`${type} timed out after ${timeoutMs}ms`)); }, timeoutMs);
    pending.set(id, { resolve: resolveSend, reject: rejectSend, timer });
    child.stdin.write(JSON.stringify({ id, type, ...extra }) + '\n');
  });
}

try {
  for (let i = 0; !handshake && i < 200; i++) {
    if (childExit !== null) throw new Error(`Core exited ${childExit} before handshaking`);
    await sleep(50);
  }
  if (!handshake) throw new Error('Core is unreachable: no native handshake on stdout');
  capture.collector.profile = handshake.profile;
  // Process identity comes from the core's own per-process epoch, not a PID: a
  // PID is reusable and the judge treats identity change as a hard finding.
  const processGeneration = handshake.processEpoch;
  if (typeof processGeneration !== 'string' || !processGeneration)
    throw new Error('Core handshake carried no processEpoch; process identity cannot be established');
  capture.collector.processGeneration = processGeneration;

  if (load > 0) {
    await send('zoom-join', { payload: { meetingNumber: '1234567890', displayName: 'snapshot-collector' } }, 25000);
    await sleep(4000);
  }
  if (capture.recordingExpected) {
    const recordingFolder = join(REPO, 'artifacts', `runtime-snapshot-capture-${Date.now()}`);
    await mkdir(recordingFolder, { recursive: true });
    await send('media-core-sync', { elapsedMs: 0, commands: [
      { type: 'start-program-output', destinations: ['recording'], isoSourceIds: [], isoParticipantIds: [] },
      { type: 'set-recording-targets', targetFolder: recordingFolder, filenamePrefix: 'capture', format: 'mp4', quality: 'high', isoSourceIds: [], isoParticipantIds: [] },
      { type: 'start-recording-session', sessionId: 'runtime-snapshot-capture' },
    ] }, 20000);
    capture.collector.recordingFolder = recordingFolder;
  }

  // Fixed-schedule sampling from a monotonic anchor, so a slow reply shifts one
  // sample rather than accumulating drift into the declared interval.
  const firstAt = monotonicMs();
  const total = Math.max(1, Math.floor((seconds * 1000) / intervalMs));
  for (let i = 0; i < total; i++) {
    const dueAt = firstAt + i * intervalMs;
    const waitMs = dueAt - monotonicMs();
    if (waitMs > 0) await sleep(waitMs);
    const collectedAtMs = monotonicMs();
    const reply = await send('snapshot', {}, 10000);
    if (!reply.ok || !reply.snapshot) throw new Error(`Core refused a snapshot request: ${JSON.stringify(reply).slice(0, 200)}`);
    capture.samples.push({ processGeneration, collectedAtMs, snapshot: reply.snapshot });
  }
  capture.collector.finishedUtc = new Date().toISOString();
  capture.collector.observedSeconds = (monotonicMs() - firstAt) / 1000;
} catch (error) {
  capture.errors.push(error.message);
  process.exitCode = 1;
} finally {
  // Evidence is written even when the run ended badly — that run is exactly the
  // one worth reading.
  if (capture.recordingExpected && capture.samples.length && childExit === null)
    await send('media-core-sync', { elapsedMs: 0, commands: [{ type: 'stop-recording-session', reason: 'snapshot capture complete' }] }, 20000).catch(() => {});
  try { child.stdin.end(); } catch { /* already gone */ }
  await Promise.race([exited, sleep(5000)]);
  if (child.exitCode === null) { child.kill(); await Promise.race([exited, sleep(2000)]); }
  capture.collector.coreExitCode = child.exitCode;
  if (!capture.samples.length) {
    // LOUD. An envelope with no samples is not evidence, and it must never be
    // mistaken for a clean run: the judge would call it unverified, which reads
    // the same as a healthy short capture unless we say otherwise here.
    capture.collectorFailed = true;
    capture.errors.push('No sessionState samples were captured; this envelope establishes nothing');
    process.exitCode = 2;
  }
  await mkdir(dirname(outPath), { recursive: true });
  await writeFile(outPath, JSON.stringify(capture, null, 2) + '\n');
  await writeFile(outPath.replace(/\.json$/, '') + '-core-stderr.log', stderr);
  process.stderr.write(`${capture.collectorFailed ? 'COLLECTOR FAILED' : 'collected'} ${capture.samples.length} sample(s) at ${intervalMs}ms -> ${outPath}\n`);
  for (const message of capture.errors) process.stderr.write(`  error: ${message}\n`);
}
