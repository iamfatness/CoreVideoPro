import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { performance } from 'node:perf_hooks';

const counters = ['produced', 'delivered', 'underruns', 'overflows', 'gpuNotReady', 'deadlineMisses', 'outputSequenceGaps', 'displayBusy', 'displayUnconsumed'];
const numeric = value => Number.isSafeInteger(value) && value >= 0;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
let stopRequested = false;

export function summarizeBufferInterval(baseline, end, frames, samples = []) {
  const errors = [], delta = {};
  const expectedGeneration = baseline?.generation;
  if (!numeric(expectedGeneration) || expectedGeneration === 0) errors.push('Missing valid baseline buffer generation.');
  let previous = null;
  for (const [index, buffer] of [baseline, ...samples.map(sample => sample?.programBuffer), end].entries()) {
    const label = index === 0 ? 'baseline' : index === samples.length + 1 ? 'end' : `sample ${index}`;
    for (const key of counters) {
      if (!numeric(buffer?.[key])) errors.push(`${label}: missing valid ${key} evidence.`);
      else if (previous && numeric(previous[key]) && buffer[key] < previous[key])
        errors.push(`${label}: ${key} reset within the measurement interval.`);
    }
    previous = buffer;
    if (buffer?.activeFrames !== frames) errors.push(`${label}: requested buffer depth was not active.`);
    if (buffer?.status !== 'running') errors.push(`${label}: buffer status was not running.`);
    if (!numeric(buffer?.generation) || buffer.generation === 0 || buffer.generation !== expectedGeneration)
      errors.push(`${label}: buffer generation is missing or changed within measurement.`);
  }
  for (const key of counters) {
    if (!numeric(baseline?.[key]) || !numeric(end?.[key])) {
      delta[key] = null;
      continue;
    }
    delta[key] = end[key] - baseline[key];
    if (delta[key] < 0) errors.push(`${key} reset within the measurement interval.`);
  }
  if (baseline?.activeFrames !== frames || end?.activeFrames !== frames) errors.push('Requested buffer depth was not active throughout measurement.');
  if (!numeric(delta.delivered) || delta.delivered === 0) errors.push('No verified internal delivery progress.');
  for (const key of ['underruns', 'overflows', 'gpuNotReady', 'deadlineMisses', 'outputSequenceGaps']) {
    if (delta[key] === null) errors.push(`Missing ${key} evidence.`);
    else if (delta[key] > 0) errors.push(`${key} increased by ${delta[key]}.`);
  }
  // This checks sustained liveness, not a frame-rate or presentation guarantee.
  // Short final samples cannot replace evidence from a full sampling window.
  let previousElapsed = 0, windowElapsed = 0, windowDelivered = baseline?.delivered, windows = 0;
  for (const [index, sample] of samples.entries()) {
    if (!numeric(sample?.elapsedMs) || sample.elapsedMs <= previousElapsed) {
      errors.push(`sample ${index + 1}: elapsed time is missing or not increasing.`);
      continue;
    }
    previousElapsed = sample.elapsedMs;
    if (sample.elapsedMs - windowElapsed >= 500) {
      ++windows;
      if (!numeric(sample.programBuffer?.delivered) || !numeric(windowDelivered) || sample.programBuffer.delivered <= windowDelivered)
        errors.push(`sample ${index + 1}: no internal delivery progress in a full sampled window.`);
      windowElapsed = sample.elapsedMs;
      windowDelivered = sample.programBuffer?.delivered;
    }
  }
  if (windows === 0) errors.push('Missing full sampled window for ongoing delivery progress.');
  return { delta, errors };
}

async function runDepth(nativeCore, frames, durationMs) {
  const result = { frames, scope: 'synthetic/internal-only', enabledOutputs: [], samples: [], errors: [], stderrTail: '', framePerformancePassed: false };
  const child = spawn(nativeCore, [], {
    cwd: dirname(nativeCore), windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'],
    env: { ...process.env, COREVIDEO_PROGRAM_BUFFER_FRAMES: String(frames) }
  });
  result.pid = child.pid;
  const pending = new Map();
  let stdout = '', nextId = 0, failure = null, exited = false;
  const fail = error => {
    failure ??= error;
    for (const entry of pending.values()) { clearTimeout(entry.timer); entry.reject(error); }
    pending.clear();
  };
  const exit = new Promise(resolveExit => child.once('close', (code, signal) => {
    exited = true;
    result.exitCode = code; result.exitSignal = signal;
    fail(new Error(`Native child closed (${code ?? signal}).`)); resolveExit();
  }));
  child.on('error', fail);
  child.stdin.on('error', fail);
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stderr.on('data', text => { result.stderrTail = (result.stderrTail + text).slice(-65536); });
  child.stdout.on('data', chunk => {
    stdout += chunk;
    let newline;
    while ((newline = stdout.indexOf('\n')) >= 0) {
      const line = stdout.slice(0, newline).trim(); stdout = stdout.slice(newline + 1);
      if (!line) continue;
      if (line.length > 1024 * 1024) { fail(new Error('RPC line exceeds 1 MiB bound.')); child.kill(); return; }
      let message;
      try { message = JSON.parse(line); } catch { fail(new Error('Invalid JSON on native stdout.')); continue; }
      if (message.type === 'handshake') result.profile = message.profile;
      const entry = pending.get(String(message.id));
      if (!entry) continue;
      clearTimeout(entry.timer); pending.delete(String(message.id));
      if (message.ok !== true) entry.reject(new Error(`RPC rejected: ${JSON.stringify(message.error ?? message).slice(0, 1000)}`));
      else entry.resolve(message);
    }
    if (stdout.length > 1024 * 1024) { fail(new Error('Unterminated RPC line exceeds 1 MiB bound.')); child.kill(); }
  });
  const request = (type, fields = {}) => new Promise((resolveRequest, reject) => {
    if (failure) { reject(failure); return; }
    const id = `buffer-smoke-${++nextId}`;
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`${type} timed out after 5 seconds.`)); }, 5000);
    pending.set(id, { resolve: resolveRequest, reject, timer });
    child.stdin.write(JSON.stringify({ id, type, ...fields }) + '\n');
  });
  const interrupted = () => { stopRequested = true; fail(new Error('Smoke interrupted.')); child.kill(); };
  process.once('SIGINT', interrupted); process.once('SIGTERM', interrupted);
  try {
    await request('handshake', { protocolVersion: { major: 1, minor: 0 } });
    await request('media-core-sync', { commands: [
      { type: 'load-scene-graph', sceneId: `synthetic-buffer-${frames}`, routes: [] },
      { type: 'start-program-output', destinations: [], destinationSettings: [] }
    ] });
    const primeDeadline = performance.now() + 15000;
    let snapshot;
    while (performance.now() < primeDeadline) {
      snapshot = (await request('snapshot')).snapshot;
      const buffer = snapshot?.programBuffer;
      result.primingBuffer = buffer ?? null;
      if (buffer?.status === 'unsupported' || buffer?.status === 'failed') throw new Error(`Program buffer is ${buffer.status}.`);
      if (buffer?.activeFrames === frames && numeric(buffer.delivered) && buffer.delivered >= frames + 5) break;
      await sleep(250);
    }
    if (snapshot?.programBuffer?.activeFrames !== frames || !(snapshot?.programBuffer?.delivered >= frames + 5))
      throw new Error('Buffer failed to prime with the requested depth within 15 seconds.');
    result.baseline = snapshot.programBuffer;
    result.startTime = new Date().toISOString();
    const start = performance.now();
    do {
      await sleep(Math.min(500, Math.max(1, durationMs - (performance.now() - start))));
      snapshot = (await request('snapshot')).snapshot;
      result.samples.push({ elapsedMs: Math.round(performance.now() - start), programBuffer: snapshot?.programBuffer ?? null });
    } while (performance.now() - start < durationMs && result.samples.length < 64);
    result.endTime = new Date().toISOString();
    result.measuredDurationMs = Math.round(performance.now() - start);
    result.end = snapshot?.programBuffer;
    const summary = summarizeBufferInterval(result.baseline, result.end, frames, result.samples);
    result.delta = summary.delta; result.errors.push(...summary.errors);
    if (result.measuredDurationMs < durationMs) result.errors.push('Incomplete measurement duration.');
  } catch (error) {
    result.errors.push(error.message);
  } finally {
    child.stdin.end();
    if (!exited) await Promise.race([exit, sleep(3000)]);
    if (!exited) { result.forcedChildCleanup = true; result.errors.push('Owned child required forced shutdown.'); child.kill(); await Promise.race([exit, sleep(1000)]); }
    if (!exited) result.errors.push('Owned child did not confirm shutdown.');
    else if (result.exitCode !== 0) result.errors.push(`Owned child exited abnormally (${result.exitCode ?? result.exitSignal}).`);
    process.removeListener('SIGINT', interrupted); process.removeListener('SIGTERM', interrupted);
  }
  result.internalSmokePassed = result.errors.length === 0;
  return result;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  try {
    const args = process.argv.slice(2), options = {};
    for (let i = 0; i < args.length; i += 2) {
      if (!['--native-core', '--output', '--duration-ms'].includes(args[i]) || !args[i + 1]) throw new Error('Usage: node scripts/qa/program-buffer-smoke.mjs --native-core EXE --output REPORT.json [--duration-ms 10000]');
      options[args[i]] = args[i + 1];
    }
    if (!options['--native-core'] || !options['--output']) throw new Error('Explicit --native-core and --output are required.');
    const nativeCore = resolve(options['--native-core']), output = resolve(options['--output']);
    const durationMs = Number(options['--duration-ms'] ?? 10000);
    if (!Number.isSafeInteger(durationMs) || durationMs < 1000 || durationMs > 30000) throw new Error('Duration must be 1000..30000 milliseconds per depth.');
    if (!existsSync(nativeCore)) throw new Error('Native executable does not exist.');
    if (existsSync(output)) throw new Error('Refusing to overwrite an existing report.');
    const report = { schemaVersion: 1, scope: 'synthetic/internal-only', nativeCore, durationMs,
      framePerformancePassed: false, limitation: 'No physical presentation or enabled external destination is measured. Internal smoke success cannot establish frame-performance acceptance.', runs: [] };
    for (const frames of [2, 3]) {
      if (stopRequested) break;
      report.runs.push(await runDepth(nativeCore, frames, durationMs));
    }
    report.internalSmokePassed = report.runs.length === 2 && report.runs.every(run => run.internalSmokePassed);
    mkdirSync(dirname(output), { recursive: true });
    writeFileSync(output, JSON.stringify(report, null, 2) + '\n');
    process.stdout.write(JSON.stringify({ report: output, internalSmokePassed: report.internalSmokePassed, framePerformancePassed: false }) + '\n');
    process.exitCode = report.internalSmokePassed ? 0 : 1;
  } catch (error) {
    process.stderr.write(error.message + '\n'); process.exitCode = 1;
  }
}
