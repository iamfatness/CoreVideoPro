import test from 'node:test';
import assert from 'node:assert/strict';
import { assessRuntimeSnapshots } from './runtime-snapshot-qualification.mjs';

function clean() {
  const w = { generation: 1, observed: true, progressAgeMs: 2, completedTicks: 10 };
  const snapshot = {
    realtimeEvidence: { metricVersion: 'realtime-worker-evidence-v1', render: { ...w, completedSlots: 10, skippedSlots: 0, deadlineMisses: 0 },
      audio: { ...w, pacerReanchors: 0, discardedTimelineNs: 0 }, videoOutput: { ...w } },
    programBuffer: { generation: 1, status: 'running', produced: 10, delivered: 10, underruns: 0, overflows: 0, gpuNotReady: 0, deadlineMisses: 0, outputSequenceGaps: 0, displayBusy: 0, displayUnconsumed: 0 },
    encoderEvidence: { metricVersion: 'async-encoder-evidence-v1', generation: 1, lifecycleState: 'live', operation: 'idle', operationAgeMs: 0,
      queueDepth: 0, oldestQueuedAgeMs: 0, droppedVideo: 0, droppedAudio: 0, programVideoWritten: 10, programAudioPacketsWritten: 10, finalizeResult: 'none' }
  };
  return { expectedWorkers: ['render', 'audio', 'videoOutput'], recordingExpected: true,
    samples: [0, 1].map(i => ({ processGeneration: 'native-1', collectedAtMs: 1000 + i * 50, snapshot: structuredClone(snapshot) })) };
}
test('clean snapshots remain unverified, not mux/display/commit proof', () => {
  const v = assessRuntimeSnapshots(clean()); assert.equal(v.status, 'unverified'); assert.equal(v.productionAccepted, false); assert.equal(v.issues.length, 0);
});
test('each stale worker is independently attributed', () => {
  for (const worker of ['render', 'audio', 'videoOutput']) {
    const c = clean(); c.samples[1].snapshot.realtimeEvidence[worker].progressAgeMs = 500;
    assert.ok(assessRuntimeSnapshots(c).issues.some(i => i.component === worker && i.reason === 'stale worker'));
  }
});
test('blocked finalize is reported even with old successful video counts', () => {
  const c = clean(); Object.assign(c.samples[1].snapshot.encoderEvidence, { operation: 'stop', operationAgeMs: 20000, finalizeResult: 'running', lifecycleState: 'finalizing' });
  const v = assessRuntimeSnapshots(c); assert.equal(v.status, 'failed'); assert.ok(v.issues.some(i => i.reason === 'stuck finalize'));
});
test('stop waiting in queue uses native clock, never collector clock', () => {
  const c = clean(), row = c.samples[1]; row.collectedAtMs = 1e9;
  Object.assign(row.snapshot.encoderEvidence, { finalizeResult: 'pending', stopRequestedMs: 100 });
  assert.ok(assessRuntimeSnapshots(c).missingEvidence.some(x => x.includes('stop total age')));
  row.nativeNowMs = 20000; assert.ok(assessRuntimeSnapshots(c).issues.some(x => x.reason === 'stuck finalize'));
});
test('slot loss, discarded audio and encoder backpressure are separate failures', () => {
  const c = clean(), s = c.samples[1].snapshot;
  s.realtimeEvidence.render.skippedSlots = 2; s.realtimeEvidence.audio.pacerReanchors = 1; s.realtimeEvidence.audio.discardedTimelineNs = 20000000;
  s.programBuffer.deadlineMisses = 1; Object.assign(s.encoderEvidence, { droppedAudio: 3, queueDepth: 5, oldestQueuedAgeMs: 2000 });
  const v = assessRuntimeSnapshots(c); assert.equal(v.status, 'failed');
  for (const field of ['skippedSlots', 'pacerReanchors', 'discardedTimelineNs', 'deadlineMisses', 'droppedAudio']) assert.ok(v.issues.some(x => x.field === field));
  assert.ok(v.issues.some(x => x.reason === 'stale queue'));
});
test('process, worker and buffer generation changes cannot conceal reset', () => {
  const c = clean(), row = c.samples[1]; row.processGeneration = 'native-2'; row.snapshot.realtimeEvidence.render.generation = 2; row.snapshot.programBuffer.generation = 2;
  const v = assessRuntimeSnapshots(c); for (const component of ['process', 'render', 'programBuffer']) assert.ok(v.issues.some(x => x.component === component && x.reason === 'generation changed'));
});
test('historical baseline losses are not attributed to a later interval', () => {
  const c = clean(); for (const s of c.samples) s.snapshot.programBuffer.underruns = 5;
  assert.equal(assessRuntimeSnapshots(c).issues.length, 0);
});
