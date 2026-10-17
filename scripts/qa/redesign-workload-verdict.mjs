import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { validateFramePerformance } from './validate-frame-performance.mjs';

// This gate consumes observations, never the successful return of a start command.
export function judgeRedesignWorkload(report) {
  const errors = [];
  const rows = report?.samples;
  const inputs = report?.requiredInputIds;
  const iso = report?.requiredIsoIds;
  const additional = report?.requiredAdditionalProcesses ?? [];
  if (!Array.isArray(additional) || additional.some(x => typeof x !== 'string' || !x))
    errors.push('Additional workload process names must be an array of nonempty names.');
  if (!Array.isArray(inputs) || inputs.length !== 10 || new Set(inputs).size !== 10)
    errors.push('Declare the eight remote inputs, webcam and screen source IDs.');
  if (!Array.isArray(iso) || new Set(iso).size !== iso.length)
    errors.push('Declare the actual selected ISO outputs.');
  if (!Array.isArray(rows) || rows.length < 2 || rows.length > 100000)
    errors.push('Bounded workload samples are required.');
  const samples = Array.isArray(rows) && rows.length <= 100000 ? rows : [];
  const counters = new Map();
  const progress = new Map();
  const additionalGenerations = new Map();
  let previous;
  for (const [index, row] of samples.entries()) {
    const s = row?.snapshot ?? {};
    if (!Number.isFinite(row?.collectedAtMs) || !row?.processGeneration)
      errors.push(`${index}: missing collector time/process identity.`);
    if (previous && (row?.processGeneration !== previous.processGeneration ||
        row?.collectedAtMs <= previous.collectedAtMs || row?.collectedAtMs - previous.collectedAtMs > 10000))
      errors.push(`${index}: process changed or telemetry coverage broke.`);
    previous = row ?? {};
    for (const name of Array.isArray(additional) ? additional : []) {
      const observed = Array.isArray(row?.additionalProcesses) ? row.additionalProcesses.filter(x => x?.name === name) : [];
      if (observed.length !== 1 || !observed[0].processGeneration ||
          (additionalGenerations.has(name) && additionalGenerations.get(name) !== observed[0].processGeneration))
        errors.push(`${index}: required additional workload ${name} stopped or restarted.`);
      else additionalGenerations.set(name, observed[0].processGeneration);
    }
    if (row?.displayState?.visible !== true || row?.displayState?.minimized !== false)
      errors.push(`${index}: visible, restored monitor window was not observed.`);
    if (s.outputProfile?.width !== 1920 || s.outputProfile?.height !== 1080 || s.outputProfile?.fps !== 60)
      errors.push(`${index}: output is not 1080p60.`);
    if (s.meetingState !== 'in_meeting') errors.push(`${index}: real meeting is not connected.`);
    const ready = new Set(s.realtimeEvidence?.monitorWorker?.inputs?.sources
      ?.filter(x => x.state === 'ready').map(x => x.sourceId) ?? []);
    if (Array.isArray(inputs) && inputs.some(id => !ready.has(id))) errors.push(`${index}: a required input is not ready.`);
    if (s.recording?.active !== true || s.recording?.lifecycle?.state !== 'producing')
      errors.push(`${index}: recording is not producing.`);
    const streams = s.recording?.streams ?? [];
    if (!streams.some(x => x.kind === 'program' && x.framesWritten > 0) ||
        (Array.isArray(iso) && iso.some(id => !streams.some(x => x.sourceId === id && x.framesWritten > 0))))
      errors.push(`${index}: required recording tracks have no written frames.`);
    if (s.virtualCamera?.enabled !== true || s.virtualCamera?.status !== 'live')
      errors.push(`${index}: virtual camera is not live.`);
    const advancing = {
      camera: s.virtualCamera?.framesPublished,
      ...Object.fromEntries((Array.isArray(inputs) ? inputs : []).map(id =>
        ['input:' + id, s.realtimeEvidence?.monitorWorker?.inputs?.sources?.find(x => x.sourceId === id)?.frameId])),
      ...Object.fromEntries(['program', ...(Array.isArray(iso) ? iso : [])].map(id =>
        ['recording:' + id, streams.find(x => id === 'program' ? x.kind === 'program' : x.sourceId === id)?.framesWritten])),
    };
    for (const [name, value] of Object.entries(advancing)) {
      if (!Number.isSafeInteger(value) || value < 0 || (progress.has(name) && value <= progress.get(name)))
        errors.push(`${index}: ${name} did not advance or lacks frame evidence.`);
      progress.set(name, value);
    }
    const metrics = {
      recordingMissing: s.recording?.proof?.programMissingFrames,
      recordingQueueDropped: s.recording?.proof?.encoderQueueDroppedVideoFrames,
      recordingAudioDropped: s.recording?.proof?.encoderQueueDroppedAudioPackets,
      audioLostSamples: s.realtimeEvidence?.audio?.audioLostSamples,
      audioReanchors: s.realtimeEvidence?.audio?.pacerReanchors,
      bufferUnderruns: s.programBuffer?.underruns,
      cameraReplaced: s.virtualCamera?.pendingFramesReplaced,
      cameraExceptions: s.virtualCamera?.publicationExceptions,
    };
    for (const [name, value] of Object.entries(metrics)) {
      if (!Number.isSafeInteger(value) || value < 0) errors.push(`${index}: missing ${name}.`);
      else if (counters.has(name) && value !== counters.get(name))
        errors.push(`${index}: ${name} increased or reset.`);
      counters.set(name, value);
    }
  }
  const durationSeconds = samples.length > 1 ? (samples.at(-1)?.collectedAtMs - samples[0]?.collectedAtMs) / 1000 : 0;
  if (!Number.isFinite(durationSeconds) || durationSeconds < 1800) errors.push('Less than 30 minutes of simultaneous full-workload coverage.');
  const frameVerdict = validateFramePerformance(report?.framePerformanceReport);
  const start = Date.parse(report?.startTime), end = Date.parse(report?.endTime);
  const frameStart = Date.parse(report?.framePerformanceReport?.startTime);
  const frameEnd = Date.parse(report?.framePerformanceReport?.endTime);
  if (![start, end, frameStart, frameEnd].every(Number.isFinite) || end <= start ||
      Math.abs((end - start) / 1000 - durationSeconds) > 1 || frameStart > start || frameEnd < end)
    errors.push('Frame evidence does not cover the same measured workload interval.');
  if (report?.errors?.length) errors.push('Collector reported errors.');
  if (!frameVerdict.framePerformancePassed) errors.push('Per-frame GPU, display and output evidence did not pass.');
  const evidence = report?.framePerformanceReport?.performanceEvidence;
  const expectedOutputs = ['recording:program', 'virtual-camera:program', ...(Array.isArray(iso) ? iso.map(id => 'recording:iso:' + id) : [])];
  if (expectedOutputs.some(id => !evidence?.enabledOutputs?.includes(id)))
    errors.push('Frame evidence omits an enabled output.');
  if (['preview-presentation', 'multiview-presentation'].some(id => !evidence?.requiredPaths?.includes(id)))
    errors.push('Frame evidence omits Preview or multiview display.');
  if (report?.cameraPixelVerdict?.osCameraContinuityVerified !== true)
    errors.push('Independent Windows camera pixel continuity did not pass.');
  const cameraIdentity = report?.cameraInstallation;
  if (cameraIdentity?.moduleInspection !== 'observed' ||
      cameraIdentity?.loadedIdentityMatchesRegistration !== true ||
      cameraIdentity?.registeredFileMatchesManifest !== true ||
      !/^[a-f0-9]{64}$/i.test(report?.candidateCameraSha256 ?? '') ||
      cameraIdentity?.registeredFileSha256?.toLowerCase() !== report?.candidateCameraSha256?.toLowerCase())
    errors.push('The loaded Windows camera DLL was not verified as the tested candidate.');
  if (!(report?.cameraPixelVerdict?.durationSeconds >= durationSeconds))
    errors.push('Camera receiver does not cover the workload duration.');
  const cameraStart = Date.parse(report?.cameraPixelCoverage?.startTime);
  const cameraEnd = Date.parse(report?.cameraPixelCoverage?.endTime);
  if (report?.cameraPixelCoverage?.complete !== true ||
      ![cameraStart, cameraEnd].every(Number.isFinite) || cameraStart > start || cameraEnd < end)
    errors.push('Camera pixel evidence does not cover the same measured workload interval.');
  if (report?.monitorProbeVerdict?.functionalPassed !== true)
    errors.push('Blocked-UI video regression probe did not pass.');
  return { schema: 'redesign-workload-verdict-v1', passed: errors.length === 0,
    durationSeconds, errors: [...new Set(errors)], frameVerdict };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    if (process.argv.length !== 3) throw new Error('Usage: node scripts/qa/redesign-workload-verdict.mjs REPORT.json');
    const result = judgeRedesignWorkload(JSON.parse(readFileSync(process.argv[2], 'utf8')));
    console.log(JSON.stringify(result, null, 2)); process.exitCode = result.passed ? 0 : 1;
  } catch (error) { console.error(error.message); process.exitCode = 2; }
}
