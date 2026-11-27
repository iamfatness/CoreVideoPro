// Timestamp verdict for a RECEIVED stream (validate-gpu-encode.mjs). Pure: it
// takes packet times in seconds, in file order: presentation times, plus the
// video decode times when the encoder may reorder (B-frames on the raw path).
//
// Added after the 1.0.0 qualification: a 30 fps destination on the 60 Hz Program
// clock stamped its pictures at twice real time, and no average-fps gate could
// say so plainly. The audio track is the reference clock here, because it counts
// real 48 kHz samples.

const AAC_PACKET_SECONDS = 1024 / 48000;
// 1.5 AAC packets: tolerates millisecond FLV timestamps, not one lost packet.
export const MAX_AUDIO_STEP_SECONDS = 1.5 * AAC_PACKET_SECONDS;
// The two tracks start and end together (they stop on one command).
export const MAX_AV_EDGE_SECONDS = 0.25;
export const MAX_AV_DRIFT_SECONDS = 0.25;
// Pictures are taken from the Program's 60 Hz grid.
const PROGRAM_TICK_SECONDS = 1 / 60;
// Capture length below this share of the run means the stream stopped early
// (startup and an unfinished last HLS segment cost a few seconds at most).
export const MIN_CAPTURE_SHARE = 0.8;

const median = (values) => {
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
};
const steps = (times) => times.slice(1).map((t, i) => t - times[i]);
const ms = (seconds) => Math.round(seconds * 10000) / 10;

export function judgeStreamTiming({ video: videoInFileOrder, videoDecodeTimes, audio, fps, runSeconds = 0,
  audioTailSeconds = 0 }) {
  // With decode times, file order is decode order: judge pictures as presented.
  const video = videoDecodeTimes ? [...videoInFileOrder].sort((a, b) => a - b) : videoInFileOrder;
  const failures = [];
  const report = {};
  if (video.length < 2) failures.push(`only ${video.length} video packets in the received stream`);
  if (audio.length < 2) failures.push(`only ${audio.length} audio packets in the received stream`);
  if (failures.length) return { failures, report };

  const period = 1 / fps;
  const videoSteps = steps(video);
  const audioSteps = steps(audio);
  const videoSpan = video.at(-1) - video[0];
  const audioSpan = audio.at(-1) - audio[0] + AAC_PACKET_SECONDS;

  const backwards = (list) => list.filter((step) => step <= 0).length;
  if (backwards(videoSteps)) failures.push(`video timestamps went backwards or repeated ${backwards(videoSteps)} time(s)`);
  const decodeBackwards = videoDecodeTimes ? backwards(steps(videoDecodeTimes)) : 0;
  if (decodeBackwards) failures.push(`video decode times went backwards or repeated ${decodeBackwards} time(s)`);
  if (backwards(audioSteps)) failures.push(`audio timestamps went backwards or repeated ${backwards(audioSteps)} time(s)`);

  // Audio continuity: any hole is a failure.
  const audioHoles = audioSteps.filter((step) => step > MAX_AUDIO_STEP_SECONDS);
  report.audioPackets = audio.length;
  report.audioGaps = audioHoles.length;
  report.maxAudioGapMs = ms(Math.max(...audioSteps));
  if (audioHoles.length) {
    failures.push(`${audioHoles.length} audio gap(s) over ${ms(MAX_AUDIO_STEP_SECONDS)}ms ` +
      `(largest step ${report.maxAudioGapMs}ms)`);
  }

  // Video cadence: the ordinary step is one destination frame period. Judged
  // as the median of five-step averages, because a rate that does not divide 60
  // alternates step lengths on the Program grid; a few holes do not move it.
  const window = Math.min(5, videoSteps.length);
  const averages = videoSteps.slice(window - 1)
    .map((_, i) => (video[i + window] - video[i]) / window);
  const cadence = median(averages);
  report.videoPackets = video.length;
  report.videoStepMs = ms(cadence);
  if (Math.abs(cadence - period) > period * 0.1) {
    failures.push(`video cadence: typical step ${ms(cadence)}ms, expected ${ms(period)}ms at ${fps}fps`);
  }
  // Per-frame delivery, reported in full; the caller states the rate verdict.
  const longestGridStep = Math.ceil(period / PROGRAM_TICK_SECONDS - 1e-6) * PROGRAM_TICK_SECONDS;
  const holeAbove = Math.max(period * 1.5, longestGridStep + PROGRAM_TICK_SECONDS / 2);
  const videoHoles = videoSteps.filter((step) => step > holeAbove);
  report.videoGaps = videoHoles.length;
  report.missingPictures = videoHoles.reduce((sum, step) => sum + Math.round(step / period) - 1, 0);
  report.maxVideoStepMs = ms(Math.max(...videoSteps));
  report.shortVideoSteps = videoSteps.filter((step) => step > 0 && step < period * 0.5).length;

  if (runSeconds > 0 && audioSpan < runSeconds * MIN_CAPTURE_SHARE) {
    failures.push(`the stream stopped early: ${Math.round(audioSpan * 10) / 10}s of audio from a ${runSeconds}s run`);
  }

  // Pictures per second of AUDIO time: independent of the video timestamps.
  const picturesPerAudioSecond = video.length / audioSpan;
  report.picturesPerAudioSecond = Math.round(picturesPerAudioSecond * 100) / 100;
  if (picturesPerAudioSecond > fps * 1.02) {
    failures.push(`${report.picturesPerAudioSecond} pictures per second of audio at a ${fps}fps destination`);
  }

  // One clock: both tracks start together, end together, and neither runs away.
  const startOffset = video[0] - audio[0];
  const endOffset = video.at(-1) - audio.at(-1);
  // A container that groups audio frames can lose its unfinished group at Stop:
  // that much, and only in that direction (audio ending first), is not offset.
  const judgedEndOffset = endOffset > 0 ? Math.max(0, endOffset - audioTailSeconds) : endOffset;
  const drift = judgedEndOffset - startOffset;
  report.startOffsetMs = ms(startOffset);
  report.endOffsetMs = ms(endOffset);
  report.driftMs = ms(endOffset - startOffset);
  report.videoSpanSeconds = Math.round(videoSpan * 1000) / 1000;
  report.audioSpanSeconds = Math.round(audioSpan * 1000) / 1000;
  if (Math.abs(drift) > MAX_AV_DRIFT_SECONDS) {
    failures.push(`A/V timestamps drifted ${ms(drift)}ms over ${report.audioSpanSeconds}s of audio`);
  }
  for (const [name, offset] of [["start", startOffset], ["end", judgedEndOffset]]) {
    if (Math.abs(offset) > MAX_AV_EDGE_SECONDS) {
      failures.push(`A/V offset at the ${name} of the capture is ${ms(offset)}ms (limit ${ms(MAX_AV_EDGE_SECONDS)}ms)`);
    }
  }
  return { failures, report };
}
