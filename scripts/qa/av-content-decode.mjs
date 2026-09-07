import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { readFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { detectFlashPulses, detectBeepPulses, alignIdentifiedPulses, validateAudioTimeline, summarizePacketTiming } from './av-content-analysis.mjs';
const exec = promisify(execFile);

export async function decodeRecordedAvFile(path, { ffmpeg = 'ffmpeg', ffprobe = 'ffprobe', runTool } = {}) {
  const tool = runTool ?? ((command, args, extra = {}) => exec(command, args, { windowsHide: true, timeout: 60000, maxBuffer: 16 * 1024 * 1024, ...extra }));
  const result = { artifact: path, errors: [], coverageErrors: [], decodeCompleted: false, analysisValid: false, framePerformancePassed: false };
  const error = (stage, failure) => result.errors.push({ stage, message: failure.message.slice(0, 4000) });
  let video, audio, timestamps, gray, samples, audioLog;
  try {
    result.artifactSha256 = createHash('sha256').update(await readFile(path)).digest('hex');
    const { stdout } = await tool(ffprobe, ['-v', 'error', '-count_packets', '-count_frames', '-show_streams', '-show_format', '-of', 'json', path]);
    const probe = JSON.parse(stdout);
    result.streams = probe.streams; result.format = probe.format;
    video = probe.streams.find(stream => stream.codec_type === 'video');
    audio = probe.streams.find(stream => stream.codec_type === 'audio');
    const count = value => value !== undefined && Number.isSafeInteger(Number(value)) && Number(value) >= 0 ? Number(value) : null;
    result.videoPacketCount = count(video?.nb_read_packets); result.audioPacketCount = count(audio?.nb_read_packets);
    result.probedDecodedVideoFrames = count(video?.nb_read_frames); result.probedDecodedAudioFrames = count(audio?.nb_read_frames);
    if (!video || !audio || video.width !== 1920 || video.height !== 1080) throw new Error('Expected 1080p video and audio streams.');
  } catch (failure) { error('stream-probe', failure); }
  try {
    const { stdout } = await tool(ffprobe, ['-v', 'error', '-show_packets', '-show_entries', 'packet=stream_index,pts_time,dts_time,duration_time,flags,size', '-of', 'json', path]);
    const packets = JSON.parse(stdout).packets;
    result.videoPackets = summarizePacketTiming(packets.filter(packet => packet.stream_index === video?.index));
    result.audioPackets = summarizePacketTiming(packets.filter(packet => packet.stream_index === audio?.index));
  } catch (failure) { error('packet-probe', failure); }
  try {
    const { stdout } = await tool(ffprobe, ['-v', 'error', '-select_streams', 'v:0', '-show_frames', '-show_entries',
      'frame=best_effort_timestamp_time,key_frame,pict_type,pkt_dts_time,duration_time', '-of', 'json', path]);
    const frames = JSON.parse(stdout).frames;
    timestamps = frames.map(frame => frame.best_effort_timestamp_time === undefined ? NaN : Number(frame.best_effort_timestamp_time));
    result.decodedVideoFrames = frames.length;
    result.videoFrameTiming = { firstFrames: frames.slice(0, 8), lastFrames: frames.slice(-4),
      keyframes: frames.filter(frame => frame.key_frame === 1).slice(0, 64), timestampPrecisionSeconds: 0.000001 };
    result.videoFramePts = timestamps;
    result.firstVideoPts = timestamps[0] ?? null; result.lastVideoPts = timestamps.at(-1) ?? null;
    if (timestamps.length > 1) {
      const intervals = timestamps.slice(1).map((pts, index) => pts - timestamps[index]);
      result.worstVideoPtsIntervalMs = Math.max(...intervals) * 1000;
      result.averageVideoFps = (timestamps.length - 1) / (timestamps.at(-1) - timestamps[0]);
    }
  } catch (failure) { error('video-frame-probe', failure); }
  try {
    const response = await tool(ffmpeg, ['-v', 'error', '-xerror', '-i', path, '-map', '0:v:0', '-vf', 'scale=1:1',
      '-fps_mode', 'passthrough', '-pix_fmt', 'gray', '-f', 'rawvideo', 'pipe:1'], { encoding: 'buffer' });
    gray = response.stdout; result.rawDecodedVideoFrames = gray.length;
  } catch (failure) { error('video-decode', failure); }
  try {
    const response = await tool(ffmpeg, ['-hide_banner', '-nostats', '-v', 'info', '-xerror', '-i', path, '-map', '0:a:0', '-af',
      'aformat=sample_fmts=flt:sample_rates=48000:channel_layouts=mono,asettb=1/48000,ashowinfo', '-f', 'f32le', 'pipe:1'], { encoding: 'buffer' });
    audioLog = response.stderr.toString();
    samples = new Float32Array(response.stdout.length / 4);
    for (let index = 0; index < samples.length; ++index) samples[index] = response.stdout.readFloatLE(index * 4);
    result.decodedAudioSamples = samples.length;
    result.firstAudioPts = Number(audioLog.match(/\bn:0\s+pts:(-?\d+)/)?.[1]) / 48000;
    result.audioFrameTimestampLines = audioLog.split(/\r?\n/).filter(line => /\bn:\d+\s+pts:/.test(line)).slice(0, 1500);
  } catch (failure) { error('audio-decode', failure); }
  result.decodeCompleted = !!gray?.length && !!samples?.length;
  if (gray && timestamps) {
    try { result.flashes = detectFlashPulses(gray, timestamps); } catch (failure) { error('flash-detection', failure); }
  }
  if (samples && audioLog) {
    try {
      result.audioTimeline = validateAudioTimeline(audioLog, samples.length);
      result.firstAudioPts = result.audioTimeline.firstPts;
      result.beeps = detectBeepPulses(samples, result.firstAudioPts);
    } catch (failure) { error('audio-timeline-or-beeps', failure); }
  }
  if (result.flashes && result.beeps) {
    try {
      result.alignment = alignIdentifiedPulses(result.flashes, result.beeps);
      if (!result.alignment.sufficientPairs) result.coverageErrors.push('Fewer than four uniquely identified pairs; partial offsets are diagnostic.');
      if (!result.alignment.interiorCoverageComplete) result.coverageErrors.push('Unmatched or missing interior pulse events.');
    } catch (failure) { error('pulse-identification', failure); }
  }
  result.analysisValid = result.errors.length === 0 && !!result.alignment;
  result.alignmentCoverageComplete = result.analysisValid && result.coverageErrors.length === 0;
  result.alignmentError = [...result.errors.map(item => `${item.stage}: ${item.message}`), ...result.coverageErrors].join(' ') || null;
  return result;
}
