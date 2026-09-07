// Operates an already-running WinUI shell in an explicitly authorized test meeting.
// Does not launch a process/window. API selection exercises the command/VM path,
// not a physical pointer click or proof of SwapChainPanel pixels reaching display.
import fs from 'node:fs';
import path from 'node:path';
import { parseArgs } from 'node:util';

const { values } = parseArgs({ options: {
  base: { type: 'string' }, 'tiles-scene': { type: 'string' }, 'other-scene': { type: 'string' },
  output: { type: 'string' }, 'duration-seconds': { type: 'string', default: '3600' },
  'cancel-file': { type: 'string' },
} });
for (const name of ['base', 'tiles-scene', 'other-scene', 'output'])
  if (!values[name]) throw new Error(`Required --${name}`);
const base = new URL(values.base);
if (!['http:', 'https:'].includes(base.protocol) || base.username || base.password || base.search || base.hash)
  throw new Error('Base must be HTTP(S), without credentials/query/fragment');
const duration = Number(values['duration-seconds']);
if (!Number.isFinite(duration) || duration < 1 || duration > 86400) throw new Error('Duration must be 1..86400 seconds');
if (values['tiles-scene'] === values['other-scene']) throw new Error('Distinct scene IDs required');
const output = path.resolve(values.output);
fs.mkdirSync(output, { recursive: true });
const headers = { 'content-type': 'application/json' };
if (process.env.COREVIDEO_TEST_API_TOKEN) headers.authorization = `Bearer ${process.env.COREVIDEO_TEST_API_TOKEN}`;
const report = { running: true, functionalPassed: false, performanceAccepted: false,
  scope: 'API/VM and native render evidence only; internal frame rate is not 60Hz display/output proof',
  startTime: new Date().toISOString(), durationSeconds: duration, health: [], selections: [],
  maxHttpMs: 0, restored: false, internalCountersClean: true };
const counterNames = ['produced', 'delivered', 'underruns', 'overflows', 'gpuNotReady', 'deadlineMisses', 'outputSequenceGaps', 'displayUnconsumed', 'displayBusy'];
const missNames = counterNames.slice(2);
let initial, previous, bufferInitial, firstFrame, firstSampleAt, mutated = false, cancelled = false;
process.on('SIGINT', () => { cancelled = true; });
process.on('SIGTERM', () => { cancelled = true; });
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const check = (ok, message) => { if (!ok) throw new Error(message); };
function cancellation() {
  check(!cancelled && !(values['cancel-file'] && fs.existsSync(values['cancel-file'])), 'Cancelled');
}
function write(name) {
  const target = path.join(output, name);
  fs.writeFileSync(target + '.tmp', JSON.stringify(report, null, 2));
  fs.renameSync(target + '.tmp', target);
}
async function request(endpoint, body) {
  const started = performance.now();
  try {
    const response = await fetch(new URL(endpoint, base), { method: body ? 'POST' : 'GET', headers,
      body: body ? JSON.stringify(body) : undefined, signal: AbortSignal.timeout(3000) });
    check(response.ok, `HTTP ${response.status} at ${endpoint}`);
    const data = await response.json();
    if (body) check(data.ok === true, `Invocation rejected: ${body.action}`);
    return data;
  } catch (error) {
    if (/^(HTTP |Invocation rejected:)/.test(error.message)) throw error;
    throw new Error(`Request failed or timed out at ${endpoint}`);
  } finally { report.maxHttpMs = Math.max(report.maxHttpMs, Math.round(performance.now() - started)); }
}
const state = () => request('/state');
const invoke = (action, ...args) => request('/invoke', { action, args });
function safe(s, allowAutoTake = false) {
  check(s.engineOn === true && s.zoomStatus === 'Zoom Live', 'Meeting/capture lost');
  check(s.automationOn === false && (allowAutoTake || s.autoTake === false), 'Automation must be off');
  check(s.recording === false && s.streaming === false, 'Recording/streaming must be off');
  check(Number.isSafeInteger(s.nativeProgramFrameCount), 'Native frame evidence absent');
}
function matches(s, scene, frame) {
  return s.activeSceneId === scene && s.nativeActiveSceneId === scene && s.nativeRenderedSceneId === scene &&
    typeof s.nativeRenderPlanId === 'string' && s.nativeRenderPlanId.startsWith(scene + ':') && s.nativeProgramFrameCount > frame;
}
async function converge(predicate, label, restoring = false) {
  const deadline = performance.now() + 8000;
  do {
    if (!restoring) cancellation();
    const s = await state(); safe(s);
    if (predicate(s)) return s;
    await sleep(100);
  } while (performance.now() < deadline);
  throw new Error(`Timed out: ${label}`);
}
function health(s) {
  safe(s);
  const tileSources = (s.nativeProgramVideoSources ?? []).filter(source => source.layerId?.startsWith('tile:'));
  if (s.activeSceneId === values['tiles-scene']) check(tileSources.length > 0, 'Tiles has no admitted live video sources');
  const now = performance.now(), b = s.nativeProgramBuffer;
  check(b && Number.isSafeInteger(b.generation) && b.activeFrames > 0, 'Active Program buffer diagnostics required');
  for (const name of counterNames) check(Number.isSafeInteger(b[name]) && b[name] >= 0, `Missing counter ${name}`);
  if (previous) {
    check(s.nativeProgramFrameCount > previous.frame, 'Renderer frame freeze/reset');
    check(b.generation === previous.generation, 'Program buffer generation changed');
    for (const name of counterNames) check(b[name] >= previous.buffer[name], `Counter reset: ${name}`);
    check(b.delivered > previous.buffer.delivered, 'Program delivery froze');
  } else { bufferInitial = { ...b }; firstFrame = s.nativeProgramFrameCount; firstSampleAt = now; }
  const counters = Object.fromEntries(counterNames.map(name => [name, b[name]]));
  const misses = Object.fromEntries(missNames.map(name => [name, b[name] - bufferInitial[name]]));
  if (Object.values(misses).some(value => value > 0)) report.internalCountersClean = false;
  report.health.push({ elapsedMs: Math.round(now - firstSampleAt), frame: s.nativeProgramFrameCount,
    programSceneId: s.activeSceneId, tileSourceIds: tileSources.map(source => source.sourceId),
    generation: b.generation, activeFrames: b.activeFrames, occupancy: b.occupancy,
    zoomLive: s.zoomStatus === 'Zoom Live', engineOn: s.engineOn, recording: s.recording, streaming: s.streaming,
    counters, missesSinceStart: misses,
    intervalInternalFps: previous ? (s.nativeProgramFrameCount - previous.frame) * 1000 / (now - previous.at) : null });
  report.internalWallClockFps = now > firstSampleAt ? (s.nativeProgramFrameCount - firstFrame) * 1000 / (now - firstSampleAt) : null;
  previous = { frame: s.nativeProgramFrameCount, generation: b.generation, buffer: counters, at: now };
}
const modes = { Program: 'program', Preview: 'preview', ProgramPreview: 'program-preview', Multiview: 'multiview' };
try {
  initial = await state(); safe(initial, true);
  check(initial.activeSceneId && initial.previewSceneId && modes[initial.viewMode] && typeof initial.autoTake === 'boolean', 'Initial restoration state missing');
  mutated = true;
  if (initial.autoTake) await invoke('automation.autoTake.set', false);
  await invoke('view.setMode', 'program-preview');
  const deadline = performance.now() + duration * 1000;
  let nextSelection = performance.now(), selectionIndex = 0;
  while (performance.now() < deadline) {
    cancellation();
    health(await state());
    if (performance.now() >= nextSelection) {
      const before = await state(); safe(before);
      const target = [values['tiles-scene'], values['other-scene']][selectionIndex++ % 2];
      await invoke('scene.select', target);
      const cued = await converge(s => s.previewSceneId === target && s.nativePreviewSceneId === target &&
        s.activeSceneId === before.activeSceneId && s.nativeActiveSceneId === before.nativeActiveSceneId &&
        s.nativeProgramFrameCount > before.nativeProgramFrameCount, 'cue');
      if (target !== before.activeSceneId) await invoke('transport.take');
      const shown = await converge(s => matches(s, target, cued.nativeProgramFrameCount), 'rendered Take');
      report.selections.push({ elapsedMs: Math.round(performance.now() - firstSampleAt), sceneId: target,
        frame: shown.nativeProgramFrameCount, renderPlanId: shown.nativeRenderPlanId });
      nextSelection += 60000;
    }
    write('progress.json');
    await sleep(Math.min(5000, Math.max(0, deadline - performance.now())));
  }
  cancellation();
  if (!previous || performance.now() - previous.at >= 500) health(await state());
  report.functionalPassed = true;
} catch (error) { report.error = error.message; }
finally {
  if (initial && mutated) {
    try {
      const current = await state(); safe(current);
      if (current.activeSceneId !== initial.activeSceneId) {
        await invoke('scene.select', initial.activeSceneId);
        await converge(s => s.previewSceneId === initial.activeSceneId && s.nativePreviewSceneId === initial.activeSceneId, 'restore cue', true);
        await invoke('transport.take');
      }
      await invoke('scene.select', initial.previewSceneId);
      await converge(s => matches(s, initial.activeSceneId, current.nativeProgramFrameCount) &&
        s.previewSceneId === initial.previewSceneId && s.nativePreviewSceneId === initial.previewSceneId, 'restore native buses', true);
      report.restored = true;
    } catch (error) { report.restoreError = error.message; }
    try {
      await invoke('view.setMode', modes[initial.viewMode]);
      check((await state()).viewMode === initial.viewMode, 'View restore not observed');
    } catch (error) { report.viewRestoreError = error.message; report.restored = false; }
    try {
      await invoke('automation.autoTake.set', initial.autoTake);
      check((await state()).autoTake === initial.autoTake, 'Auto-Take restore not observed');
    } catch (error) { report.autoTakeRestoreError = error.message; report.restored = false; }
  }
  report.running = false; report.endTime = new Date().toISOString();
  if (!report.restored) report.functionalPassed = false;
  write('progress.json'); write('tiles-live-soak-results.json');
  console.log(JSON.stringify({ functionalPassed: report.functionalPassed, restored: report.restored,
    healthSamples: report.health.length, selections: report.selections.length, internalCountersClean: report.internalCountersClean,
    performanceAccepted: false, error: report.error, restoreError: report.restoreError }));
  if (!report.functionalPassed) process.exitCode = 1;
}
