import test from 'node:test';
import assert from 'node:assert/strict';
import { judgeRedesignWorkload } from './redesign-workload-verdict.mjs';

function completeEvidence() {
  const requiredInputIds = [...Array.from({ length: 8 }, (_, i) => 'zoom:' + (1000 + i)), 'capture:camera', 'capture:screen:2'];
  const requiredIsoIds = [...requiredInputIds.slice(0, 7), 'capture:camera'];
  const startTime = '2026-10-16T12:00:00Z', endTime = '2026-10-16T12:30:00Z';
  const enabledOutputs = ['recording:program', 'virtual-camera:program', ...requiredIsoIds.map(id => 'recording:iso:' + id)];
  const requiredPaths = ['cpu-submission', 'program-gpu', 'program-presentation', 'preview-presentation', 'multiview-presentation', ...enabledOutputs];
  const intervalTicks = { start: '1000000', end: String(1000000 + 1800 * 10000000), frequency: 10000000 };
  const perSample = id => id.startsWith('zoom:') ? 450 : 600;
  const isoCount = id => perSample(id) * 180 + 1;
  const sourcePictures = () => ({ schema: 'source-picture-verdict-v1', sourcePicturesVerified: true,
    startTicks: intervalTicks.start, endTicks: intervalTicks.end, qpcFrequency: intervalTicks.frequency,
    durationSeconds: 1800, samples: 108001, cameraSourceId: 'capture:camera', screenSourceId: 'capture:screen:2',
    invalid: 0, mismatched: 0, stale: 0, cameraMissing: 0, cameraReordered: 0, cameraRepeated: 0,
    cameraPermittedRateConversionRepeats: 0, cameraBodyChanges: 108000, errors: [],
    cameraRateNumerator: 60, cameraRateDenominator: 1, maximumCameraObservationAgeMs: 75,
    maximumScreenPictureAgeMs: 180, longestCameraBodyHoldMs: 0 });
  const pixelProof = sourceId => ({ schema: 'iso-pixel-arrival-verdict-v1', sourceId,
    path: 'fixture/' + sourceId + '.mp4', trace: 'fixture/arrival-trace.bin',
    startTicks: intervalTicks.start, endTicks: intervalTicks.end,
    isoPixelArrivalFidelityPassed: true, decoderComplete: true, decoderErrors: '',
    expectedSamples: isoCount(sourceId), recordedSamples: isoCount(sourceId),
    invalid: 0, rangeViolations: 0, firstMismatchIndex: null,
    coverage: { sourceEpoch: 1, durationSeconds: 1800 } });
  return { startTime, endTime, requiredInputIds, requiredIsoIds, monitorProbeVerdict: { functionalPassed: true },
    launchEvidence: { installed: true, method: 'desktop-shortcut', elevated: false, processGeneration: 'one',
      priorityClass: 'Normal', receiverPriorityClass: 'Normal' },
    sourcePictureVerdict: { osCamera: sourcePictures(), recording: sourcePictures() },
    qpcFrequency: intervalTicks.frequency,
    isoFileEvidence: requiredIsoIds.map(sourceId => ({ sourceId, path: 'fixture/' + sourceId + '.mp4',
      measurement: 'ffprobe-count-packets', width: 1920, height: 1080, packets: isoCount(sourceId), muxVideoFrameCount: isoCount(sourceId) })),
    candidateCameraSha256: 'a'.repeat(64),
    cameraInstallation: { moduleInspection: 'observed', loadedIdentityMatchesRegistration: true,
      registeredFileMatchesManifest: true, registeredFileSha256: 'a'.repeat(64) },
    cameraPixelVerdict: { osCameraContinuityVerified: true, durationSeconds: 1800,
      firstIdentity: 1, lastIdentity: 108001, samples: 108001 },
    recordingPixelVerdict: { recordedPixelContinuityPassed: true, identityRangeComplete: true,
      firstIdentity: 1, lastIdentity: 108001, samples: 108001 },
    cameraPixelCoverage: { complete: true, startTime, endTime },
    samples: Array.from({ length: 181 }, (_, i) => ({ collectedAtMs: i * 10000, qpcTicks: String(1000000 + i * 100000000), processGeneration: 'one',
      displayState: { visible: true, minimized: false, desktopUnlocked: true }, snapshot: {
      outputProfile: { width: 1920, height: 1080, fps: 60 }, meetingState: 'in_meeting',
      captureDevices: [{ isoCpuArrivals: { sourceId: 'capture:camera', demanded: true,
        observed: i * 600 + 1, accepted: i * 600 + 1, drained: i * 600 + 1,
        overflowed: 0, released: 0, pending: 0, bytes: 0 } }],
      zoomSubscriptionChurn: { sources: requiredIsoIds.filter(id => id.startsWith('zoom:')).map(sourceId => ({
        isoCpuArrivals: { sourceId, demanded: true, observed: i * perSample(sourceId) + 1, accepted: i * perSample(sourceId) + 1,
          drained: i * perSample(sourceId) + 1, overflowed: 0, released: 0, pending: 0, bytes: 0 },
        isoRing: { observed: true, epoch: 1, written: i * perSample(sourceId) + 1, next: i * perSample(sourceId) + 1, lost: 0 },
      })) },
      realtimeEvidence: { audio: { audioLostSamples: 0, pacerReanchors: 0 }, monitorWorker: { inputs: { sources: requiredInputIds.map(sourceId => ({ sourceId, state: 'ready', frameId: i * 600 + 1 })) } } },
      recording: { active: true, lifecycle: { state: 'producing' },
        streams: [{ kind: 'program', framesWritten: i * 600 + 1 }, ...requiredIsoIds.map(sourceId => ({ sourceId, path: 'fixture/' + sourceId + '.mp4', framesWritten: i * perSample(sourceId) + 1, readiness: 'ready', droppedFrames: 0, droppedAudioPackets: 0 }))],
        proof: { programMissingFrames: 0, encoderQueueDroppedVideoFrames: 0, encoderQueueDroppedAudioPackets: 0 } },
      programBuffer: { underruns: 0 },
      virtualCamera: { enabled: true, status: 'live', framesPublished: i * 600 + 1, pendingFramesReplaced: 0, publicationExceptions: 0 },
    } })),
    framePerformanceReport: { startTime, endTime, soakPassed: true, errorMatches: [], performanceEvidence: {
      metricVersion: 'anchored-deadline-v1', targetFps: 60, clock: 'monotonic', requiredPaths, enabledOutputs, intervalTicks,
      paths: requiredPaths.map(id => id.startsWith('recording:iso:') ? ({ id, measurement: 'source-arrival-fidelity',
        coverage: { complete: true, startTime, endTime }, proof: pixelProof(id.slice('recording:iso:'.length)) }) : ({ id, measurement: 'completion-deadlines', errors: [],
        coverage: { complete: true, startTime, endTime }, firstSlot: 1, lastSlot: 108000, expectedSlots: 108000,
        completedSlots: 108000, uniqueCompletedSlots: 108000, deadlineMisses: 0, skippedSlots: 0 })),
    } },
  };
}

test('healthy output counters cannot qualify missing, stale or incomplete source pictures', () => {
  for (const destination of ['osCamera', 'recording']) {
    for (const mutate of [
      report => { delete report.sourcePictureVerdict; },
      report => { report.sourcePictureVerdict[destination].sourcePicturesVerified = false; },
      report => { report.sourcePictureVerdict[destination].cameraMissing = 142; },
      report => { report.sourcePictureVerdict[destination].cameraRepeated = 147; },
      report => { report.sourcePictureVerdict[destination].cameraPermittedRateConversionRepeats = 1000; },
      report => { report.sourcePictureVerdict[destination].maximumScreenPictureAgeMs = 217; },
      report => { report.sourcePictureVerdict[destination].stale = 82; },
      report => { report.sourcePictureVerdict[destination].startTicks = '999999'; },
      report => { report.sourcePictureVerdict[destination].durationSeconds = 65; },
      report => { report.sourcePictureVerdict[destination].cameraBodyChanges = 0; },
    ]) {
      const report = completeEvidence(); mutate(report);
      assert.equal(judgeRedesignWorkload(report).passed, false);
    }
  }
});

test('ISO arrival fidelity requires decoded pictures over the exact workload clock interval', () => {
  for (const change of [
    path => { path.measurement = 'completion-deadlines'; },
    path => { path.proof.isoPixelArrivalFidelityPassed = false; },
    path => { path.proof.recordedSamples--; },
    path => { path.proof.startTicks = '999999'; },
    path => { path.proof.path = 'another-recording.mp4'; },
  ]) {
    const report = completeEvidence();
    const path = report.framePerformanceReport.performanceEvidence.paths.find(path => path.id.startsWith('recording:iso:'));
    change(path);
    assert.equal(judgeRedesignWorkload(report).passed, false);
  }
});

test('qualification rejects a mismatched launch or background/boosted process priorities', () => {
  for (const change of [
    r => { delete r.launchEvidence; },
    r => { r.launchEvidence.installed = false; },
    r => { r.launchEvidence.method = 'development-executable'; },
    r => { r.launchEvidence.elevated = true; },
    r => { r.launchEvidence.processGeneration = 'another-run'; },
    r => { r.launchEvidence.priorityClass = 'BelowNormal'; },
    r => { r.launchEvidence.priorityClass = 'High'; },
    r => { r.launchEvidence.receiverPriorityClass = 'BelowNormal'; },
    r => { r.launchEvidence.receiverPriorityClass = 'High'; },
  ]) {
    const report = completeEvidence(); change(report);
    const verdict = judgeRedesignWorkload(report);
    assert.equal(verdict.passed, false);
    assert.ok(verdict.errors.some(error => error.includes('observed Normal process priority')));
  }
});

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
