// Offline qualification: never launches processes, joins meetings, or changes app state.
import { readFileSync, statSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';
import { assessRuntimeSnapshots } from './runtime-snapshot-qualification.mjs';

export const VERSION = 'production-qualification-v1';
const natural = x => Number.isSafeInteger(x) && x >= 0;
const text = x => typeof x === 'string' && x.trim().length > 0;
const ns = x => typeof x === 'string' && /^\d{1,24}$/.test(x) ? BigInt(x) : null;
const hash = x => typeof x === 'string' && /^[a-f0-9]{64}$/i.test(x);
const ceil = (a, b) => (a + b - 1n) / b;
const failures = ['underruns', 'overflows', 'deadlineMisses', 'outputSequenceGaps', 'gpuNotReady', 'audioLostSamples', 'audioReanchors'];

export function qualify(input) {
  const issues = [], missing = [], stages = {};
  const fail = message => issues.push(message), unknown = message => missing.push(message);
  const e = input && typeof input === 'object' ? input : {};
  if (e.version !== VERSION) unknown('Unsupported/missing evidence version');
  if (!['measured', 'synthetic'].includes(e.mode)) unknown('Evidence origin missing');
  for (const field of ['os', 'cpu', 'gpu', 'gpuDriver']) if (!text(e.hardware?.[field])) unknown(`hardware.${field}`);
  for (const field of ['shell', 'native', 'zoom']) if (!hash(e.binarySha256?.[field])) unknown(`binarySha256.${field}`);
  if (!text(e.processGeneration)) unknown('processGeneration');
  if (e.workload?.width !== 1920 || e.workload?.height !== 1080 || e.workload?.fps !== 60 || ![2, 3].includes(e.workload?.bufferFrames)) fail('Requires 1920x1080 at 60fps and buffer depth 2 or 3');
  for (const field of ['sources', 'outputs', 'operations']) if (!Array.isArray(e.workload?.[field])) unknown(`Explicit workload.${field} inventory`);
  const targets = e.workload?.outputs;
  if (Array.isArray(targets) && (targets.some(x => !text(x?.id) || !['recording', 'transport'].includes(x.kind)) || new Set(targets.map(x => x.id)).size !== targets.length)) fail('Invalid/duplicate output inventory');
  const start = ns(e.interval?.anchorNs), count = e.interval?.slots;
  const valid = start !== null && natural(count) && count > 0 && count <= 60 * 3600 * 24 && [2, 3].includes(e.workload?.bufferFrames);
  if (!valid) unknown('Bounded measurement anchor/slot count');
  if (!Number.isFinite(e.interval?.durationSeconds) || (valid && e.interval.durationSeconds !== count / 60)) fail('Requested duration must equal the independently declared slot interval');
  if (e.mode === 'measured' && !(e.interval?.durationSeconds >= 3600)) unknown('Full-hour production qualification duration');
  if (!text(e.interval?.startedUtc) || !Number.isFinite(Date.parse(e.interval.startedUtc))) unknown('Measurement UTC start');
  if (!natural(e.appliedRevision) || e.requestedRevision !== e.appliedRevision) unknown('Requested/applied revision convergence');
  stages.requested = natural(e.requestedRevision) ? 'available' : 'unverified';
  stages.applied = natural(e.appliedRevision) && e.requestedRevision === e.appliedRevision ? 'available' : 'unverified';
  if (!Array.isArray(e.errors)) unknown('Explicit error coverage');
  else if (e.errors.length) fail('Runtime/collector errors recorded');
  if (e.coverage?.complete !== true || e.coverage?.processGeneration !== e.processGeneration) unknown('Whole interval coverage for the declared process generation');
  for (const field of failures) {
    const before = e.counters?.before?.[field], after = e.counters?.after?.[field];
    if (!natural(before) || !natural(after)) unknown(`Counter coverage: ${field}`);
    else if (after < before) fail(`Counter reset: ${field}`);
    else if (after > before) fail(`Production loss: ${field} +${after - before}`);
  }
  const records = new Map();
  for (const stage of ['rendered', 'delivered', 'presented']) {
    const path = e[stage];
    stages[stage] = 'unverified';
    if (path?.measurement !== (stage === 'presented' ? 'display-completion' : 'gpu-completion') || !Array.isArray(path.frames)) {
      unknown(`${stage}: actual completion evidence required, not a submitted counter`); continue;
    }
    if (!valid) continue;
    if (path.frames.length !== count) { fail(`${stage}: expected ${count} unique frames, found ${path.frames.length}`); continue; }
    let prior = null;
    const bySlot = new Map();
    path.frames.forEach((frame, index) => {
      if (!frame || typeof frame !== 'object') { fail(`${stage}: malformed frame at ${index}`); return; }
      const done = ns(frame.completedNs);
      const deadline = start + ceil(BigInt(index + e.workload.bufferFrames) * 1000000000n, 60n);
      if (frame.slot !== index || bySlot.has(frame.slot)) fail(`${stage}: reordered/duplicate/missing slot at ${index}`);
      if (!text(frame.frameId) || frame.processGeneration !== e.processGeneration || frame.revision !== e.appliedRevision) fail(`${stage}: identity/generation/revision mismatch at ${index}`);
      if (done === null) unknown(`${stage}: completion timestamp at ${index}`);
      else {
        if (done < start || done > deadline) fail(`${stage}: deadline violation at ${index}`);
        const slotStart = start + ceil(BigInt(index + e.workload.bufferFrames - 1) * 1000000000n, 60n);
        if (stage === 'presented' && done <= slotStart) fail(`${stage}: completion before scheduled slot window at ${index}`);
        if (prior !== null && (done <= prior || (stage !== 'rendered' && done - prior > 16666667n))) fail(`${stage}: completion cadence violation at ${index}`);
        prior = done;
      }
      if (stage !== 'rendered') {
        const previous = records.get(stage === 'delivered' ? 'rendered' : 'delivered')?.get(index);
        if (!previous || frame.frameId !== previous.frameId || (done !== null && ns(previous.completedNs) !== null && done < ns(previous.completedNs))) fail(`${stage}: does not match upstream completed frame at ${index}`);
      }
      bySlot.set(index, frame);
    });
    if (new Set(path.frames.map(f => f?.frameId)).size !== count) fail(`${stage}: repeated frame identities`);
    records.set(stage, bySlot); stages[stage] = 'available';
  }
  for (const target of Array.isArray(targets) ? targets : []) {
    const output = e.destinations?.[target.id];
    for (const stage of target.kind === 'recording' ? ['muxed', 'committed', 'completed'] : ['accepted']) stages[`${target.id}.${stage}`] = 'unverified';
    if (!output || output.processGeneration !== e.processGeneration || !text(output.generation)) { unknown(`${target.id}: destination generation evidence`); continue; }
    if (target.kind !== 'recording') { unknown(`${target.id}: transport completion adapter not implemented in Wave 0`); continue; }
    for (const stage of ['muxed', 'committed']) {
      const range = output[stage];
      if (!valid || range?.firstSlot !== 0 || range?.lastSlot !== count - 1 || range?.uniqueFrames !== count || range?.gaps !== 0) unknown(`${target.id}: complete ${stage} range`);
      else stages[`${target.id}.${stage}`] = 'available';
    }
    if (output.completed?.state !== 'completed' || output.completed?.closed !== true || !hash(output.completed?.artifactSha256)) unknown(`${target.id}: closed and hashed final artifact`);
    else stages[`${target.id}.completed`] = 'available';
    const decode = output.decode;
    if (!decode || decode.artifactSha256 !== output.completed?.artifactSha256 || !text(decode.toolVersion) || decode.success !== true || !Array.isArray(decode.frames)) { unknown(`${target.id}: independent full decode of the same artifact`); continue; }
    if (!valid) continue;
    if (decode.width !== 1920 || decode.height !== 1080 || decode.frames.length !== count) fail(`${target.id}: decoded dimensions/frame count`);
    const scale = ns(decode.timeBaseDenominator), numerator = ns(decode.timeBaseNumerator);
    if (scale === null || scale === 0n || numerator === null || numerator === 0n) { unknown(`${target.id}: decode time base`); continue; }
    if (numerator * 60n > scale) { fail(`${target.id}: decode time base cannot resolve 60fps`); continue; }
    // Allow only the declared container time-base quantization, not a percentage tolerance.
    let priorPts = null;
    for (let i = 0; i < decode.frames.length; i++) {
      const frame = decode.frames[i];
      if (!frame || typeof frame !== 'object') { fail(`${target.id}: malformed decoded frame at ${i}`); continue; }
      const pts = ns(frame.pts);
      if (pts !== null && priorPts !== null && pts <= priorPts) fail(`${target.id}: decoded PTS is not strictly increasing at ${i}`);
      priorPts = pts;
      if (pts === null || (pts * numerator * 60n - BigInt(i) * scale < 0n ? BigInt(i) * scale - pts * numerator * 60n : pts * numerator * 60n - BigInt(i) * scale) >= numerator * 60n) fail(`${target.id}: decoded PTS discontinuity at ${i}`);
      if (frame.slot !== i || frame.frameId !== records.get('delivered')?.get(i)?.frameId || frame.identityMethod !== 'decoded-content-marker') unknown(`${target.id}: decoded content identity at ${i}`);
    }
    if (decode.audio?.sampleRate !== 48000 || decode.audio?.samples !== count * 800 || decode.audio?.lostSamples !== 0 || decode.audio?.firstSample !== 0 || decode.audio?.avAlignmentVerified !== true) unknown(`${target.id}: complete aligned audio sample coverage`);
    const duration = ns(decode.durationTicks);
    if (duration === null || duration === 0n || !valid || (duration * numerator * 60n - BigInt(count) * scale < 0n ? BigInt(count) * scale - duration * numerator * 60n : duration * numerator * 60n - BigInt(count) * scale) >= numerator * 60n) fail(`${target.id}: decoded duration does not cover requested interval`);
  }
  const evidencePassed = issues.length === 0 && missing.length === 0;
  return { version: VERSION, status: issues.length ? 'failed' : missing.length ? 'unverified' : e.mode === 'synthetic' ? 'synthetic-passed' : 'passed', productionAccepted: evidencePassed && e.mode === 'measured', evidencePassed, stages, issues, missingEvidence: missing, hardware: e.hardware, workload: e.workload, interval: e.interval, synthetic: e.mode === 'synthetic' };
}

// Preserve only observed numeric loss counters from old functional soaks. Polls,
// recording booleans and averages are deliberately NOT manufactured into slots.
export function adaptLegacySoak(report) {
  const samples = report.samples ?? report.health ?? [];
  const buffers = samples.map(s => s.buffer).filter(x => x && typeof x === 'object');
  const counters = { before: {}, after: {} };
  const errors = Array.isArray(report.errors) ? report.errors.map(() => 'Legacy collector error') : [];
  for (const field of failures) {
    const values = buffers.map(b => b[field]).filter(natural);
    if (values.length) { counters.before[field] = values[0]; counters.after[field] = values.at(-1); }
    if (values.some((v, i) => i && v < values[i - 1])) errors.push(`Legacy counter reset: ${field}`);
  }
  return { version: VERSION, mode: 'measured', counters, errors,
    workload: { width: report.configuration?.resolution === '1920x1080' ? 1920 : undefined,
      height: report.configuration?.resolution === '1920x1080' ? 1080 : undefined,
      fps: report.configuration?.fps, bufferFrames: report.configuration?.bufferFrames,
      operations: (report.operations ?? []).map(o => ({ action: o.action, ok: o.ok })) },
    interval: { durationSeconds: report.durationSeconds, startedUtc: report.startTime }, coverage: { complete: false } };
}

export function syntheticEvidence() {
  const e = { version: VERSION, mode: 'synthetic', hardware: { os: 'synthetic', cpu: 'synthetic', gpu: 'synthetic', gpuDriver: 'synthetic' }, binarySha256: { shell: 'a'.repeat(64), native: 'b'.repeat(64), zoom: 'c'.repeat(64) }, processGeneration: 'fixture', requestedRevision: 1, appliedRevision: 1, workload: { width: 1920, height: 1080, fps: 60, bufferFrames: 3, sources: [], outputs: [], operations: [] }, interval: { anchorNs: '1000000000', slots: 60, durationSeconds: 1, startedUtc: '2026-01-01T00:00:00Z' }, errors: [], coverage: { complete: true, processGeneration: 'fixture' }, counters: { before: {}, after: {} } };
  for (const name of failures) e.counters.before[name] = e.counters.after[name] = 0;
  for (const [stage, offset] of [['rendered', -2n], ['delivered', -1n], ['presented', 0n]]) e[stage] = { measurement: stage === 'presented' ? 'display-completion' : 'gpu-completion', frames: Array.from({ length: 60 }, (_, slot) => ({ slot, frameId: `frame-${slot}`, processGeneration: 'fixture', revision: 1, completedNs: (1000000000n + ceil(BigInt(slot + 3) * 1000000000n, 60n) + offset).toString() })) };
  return e;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  try {
    const { values } = parseArgs({ options: { input: { type: 'string' }, output: { type: 'string' }, synthetic: { type: 'boolean' }, 'legacy-soak': { type: 'boolean' }, 'runtime-snapshots': { type: 'boolean' } } });
    if (!!values.input === !!values.synthetic) throw new Error('Choose --input EVIDENCE.json or --synthetic');
    if (values.input && statSync(values.input).size > 256 * 1024 * 1024) throw new Error('Evidence exceeds 256 MiB; partition long runs without omitting interval coverage');
    if (values.synthetic && values['legacy-soak']) throw new Error('--legacy-soak requires --input');
    if (values['runtime-snapshots'] && (values.synthetic || values['legacy-soak'])) throw new Error('--runtime-snapshots requires only --input');
    let evidence = values.synthetic ? syntheticEvidence() : JSON.parse(readFileSync(values.input, 'utf8').replace(/^\uFEFF/, ''));
    if (values['legacy-soak']) evidence = adaptLegacySoak(evidence);
    const verdict = values['runtime-snapshots'] ? assessRuntimeSnapshots(evidence) : qualify(evidence), json = JSON.stringify(verdict, null, 2) + '\n';
    if (values.output) writeFileSync(resolve(values.output), json, { flag: 'wx' });
    process.stdout.write(json);
    process.exitCode = verdict.evidencePassed ? 0 : 1;
  } catch (error) { process.stderr.write(`Qualification failed: ${error.message}\n`); process.exitCode = 1; }
}
