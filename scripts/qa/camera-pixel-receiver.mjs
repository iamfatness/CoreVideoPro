import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

export function judgeCameraPixels(text, { warmupSeconds = 30, minimumSeconds = 30, startHostUs, endHostUs } = {}) {
  const errors = [], rows = [];
  let header, completion;
  for (const line of text.split(/\r?\n/).filter(Boolean)) {
    let row;
    try { row = JSON.parse(line); } catch { errors.push('Malformed evidence line.'); continue; }
    if (row.schema) {
      if (header || rows.length) errors.push('Unexpected/repeated format header.');
      header = row;
    } else if (row.complete === true) {
      if (completion) errors.push('Repeated completion record.');
      completion = row;
    } else if (Number.isSafeInteger(row.sample)) {
      if (completion) errors.push('Sample after completion.');
      rows.push(row);
    } else errors.push('Unknown evidence record.');
  }
  if (header?.schema !== 'camera-pixel-receiver-v1' || header.width !== 1920 || header.height !== 1080 ||
      header.fpsNumerator !== 60 || header.fpsDenominator !== 1) errors.push('Missing negotiated 1080p60 evidence.');
  if (!completion || completion.samples !== rows.length) errors.push('Incomplete capture.');
  if (completion?.frameFailures !== undefined &&
      (!Number.isSafeInteger(completion.frameFailures) || completion.frameFailures !== 0))
    errors.push('Receiver reported frame acquisition or evidence-capacity failures.');
  const receiverMode = header?.receiverMode ?? 'os-camera';
  if (!['os-camera', 'direct-dll'].includes(receiverMode)) errors.push('Unknown receiver mode.');
  let previous;
  for (const row of rows) {
    if (row.sample !== (previous?.sample ?? 0) + 1 || !Number.isSafeInteger(row.arrivalUs) || row.arrivalUs < 0 ||
        !Number.isSafeInteger(row.pts100ns) || row.pts100ns < 0 ||
        (previous && (row.arrivalUs <= previous.arrivalUs || row.pts100ns <= previous.pts100ns)))
      errors.push('Missing/reordered receiver samples or timestamps.');
    previous = row;
  }
  const hasWindow = startHostUs !== undefined || endHostUs !== undefined;
  let measured = rows.filter(row => row.arrivalUs >= warmupSeconds * 1e6);
  if (hasWindow) {
    if (!Number.isSafeInteger(startHostUs) || !Number.isSafeInteger(endHostUs) || startHostUs < 0 ||
        endHostUs <= startHostUs || (endHostUs - startHostUs) / 1e6 < minimumSeconds)
      errors.push('Invalid or insufficient declared receiver interval.');
    if (rows.some((row, i) => !Number.isSafeInteger(row.arrivalHostUs) || row.arrivalHostUs < 0 ||
        (i > 0 && row.arrivalHostUs <= rows[i - 1].arrivalHostUs)))
      errors.push('Missing/reordered receiver host clock.');
    // Include the adjacent samples bracketing both declared endpoints. This
    // proves continuity across the entire workload, rather than picking a
    // convenient contiguous interior or silently discarding a boundary loss.
    const before = rows.findLastIndex(row => row.arrivalHostUs <= startHostUs);
    const after = rows.findIndex(row => row.arrivalHostUs >= endHostUs);
    if (before < 0 || after < before) {
      errors.push('Receiver does not bracket the declared workload interval.');
      measured = [];
    } else measured = rows.slice(before, after + 1);
  }
  let invalid = 0, repeated = 0, missing = 0, reordered = 0;
  const intervals = [];
  previous = null;
  for (const row of measured) {
    const valid = Number.isSafeInteger(row.identity) && row.identity >= 0 && row.identity <= 0xffffffff;
    if (!valid) ++invalid;
    if (previous) {
      intervals.push((row.arrivalUs - previous.arrivalUs) / 1000);
      if (valid && Number.isSafeInteger(previous.identity)) {
        const delta = row.identity - previous.identity;
        if (delta === 0) ++repeated;
        else if (delta < 0) ++reordered;
        else missing += delta - 1;
      }
    }
    previous = row;
  }
  intervals.sort((a, b) => a - b);
  const percentile = p => intervals.length ? intervals[Math.min(intervals.length - 1, Math.floor(p * intervals.length))] : null;
  const durationSeconds = measured.length > 1 ? (measured.at(-1).arrivalUs - measured[0].arrivalUs) / 1e6 : 0;
  // One interval of tolerance only accounts for endpoint sampling, not missing frames.
  if (durationSeconds + 1 / 60 < minimumSeconds) errors.push('Insufficient measured duration.');
  // Negotiated 60/1 alone is not delivery proof: a source can increment its
  // identity once per request yet emit only 50 samples/s. Keep a one-sample
  // endpoint allowance for arrival jitter; identity continuity remains strict.
  const expectedSamples = Math.round(durationSeconds * 60) + 1;
  if (Math.abs(measured.length - expectedSamples) > 1) errors.push('Measured sample cadence differs from negotiated 60/1.');
  if (invalid) errors.push(`${invalid} undecodable/torn pixel identities.`);
  if (repeated || missing || reordered) errors.push(`Continuity failed: repeated=${repeated}, missing=${missing}, reordered=${reordered}.`);
  const maximumIntervalMs = intervals.at(-1) ?? null;
  if (maximumIntervalMs === null || maximumIntervalMs > 33.4) errors.push('Unexplained receiver interval above 33.4 ms or missing timing.');
  const anchored = Number.isSafeInteger(header?.startUnixUs) && header.startUnixUs > 0 && measured.length > 1 &&
    Number.isSafeInteger(header.startUnixUs + measured.at(-1).arrivalUs);
  const coverage = { complete: anchored && errors.length === 0,
    startTime: anchored ? new Date((header.startUnixUs + measured[0].arrivalUs) / 1000).toISOString() : null,
    endTime: anchored ? new Date((header.startUnixUs + measured.at(-1).arrivalUs) / 1000).toISOString() : null };
  return { schema: 'camera-pixel-verdict-v1', receiverPixelContinuityPassed: errors.length === 0,
    receiverScheduling: header?.receiverScheduling ?? 'ordinary',
    receiverMode, osCameraContinuityVerified: errors.length === 0 && receiverMode === 'os-camera',
    physicalDisplayVerified: false, warmupSeconds: hasWindow ? null : warmupSeconds,
    ...(hasWindow ? { measurement: { startHostUs, endHostUs, includesEndpointBrackets: true } } : {}),
    durationSeconds, samples: measured.length, coverage,
    firstIdentity: measured[0]?.identity ?? null, lastIdentity: measured.at(-1)?.identity ?? null,
    invalid, repeated, missing, reordered, p50IntervalMs: percentile(.5), p99IntervalMs: percentile(.99), maximumIntervalMs,
    errors: [...new Set(errors)], limitation: receiverMode === 'direct-dll'
      ? 'Direct DLL diagnostic pixels. Does not validate OS registration, Frame Server, Zoom display, audio synchronization or installed-camera qualification.'
      : 'Independent OS receiver pixels. Does not prove Zoom display, audio synchronization, monitor presentation or another hardware workload.' };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    if (process.argv.length !== 3) throw new Error('Usage: node scripts/qa/camera-pixel-receiver.mjs RECEIVER.ndjson');
    const report = judgeCameraPixels(readFileSync(process.argv[2], 'utf8'));
    console.log(JSON.stringify(report, null, 2));
    process.exitCode = report.receiverPixelContinuityPassed ? 0 : 1;
  } catch (error) { console.error(error.message); process.exitCode = 2; }
}
