import test from 'node:test';
import assert from 'node:assert/strict';
import { summarizeCameraDelivery } from './vcam-delivery-evidence.mjs';

function record(overrides = {}) {
  const values = { stream: '000000123ABC', run: 1, emitted: 60, fresh: 55, held: 4, slate: 1,
    failed: 0, formatMismatch: 0, readFresh: 55, unchanged: 3, contended: 1, unavailable: 1,
    uninitialized: 0, invalidHeader: 0, publicationObserved: 1, lastPublication: 70, lastSeq: 142,
    maxIntervalHns: 400000, programIdentityVerified: 0, receiverVerified: 0, ...overrides };
  return `[10:00:00.000 pid=123] [vcam-delivery-v1] ${Object.entries(values).map(([k,v]) => `${k}=${v}`).join(' ')}`;
}

test('legacy and absent logs remain unavailable, never a delivery pass', () => {
  const result = summarizeCameraDelivery('[10:00:00 pid=123] Fill #61: readLatest=1');
  assert.equal(result.available, false);
  assert.equal(result.receiverVerified, false);
});
test('held frames and contended reads remain distinct, with explicit lifetime maximum', () => {
  const result = summarizeCameraDelivery(record() + '\n' + record({emitted: 120, fresh: 112, held: 7, unchanged: 6, readFresh: 112}));
  assert.equal(result.streams[0].delta.emitted, 60);
  assert.equal(result.streams[0].delta.held, 3);
  assert.equal(result.streams[0].delta.contended, 0);
  assert.equal(result.streams[0].lifetimeMaximumEmissionIntervalMs, 40);
  assert.equal(result.receiverVerified, false);
});
test('restarts and multiple stream instances cannot be combined into one cadence', () => {
  const result = summarizeCameraDelivery([record(), record({run: 2}), record({stream: 'ABCDEF'})].join('\n'));
  assert.equal(result.streams.length, 3);
  assert.ok(result.streams.every(s => s.delta === null));
});
test('counter regressions invalidate window deltas', () => {
  const result = summarizeCameraDelivery(record({emitted: 120, fresh: 115}) + '\n' + record());
  assert.equal(result.streams[0].counterResetObserved, true);
  assert.equal(result.streams[0].delta, null);
});
test('truncated or contradictory records are not accepted as healthy evidence', () => {
  const result = summarizeCameraDelivery([record({emitted: 99}), record({receiverVerified: 1}),
    '[pid=123] [vcam-delivery-v1] stream=123 emitted=60'].join('\n'));
  assert.equal(result.malformedRecords, 3);
  assert.equal(result.available, false);
});

function correlated(overrides = {}) {
  return record({ programIdentityVerified: 1, correlatedReads: 55, uncorrelatedReads: 0,
    unobservedProgramFrames: 0, programEpochChanges: 0, programRegressions: 0,
    lastProgramSequence: 70, lastReadProgramSequence: 70, epoch: '12345678123456780000000000000001', ...overrides })
    .replace('[vcam-delivery-v1]', '[vcam-delivery-v2]');
}
test('V2 identity correlation does not become receiver presentation proof', () => {
  const result = summarizeCameraDelivery(correlated());
  assert.equal(result.programIdentityVerified, true);
  assert.equal(result.receiverVerified, false);
  assert.equal(result.streams[0].last.lastProgramSequence, 70);
});
test('unknown reads and incomplete V2 records cannot certify Program identity', () => {
  assert.equal(summarizeCameraDelivery(correlated({correlatedReads: 54, uncorrelatedReads: 1})).programIdentityVerified, false);
  assert.equal(summarizeCameraDelivery(correlated({epoch: '0'})).malformedRecords, 1);
  assert.equal(summarizeCameraDelivery(correlated({correlatedReads: 54})).malformedRecords, 1);
  assert.equal(summarizeCameraDelivery(correlated({programRegressions: 1})).programIdentityVerified, false);
});
