import test from 'node:test';
import assert from 'node:assert/strict';
import { assessProgramSourceAdmissions } from './program-source-admission-verdict.mjs';
import { assessRuntimeSnapshots } from './runtime-snapshot-qualification.mjs';

const live = () => ({
  programFrame: { health: 'live', videoSources: [{ sourceId: 'zoom:16824320' }] },
  programSourceAdmission: { version: 1, sources: [{ sourceId: '16824320', state: 'ready',
    requestedEpoch: 141, actualEpoch: 141, requestedFrameId: 24592, actualFrameId: 24592,
    requestedCapture100ns: 3686749862470, actualCapture100ns: 3686749862470 }] }
});

test('gray-source refusal fails even with live health and perfect output markers', () => {
  const s = live();
  Object.assign(s, { programCounterPixels: { passed: true, missing: 0, repeated: 0 } });
  Object.assign(s.programSourceAdmission.sources[0], { state: 'unavailable',
    reason: 'preparation-retiring-generation-capacity', actualEpoch: null, actualFrameId: null, actualCapture100ns: null });
  const v = assessProgramSourceAdmissions(s);
  assert.equal(v.sourceAdmissionsVerified, false);
  assert.ok(v.issues.some(i => i.reason === 'Program source image not ready'));
  const runtime = assessRuntimeSnapshots({ recordingExpected: false, expectedWorkers: [],
    samples: [{ processGeneration: 'native', collectedAtMs: 1, snapshot: s }] });
  assert.equal(runtime.status, 'failed');
  assert.ok(runtime.issues.some(i => i.component === 'programSourceAdmission'));
});

test('ready source identity is separate from output and display qualification', () => {
  assert.equal(assessProgramSourceAdmissions(live()).sourceAdmissionsVerified, true);
});

test('missing source proof or a dropped source list cannot qualify an expected source', () => {
  const s = live(); delete s.programSourceAdmission;
  assert.equal(assessProgramSourceAdmissions(s).sourceAdmissionsVerified, false);
  const empty = live(); empty.programFrame.videoSources = []; empty.programSourceAdmission.sources = [];
  assert.equal(assessProgramSourceAdmissions(empty, ['zoom:16824320']).sourceAdmissionsVerified, false);
});

test('held, mismatched, duplicate and incomplete actual identities fail', () => {
  for (const change of [{ state: 'held' }, { actualEpoch: 140 }, { actualFrameId: 24591 },
    { actualCapture100ns: 1 }, { actualFrameId: null }]) {
    const s = live(); Object.assign(s.programSourceAdmission.sources[0], change);
    assert.equal(assessProgramSourceAdmissions(s).sourceAdmissionsVerified, false);
  }
  const s = live(); s.programSourceAdmission.sources.push(structuredClone(s.programSourceAdmission.sources[0]));
  assert.equal(assessProgramSourceAdmissions(s).sourceAdmissionsVerified, false);
});
