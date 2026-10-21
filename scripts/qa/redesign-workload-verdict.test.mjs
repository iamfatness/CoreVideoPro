import test from 'node:test';
import assert from 'node:assert/strict';
import { judgeRedesignWorkload } from './redesign-workload-verdict.mjs';

function completeEvidence() {
  const requiredInputIds = [...Array.from({ length: 8 }, (_, i) => 'zoom:' + (1000 + i)), 'capture:camera', 'capture:screen:2'];
  const requiredIsoIds = [...requiredInputIds.slice(0, 7), 'capture:camera'];
  const startTime = '2026-10-16T12:00:00Z', endTime = '2026-10-16T12:30:00Z';
  const enabledOutputs = ['recording:program', 'virtual-camera:program', ...requiredIsoIds.map(id => 'recording:iso:' + id)];
  const requiredPaths = ['cpu-submission', 'program-gpu', 'program-presentation', 'preview-presentation', 'multiview-presentation', ...enabledOutputs];
  return { startTime, endTime, requiredInputIds, requiredIsoIds, monitorProbeVerdict: { functionalPassed: true },
    isoFileEvidence: requiredIsoIds.map(sourceId => ({ sourceId, path: 'fixture/' + sourceId + '.mp4',
      measurement: 'ffprobe-count-packets', width: 1920, height: 1080, packets: 108001, muxVideoFrameCount: 108001 })),
    candidateCameraSha256: 'a'.repeat(64),
    cameraInstallation: { moduleInspection: 'observed', loadedIdentityMatchesRegistration: true,
      registeredFileMatchesManifest: true, registeredFileSha256: 'a'.repeat(64) },
    cameraPixelVerdict: { osCameraContinuityVerified: true, durationSeconds: 1800,
      firstIdentity: 1, lastIdentity: 108001, samples: 108001 },
    recordingPixelVerdict: { recordedPixelContinuityPassed: true, identityRangeComplete: true,
      firstIdentity: 1, lastIdentity: 108001, samples: 108001 },
    cameraPixelCoverage: { complete: true, startTime, endTime },
    samples: Array.from({ length: 181 }, (_, i) => ({ collectedAtMs: i * 10000, processGeneration: 'one',
      displayState: { visible: true, minimized: false, desktopUnlocked: true }, snapshot: {
      outputProfile: { width: 1920, height: 1080, fps: 60 }, meetingState: 'in_meeting',
      captureDevices: [{ isoCpuArrivals: { sourceId: 'capture:camera', demanded: true,
        observed: i * 600 + 1, accepted: i * 600 + 1, drained: i * 600 + 1,
        overflowed: 0, released: 0, pending: 0, bytes: 0 } }],
      zoomSubscriptionChurn: { sources: requiredIsoIds.filter(id => id.startsWith('zoom:')).map(sourceId => ({
        isoCpuArrivals: { sourceId, demanded: true, observed: i * 600 + 1, accepted: i * 600 + 1,
          drained: i * 600 + 1, overflowed: 0, released: 0, pending: 0, bytes: 0 },
        isoRing: { observed: true, epoch: 1, written: i * 600 + 1, next: i * 600 + 1, lost: 0 },
      })) },
      realtimeEvidence: { audio: { audioLostSamples: 0, pacerReanchors: 0 }, monitorWorker: { inputs: { sources: requiredInputIds.map(sourceId => ({ sourceId, state: 'ready', frameId: i * 600 + 1 })) } } },
      recording: { active: true, lifecycle: { state: 'producing' },
        streams: [{ kind: 'program', framesWritten: i * 600 + 1 }, ...requiredIsoIds.map(sourceId => ({ sourceId, path: 'fixture/' + sourceId + '.mp4', framesWritten: i * 600 + 1, readiness: 'ready', droppedFrames: 0, droppedAudioPackets: 0 }))],
        proof: { programMissingFrames: 0, encoderQueueDroppedVideoFrames: 0, encoderQueueDroppedAudioPackets: 0 } },
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

test('seven Zoom feeds plus background media cannot qualify as eight remote feeds', () => {
  const report = completeEvidence();
  report.requiredInputIds[7] = 'background:media-show';
  for (const row of report.samples) row.snapshot.realtimeEvidence.monitorWorker.inputs.sources[7].sourceId = 'background:media-show';
  const result = judgeRedesignWorkload(report);
  assert.equal(result.passed, false);
  assert.ok(result.errors.some(error => error.includes('eight distinct Zoom')));
});

test('an unavailable declared eighth feed fails even when replacement background frames advance', () => {
  const report = completeEvidence();
  for (const row of report.samples) row.snapshot.realtimeEvidence.monitorWorker.inputs.sources[7].sourceId = 'background:media-show';
  const result = judgeRedesignWorkload(report);
  assert.equal(result.passed, false);
  assert.ok(result.errors.some(error => error.includes('required input is not ready')));
});

test('1080p inputs and clean counters cannot conceal low-resolution or unmatched ISO files', () => {
  for (const change of [
    report => { delete report.isoFileEvidence; },
    report => { report.isoFileEvidence[0].width = 320; report.isoFileEvidence[0].height = 180; },
    report => { report.isoFileEvidence[0].packets--; },
    report => { report.isoFileEvidence[0].path = 'another-run.mp4'; },
  ]) {
    const report = completeEvidence(); change(report);
    assert.equal(judgeRedesignWorkload(report).passed, false);
  }
});

test('advancing outputs cannot hide sampled capture arrivals or missing arrival evidence', () => {
  for (const change of [
    s => { delete s.captureDevices; },
    s => { s.captureDevices[0].isoCpuArrivals.demanded = false; },
    s => { s.captureDevices[0].isoCpuArrivals.observed += 2; },
    s => { s.captureDevices[0].isoCpuArrivals.overflowed++; },
    s => { s.captureDevices[0].isoCpuArrivals.released++; },
    s => { s.captureDevices[0].isoCpuArrivals.pending = 5; },
    s => { s.captureDevices[0].isoCpuArrivals.bytes = 32 * 1024 * 1024 + 1; },
  ]) {
    const report = completeEvidence(); change(report.samples[10].snapshot);
    assert.equal(judgeRedesignWorkload(report).passed, false);
  }
});

test('advancing files and clean Program pixels cannot hide Zoom helper or playout losses', () => {
  for (const change of [
    s => { delete s.zoomSubscriptionChurn; },
    s => { delete s.zoomSubscriptionChurn.sources[0].isoRing; },
    s => { s.zoomSubscriptionChurn.sources[0].isoRing.observed = false; },
    s => { s.zoomSubscriptionChurn.sources[0].isoRing.lost++; },
    s => { s.zoomSubscriptionChurn.sources[0].isoRing.epoch++; },
    s => { s.zoomSubscriptionChurn.sources[0].isoRing.next += 2; },
    s => { s.zoomSubscriptionChurn.sources[0].isoRing.written += 5; },
    s => { s.zoomSubscriptionChurn.sources[0].isoCpuArrivals.observed += 2; },
    s => { s.zoomSubscriptionChurn.sources[0].isoCpuArrivals.overflowed++; },
  ]) {
    const report = completeEvidence(); change(report.samples[10].snapshot);
    assert.equal(judgeRedesignWorkload(report).passed, false);
  }
});

test('recording counters cannot replace finalized pixels or pixels from the measured interval', () => {
  for (const change of [
    r => { delete r.recordingPixelVerdict; },
    r => { r.recordingPixelVerdict.recordedPixelContinuityPassed = false; },
    r => { r.recordingPixelVerdict.firstIdentity++; },
    r => { r.recordingPixelVerdict.lastIdentity++; },
    r => { r.recordingPixelVerdict.samples--; },
  ]) {
    const report = completeEvidence(); change(report);
    assert.equal(judgeRedesignWorkload(report).passed, false);
  }
});

test('clean video cannot conceal lost recording audio or a discarded audio clock interval', () => {
  for (const change of [
    s => { s.recording.proof.encoderQueueDroppedAudioPackets++; },
    s => { s.realtimeEvidence.audio.audioLostSamples++; },
    s => { s.realtimeEvidence.audio.pacerReanchors++; },
  ]) {
    const report = completeEvidence(); change(report.samples[10].snapshot);
    assert.equal(judgeRedesignWorkload(report).passed, false);
  }
});

test('advancing Program and ISO counters cannot hide an ISO output losing video or audio', () => {
  for (const change of [
    track => { track.droppedFrames++; },
    track => { track.droppedAudioPackets++; },
    track => { delete track.droppedFrames; },
    track => { delete track.droppedAudioPackets; },
    track => { track.readiness = 'missing'; },
  ]) {
    const report = completeEvidence();
    change(report.samples[10].snapshot.recording.streams[1]);
    assert.equal(judgeRedesignWorkload(report).passed, false);
  }
});

test('registration alone or a cached old DLL cannot qualify a camera fix', () => {
  for (const change of [
    r => { delete r.cameraInstallation; },
    r => { r.cameraInstallation.moduleInspection = 'unavailable'; },
    r => { r.cameraInstallation.loadedIdentityMatchesRegistration = false; },
    r => { r.cameraInstallation.registeredFileSha256 = 'b'.repeat(64); },
  ]) {
    const report = completeEvidence(); change(report);
    const verdict = judgeRedesignWorkload(report);
    assert.equal(verdict.passed, false);
    assert.ok(verdict.errors.some(x => x.includes('tested candidate')));
  }
});

test('a declared concurrent game must remain running throughout qualification', () => {
  const report = completeEvidence();
  report.requiredAdditionalProcesses = ['Overwatch'];
  for (const row of report.samples) row.additionalProcesses = [{ name: 'Overwatch', processGeneration: 'game:one' }];
  assert.equal(judgeRedesignWorkload(report).passed, true);
  report.samples[90].additionalProcesses = [];
  assert.equal(judgeRedesignWorkload(report).passed, false);
  report.samples[90].additionalProcesses = [{ name: 'Overwatch', processGeneration: 'game:two' }];
  assert.equal(judgeRedesignWorkload(report).passed, false);
});

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
test('a locked or unobserved desktop cannot qualify even with a visible window', () => {
  for (const unlocked of [false, undefined]) {
    const evidence = completeEvidence();
    evidence.samples[90].displayState.desktopUnlocked = unlocked;
    const verdict = judgeRedesignWorkload(evidence);
    assert.equal(verdict.passed, false);
    assert.ok(verdict.errors.some(x => x.includes('unlocked desktop')));
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
