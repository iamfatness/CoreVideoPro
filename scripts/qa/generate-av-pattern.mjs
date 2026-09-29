// A playable, self-identifying 1080p60 flash/beep source for end-to-end A/V QA.
// Each flash has a distinct duration, so a missing cue cannot be hidden by
// nearest-neighbor pairing after a capture or stream restart.
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { mkdir, writeFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { FLASH_BEEP_PULSES } from './av-content-analysis.mjs';
import { decodeRecordedAvFile } from './av-content-decode.mjs';

const exec = promisify(execFile);
export async function generateAvPattern(path, { ffmpeg = 'ffmpeg', ffprobe = 'ffprobe' } = {}) {
  const output = resolve(path);
  await mkdir(dirname(output), { recursive: true });
  const videoPulseExpression = FLASH_BEEP_PULSES.map(p =>
    `gte(n,${p.startFrame})*lt(n,${p.startFrame + p.durationFrames})`).join('+');
  const audioPulseExpression = FLASH_BEEP_PULSES.map(p =>
    `gte(t,${p.startFrame / 60})*lt(t,${(p.startFrame + p.durationFrames) / 60})`).join('+');
  await exec(ffmpeg, ['-hide_banner', '-v', 'error', '-y', '-f', 'lavfi', '-i',
    'color=c=black:s=1920x1080:r=60:d=14', '-f', 'lavfi', '-i',
    `aevalsrc='if(${audioPulseExpression},0.7*sin(2*PI*1000*t),0)':s=48000:d=14`,
    '-vf', `drawbox=color=white:t=fill:enable='${videoPulseExpression}'`,
    '-ac', '2',
    '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '18', '-pix_fmt', 'yuv420p',
    '-c:a', 'aac', '-b:a', '192k', '-movflags', '+faststart', output],
  { windowsHide: true, timeout: 120000, maxBuffer: 16 * 1024 * 1024 });
  const decoded = await decodeRecordedAvFile(output, { ffmpeg, ffprobe });
  const alignment = decoded.alignment;
  if (!decoded.analysisValid || !alignment?.sufficientPairs ||
      !alignment.interiorCoverageComplete || alignment.pairs.length !== FLASH_BEEP_PULSES.length ||
      Math.abs(alignment.medianAudioMinusVideoMs) > 1000 / 60) {
    throw new Error(`Generated pattern failed source preflight: ${decoded.alignmentError ?? JSON.stringify(alignment)}`);
  }
  const manifest = {
    path: output, fps: 60, sampleRate: 48000, durationSeconds: 14,
    cue: 'full-frame white flash and 1 kHz beep; duration identifies the cue',
    pulses: FLASH_BEEP_PULSES.map(p => ({ ...p, expectedVideoPts: p.startFrame / 60,
      expectedAudioSample: p.startFrame * 800 })),
    sourceMedianVideoMinusAudioMs: -alignment.medianAudioMinusVideoMs,
    sourcePairs: alignment.pairs,
    artifactSha256: decoded.artifactSha256,
  };
  await writeFile(`${output}.json`, JSON.stringify(manifest, null, 2) + '\n');
  return manifest;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const output = process.argv[2];
  if (!output) throw new Error('Usage: node scripts/qa/generate-av-pattern.mjs OUTPUT.mp4');
  const result = await generateAvPattern(output);
  console.log(JSON.stringify(result, null, 2));
}
