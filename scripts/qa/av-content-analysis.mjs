const median = values => {
  const sorted = [...values].sort((a, b) => a - b);
  return sorted.length % 2 ? sorted[(sorted.length - 1) / 2] : (sorted[sorted.length / 2 - 1] + sorted[sorted.length / 2]) / 2;
};

export function summarizePacketTiming(packets) {
  const timestamp = value => value === undefined || value === 'N/A' ? null : Number(value);
  const values = packets.map(packet => ({ pts: timestamp(packet.pts_time), dts: timestamp(packet.dts_time),
    duration: timestamp(packet.duration_time), flags: packet.flags ?? null, bytes: Number(packet.size) || null }));
  const keys = values.filter(packet => packet.flags?.includes('K'));
  return { count: values.length, firstPackets: values.slice(0, 8), lastPackets: values.slice(-4), keyPacketCount: keys.length,
    keyPackets: keys.slice(0, 64), firstKeyPacketPts: keys[0]?.pts ?? null,
    negativePtsPackets: values.filter(packet => Number.isFinite(packet.pts) && packet.pts < 0).length,
    negativeDtsPackets: values.filter(packet => Number.isFinite(packet.dts) && packet.dts < 0).length };
}

export function assessRecordingVideoEvidence(decoded, proof) {
  const errors = [], accepted = proof?.programFrameCount;
  const counters = ['recordingVideoPrerollFrameCount', 'recordingVideoTailFrameCount', 'recordingMuxVideoFrameCount',
    'recordingAudioPrerollSampleCount', 'recordingAudioTailSampleCount'];
  for (const key of counters) if (!Number.isSafeInteger(proof?.[key]) || proof[key] < 0) errors.push(`Missing or invalid explicit ${key}.`);
  const requestedAt = proof?.recordingRequestedAt100ns, readyAt = proof?.recordingWriterReadyAt100ns,
    epoch = proof?.recordingMuxEpoch100ns, startupDroppedAudioPackets = proof?.recordingStartupDroppedAudioPackets;
  if (startupDroppedAudioPackets !== 0) errors.push('Missing or nonzero recordingStartupDroppedAudioPackets.');
  if (![requestedAt, readyAt, epoch].every(value => Number.isSafeInteger(value) && value >= 0)
      || requestedAt > readyAt || readyAt > epoch) errors.push('Missing or invalid requested <= writer-ready <= mux-epoch startup timestamps.');
  const preroll = proof?.recordingVideoPrerollFrameCount, tail = proof?.recordingVideoTailFrameCount;
  const mux = proof?.recordingMuxVideoFrameCount;
  if (!Number.isSafeInteger(accepted) || accepted <= 0) errors.push('Missing positive native accepted video frame count.');
  if (!Number.isSafeInteger(mux) || mux !== accepted + preroll + tail) errors.push('Real frames plus explicit preroll/tail do not equal mux frames.');
  for (const key of ['videoPacketCount', 'decodedVideoFrames', 'rawDecodedVideoFrames']) {
    if (!Number.isSafeInteger(decoded?.[key]) || decoded[key] !== mux) errors.push(`${key} does not match explicit native mux frame count.`);
  }
  if (decoded?.videoPackets?.count !== mux) errors.push('Enumerated video packet count does not match explicit native mux frame count.');
  const first = decoded?.videoPackets?.firstPackets?.[0];
  if (!first?.flags?.includes('K')) errors.push('First video packet is not identified as a keyframe.');
  if (!Number.isFinite(first?.pts) || first.pts < 0 || decoded?.videoPackets?.negativePtsPackets !== 0)
    errors.push('Missing or negative video packet PTS evidence.');
  const timing = {};
  const timestamps = decoded?.videoFramePts;
  if (errors.length === 0 && Array.isArray(timestamps) && timestamps.length === mux && timestamps.every(Number.isFinite)
      && timestamps.every((pts, index) => !index || pts > timestamps[index - 1])) {
    const groups = { startup: [], steadyRealFrames: [], tail: [] };
    for (let index = 1; index < timestamps.length; index++) {
      const group = index <= preroll ? groups.startup : index >= preroll + accepted ? groups.tail : groups.steadyRealFrames;
      group.push((timestamps[index] - timestamps[index - 1]) * 1000);
    }
    for (const [name, intervals] of Object.entries(groups)) timing[name] = { intervalCount: intervals.length,
      worstIntervalMs: intervals.length ? Math.max(...intervals) : null };
  }
  return { acceptedNativeFrames: accepted ?? null, muxFrames: mux ?? null, videoPrerollFrames: preroll ?? null,
    videoTailFrames: tail ?? null, audioPrerollSamples: proof?.recordingAudioPrerollSampleCount ?? null,
    audioTailSamples: proof?.recordingAudioTailSampleCount ?? null, timing,
    startup: { requestedAt100ns: requestedAt ?? null, writerReadyAt100ns: readyAt ?? null, muxEpoch100ns: epoch ?? null,
      droppedAudioPackets: startupDroppedAudioPackets ?? null,
      writerPreparationMs: Number.isSafeInteger(readyAt) && Number.isSafeInteger(requestedAt) ? (readyAt - requestedAt) / 10000 : null,
      requestToMuxEpochMs: Number.isSafeInteger(epoch) && Number.isSafeInteger(requestedAt) ? (epoch - requestedAt) / 10000 : null },
    limitation: 'Synthetic padding is not a real rendered frame. Count reconciliation does not establish 60 fps or presentation performance.',
    framePerformancePassed: false, passed: errors.length === 0, errors };
}

// Unique durations identify each event independently in audio and video. Unequal
// onset spacing also prevents a one-second slip from resembling the same signal.
export const FLASH_BEEP_PULSES = [60, 90, 132, 180, 234, 294, 360, 432].map((startFrame, index) =>
  ({ id: `pulse-${index}`, startFrame, durationFrames: (index + 1) * 6 }));

export function validateAudioTimeline(log, decodedSamples) {
  const frames = [...log.matchAll(/\bn:(\d+)\s+pts:(-?\d+)\s+pts_time:([^\s]+).*?rate:(\d+)\s+nb_samples:(\d+)/g)]
    .map(match => ({ index: Number(match[1]), pts: Number(match[2]), ptsTime: Number(match[3]), rate: Number(match[4]), samples: Number(match[5]) }));
  if (!frames.length) throw new Error('Missing decoded audio frame timestamps.');
  let total = 0;
  for (let index = 0; index < frames.length; ++index) {
    const frame = frames[index], previous = frames[index - 1];
    if (frame.index !== index || frame.rate !== 48000 || !Number.isSafeInteger(frame.pts) ||
        !Number.isSafeInteger(frame.samples) || frame.samples <= 0 || !Number.isFinite(frame.ptsTime) ||
        Math.abs(frame.ptsTime - frame.pts / 48000) > 0.00002)
      throw new Error('Invalid decoded audio timeline metadata.');
    if (previous && frame.pts !== previous.pts + previous.samples) throw new Error('Decoded audio PTS discontinuity; contiguous sample-index timing would be false.');
    total += frame.samples;
  }
  if (total !== decodedSamples) throw new Error('Audio timestamp coverage does not match decoded sample count.');
  return { firstPts: frames[0].pts / 48000, frames: frames.length, samples: total, contiguous: true };
}

export function detectFlashPulses(grayFrames, timestamps) {
  detectFlashes(grayFrames, timestamps); // validates every timestamp and sample count
  const pulses = [];
  let start = grayFrames[0] >= 180 ? 0 : -1;
  for (let index = 1; index < grayFrames.length; ++index) {
    if (grayFrames[index] >= 180 && start < 0) start = index;
    if (grayFrames[index] < 180 && start >= 0) {
      if (start > 0) pulses.push({ startPts: timestamps[start], duration: timestamps[index] - timestamps[start] });
      start = -1;
    }
  }
  return pulses;
}

export function detectBeepPulses(samples, firstPts) {
  detectBeeps(samples, firstPts); // validates finite samples and non-silent signal
  let peak = 0; for (const sample of samples) peak = Math.max(peak, Math.abs(sample));
  const pulses = []; let start = -1, last = -1;
  for (let index = 0; index < samples.length; ++index) {
    if (Math.abs(samples[index]) >= peak * 0.25) {
      if (start < 0) start = index;
      last = index;
    } else if (start >= 0 && index - last > 4800) {
      if (start > 0) pulses.push({ startPts: firstPts + start / 48000, duration: (last - start + 1) / 48000 });
      start = -1;
    }
  }
  return pulses;
}

export function alignIdentifiedPulses(flashes, beeps) {
  const identify = pulses => pulses.map(pulse => {
    const matches = FLASH_BEEP_PULSES.filter(expected => Math.abs(pulse.duration - expected.durationFrames / 60) <= 1 / 30);
    return { ...pulse, id: matches.length === 1 ? matches[0].id : null };
  });
  const video = identify(flashes), audio = identify(beeps), pairs = [];
  for (const expected of FLASH_BEEP_PULSES) {
    const v = video.filter(pulse => pulse.id === expected.id), a = audio.filter(pulse => pulse.id === expected.id);
    if (v.length === 1 && a.length === 1) pairs.push({ pulseId: expected.id, videoPts: v[0].startPts, audioPts: a[0].startPts,
      audioMinusVideoMs: (a[0].startPts - v[0].startPts) * 1000 });
  }
  if (!pairs.length) throw new Error('No uniquely identified flash/beep pairs; alignment remains unmeasured.');
  const unmatchedVideo = video.filter(pulse => !pairs.some(pair => pair.pulseId === pulse.id));
  const unmatchedAudio = audio.filter(pulse => !pairs.some(pair => pair.pulseId === pulse.id));
  const first = FLASH_BEEP_PULSES.findIndex(p => p.id === pairs[0].pulseId), last = FLASH_BEEP_PULSES.findIndex(p => p.id === pairs.at(-1).pulseId);
  const missingInteriorIds = FLASH_BEEP_PULSES.slice(first, last + 1).filter(p => !pairs.some(pair => pair.pulseId === p.id)).map(p => p.id);
  const inside = (pulse, key) => pulse.startPts > pairs[0][key] && pulse.startPts < pairs.at(-1)[key];
  const offsets = pairs.map(pair => pair.audioMinusVideoMs);
  return { pairs, medianAudioMinusVideoMs: median(offsets), minAudioMinusVideoMs: Math.min(...offsets), maxAudioMinusVideoMs: Math.max(...offsets),
    sufficientPairs: pairs.length >= 4,
    unmatchedVideo, unmatchedAudio, missingInteriorIds,
    interiorCoverageComplete: !missingInteriorIds.length && !unmatchedVideo.some(p => inside(p, 'videoPts')) && !unmatchedAudio.some(p => inside(p, 'audioPts')),
    identification: 'Each pulse has a unique duration spaced 100 ms apart; duration classification permits two frames of onset/end quantization. Alignment threshold remains one frame.',
    limitation: 'Decoded content timing, not physical presentation. Incomplete leading/trailing pulses are excluded; missing interior pulses prevent alignment acceptance.' };
}

export function detectFlashes(grayFrames, timestamps) {
  if (grayFrames.length !== timestamps.length || !timestamps.length) throw new Error('Decoded video and frame PTS counts differ or are empty.');
  if (!Number.isFinite(timestamps[0])) throw new Error('First video frame PTS is invalid.');
  const flashes = [];
  for (let index = 1; index < grayFrames.length; ++index) {
    if (!Number.isFinite(timestamps[index]) || timestamps[index] <= timestamps[index - 1]) throw new Error('Video frame PTS is invalid or non-increasing.');
    if (grayFrames[index] >= 180 && grayFrames[index - 1] < 180) flashes.push(timestamps[index]);
  }
  return flashes;
}

export function detectBeeps(samples, firstPts, sampleRate = 48000) {
  if (!samples.length || !Number.isFinite(firstPts)) throw new Error('Missing decoded audio or audio PTS.');
  let peak = 0;
  for (const value of samples) { if (!Number.isFinite(value)) throw new Error('Non-finite decoded audio.'); peak = Math.max(peak, Math.abs(value)); }
  if (peak < 0.01) throw new Error('Decoded audio is silent or below the fixture detection floor.');
  const events = [];
  let lastSignal = -Infinity;
  for (let index = 0; index < samples.length; ++index) {
    if (Math.abs(samples[index]) < peak * 0.25) continue;
    if (index - lastSignal > sampleRate * 0.2) events.push(firstPts + index / sampleRate);
    lastSignal = index;
  }
  return events;
}

export function alignEvents(flashes, beeps) {
  const pairs = [], used = new Set();
  for (const videoPts of flashes) {
    let best = -1, distance = 0.25;
    for (let index = 0; index < beeps.length; ++index) {
      if (!used.has(index) && Math.abs(beeps[index] - videoPts) < distance) { best = index; distance = Math.abs(beeps[index] - videoPts); }
    }
    if (best >= 0) { used.add(best); pairs.push({ videoPts, audioPts: beeps[best], audioMinusVideoMs: (beeps[best] - videoPts) * 1000 }); }
  }
  if (pairs.length < 4) throw new Error('Fewer than four matched flash/beep events; alignment remains unmeasured.');
  const offsets = pairs.map(pair => pair.audioMinusVideoMs);
  const unmatchedFlashPts = flashes.filter(pts => !pairs.some(pair => pair.videoPts === pts));
  const unmatchedBeepPts = beeps.filter((_, index) => !used.has(index));
  // A recording may begin/end partway through a pulse. Missing pulses strictly
  // between measured matched events cannot be explained by those boundaries.
  const firstMatched = Math.min(pairs[0].videoPts, pairs[0].audioPts);
  const lastMatched = Math.max(pairs.at(-1).videoPts, pairs.at(-1).audioPts);
  const interior = pts => pts > firstMatched && pts < lastMatched;
  const interiorUnmatchedFlashPts = unmatchedFlashPts.filter(interior);
  const interiorUnmatchedBeepPts = unmatchedBeepPts.filter(interior);
  return { pairs, medianAudioMinusVideoMs: median(offsets), minAudioMinusVideoMs: Math.min(...offsets), maxAudioMinusVideoMs: Math.max(...offsets),
    unmatchedFlashes: flashes.length - pairs.length, unmatchedBeeps: beeps.length - pairs.length,
    unmatchedFlashPts, unmatchedBeepPts, interiorUnmatchedFlashPts, interiorUnmatchedBeepPts,
    interiorCoverageComplete: interiorUnmatchedFlashPts.length === 0 && interiorUnmatchedBeepPts.length === 0,
    limitation: 'Content alignment uses nearest pulse within 250 ms; larger offsets cannot be identified. Video onset precision is one frame; audio threshold onset precision is one sample plus codec ringing.' };
}

export function sourceCorrectedAlignment(measured, reference) {
  const pairs = measured.pairs.map(pair => ({ ...pair,
    sourceCorrectedAudioMinusVideoMs: pair.audioMinusVideoMs - (pair.pulseId
      ? reference.pairs.find(source => source.pulseId === pair.pulseId)?.audioMinusVideoMs ?? NaN
      : reference.medianAudioMinusVideoMs) }));
  const coverageComplete = measured.interiorCoverageComplete === true && reference.interiorCoverageComplete === true &&
    measured.sufficientPairs !== false && reference.sufficientPairs !== false;
  return { pairs, medianAudioMinusVideoMs: median(pairs.map(pair => pair.sourceCorrectedAudioMinusVideoMs)),
    maxAbsoluteAudioMinusVideoMs: Math.max(...pairs.map(pair => Math.abs(pair.sourceCorrectedAudioMinusVideoMs))),
    coverage: coverageComplete ? 'matched-interior-events' : 'incomplete',
    alignmentWithinOneVideoFrame: coverageComplete && pairs.length >= 4 && pairs.every(pair => Math.abs(pair.sourceCorrectedAudioMinusVideoMs) <= 1000 / 60) };
}
