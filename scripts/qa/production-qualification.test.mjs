import test from 'node:test';
import assert from 'node:assert/strict';
import { qualify, syntheticEvidence, adaptLegacySoak } from './production-qualification.mjs';

function recording() {
  const e = syntheticEvidence();
  e.workload.outputs.push({ id: 'program', kind: 'recording' });
  e.destinations = { program: {
    generation: 'recording-1', processGeneration: 'fixture',
    muxed: { firstSlot: 0, lastSlot: 59, uniqueFrames: 60, gaps: 0 },
    committed: { firstSlot: 0, lastSlot: 59, uniqueFrames: 60, gaps: 0 },
    completed: { state: 'completed', closed: true, artifactSha256: 'd'.repeat(64) },
    decode: { success: true, artifactSha256: 'd'.repeat(64), toolVersion: 'synthetic decoder', width: 1920, height: 1080,
      timeBaseNumerator: '1', timeBaseDenominator: '60000', durationTicks: '60000',
      frames: e.delivered.frames.map(f => ({ slot: f.slot, frameId: f.frameId, pts: String(f.slot * 1000), identityMethod: 'decoded-content-marker' })),
      audio: { sampleRate: 48000, samples: 48000, lostSamples: 0, firstSample: 0, avAlignmentVerified: true } }
  } };
  return e;
}

test('synthetic success can never qualify production', () => {
  const v = qualify(syntheticEvidence());
  assert.equal(v.status, 'synthetic-passed'); assert.equal(v.productionAccepted, false);
});
test('requested booleans and average FPS cannot replace completion', () => {
  const e = syntheticEvidence(); delete e.presented;
  e.recording = true; e.averageFps = 60;
  const v = qualify(e); assert.equal(v.status, 'unverified'); assert.equal(v.stages.presented, 'unverified');
});
test('one deadline miss fails despite perfect remaining frames', () => {
  const e = syntheticEvidence(); e.presented.frames[30].completedNs = String(BigInt(e.presented.frames[30].completedNs) + 1n);
  assert.equal(qualify(e).status, 'failed');
});
test('buffer underrun and reset independently fail', () => {
  for (const mutate of [e => e.counters.after.underruns++, e => { e.counters.before.overflows = 2; }]) {
    const e = syntheticEvidence(); mutate(e); assert.equal(qualify(e).status, 'failed');
  }
});
test('duplicate, missing and stale frame identities cannot pass', () => {
  for (const mutate of [e => e.delivered.frames.pop(), e => e.delivered.frames[3].frameId = 'frame-2', e => e.rendered.frames[0].processGeneration = 'restarted']) {
    const e = syntheticEvidence(); mutate(e); assert.equal(qualify(e).status, 'failed');
  }
});
test('present submission and incomplete counter coverage remain unverified', () => {
  const e = syntheticEvidence(); e.presented.measurement = 'present-submitted'; delete e.counters.after.audioLostSamples;
  const v = qualify(e); assert.equal(v.status, 'unverified'); assert.ok(v.missingEvidence.some(x => x.includes('audioLostSamples')));
});
test('closed recording requires muxed and committed full ranges plus actual decode', () => {
  const e = recording(); assert.equal(qualify(e).evidencePassed, true);
  delete e.destinations.program.committed;
  assert.equal(qualify(e).status, 'unverified');
});
test('truncated recording, shifted PTS and wrong duration fail', () => {
  for (const mutate of [e => e.destinations.program.decode.frames.pop(), e => e.destinations.program.decode.frames[0].pts = '1000', e => e.destinations.program.decode.durationTicks = '59000']) {
    const e = recording(); mutate(e); assert.equal(qualify(e).status, 'failed');
  }
});
test('padded or substituted decoded frames lack source content proof', () => {
  const e = recording(); e.destinations.program.decode.frames[20].frameId = 'frame-19';
  assert.equal(qualify(e).status, 'unverified');
});
test('decode of another artifact and short measured runs cannot qualify', () => {
  const e = recording(); e.destinations.program.decode.artifactSha256 = 'e'.repeat(64);
  assert.equal(qualify(e).status, 'unverified');
  const short = syntheticEvidence(); short.mode = 'measured';
  assert.equal(qualify(short).productionAccepted, false);
});
test('no completion may precede its matching upstream packet', () => {
  const e = syntheticEvidence(); e.presented.frames[20].completedNs = e.rendered.frames[20].completedNs;
  assert.equal(qualify(e).status, 'failed');
});
test('every declared output must be evidenced; unsupported transport is unverified', () => {
  const e = syntheticEvidence(); e.workload.outputs.push({ id: 'srt', kind: 'transport' });
  assert.equal(qualify(e).status, 'unverified');
});
test('legacy functional success cannot synthesize frame completion; losses survive conversion', () => {
  const legacy = { configuration: { resolution: '1920x1080', fps: 60, bufferFrames: 3 }, durationSeconds: 3600,
    startTime: '2026-01-01T00:00:00Z', errors: [], samples: [{ buffer: { underruns: 0 } }, { buffer: { underruns: 7 } }], operations: [] };
  const e = adaptLegacySoak(legacy), v = qualify(e);
  assert.equal(v.status, 'failed'); assert.equal(v.productionAccepted, false);
  assert.ok(v.issues.some(x => x.includes('underruns +7'))); assert.equal(v.stages.presented, 'unverified');
});
test('legacy mid-run reset stays failed even if final count recovered', () => {
  const e = adaptLegacySoak({ samples: [9, 0, 12].map(underruns => ({ buffer: { underruns } })) });
  assert.ok(e.errors.some(x => x.includes('reset')));
});
for (const seconds of [1, 3600]) test(`compressed completion timestamps cannot prove ${seconds}s coverage`, () => {
  const e = syntheticEvidence(); e.interval.slots = seconds * 60; e.interval.durationSeconds = seconds;
  if (seconds === 3600) e.mode = 'measured';
  for (const [stage, offset] of [['rendered', 0], ['delivered', 1], ['presented', 2]]) {
    e[stage].frames = Array.from({ length: e.interval.slots }, (_, slot) => ({ slot, frameId: `frame-${slot}`, processGeneration: 'fixture', revision: 1,
      completedNs: String(1000000000n + BigInt(slot * 3 + offset)) }));
  }
  const v = qualify(e); assert.equal(v.productionAccepted, false); assert.equal(v.status, 'failed');
  assert.ok(v.issues.some(x => x.includes('before scheduled slot window')));
});
test('presentation at preceding slot deadline fails the strict lower boundary', () => {
  const e = syntheticEvidence();
  for (const stage of ['rendered', 'delivered', 'presented']) e[stage].frames[0].completedNs = '1033333334';
  assert.ok(qualify(e).issues.some(x => x.includes('before scheduled slot window at 0')));
});
test('coarse timebase with zero PTS and duration cannot pass recording integrity', () => {
  const e = recording(), d = e.destinations.program.decode;
  d.timeBaseNumerator = '10'; d.timeBaseDenominator = '1'; d.durationTicks = '0';
  for (const frame of d.frames) frame.pts = '0';
  const v = qualify(e); assert.equal(v.status, 'failed'); assert.ok(v.issues.some(x => x.includes('cannot resolve 60fps')));
});
test('fine timebase still requires strictly increasing PTS and nonzero duration', () => {
  const e = recording(), d = e.destinations.program.decode;
  for (const frame of d.frames) frame.pts = '0'; d.durationTicks = '0';
  const v = qualify(e); assert.equal(v.status, 'failed');
  assert.ok(v.issues.some(x => x.includes('not strictly increasing'))); assert.ok(v.issues.some(x => x.includes('duration')));
});
