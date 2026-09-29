// Compare duration-identified flash/beep cues in same-run decoded outputs.
// A missing requested leg is a failure, never an implied end-to-end pass.
import { writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { decodeRecordedAvFile } from './av-content-decode.mjs';

const options = {};
const requireAll = process.argv.includes('--require-all');
const allowed = new Set(['--source', '--recording', '--rtmp', '--youtube', '--output', '--ffmpeg', '--ffprobe']);
for (let index = 2; index < process.argv.length; ++index) {
  const key = process.argv[index];
  if (key === '--require-all') continue;
  const value = process.argv[++index];
  if (!allowed.has(key) || !value) throw new Error('Usage: node scripts/qa/measure-av-pattern.mjs --source FILE [--recording FILE] [--rtmp FILE] [--youtube FILE] [--require-all] [--output report.json]');
  options[key] = value;
}
if (!options['--source']) throw new Error('--source pattern file is required');
const ffmpeg = options['--ffmpeg'] ?? 'ffmpeg', ffprobe = options['--ffprobe'] ?? 'ffprobe';
const roles = ['source', 'recording', 'rtmp', 'youtube'];
const report = { units: 'video minus audio, ms; negative means audio lags', framesPerSecond: 60,
  legs: {}, missingLegs: [], errors: [], endToEndMeasured: false };
let sourceOffset = null;
const seenHashes = new Map();
for (const role of roles) {
  const input = options[`--${role}`];
  if (!input) { if (role !== 'source') report.missingLegs.push(role); continue; }
  const file = resolve(input);
  const decode = await decodeRecordedAvFile(file, { ffmpeg, ffprobe, allowAnyVideoSize: role !== 'source',
    transportTimestampPrecisionMs: role === 'rtmp' ? 1 : 0 });
  const alignment = decode.alignment;
  const valid = decode.analysisValid && alignment?.sufficientPairs && alignment.interiorCoverageComplete;
  const videoMinusAudioMs = valid ? -alignment.medianAudioMinusVideoMs : null;
  if (role === 'source') sourceOffset = videoMinusAudioMs;
  report.legs[role] = {
    path: file, sha256: decode.artifactSha256 ?? null,
    video: decode.streams?.find(stream => stream.codec_type === 'video')?.codec_name ?? null,
    audio: decode.streams?.find(stream => stream.codec_type === 'audio')?.codec_name ?? null,
    pairedCueIds: alignment?.pairs?.map(pair => pair.pulseId) ?? [],
    duplicateVideoPts: decode.duplicateVideoPts ?? null,
    audioPtsJitterSamples: decode.audioTimeline?.maxPtsJitterSamples ?? null,
    videoMinusAudioMs,
    framesAt60: videoMinusAudioMs === null ? null : videoMinusAudioMs * 60 / 1000,
    spreadMs: valid ? alignment.maxAudioMinusVideoMs - alignment.minAudioMinusVideoMs : null,
    sourceCorrectedMs: role === 'source' || videoMinusAudioMs === null || sourceOffset === null
      ? null : videoMinusAudioMs - sourceOffset,
    valid: Boolean(valid), error: decode.alignmentError,
  };
  if (decode.artifactSha256 && seenHashes.has(decode.artifactSha256))
    report.errors.push(`${role}: identical artifact to ${seenHashes.get(decode.artifactSha256)}; independent output evidence is required`);
  else if (decode.artifactSha256) seenHashes.set(decode.artifactSha256, role);
  if (!valid) report.errors.push(`${role}: ${decode.alignmentError ?? 'insufficient identified cues'}`);
}
report.endToEndMeasured = report.errors.length === 0 && report.missingLegs.length === 0;
if (options['--output']) await writeFile(resolve(options['--output']), JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify(report, null, 2));
if (report.errors.length || (requireAll && report.missingLegs.length)) process.exitCode = 1;
