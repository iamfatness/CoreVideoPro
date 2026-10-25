// Pure rules for scripts/validate-iso-record.mjs (no I/O; unit-tested).

// An ISO on the software encoder is a NOTE, not a failure. Only the exact
// structured admission message is waived; anything else in recording.warning fails.
export function classifyRecordingWarning(warning, isoAdmission) {
  if (!warning) return { kind: 'none', text: '' };
  if (isoAdmission?.decision === 'admit-with-warning' && typeof isoAdmission.message === 'string' &&
      isoAdmission.message.length > 0 && warning === isoAdmission.message)
    return { kind: 'note', text: warning };
  return { kind: 'failure', text: `recording.warning surfaced: ${warning}` };
}

const REASONS = {
  'hardware-capacity-exhausted': 'GPU sessions used up',
  'hardware-unavailable': 'no GPU encoder',
  'operator-selected': 'set to software',
  'hardware-requested-software-used': 'GPU encoder requested, software encoder used',
};

export function isoPlacementNotes(streams, fps) {
  const notes = [];
  for (const s of streams ?? []) {
    if (s?.kind !== 'iso' || s.encoderPath !== 'software') continue;
    const name = s.displayName ?? s.sourceId;
    const label = s.isoIndex ? `ISO-${s.isoIndex} ${name}` : `${name}`;
    const reason = s.fallbackReason ? ` (${REASONS[s.fallbackReason] ?? s.fallbackReason})` : '';
    let line = `${label} is on the CPU software encoder${reason}`;
    if (s.videoWorkUs > 0 && s.muxVideoFrameCount > 0) {
      line += ` · ${(s.videoWorkUs / s.muxVideoFrameCount / 1000).toFixed(1)} ms per frame`;
      if (fps > 0) line += ` of a ${(1000 / fps).toFixed(1)} ms budget`;
    } else line += ' · cost not measured yet';
    line += typeof s.droppedFrames === 'number' ? ` · ${s.droppedFrames} frames dropped` : ' · dropped frames not reported';
    notes.push(line);
  }
  return notes;
}

export function isoLossRegressions(baselineStreams, stopStreams) {
  const out = [];
  const base = new Map((baselineStreams ?? []).filter((s) => s.kind === 'iso').map((s) => [s.sourceId, s]));
  for (const s of (stopStreams ?? []).filter((x) => x.kind === 'iso')) {
    const b = base.get(s.sourceId);
    if (!b) { out.push(`ISO ${s.sourceId} has no steady-state baseline`); continue; }
    for (const [name, get] of [['droppedFrames', (x) => x.droppedFrames], ['writeQueue.dropped', (x) => x.writeQueue?.dropped]]) {
      const before = get(b), after = get(s);
      if (typeof before !== 'number' || typeof after !== 'number') out.push(`ISO ${s.sourceId} ${name} not reported`);
      else if (after > before) out.push(`ISO ${s.sourceId} steady-state ${name} ${before} -> ${after}`);
    }
  }
  return out;
}

// Zero tolerance (#935 review M1): any ISO frame or packet the writer dropped is
// loss, whenever it happened. The core already excludes the startup-clipped head
// (droppedFrames = dropped - startupDropped), so a nonzero count is real loss.
export function isoDropFailures(stopStreams) {
  const out = [];
  for (const s of (stopStreams ?? []).filter((x) => x?.kind === 'iso')) {
    for (const [name, value] of [['droppedFrames', s.droppedFrames], ['writeQueue.dropped', s.writeQueue?.dropped],
                                 ['writeQueue.droppedAudio', s.writeQueue?.droppedAudio]]) {
      if (name === 'writeQueue.droppedAudio' && value === undefined) continue;  // older cores never reported it
      if (typeof value !== 'number') out.push(`ISO ${s.sourceId} ${name} not reported`);
      else if (value > 0) out.push(`ISO ${s.sourceId} ${name} = ${value} (any ISO drop fails)`);
    }
  }
  return out;
}

// Owner ruling D8: Program on the software encoder is a FAIL for RC. The value is
// MEASURED (recording.encoder.encoderPath, read back from the encoder MFT the
// Program writer instantiated, #935 B1); hardwareRequested is only the request.
// Not measured is a failure, never assumed hardware.
export function programEncoderFailure(recording) {
  const path = recording?.encoder?.encoderPath;
  if (path === 'hardware') return null;
  if (path === 'software') return 'Program recorded on the CPU software encoder (Program on software is a FAIL for RC)';
  return 'Program encoder path was not measured (recording.encoder.encoderPath absent; cannot confirm hardware encoding)';
}
