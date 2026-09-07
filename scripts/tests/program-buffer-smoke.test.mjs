import test from 'node:test';
import assert from 'node:assert/strict';
import { summarizeBufferInterval } from '../qa/program-buffer-smoke.mjs';

const sample = () => ({ activeFrames: 3, status: 'running', generation: 1, produced: 20, delivered: 17, underruns: 0, overflows: 0, gpuNotReady: 2, deadlineMisses: 0, outputSequenceGaps: 0, displayBusy: 0, displayUnconsumed: 16 });

test('interval deltas ignore priming counts and preserve internal versus display distinction', () => {
  const baseline = sample(), end = { ...baseline, produced: 620, delivered: 617, displayUnconsumed: 616 };
  const result = summarizeBufferInterval(baseline, end, 3, [{ elapsedMs: 500, programBuffer: end }]);
  assert.deepEqual(result.errors, []);
  assert.equal(result.delta.delivered, 600);
  assert.equal(result.delta.gpuNotReady, 0);
  assert.equal(result.delta.displayUnconsumed, 600);
});

for (const counter of ['underruns', 'overflows', 'gpuNotReady', 'deadlineMisses', 'outputSequenceGaps']) {
  test(`${counter} increments fail internal smoke`, () => {
    const baseline = sample(), end = { ...baseline, delivered: 18, [counter]: baseline[counter] + 1 };
    assert.ok(summarizeBufferInterval(baseline, end, 3).errors.some(error => error.includes(counter)));
  });
}

test('missing/reset counters and unsupported depth cannot produce smoke success', () => {
  assert.ok(summarizeBufferInterval(sample(), null, 3).errors.length);
  assert.ok(summarizeBufferInterval(sample(), { ...sample(), delivered: 1 }, 3).errors.some(error => error.includes('reset')));
  assert.ok(summarizeBufferInterval(sample(), { ...sample(), delivered: 18 }, 2).errors.some(error => error.includes('depth')));
  assert.ok(summarizeBufferInterval(sample(), sample(), 3).errors.some(error => error.includes('progress')));
});

for (const [name, patch] of [
  ['missing generation', { generation: undefined }],
  ['changed generation', { generation: 2 }],
  ['zero generation', { generation: 0 }],
  ['wrong depth', { activeFrames: 2 }],
  ['failed status', { status: 'failed' }],
  ['missing status', { status: undefined }],
]) test(`interim ${name} fails even when baseline and end are healthy`, () => {
  const baseline = sample(), end = { ...baseline, delivered: 617 };
  const samples = [{ programBuffer: { ...baseline, delivered: 300, ...patch } }];
  assert.ok(summarizeBufferInterval(baseline, end, 3, samples).errors.some(error => error.startsWith('sample 1:')));
});

test('missing baseline or changed end generation fails despite progressing counters', () => {
  const baseline = sample(), end = { ...baseline, delivered: 617 };
  assert.ok(summarizeBufferInterval({ ...baseline, generation: undefined }, end, 3).errors.some(error => error.includes('baseline buffer generation')));
  assert.ok(summarizeBufferInterval(baseline, { ...end, generation: 2 }, 3).errors.some(error => error.startsWith('end:')));
});

test('every healthy sample retains one generation', () => {
  const baseline = sample(), end = { ...baseline, delivered: 617 };
  const samples = [100, 200, 300].map((delivered, index) => ({ elapsedMs: (index + 1) * 500, programBuffer: { ...baseline, delivered } }));
  assert.deepEqual(summarizeBufferInterval(baseline, end, 3, samples).errors, []);
});

for (const counter of ['produced', 'delivered', 'underruns', 'overflows', 'gpuNotReady', 'deadlineMisses', 'outputSequenceGaps', 'displayBusy', 'displayUnconsumed']) {
  test(`interim missing or reset ${counter} fails even after recovery`, () => {
    const baseline = { ...sample(), [counter]: 10 }, end = { ...baseline, delivered: 40 };
    for (const value of [undefined, 9]) {
      const samples = [{ elapsedMs: 500, programBuffer: { ...baseline, delivered: 30, [counter]: value } },
        { elapsedMs: 1000, programBuffer: end }];
      assert.ok(summarizeBufferInterval(baseline, end, 3, samples).errors.some(error => error.startsWith('sample 1:') && error.includes(counter)));
    }
  });
}

test('one delivery followed by frozen telemetry fails ongoing progress', () => {
  const baseline = sample(), end = { ...baseline, delivered: 18 };
  const samples = [500, 1000, 1500].map(elapsedMs => ({ elapsedMs, programBuffer: end }));
  assert.ok(summarizeBufferInterval(baseline, end, 3, samples).errors.some(error => error.includes('full sampled window')));
});

test('sub-window samples cannot hide a frozen full window', () => {
  const baseline = sample(), end = { ...baseline, delivered: 18 };
  const samples = [{ elapsedMs: 250, programBuffer: baseline }, { elapsedMs: 500, programBuffer: baseline },
    { elapsedMs: 1000, programBuffer: end }];
  assert.ok(summarizeBufferInterval(baseline, end, 3, samples).errors.some(error => error.startsWith('sample 2:') && error.includes('progress')));
});

test('absent samples or invalid timestamps cannot establish sustained progress', () => {
  const baseline = sample(), end = { ...baseline, delivered: 100 };
  for (const samples of [[], [{ programBuffer: end }], [{ elapsedMs: 0, programBuffer: end }],
    [{ elapsedMs: 500, programBuffer: end }, { elapsedMs: 499, programBuffer: end }]]) {
    assert.ok(summarizeBufferInterval(baseline, end, 3, samples).errors.length);
  }
});
