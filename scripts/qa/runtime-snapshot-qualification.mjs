// Read-only adapter for captured native sessionState snapshots. Never calls an API.
const count = x => Number.isSafeInteger(x) && x >= 0;
const age = x => typeof x === 'number' && Number.isFinite(x) && x >= 0;
export const DEFAULT_RUNTIME_POLICY = Object.freeze({ staleWorkerMs: 250, encoderQueueAgeMs: 1000, operationAgeMs: 5000, finalizeAgeMs: 10000 });

export function assessRuntimeSnapshots(capture) {
  const issues = [], missing = [], observations = [];
  const policy = { ...DEFAULT_RUNTIME_POLICY, ...capture?.policy };
  const add = (sample, component, reason, values = {}) => issues.push({ sample, component, reason, ...values });
  for (const [name, value] of Object.entries(policy)) if (!age(value) || value === 0) missing.push(`Invalid diagnostic policy ${name}`);
  const samples = capture?.samples;
  if (!Array.isArray(samples) || !samples.length || samples.length > 100000) missing.push('Bounded captured samples required');
  const required = capture?.expectedWorkers;
  if (!Array.isArray(required) || required.some(x => !['render', 'audio', 'videoOutput'].includes(x)) || new Set(required).size !== required.length) missing.push('Explicit expectedWorkers required');
  if (typeof capture?.recordingExpected !== 'boolean') missing.push('Explicit recordingExpected required');
  let previous;
  const counters = new Map(), generations = new Map();
  function generation(index, component, value) {
    if (!count(value)) { missing.push(`${index}:${component} generation`); return; }
    if (generations.has(component) && generations.get(component) !== value) add(index, component, 'generation changed', { before: generations.get(component), after: value });
    generations.set(component, value);
  }
  function counter(index, component, field, value, isLoss = true) {
    if (!count(value)) { missing.push(`${index}:${component}.${field}`); return; }
    const key = `${component}.${field}`, before = counters.get(key);
    if (before !== undefined && value < before) add(index, component, 'counter reset', { field, before, after: value });
    // The first snapshot is the interval baseline, not evidence of when historic loss occurred.
    if (before !== undefined && isLoss && value > before) add(index, component, 'loss increased', { field, delta: value - before });
    counters.set(key, value);
  }
  for (const [index, row] of (Array.isArray(samples) && samples.length <= 100000 ? samples : []).entries()) {
    if (!row || typeof row !== 'object') { missing.push(`${index}: malformed sample`); continue; }
    if (typeof row.processGeneration !== 'string' || !row.processGeneration) missing.push(`${index}: collector processGeneration`);
    if (!age(row.collectedAtMs)) missing.push(`${index}: collector monotonic collectedAtMs`);
    if (previous) {
      if (previous.processGeneration !== row.processGeneration) add(index, 'process', 'generation changed');
      if (row.collectedAtMs <= previous.collectedAtMs) add(index, 'collector', 'non-monotonic sample time');
    }
    previous = row;
    const s = row.snapshot ?? {}, rt = s.realtimeEvidence;
    if (rt?.metricVersion !== 'realtime-worker-evidence-v1') missing.push(`${index}: realtimeEvidence version`);
    for (const name of ['render', 'audio', 'videoOutput']) {
      const w = rt?.[name];
      if (!w) { if (required?.includes(name)) missing.push(`${index}:${name} worker`); continue; }
      generation(index, name, w.generation);
      const expected = required?.includes(name);
      if (expected && (w.observed !== true || !age(w.progressAgeMs))) missing.push(`${index}:${name} has no observed progress`);
      else if (expected && w.progressAgeMs > policy.staleWorkerMs) add(index, name, 'stale worker', { progressAgeMs: w.progressAgeMs });
      counter(index, name, name === 'render' ? 'completedSlots' : 'completedTicks', name === 'render' ? w.completedSlots : w.completedTicks, false);
      if (name === 'render') for (const field of ['skippedSlots', 'deadlineMisses']) counter(index, name, field, w[field]);
      if (name === 'audio') for (const field of ['pacerReanchors', 'discardedTimelineNs']) counter(index, name, field, w[field]);
      observations.push({ sample: index, component: name, observed: w.observed, progressAgeMs: w.progressAgeMs,
        lockWaitMaximumNs: w.lockWaitMaximumNs, workMaximumNs: w.workMaximumNs });
    }
    const buffer = s.programBuffer;
    if (!buffer) missing.push(`${index}:programBuffer`);
    else {
      generation(index, 'programBuffer', buffer.generation);
      for (const field of ['underruns', 'overflows', 'gpuNotReady', 'deadlineMisses', 'outputSequenceGaps']) counter(index, 'programBuffer', field, buffer[field]);
      for (const field of ['produced', 'delivered', 'displayUnconsumed', 'displayBusy']) counter(index, 'programBuffer', field, buffer[field], false);
      if (buffer.status === 'failed') add(index, 'programBuffer', 'failed');
      observations.push({ sample: index, component: 'programBuffer', occupancy: buffer.occupancy, capacity: buffer.capacity,
        displayUnconsumed: buffer.displayUnconsumed, displayBusy: buffer.displayBusy });
    }
    const enc = s.encoderEvidence;
    if (!enc) { if (capture.recordingExpected) missing.push(`${index}:encoderEvidence`); continue; }
    if (enc.metricVersion !== 'async-encoder-evidence-v1') missing.push(`${index}:encoderEvidence version`);
    generation(index, 'encoder', enc.generation);
    for (const field of ['droppedVideo', 'droppedAudio']) counter(index, 'encoder', field, enc[field]);
    // startupDroppedVideo is video shed behind the recording writer's SYNCHRONOUS open,
    // which clips the head of the show but loses nothing from the file. It is tracked as
    // a non-loss counter (reset/monotonicity still checked, an increase is not an issue)
    // so it stays visible without re-creating the false red it was split out of. It may be
    // absent on a core that predates the split, so only judge it when the field is present.
    if (enc.startupDroppedVideo !== undefined) counter(index, 'encoder', 'startupDroppedVideo', enc.startupDroppedVideo, false);
    for (const field of ['programVideoWritten', 'programAudioPacketsWritten']) counter(index, 'encoder', field, enc[field], false);
    if (enc.lifecycleState === 'failed' || enc.finalizeResult === 'failed' || enc.firstFailure) add(index, 'encoder', 'writer failure reported'); // Do not copy possibly private failure strings.
    if (!count(enc.queueDepth) || !age(enc.oldestQueuedAgeMs)) missing.push(`${index}:encoder queue coverage`);
    else if (enc.queueDepth > 0 && enc.oldestQueuedAgeMs > policy.encoderQueueAgeMs) add(index, 'encoder', 'stale queue', { queueDepth: enc.queueDepth, oldestQueuedAgeMs: enc.oldestQueuedAgeMs });
    if (!age(enc.operationAgeMs)) missing.push(`${index}:encoder operation age`);
    else if (enc.operationAgeMs > policy.operationAgeMs) add(index, 'encoder', 'stuck operation', { operation: enc.operation, operationAgeMs: enc.operationAgeMs });
    const pendingStop = enc.finalizeResult === 'pending' || enc.finalizeResult === 'running' || enc.lifecycleState === 'finalizing';
    // operationAge alone misses a stop stuck behind another queued writer call.
    // Native timestamps share the native monotonic clock; collectedAtMs does NOT.
    // A collector must provide nativeNowMs to compare them, or we only use age fields.
    let finalizeAgeMs = enc.operation === 'stop' ? enc.operationAgeMs : undefined;
    if (age(row.nativeNowMs) && age(enc.stopRequestedMs) && enc.stopRequestedMs > 0 && row.nativeNowMs >= enc.stopRequestedMs) finalizeAgeMs = row.nativeNowMs - enc.stopRequestedMs;
    if (pendingStop && !age(finalizeAgeMs)) missing.push(`${index}:pending stop total age unavailable`);
    else if (pendingStop && finalizeAgeMs > policy.finalizeAgeMs) add(index, 'encoder', 'stuck finalize', { finalizeAgeMs, finalizeResult: enc.finalizeResult });
    observations.push({ sample: index, component: 'encoder', generation: enc.generation, operation: enc.operation,
      operationAgeMs: enc.operationAgeMs, queueDepth: enc.queueDepth, oldestQueuedAgeMs: enc.oldestQueuedAgeMs,
      programVideoWritten: enc.programVideoWritten, programAudioPacketsWritten: enc.programAudioPacketsWritten,
      startupDroppedVideo: enc.startupDroppedVideo ?? null,
      // ISO-3: per-source distinct-frame accounting. An ISO stem's framesWritten is an
      // append count; this is the only place the repeat-freeness of a stem is visible.
      isoVideoBySource: enc.isoVideoBySource ?? null,
      finalizeResult: enc.finalizeResult });
  }
  missing.push('Snapshots do not prove whole-interval per-slot GPU completion', 'Snapshots do not prove display completion',
    'Encoder calls do not prove muxed/committed ranges or validated completed artifacts');
  return { version: 'current-runtime-snapshot-assessment-v1', status: issues.length ? 'failed' : 'unverified', productionAccepted: false,
    policy, expectedWorkers: required, recordingExpected: capture?.recordingExpected, sampleCount: samples?.length ?? 0,
    issues, missingEvidence: [...new Set(missing)], observations };
}
