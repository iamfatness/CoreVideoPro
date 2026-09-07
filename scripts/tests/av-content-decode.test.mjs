import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { decodeRecordedAvFile } from '../qa/av-content-decode.mjs';

test('silent audio analysis preserves packet/frame/PTS evidence instead of throwing away the diagnostic report', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'av-decode-diagnostics-'));
  try {
    const artifact = join(directory, 'synthetic-input'); await writeFile(artifact, 'unit-test-data');
    const runTool = async (command, args) => {
      if (args.includes('-count_packets')) return { stdout: JSON.stringify({ streams: [
        { index: 0, codec_type: 'video', width: 1920, height: 1080, nb_read_packets: '2', nb_read_frames: '2' },
        { index: 1, codec_type: 'audio', nb_read_packets: '1', nb_read_frames: '1' }
      ] }) };
      if (args.includes('-show_packets')) return { stdout: JSON.stringify({ packets: [
        { stream_index: 0, pts_time: '0.9', flags: 'K_' }, { stream_index: 0, pts_time: '0.916667', flags: '__' },
        { stream_index: 1, pts_time: '0', flags: 'K_' }
      ] }) };
      if (args.includes('-show_frames')) return { stdout: JSON.stringify({ frames: [
        { best_effort_timestamp_time: '0.9', key_frame: 1 }, { best_effort_timestamp_time: '0.916667', key_frame: 0 }
      ] }) };
      if (args.includes('-vf')) return { stdout: Buffer.from([0, 255]) };
      return { stdout: Buffer.alloc(8), stderr: Buffer.from('[ashowinfo] n:0 pts:0 pts_time:0 rate:48000 nb_samples:2') };
    };
    const result = await decodeRecordedAvFile(artifact, { runTool });
    assert.equal(result.videoPacketCount, 2);
    assert.equal(result.decodedVideoFrames, 2);
    assert.equal(result.rawDecodedVideoFrames, 2);
    assert.equal(result.firstVideoPts, 0.9);
    assert.equal(result.firstAudioPts, 0);
    assert.equal(result.videoPackets.firstKeyPacketPts, 0.9);
    assert.equal(result.decodeCompleted, true);
    assert.equal(result.analysisValid, false);
    assert.equal(result.framePerformancePassed, false);
    assert.ok(result.errors.some(error => error.stage === 'audio-timeline-or-beeps' && error.message.includes('silent')));
  } finally { await rm(directory, { recursive: true, force: true }); }
});
