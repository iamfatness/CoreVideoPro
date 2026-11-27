import assert from "node:assert/strict";
import test from "node:test";
import { judgeStreamTiming } from "./stream-timing-judge.mjs";

const AAC = 1024 / 48000;
const range = (n, f) => Array.from({ length: n }, (_, i) => f(i));
const audio = (seconds, start = 0) => range(Math.round(seconds / AAC), (i) => start + i * AAC);
const video = (fps, seconds, start = 0) => range(Math.round(seconds * fps), (i) => start + i / fps);
const failed = (verdict, pattern) =>
  assert.ok(verdict.failures.some((f) => pattern.test(f)), `expected ${pattern} in ${JSON.stringify(verdict.failures)}`);

for (const fps of [30, 60]) {
  test(`a clean ${fps} fps stream passes with no gaps`, () => {
    const verdict = judgeStreamTiming({ video: video(fps, 20), audio: audio(20), fps });
    assert.deepEqual(verdict.failures, []);
    assert.equal(verdict.report.videoGaps, 0);
    assert.equal(verdict.report.audioGaps, 0);
  });
}

test("millisecond-quantised FLV timestamps pass", () => {
  const ms = (list) => list.map((t) => Math.round(t * 1000) / 1000);
  assert.deepEqual(judgeStreamTiming({ video: ms(video(30, 20)), audio: ms(audio(20)), fps: 30 }).failures, []);
});

// The 1.0.0 release blocker: pictures sent at 30 per second, stamped 1/15 apart.
test("video time running at twice audio time fails on drift and cadence", () => {
  const verdict = judgeStreamTiming({ video: video(30, 20).map((t) => t * 2), audio: audio(20), fps: 30 });
  failed(verdict, /A\/V timestamps drifted/);
  failed(verdict, /video cadence/);
});

// Same defect when the sink cuts both tracks at one media time (-t): the spans
// agree, so only the pictures-per-audio-second and cadence checks can see it.
test("a media-time-truncated capture of the doubled clock still fails", () => {
  const verdict = judgeStreamTiming({ video: video(30, 10).map((t) => t * 2), audio: audio(20), fps: 30 });
  failed(verdict, /video cadence/);
});

test("slow drift of 20 ms per second fails", () => {
  const verdict = judgeStreamTiming({ video: video(30, 30).map((t) => t * 1.02), audio: audio(30), fps: 30 });
  failed(verdict, /A\/V timestamps drifted/);
});

test("a constant A/V offset fails even without drift", () => {
  failed(judgeStreamTiming({ video: video(30, 20, 0.4), audio: audio(20), fps: 30 }), /A\/V/);
});

test("more pictures than the destination rate fails", () => {
  failed(judgeStreamTiming({ video: video(60, 20), audio: audio(20), fps: 30 }), /pictures per second of audio/);
});

test("a single 3.8 s audio gap fails", () => {
  const holed = audio(30).filter((t) => t < 10 || t >= 13.8);
  const verdict = judgeStreamTiming({ video: video(30, 30), audio: holed, fps: 30 });
  failed(verdict, /audio gap/);
  assert.equal(verdict.report.audioGaps, 1);
  assert.ok(verdict.report.maxAudioGapMs > 3700);
});

test("one missing AAC packet is a gap", () => {
  const holed = audio(20).filter((_, i) => i !== 300);
  failed(judgeStreamTiming({ video: video(30, 20), audio: holed, fps: 30 }), /audio gap/);
});

test("missing pictures are counted and reported, not hidden", () => {
  const holed = video(30, 20).filter((_, i) => i !== 100 && i !== 101);
  const verdict = judgeStreamTiming({ video: holed, audio: audio(20), fps: 30 });
  assert.deepEqual(verdict.failures, []);
  assert.equal(verdict.report.videoGaps, 1);
  assert.equal(verdict.report.missingPictures, 2);
});

test("backward timestamps fail", () => {
  const v = video(30, 10);
  [v[50], v[51]] = [v[51], v[50]];
  failed(judgeStreamTiming({ video: v, audio: audio(10), fps: 30 }), /video timestamps went backwards/);
});

test("no audio is a failure, never a pass", () => {
  failed(judgeStreamTiming({ video: video(30, 10), audio: [], fps: 30 }), /audio packets/);
});

// Pictures sit on the 60 Hz Program grid, so a non-divisor rate alternates step
// lengths (25 on 60: 3,2,3,2,2 ticks). Healthy, and not holes.
for (const fps of [24, 25, 50]) {
  test(`a healthy ${fps} fps stream on the 60 Hz grid passes with no gaps`, () => {
    const gridded = range(Math.ceil(20 * fps), (i) => Math.ceil((i * 60) / fps - 1e-9) / 60);
    const verdict = judgeStreamTiming({ video: gridded, audio: audio(20), fps });
    assert.deepEqual(verdict.failures, []);
    assert.equal(verdict.report.videoGaps, 0);
  });
}

test("a stream that stopped early fails even though both tracks agree", () => {
  const verdict = judgeStreamTiming({ video: video(30, 8), audio: audio(8), fps: 30, runSeconds: 30 });
  failed(verdict, /stopped early/);
  assert.deepEqual(judgeStreamTiming({ video: video(30, 27), audio: audio(27), fps: 30, runSeconds: 30 }).failures, []);
});

// MPEG-TS packs several AAC frames into one PES (17 frames, 363 ms, on silence)
// and the unfinished one is lost at Stop: audio ends early, nothing drifted.
test("an unfinished MPEG-TS audio PES at Stop is not drift, only where it is allowed", () => {
  const truncated = { video: video(30, 20), audio: audio(20 - 0.36), fps: 30 };
  assert.deepEqual(judgeStreamTiming({ ...truncated, audioTailSeconds: 0.4 }).failures, []);
  failed(judgeStreamTiming(truncated), /A\/V/);
});

test("the tail allowance never excuses video ending early, or real drift", () => {
  failed(judgeStreamTiming({ video: video(30, 19.5), audio: audio(20), fps: 30, audioTailSeconds: 0.4 }), /A\/V/);
  failed(judgeStreamTiming({ video: video(30, 30).map((t) => t * 1.03), audio: audio(30), fps: 30,
    audioTailSeconds: 0.4 }), /A\/V timestamps drifted/);
  failed(judgeStreamTiming({ video: video(30, 20, 0.4), audio: audio(20), fps: 30, audioTailSeconds: 0.4 }), /start/);
});

// The raw fallback encodes B-frames: packets arrive in DECODE order, so their
// presentation times step backwards on a perfectly healthy stream.
const bFrameOrder = (pts) => {
  const out = [pts[0]];
  for (let i = 1; i < pts.length; i += 3) out.push(...[pts[i + 2], pts[i], pts[i + 1]].filter((t) => t !== undefined));
  return out;
};

test("B-frame decode order passes when decode times are monotonic", () => {
  const pictures = video(30, 20, 0);
  const verdict = judgeStreamTiming({ video: bFrameOrder(pictures), videoDecodeTimes: pictures.map((t) => t - 0.067),
    audio: audio(20), fps: 30 });
  assert.deepEqual(verdict.failures, []);
  assert.equal(verdict.report.videoGaps, 0);
});

test("with decode times, a repeated picture time or a backwards decode time still fails", () => {
  const pictures = video(30, 10);
  const dts = [...pictures];
  const repeated = [...pictures];
  repeated[40] = repeated[39];
  failed(judgeStreamTiming({ video: repeated, videoDecodeTimes: dts, audio: audio(10), fps: 30 }),
    /video timestamps went backwards or repeated/);
  [dts[50], dts[51]] = [dts[51], dts[50]];
  failed(judgeStreamTiming({ video: pictures, videoDecodeTimes: dts, audio: audio(10), fps: 30 }),
    /video decode times went backwards/);
});
