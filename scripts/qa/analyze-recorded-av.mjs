// Offline re-analysis only: never launches a media core or overwrites evidence.
import { readFile, writeFile, stat } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { decodeRecordedAvFile } from './av-content-decode.mjs';
import { sourceCorrectedAlignment, assessRecordingVideoEvidence } from './av-content-analysis.mjs';
const source = resolve(process.argv[2] ?? '');
if (!process.argv[2] || (await stat(source)).size > 4 * 1024 * 1024) throw new Error('Provide a bounded existing recording report.');
const original = JSON.parse(await readFile(source, 'utf8'));
const result = { source, framePerformancePassed: false, runs: [], errors: [] };
try {
  result.fixture = await decodeRecordedAvFile(original.fixture.path);
  for (const run of original.runs) {
    const next = { frames: run.frames, artifact: run.artifact }; result.runs.push(next);
    try {
      next.decode = await decodeRecordedAvFile(run.artifact);
      next.videoArtifactAcceptance = assessRecordingVideoEvidence(next.decode, run.finalRecording?.proof);
      if (next.decode.alignment && result.fixture.alignment)
        next.sourceCorrectedAlignment = sourceCorrectedAlignment(next.decode.alignment, result.fixture.alignment);
    } catch (error) { next.error = error.message; }
  }
} catch (error) { result.errors.push(error.message); }
const output = join(dirname(source), `analysis-${Date.now()}.json`);
await writeFile(output, JSON.stringify(result, null, 2) + '\n', { flag: 'wx' });
console.log(output);
process.exitCode = result.runs.length === 2 && result.runs.every(run => run.decode?.analysisValid && run.videoArtifactAcceptance?.passed && run.sourceCorrectedAlignment?.alignmentWithinOneVideoFrame) ? 0 : 1;
