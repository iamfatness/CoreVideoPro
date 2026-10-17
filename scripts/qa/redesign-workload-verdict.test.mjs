import test from 'node:test';
import assert from 'node:assert/strict';
import { judgeRedesignWorkload } from './redesign-workload-verdict.mjs';

function completeEvidence() {
  const requiredInputIds = Array.from({ length: 10 }, (_, i) => 'input-' + i);
  const requiredIsoIds = requiredInputIds.slice(0, 8);
  const startTime = '2026-10-16T12:00:00Z', endTime = '2026-10-16T12:30:00Z';
  const enabledOutputs = ['recording:program', 'virtual-camera:program', ...requiredIsoIds.map(id => 'recording:iso:' + id)];
  const requiredPaths = ['cpu-submission', 'program-gpu', 'program-presentation', 'preview-presentation', 'multiview-presentation', ...enabledOutputs];
  return { startTime, endTime, requiredInputIds, requiredIsoIds, monitorProbeVerdict: { functionalPassed: true },
    cameraPixelVerdict: { osCameraContinuityVerified: true, durationSeconds: 1800 },
    cameraPixelCoverage: { complete: true, startTime, endTime },
    samples: Array.from({ length: 181 }, (_, i) => ({ collectedAtMs: i * 10000, processGeneration: 'one',
      displayState: { visible: true, minimized: false }, snapshot: {
      outputProfile: { width: 1920, height: 1080, fps: 60 }, meetingState: 'in_meeting',
      realtimeEvidence: { monitorWorker: { inputs: { sources: requiredInputIds.map(sourceId => ({ sourceId, state: 'ready', frameId: i * 600 + 1 })) } } },
      recording: { active: true, lifecycle: { state: 'producing' },
        streams: [{ kind: 'program', framesWritten: i * 600 + 1 }, ...requiredIsoIds.map(sourceId => ({ sourceId, framesWritten: i * 600 + 1 }))],
        proof: { programMissingFrames: 0, encoderQueueDroppedVideoFrames: 0 } },
      programBuffer: { underruns: 0 },
      virtualCamera: { enabled: true, status: 'live', framesPublished: i * 600 + 1, pendingFramesReplaced: 0, publicationExceptions: 0 },
    } })),
    framePerformanceReport: { startTime, endTime, soakPassed: true, errorMatches: [], performanceEvidence: {
      metricVersion: 'anchored-deadline-v1', targetFps: 60, clock: 'monotonic', requiredPaths, enabledOutputs,
      paths: requiredPaths.map(id => ({ id, measurement: 'completion-deadlines', errors: [],
        coverage: { complete: true, startTime, endTime }, firstSlot: 1, lastSlot: 108000, expectedSlots: 108000,
        completedSlots: 108000, uniqueCompletedSlots: 108000, deadlineMisses: 0, skippedSlots: 0 })),
    } },
  };
}

test('complete matching evidence can qualify, but one display stall cannot', () => {
  const evidence = completeEvidence();
  assert.equal(judgeRedesignWorkload(evidence).passed, true);
  evidence.framePerformanceReport.performanceEvidence.paths.find(x => x.id === 'multiview-presentation').worstPresentationIntervalNs = 100000000;
  assert.equal(judgeRedesignWorkload(evidence).passed, false);
});

test('a stopped recording fails even when supplied frame and camera verdicts are good', () => {
  const evidence = completeEvidence();
  evidence.samples[90].snapshot.recording.active = false;
  assert.equal(judgeRedesignWorkload(evidence).passed, false);
});

test('hidden, minimized or unobserved monitor windows cannot qualify a display soak', () => {
  for (const state of [undefined, { visible: false, minimized: false }, { visible: true, minimized: true }]) {
    const evidence = completeEvidence();
    evidence.samples[90].displayState = state;
    const verdict = judgeRedesignWorkload(evidence);
    assert.equal(verdict.passed, false);
    assert.ok(verdict.errors.some(x => x.includes('monitor window')));
  }
});

test('frozen video counters fail even while all statuses say ready', () => {
  for (const path of ['input', 'camera', 'recording']) {
    const evidence = completeEvidence();
    const s = evidence.samples[90].snapshot, before = evidence.samples[89].snapshot;
    if (path === 'input') s.realtimeEvidence.monitorWorker.inputs.sources[0].frameId = before.realtimeEvidence.monitorWorker.inputs.sources[0].frameId;
    if (path === 'camera') s.virtualCamera.framesPublished = before.virtualCamera.framesPublished;
    if (path === 'recording') s.recording.streams[0].framesWritten = before.recording.streams[0].framesWritten;
    assert.equal(judgeRedesignWorkload(evidence).passed, false, path);
  }
});

test('good frame evidence from a different run cannot qualify this workload', () => {
  const evidence = completeEvidence();
  evidence.startTime = '2026-10-16T13:00:00Z';
  evidence.endTime = '2026-10-16T13:30:00Z';
  assert.equal(judgeRedesignWorkload(evidence).passed, false);
});

test('a long enough camera capture from another interval cannot qualify the workload', () => {
  const evidence = completeEvidence();
  evidence.cameraPixelCoverage = { complete: true,
    startTime: '2026-10-16T11:00:00Z', endTime: '2026-10-16T11:30:00Z' };
  assert.equal(judgeRedesignWorkload(evidence).passed, false);
  delete evidence.cameraPixelCoverage;
  assert.equal(judgeRedesignWorkload(evidence).passed, false);
});

test('malformed sample fails closed without throwing', () => {
  const evidence = completeEvidence();
  evidence.samples[90] = null;
  assert.equal(judgeRedesignWorkload(evidence).passed, false);
});

test('command success and functional success cannot qualify video delivery', () => {
  const result = judgeRedesignWorkload({ ok: true, soakPassed: true, monitorProbeVerdict: { functionalPassed: true } });
  assert.equal(result.passed, false);
  assert.ok(result.errors.some(x => x.includes('Per-frame')));
});
test('a requested recording must actually produce in every measurement sample', () => {
  const result = judgeRedesignWorkload({ samples: [0, 5000].map(collectedAtMs => ({
    collectedAtMs, processGeneration: 'one', snapshot: { recording: { active: false, lifecycle: { state: 'preparing' } } },
  })) });
  assert.equal(result.passed, false);
  assert.ok(result.errors.some(x => x.includes('recording is not producing')));
});
test('short runs, telemetry gaps and process restarts fail closed', () => {
  const result = judgeRedesignWorkload({ samples: [
    { collectedAtMs: 0, processGeneration: 'one' }, { collectedAtMs: 12000, processGeneration: 'two' },
  ] });
  assert.equal(result.passed, false);
  assert.ok(result.errors.some(x => x.includes('coverage broke')));
  assert.ok(result.errors.some(x => x.includes('30 minutes')));
});
