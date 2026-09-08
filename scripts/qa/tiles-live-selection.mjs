// Operates an already-running WinUI shell in an explicitly authorized test meeting.
// Does not launch a process/window. API selection exercises the command/VM path,
// not a physical pointer click or proof of SwapChainPanel pixels reaching display.
import fs from 'node:fs';
import path from 'node:path';
import { parseArgs } from 'node:util';

const { values } = parseArgs({ options: {
  base: { type: 'string' }, 'tiles-scene': { type: 'string' },
  'other-scene': { type: 'string' }, output: { type: 'string' },
  cycles: { type: 'string', default: '10' },
} });
for (const flag of ['base', 'tiles-scene', 'other-scene', 'output']) {
  if (!values[flag]) throw new Error(`Required --${flag}`);
}
const base = new URL(values.base);
if (!['http:', 'https:'].includes(base.protocol) || base.username || base.password || base.search || base.hash)
  throw new Error('Base must be an HTTP(S) URL without credentials/query/fragment');
const cycles = Number(values.cycles);
if (!Number.isInteger(cycles) || cycles < 1 || cycles > 100) throw new Error('Cycles must be 1..100');
if (values['tiles-scene'] === values['other-scene']) throw new Error('Two distinct scene IDs required');
const output = path.resolve(values.output);
fs.mkdirSync(output, { recursive: true });
const headers = { 'content-type': 'application/json' };
if (process.env.COREVIDEO_TEST_API_TOKEN) headers.authorization = `Bearer ${process.env.COREVIDEO_TEST_API_TOKEN}`;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const report = { ok: false, scope: 'WinUI API selection and native rendered Program evidence; not displayed-pixel proof',
  startTime: new Date().toISOString(), rows: [], maxHttpMs: 0, restored: false };
let initial;
let mutated = false;
let cancelled = false;
process.on('SIGINT', () => { cancelled = true; });
process.on('SIGTERM', () => { cancelled = true; });
function check(value, message) { if (!value) throw new Error(message); }
async function request(endpoint, body) {
  const started = performance.now();
  let response;
  try {
    response = await fetch(new URL(endpoint, base), { method: body ? 'POST' : 'GET', headers,
      body: body ? JSON.stringify(body) : undefined, signal: AbortSignal.timeout(3000) });
    const json = await response.json();
    check(response.ok, `HTTP ${response.status} at ${endpoint}`);
    if (body) check(json.ok === true, `Invocation rejected: ${body.action}`);
    return json;
  } catch (error) {
    // Do not write server error text/full state, credentials or URLs to artifacts.
    if (error.message.startsWith('HTTP ') || error.message.startsWith('Invocation rejected:')) throw error;
    throw new Error(`Request failed or timed out at ${endpoint}`);
  } finally { report.maxHttpMs = Math.max(report.maxHttpMs, Math.round(performance.now() - started)); }
}
const state = () => request('/state');
const invoke = (action, ...args) => request('/invoke', { action, args });
function safe(s) {
  check(s.engineOn === true && s.zoomStatus === 'Zoom Live', 'Requires live Zoom capture');
  check(s.automationOn === false && s.autoTake === false, 'Requires automation and auto-Take off');
  check(s.recording === false && s.streaming === false, 'Requires recording and streaming off');
  check(Number.isSafeInteger(s.nativeProgramFrameCount), 'Missing native frame evidence');
}
async function awaitState(predicate, label, restoring = false) {
  const deadline = performance.now() + 8000;
  do {
    if (!restoring) check(!cancelled, 'Cancelled');
    const current = await state();
    safe(current);
    if (predicate(current)) return current;
    await sleep(100);
  } while (performance.now() < deadline);
  throw new Error(`Timed out: ${label}`);
}
function rendered(s, scene, previousFrame) {
  return s.activeSceneId === scene && s.nativeActiveSceneId === scene &&
    s.nativeRenderedSceneId === scene && typeof s.nativeRenderPlanId === 'string' &&
    s.nativeRenderPlanId.startsWith(scene + ':') && s.nativeProgramFrameCount > previousFrame;
}
const viewModes = { Program: 'program', Preview: 'preview', ProgramPreview: 'program-preview', Multiview: 'multiview' };
try {
  initial = await state();
  safe(initial);
  check(initial.activeSceneId && initial.previewSceneId && viewModes[initial.viewMode], 'Initial buses/view unavailable');
  check(typeof initial.nativeRenderedSceneId === 'string', 'Missing native rendered-scene evidence');
  mutated = true;
  await invoke('view.setMode', 'program-preview');
  for (let cycle = 0; cycle < cycles; cycle++) {
    for (const target of [values['tiles-scene'], values['other-scene']]) {
      check(!cancelled, 'Cancelled');
      const before = await state(); safe(before);
      const started = performance.now();
      await invoke('scene.select', target);
      const cued = await awaitState(s => s.previewSceneId === target && s.nativePreviewSceneId === target &&
        s.activeSceneId === before.activeSceneId && s.nativeActiveSceneId === before.nativeActiveSceneId &&
        s.nativeProgramFrameCount > before.nativeProgramFrameCount, 'cue/Program continuity');
      if (target !== before.activeSceneId) await invoke('transport.take');
      const observed = await awaitState(s => rendered(s, target, cued.nativeProgramFrameCount), 'Take rendered ownership');
      report.rows.push({ cycle: cycle + 1, sceneId: target, frame: observed.nativeProgramFrameCount,
        renderPlanId: observed.nativeRenderPlanId, sourceIds: (observed.nativeProgramVideoSources ?? []).map(s => s.sourceId),
        elapsedMs: Math.round(performance.now() - started) });
    }
  }
  report.ok = true;
} catch (error) { report.error = error.message; }
finally {
  if (initial && mutated) {
    // Restoration never loops indefinitely. A wedged UI can prevent restoration;
    // that is a reported failure, never an assumption of successful cleanup.
    try {
      const current = await state(); safe(current);
      if (current.activeSceneId !== initial.activeSceneId) {
        await invoke('scene.select', initial.activeSceneId);
        await awaitState(s => s.previewSceneId === initial.activeSceneId && s.nativePreviewSceneId === initial.activeSceneId,
          'restore Program cue', true);
        await invoke('transport.take');
        await awaitState(s => rendered(s, initial.activeSceneId, current.nativeProgramFrameCount), 'restore Program', true);
      }
      await invoke('scene.select', initial.previewSceneId);
      await awaitState(s => rendered(s, initial.activeSceneId, current.nativeProgramFrameCount) &&
        s.previewSceneId === initial.previewSceneId && s.nativePreviewSceneId === initial.previewSceneId,
        'restore native Program and Preview', true);
      report.restored = true;
    } catch (error) { report.restoreError = error.message; report.ok = false; }
    try {
      await invoke('view.setMode', viewModes[initial.viewMode]);
      check((await state()).viewMode === initial.viewMode, 'View restoration not observed');
    } catch (error) { report.viewRestoreError = error.message; report.restored = false; report.ok = false; }
  }
  report.endTime = new Date().toISOString();
  fs.writeFileSync(path.join(output, 'tiles-live-selection-results.json'), JSON.stringify(report, null, 2));
  console.log(JSON.stringify({ ok: report.ok, selections: report.rows.length, restored: report.restored,
    maxHttpMs: report.maxHttpMs, error: report.error, restoreError: report.restoreError, viewRestoreError: report.viewRestoreError }));
  if (!report.ok) process.exitCode = 1;
}
