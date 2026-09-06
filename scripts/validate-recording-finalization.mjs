/** Real recording adapter proof, isolated generated bars + silent audio, no capture/meeting/stream.
 * node scripts/validate-recording-finalization.mjs --native-core <fresh absolute executable>
 * Requires ffmpeg AND ffprobe. Missing adapters/tools or missing lifecycle fail the run.
 */
import { spawn, execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { once } from 'node:events';
import { mkdir, readFile, writeFile, stat } from 'node:fs/promises';
import { resolve, dirname, join } from 'node:path';
import { createHash } from 'node:crypto';

const exec = promisify(execFile);
const index = process.argv.indexOf('--native-core');
if (index < 0 || !process.argv[index + 1]) throw new Error('--native-core must identify the freshly built executable');
const core = resolve(process.argv[index + 1]);
await stat(core);
for (const tool of ['ffmpeg', 'ffprobe']) await exec(tool, ['-version'], { windowsHide: true });
const outputDir = join(dirname(core), `recording-finalization-proof-${Date.now()}`);
await mkdir(outputDir, { recursive: true });
const bars = join(outputDir, 'generated-smpte-bars.png');
await exec('ffmpeg', ['-v', 'error', '-f', 'lavfi', '-i', 'smptebars=size=1920x1080',
  '-frames:v', '1', '-update', '1', bars], { windowsHide: true });
const evidence = { scope: 'Finalization and decode proof using a real local adapter configured for 1080p60, generated bars and silent audio; no camera/Zoom/network output. Frame-rate and soak acceptance are separate.',
  status: 'failed', startedAt: new Date().toISOString(), core,
  coreSha256: createHash('sha256').update(await readFile(core)).digest('hex'), outputDir, states: [] };
const child = spawn(core, [], { cwd: dirname(core), windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'], env: {
  ...process.env, COREVIDEO_ZOOM_ENGINE_PATH: '', COREVIDEO_ZOOM_SDK_JWT: '', COREVIDEO_ZOOM_USER_ZAK: '',
  COREVIDEO_ZOOM_ON_BEHALF_TOKEN: '', COREVIDEO_ZOOM_APP_PRIVILEGE_TOKEN: '',
}});
let next = 0, buffer = '', handshake, stderr = '', snapshot;
const pending = new Map();
const exited = once(child, 'exit');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
child.stderr.on('data', chunk => { stderr = (stderr + chunk).slice(-200000); });
child.stdout.on('data', chunk => {
  buffer += chunk;
  let end;
  while ((end = buffer.indexOf('\n')) >= 0) {
    const line = buffer.slice(0, end); buffer = buffer.slice(end + 1);
    if (!line.trim()) continue;
    const message = JSON.parse(line);
    if (message.type === 'handshake') handshake = message;
    const item = pending.get(message.id);
    if (item) { pending.delete(message.id); clearTimeout(item.timer); item.resolve(message); }
  }
});
child.once('exit', code => {
  for (const item of pending.values()) { clearTimeout(item.timer); item.reject(new Error(`Core exited ${code}`)); }
  pending.clear();
});
function send(type, extra = {}) {
  const id = `record-proof-${++next}`;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`${type} timed out`)); }, 15000);
    pending.set(id, { resolve, reject, timer });
    child.stdin.write(JSON.stringify({ id, type, ...extra }) + '\n');
  });
}
function observe(nextSnapshot) {
  snapshot = nextSnapshot;
  const lifecycle = snapshot?.recording?.lifecycle;
  if (lifecycle && evidence.states.at(-1)?.state !== lifecycle.state) evidence.states.push({ at: Date.now(), ...lifecycle });
  if (lifecycle?.state === 'failed' || lifecycle?.state === 'interrupted') throw new Error(`Writer failed: ${JSON.stringify(lifecycle)}`);
  return lifecycle;
}
async function sync(commands = []) {
  const reply = await send('media-core-sync', { commands });
  if (!reply.ok) throw new Error(`Command failed: ${JSON.stringify(reply)}`);
  return observe(reply.snapshot);
}
try {
  for (let i = 0; !handshake && i < 150; i++) await sleep(50);
  if (!handshake) throw new Error('No native handshake');
  evidence.profile = handshake.profile;
  if (!['media-foundation', 'avfoundation'].includes(handshake.profile?.encoder))
    throw new Error(`A real recording adapter is required; observed ${handshake.profile?.encoder}`);
  await sync([
    { type: 'set-output-profile', width: 1920, height: 1080, fps: 60, targetBitrateMbps: 8 },
    { type: 'load-scene-graph', sceneId: 'generated-recording-proof', routes: [], background: {
      mediaAssetId: 'generated-bars', mediaAssetName: 'Generated SMPTE bars', mediaAssetKind: 'image', mediaAssetPath: bars, playing: true } },
    { type: 'start-program-output', destinations: ['recording'], isoParticipantIds: [] },
    { type: 'set-recording-targets', targetFolder: outputDir, filenamePrefix: 'program', format: 'mp4', quality: 'high', isoParticipantIds: [] },
    { type: 'start-recording-session', sessionId: 'generated-bars-proof', targetFolder: outputDir,
      filenamePrefix: 'program', format: 'mp4', quality: 'high', isoParticipantIds: [] },
  ]);
  const startDeadline = Date.now() + 15000;
  while (snapshot?.recording?.lifecycle?.state !== 'live' && Date.now() < startDeadline) { await sleep(100); await sync(); }
  if (snapshot?.recording?.lifecycle?.state !== 'live') throw new Error('Recording never became verified live');
  const sessionId = snapshot.recording.lifecycle.sessionId;
  const liveUntil = Date.now() + 6000;
  while (Date.now() < liveUntil) { await sleep(200); await sync(); }
  evidence.liveRecording = snapshot.recording;
  await sync([{ type: 'stop-recording-session', reason: 'Generated recording proof complete' }]);
  const finalizeDeadline = Date.now() + 20000;
  while (snapshot?.recording?.lifecycle?.state !== 'completed' && Date.now() < finalizeDeadline) { await sleep(100); await sync(); }
  const lifecycle = snapshot?.recording?.lifecycle;
  if (lifecycle?.state !== 'completed' || lifecycle.finalized !== true || lifecycle.sessionId !== sessionId)
    throw new Error(`Matching writer finalization was not confirmed: ${JSON.stringify(lifecycle)}`);
  evidence.finalRecording = snapshot.recording;
  const artifact = resolve(dirname(core), snapshot.recording.artifactPath ?? '');
  if (!snapshot.recording.artifactPath || (await stat(artifact)).size <= 1024) throw new Error('Recording artifact is missing or empty');
  evidence.artifact = artifact;
  // The core remains alive until both container inspection and full decode finish.
  const { stdout: probeText } = await exec('ffprobe', ['-v', 'error', '-count_frames', '-show_streams', '-show_format', '-of', 'json', artifact],
    { windowsHide: true, timeout: 30000, maxBuffer: 4 * 1024 * 1024 });
  const probe = JSON.parse(probeText);
  evidence.probe = probe;
  const video = probe.streams.find(stream => stream.codec_type === 'video');
  const audio = probe.streams.find(stream => stream.codec_type === 'audio');
  if (!video || !audio || video.width !== 1920 || video.height !== 1080 || Number(video.nb_read_frames) < 180)
    throw new Error('Probe did not confirm 1080p video frames plus an audio stream');
  const videoDuration = Number(video.duration), audioDuration = Number(audio.duration);
  evidence.effectiveVideoFps = Number(video.nb_read_frames) / videoDuration;
  evidence.encoderQueueDroppedVideoFrames = snapshot.recording.proof?.encoderQueueDroppedVideoFrames ?? null;
  evidence.performanceWarnings = [];
  if (evidence.effectiveVideoFps < 57 || evidence.encoderQueueDroppedVideoFrames > 0)
    evidence.performanceWarnings.push('Configured 60fps was not sustained without queue drops; this recording does not establish frame-rate acceptance.');
  evidence.avDurationDifferenceSeconds = Math.abs(videoDuration - audioDuration);
  if (!Number.isFinite(videoDuration) || videoDuration < 5 || videoDuration > 12 || evidence.avDurationDifferenceSeconds > .5)
    throw new Error(`Duration or A/V end alignment is outside the short-proof bounds (${videoDuration}, ${audioDuration})`);
  await exec('ffmpeg', ['-v', 'error', '-xerror', '-i', artifact, '-map', '0:v:0', '-map', '0:a:0', '-f', 'null', '-'],
    { windowsHide: true, timeout: 30000, maxBuffer: 4 * 1024 * 1024 });
  // Decode actual pixels: encoded frame counters and a black file are insufficient.
  const { stdout: pixels } = await exec('ffmpeg', ['-v', 'error', '-ss', '1', '-i', artifact, '-frames:v', '1',
    '-vf', 'scale=64:36', '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-'], { windowsHide: true, encoding: 'buffer', maxBuffer: 1024 * 1024 });
  const colors = new Set();
  for (let i = 0; i + 2 < pixels.length; i += 3) colors.add(`${pixels[i] >> 5},${pixels[i + 1] >> 5},${pixels[i + 2] >> 5}`);
  if (colors.size < 8) throw new Error('Decoded picture lacks the generated color-bar content');
  evidence.decodedColorBuckets = colors.size;
  evidence.fullDecode = 'passed';
  evidence.artifactSha256 = createHash('sha256').update(await readFile(artifact)).digest('hex');
  evidence.status = 'passed';
} catch (error) {
  evidence.failure = error.message;
  evidence.lastSnapshot = snapshot;
  process.exitCode = 1;
} finally {
  child.stdin.end();
  await Promise.race([exited, sleep(5000)]);
  if (child.exitCode === null) { child.kill(); await exited; }
  await writeFile(join(outputDir, 'core-stderr.log'), stderr);
  await writeFile(join(outputDir, 'evidence.json'), JSON.stringify(evidence, null, 2) + '\n');
}
console.log(JSON.stringify({ status: evidence.status, failure: evidence.failure, evidence: join(outputDir, 'evidence.json'),
  artifact: evidence.artifact, states: evidence.states.map(state => state.state), avDurationDifferenceSeconds: evidence.avDurationDifferenceSeconds,
  effectiveVideoFps: evidence.effectiveVideoFps, encoderQueueDroppedVideoFrames: evidence.encoderQueueDroppedVideoFrames,
  performanceWarnings: evidence.performanceWarnings }));
