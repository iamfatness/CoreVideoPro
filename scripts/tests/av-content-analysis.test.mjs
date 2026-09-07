import test from 'node:test';
import assert from 'node:assert/strict';
import { FLASH_BEEP_PULSES, detectFlashes, detectBeeps, alignEvents, alignIdentifiedPulses, validateAudioTimeline, sourceCorrectedAlignment, summarizePacketTiming, assessRecordingVideoEvidence } from '../qa/av-content-analysis.mjs';

test('flash detection uses decoded PTS and ignores an already-white first frame', () => {
  assert.deepEqual(detectFlashes([255, 0, 255, 255, 0, 255], [0, 1, 2, 3, 4, 5]), [2, 5]);
  assert.throws(() => detectFlashes([0, 255], [0, 0]), /PTS/);
  assert.throws(() => detectFlashes([0, 255], [0]), /counts/);
  assert.throws(() => detectFlashes([0, 255], [NaN, 1]), /PTS/);
});

test('one outlier cannot hide behind a passing median', () => {
  const flashes = [1, 2, 3, 4, 5], reference = alignEvents(flashes, flashes);
  const measured = alignEvents(flashes, [1, 2, 3.05, 4, 5]);
  const result = sourceCorrectedAlignment(measured, reference);
  assert.equal(result.medianAudioMinusVideoMs, 0);
  assert.equal(result.alignmentWithinOneVideoFrame, false);
  assert.ok(result.maxAbsoluteAudioMinusVideoMs > 49);
});

test('unmatched interior pulse makes otherwise aligned evidence incomplete', () => {
  const flashes = [1, 2, 3, 4, 5, 6], reference = alignEvents(flashes, flashes);
  const measured = alignEvents(flashes, [1, 2, 4, 5, 6]);
  assert.deepEqual(measured.interiorUnmatchedFlashPts, [3]);
  const result = sourceCorrectedAlignment(measured, reference);
  assert.equal(result.coverage, 'incomplete');
  assert.equal(result.alignmentWithinOneVideoFrame, false);
});

test('beep detection measures signal onset rather than continuous sine peaks', () => {
  const pcm = new Float32Array(1000);
  pcm.fill(0.5, 100, 130); pcm.fill(-0.5, 600, 630);
  assert.deepEqual(detectBeeps(pcm, 2, 1000), [2.1, 2.6]);
  assert.throws(() => detectBeeps(new Float32Array(100), 0), /silent/);
});

test('alignment reports positive and negative content offsets without relabeling them as presentation proof', () => {
  const flashes = [1, 2, 3, 4, 5];
  assert.ok(Math.abs(alignEvents(flashes, flashes.map(t => t + 0.05)).medianAudioMinusVideoMs - 50) < 1e-9);
  assert.ok(Math.abs(alignEvents(flashes, flashes.map(t => t - 0.033)).medianAudioMinusVideoMs + 33) < 1e-9);
  assert.throws(() => alignEvents(flashes, flashes.map(t => t + 0.4)), /unmeasured/);
  assert.throws(() => alignEvents([1, 2, 3], [1, 2, 3]), /unmeasured/);
});

test('unique pulse identities reveal an exact one-second slip instead of matching adjacent events', () => {
  const pulses = FLASH_BEEP_PULSES.map(pulse => ({ startPts: pulse.startFrame / 60, duration: pulse.durationFrames / 60 }));
  const reference = alignIdentifiedPulses(pulses, pulses);
  const measured = alignIdentifiedPulses(pulses, pulses.map(pulse => ({ ...pulse, startPts: pulse.startPts + 1 })));
  const result = sourceCorrectedAlignment(measured, reference);
  assert.equal(result.alignmentWithinOneVideoFrame, false);
  assert.ok(Math.abs(result.medianAudioMinusVideoMs - 1000) < 1e-9);
});

test('unique sequence marks a pulse missing in both tracks as incomplete', () => {
  const pulses = FLASH_BEEP_PULSES.map(pulse => ({ startPts: pulse.startFrame / 60, duration: pulse.durationFrames / 60 }));
  const missing = pulses.filter((_, index) => index !== 3);
  const measured = alignIdentifiedPulses(missing, missing);
  assert.equal(measured.interiorCoverageComplete, false);
  assert.deepEqual(measured.missingInteriorIds, ['pulse-3']);
});

test('decoded audio validates every frame timestamp and decoded sample coverage', () => {
  const line = (n, pts, samples = 960) => `[ashowinfo] n:${n} pts:${pts} pts_time:${pts / 48000} pos:0 fmt:flt channels:1 chlayout:mono rate:48000 nb_samples:${samples}`;
  const valid = `${line(0, 4800)}\n${line(1, 5760)}`;
  assert.deepEqual(validateAudioTimeline(valid, 1920), { firstPts: 0.1, frames: 2, samples: 1920, contiguous: true });
  assert.throws(() => validateAudioTimeline(`${line(0, 4800)}\n${line(1, 6000)}`, 1920), /discontinuity/);
  assert.throws(() => validateAudioTimeline(`${line(0, 4800)}\n${line(1, 5700)}`, 1920), /discontinuity/);
  assert.throws(() => validateAudioTimeline(valid, 2000), /coverage/);
});

test('packet diagnostics expose a non-keyframe start and retained negative timestamps', () => {
  const packets = summarizePacketTiming([
    { pts_time: '-0.1', dts_time: '-0.1', flags: '__', size: '50' },
    { pts_time: '0.9', dts_time: '0.9', flags: 'K_', size: '500' }
  ]);
  assert.equal(packets.count, 2);
  assert.equal(packets.firstKeyPacketPts, 0.9);
  assert.equal(packets.negativePtsPackets, 1);
  assert.equal(packets.firstPackets[0].pts, -0.1);
});

test('recording gate requires first keyframe, nonnegative PTS, and accepted/packet/decoded counts to agree', () => {
  const decoded = { videoPacketCount: 416, decodedVideoFrames: 416, rawDecodedVideoFrames: 416,
    videoPackets: summarizePacketTiming(Array.from({ length: 416 }, (_, index) => ({ pts_time: String(index / 60), dts_time: String(index / 60), flags: index % 60 === 0 ? 'K_' : '__' }))) };
  const proof = { programFrameCount: 416, recordingVideoPrerollFrameCount: 0, recordingVideoTailFrameCount: 0,
    recordingMuxVideoFrameCount: 416, recordingAudioPrerollSampleCount: 0, recordingAudioTailSampleCount: 0,
    recordingRequestedAt100ns: 100, recordingWriterReadyAt100ns: 200, recordingMuxEpoch100ns: 300, recordingStartupDroppedAudioPackets: 0 };
  assert.equal(assessRecordingVideoEvidence(decoded, proof).passed, true);
  assert.equal(assessRecordingVideoEvidence({ ...decoded, videoPacketCount: 410, decodedVideoFrames: 356 }, proof).passed, false);
  assert.equal(assessRecordingVideoEvidence({ ...decoded, videoPackets: summarizePacketTiming([{ pts_time: '0', flags: '__' }]) }, proof).passed, false);
  assert.equal(assessRecordingVideoEvidence({ ...decoded, videoPackets: summarizePacketTiming([{ pts_time: '-0.1', flags: 'K_' }]) }, proof).passed, false);
  assert.equal(assessRecordingVideoEvidence(decoded, {}).passed, false);
});

test('explicit padding reconciles mux frames while retaining startup and real-frame cadence separately', () => {
  const decoded = { videoPacketCount: 5, decodedVideoFrames: 5, rawDecodedVideoFrames: 5,
    videoFramePts: [0, 0.3, 0.316667, 0.35, 0.36],
    videoPackets: summarizePacketTiming([0, 0.3, 0.316667, 0.35, 0.36].map(pts => ({ pts_time: String(pts), flags: 'K_' }))) };
  const proof = { programFrameCount: 3, recordingVideoPrerollFrameCount: 1, recordingVideoTailFrameCount: 1,
    recordingMuxVideoFrameCount: 5, recordingAudioPrerollSampleCount: 960, recordingAudioTailSampleCount: 480,
    recordingRequestedAt100ns: 100, recordingWriterReadyAt100ns: 2000100, recordingMuxEpoch100ns: 3000100, recordingStartupDroppedAudioPackets: 0 };
  const result = assessRecordingVideoEvidence(decoded, proof);
  assert.equal(result.passed, true);
  assert.equal(result.acceptedNativeFrames, 3);
  assert.equal(result.framePerformancePassed, false);
  assert.equal(result.timing.startup.worstIntervalMs, 300);
  assert.equal(result.timing.steadyRealFrames.intervalCount, 2);
  assert.ok(result.timing.steadyRealFrames.worstIntervalMs > 33);
  assert.equal(result.timing.tail.intervalCount, 1);
  assert.equal(result.startup.writerPreparationMs, 200);
  assert.equal(result.startup.requestToMuxEpochMs, 300);
  assert.equal(assessRecordingVideoEvidence(decoded, { ...proof, recordingStartupDroppedAudioPackets: 1 }).passed, false);
  assert.equal(assessRecordingVideoEvidence(decoded, { ...proof, recordingStartupDroppedAudioPackets: undefined }).passed, false);
  assert.equal(assessRecordingVideoEvidence(decoded, { ...proof, recordingWriterReadyAt100ns: 99 }).passed, false);
  assert.equal(assessRecordingVideoEvidence(decoded, { ...proof, recordingMuxEpoch100ns: 2000099 }).passed, false);
  assert.equal(assessRecordingVideoEvidence(decoded, { ...proof, recordingRequestedAt100ns: undefined }).passed, false);
  assert.equal(assessRecordingVideoEvidence(decoded, { ...proof, recordingMuxVideoFrameCount: 4 }).passed, false);
  assert.equal(assessRecordingVideoEvidence(decoded, { ...proof, recordingVideoTailFrameCount: undefined }).passed, false);
  assert.equal(assessRecordingVideoEvidence(decoded, { ...proof, recordingAudioPrerollSampleCount: undefined }).passed, false);
  assert.equal(assessRecordingVideoEvidence(decoded, { programFrameCount: 3 }).passed, false);
});

test('partial unique offsets survive but cannot pass coverage', () => {
  const pulses = FLASH_BEEP_PULSES.slice(0, 3).map(pulse => ({ startPts: pulse.startFrame / 60, duration: pulse.durationFrames / 60 }));
  const partial = alignIdentifiedPulses(pulses, pulses);
  assert.equal(partial.pairs.length, 3);
  assert.equal(partial.sufficientPairs, false);
  assert.equal(sourceCorrectedAlignment(partial, partial).alignmentWithinOneVideoFrame, false);
});
