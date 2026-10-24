import test from 'node:test';
import assert from 'node:assert/strict';
import { classifyRecordingWarning, isoPlacementNotes, isoLossRegressions, programEncoderFailure } from './iso-record-rules.mjs';

const note = '1 ISO source will record on the CPU software encoder, not the GPU (7 on hardware): x.';
const adm = { decision: 'admit-with-warning', code: 'iso-encoder-software-spill', message: note };

test('no warning is none', () => assert.equal(classifyRecordingWarning(null, adm).kind, 'none'));
test('exact admission message is a note', () => assert.equal(classifyRecordingWarning(note, adm).kind, 'note'));
test('admission text with the #1030 loss sentence prepended fails', () =>
  assert.equal(classifyRecordingWarning('Recording lost video frames or audio packets: ... ' + note, adm).kind, 'failure'));
test('admission text with anything appended fails', () =>
  assert.equal(classifyRecordingWarning(note + ' Could not write recording manifest to x.', adm).kind, 'failure'));
test('the sentence without a structured admission fails (no regex waiver)', () =>
  assert.equal(classifyRecordingWarning(note, undefined).kind, 'failure'));
test('a refusal is never a note', () =>
  assert.equal(classifyRecordingWarning(note, { ...adm, decision: 'refuse-iso' }).kind, 'failure'));
test('notes name the ISO and the measured cost', () =>
  assert.deepEqual(isoPlacementNotes([{ kind: 'iso', isoIndex: 8, displayName: 'Guest 8', encoderPath: 'software',
    fallbackReason: 'hardware-capacity-exhausted', videoWorkUs: 5640000, muxVideoFrameCount: 600, droppedFrames: 0 }], 60),
  ['ISO-8 Guest 8 is on the CPU software encoder (GPU sessions used up) · 9.4 ms per frame of a 16.7 ms budget · 0 frames dropped']));
test('no frames yet says not measured', () =>
  assert.match(isoPlacementNotes([{ kind: 'iso', isoIndex: 8, encoderPath: 'software', muxVideoFrameCount: 0 }], 60)[0], /cost not measured yet/));
test('hardware ISO streams produce no note', () =>
  assert.deepEqual(isoPlacementNotes([{ kind: 'iso', isoIndex: 1, encoderPath: 'hardware' }], 60), []));
test('reason map and name-only label', () => {
  const n = (r) => isoPlacementNotes([{ kind: 'iso', sourceId: 'zoom:9', encoderPath: 'software', fallbackReason: r, muxVideoFrameCount: 0 }], 0)[0];
  assert.match(n('hardware-unavailable'), /^zoom:9 is on the CPU software encoder \(no GPU encoder\)/);
  assert.match(n('operator-selected'), /\(set to software\)/);
  assert.match(n('weird'), /\(weird\)/);
  assert.doesNotMatch(n('weird'), /budget/);
});
test('budget only when fps is positive', () =>
  assert.doesNotMatch(isoPlacementNotes([{ kind: 'iso', isoIndex: 2, encoderPath: 'software', videoWorkUs: 1000, muxVideoFrameCount: 1 }], 0)[0], /budget/));
test('a writer whose drops grew fails', () =>
  assert.deepEqual(isoLossRegressions([{ kind: 'iso', sourceId: 'zoom:8', droppedFrames: 0, writeQueue: { dropped: 0 } }],
                                      [{ kind: 'iso', sourceId: 'zoom:8', droppedFrames: 2, writeQueue: { dropped: 0 } }]),
  ['ISO zoom:8 steady-state droppedFrames 0 -> 2']));
test('writeQueue.dropped growth fails', () =>
  assert.deepEqual(isoLossRegressions([{ kind: 'iso', sourceId: 'a', droppedFrames: 0, writeQueue: { dropped: 0 } }],
                                      [{ kind: 'iso', sourceId: 'a', droppedFrames: 0, writeQueue: { dropped: 3 } }]),
  ['ISO a steady-state writeQueue.dropped 0 -> 3']));
test('unchanged counters pass', () =>
  assert.deepEqual(isoLossRegressions([{ kind: 'iso', sourceId: 'a', droppedFrames: 1, writeQueue: { dropped: 0 } }],
                                      [{ kind: 'iso', sourceId: 'a', droppedFrames: 1, writeQueue: { dropped: 0 } }]), []));
test('missing counters are a failure, not zero', () =>
  assert.equal(isoLossRegressions([{ kind: 'iso', sourceId: 'zoom:8' }], [{ kind: 'iso', sourceId: 'zoom:8' }]).length > 0, true));
test('program encoder: hardware passes', () => assert.equal(programEncoderFailure({ encoder: { hardwareAccelerated: true } }), null));
test('program encoder: software fails', () =>
  assert.equal(programEncoderFailure({ encoder: { hardwareAccelerated: false } }),
    'Program recorded on the CPU software encoder (Program on software is a FAIL for RC)'));
test('program encoder: absent is a failure, never true', () => {
  assert.match(programEncoderFailure({ encoder: {} }), /not reported/);
  assert.match(programEncoderFailure(undefined), /not reported/);
});
