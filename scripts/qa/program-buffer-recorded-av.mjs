// Isolated file destination/content A/V proof. Never physical presentation proof.
import { spawn, execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { mkdir, readFile, writeFile, stat } from 'node:fs/promises';
import { resolve, dirname, join, sep } from 'node:path';
import { createHash, randomUUID } from 'node:crypto';
import { FLASH_BEEP_PULSES, sourceCorrectedAlignment, assessRecordingVideoEvidence } from './av-content-analysis.mjs';
import { decodeRecordedAvFile } from './av-content-decode.mjs';

const exec = promisify(execFile), sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const args = process.argv.slice(2), options = {};
for (let i = 0; i < args.length; i += 2) {
  if (!['--native-core', '--output-dir', '--ffmpeg', '--ffprobe'].includes(args[i]) || !args[i + 1]) throw new Error('Usage: node scripts/qa/program-buffer-recorded-av.mjs --native-core EXE --output-dir DIRECTORY [--ffmpeg EXE] [--ffprobe EXE]');
  options[args[i]] = args[i + 1];
}
if (!options['--native-core'] || !options['--output-dir']) throw new Error('Explicit native core and output directory required.');
const core = resolve(options['--native-core']), outputRoot = resolve(options['--output-dir']);
const ffmpeg = options['--ffmpeg'] ?? 'ffmpeg', ffprobe = options['--ffprobe'] ?? 'ffprobe';
await stat(core);
await mkdir(outputRoot, { recursive: true });
const directory = join(outputRoot, `recorded-av-${Date.now()}-${randomUUID().slice(0, 8)}`);
await mkdir(directory);
const report = { scope: 'synthetic-source/real-recording-destination', core, directory,
  coreSha256: createHash('sha256').update(await readFile(core)).digest('hex'), framePerformancePassed: false,
  physicalPresentationMeasured: false, networkDestinationsMeasured: false, runs: [], errors: [],
  limitation: 'Finalized file decode proves a recording artifact and measures content A/V offset. It does not prove physical display timing or per-slot completion at any external output.' };
const tool = (command, args, extra = {}) => exec(command, args, { windowsHide: true, timeout: 60000, maxBuffer: 16 * 1024 * 1024, ...extra });

const decode = path => decodeRecordedAvFile(path, { ffmpeg, ffprobe });

async function record(frames, fixture) {
  const runDirectory = join(directory, `depth-${frames}`); await mkdir(runDirectory);
  const run = { frames, runDirectory, states: [], errors: [], framePerformancePassed: false };
  const child = spawn(core, [], { cwd: dirname(core), windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'], env: {
    ...process.env, COREVIDEO_PROGRAM_BUFFER_FRAMES: String(frames), COREVIDEO_ZOOM_ENGINE_PATH: '',
    COREVIDEO_ZOOM_SDK_JWT: '', COREVIDEO_ZOOM_USER_ZAK: '', COREVIDEO_ZOOM_ON_BEHALF_TOKEN: '', COREVIDEO_ZOOM_APP_PRIVILEGE_TOKEN: ''
  } });
  let buffer = '', stderr = '', nextId = 0, closed = false, failure, snapshot;
  const pending = new Map();
  const fail = error => { failure ??= error; for (const item of pending.values()) { clearTimeout(item.timer); item.reject(error); } pending.clear(); };
  const close = new Promise(resolveClose => child.once('close', (code, signal) => { closed = true; run.exitCode = code; run.exitSignal = signal; fail(new Error('Owned native child closed.')); resolveClose(); }));
  child.on('error', fail); child.stdin.on('error', fail);
  child.stdout.setEncoding('utf8'); child.stderr.setEncoding('utf8');
  child.stderr.on('data', text => { stderr = (stderr + text).slice(-131072); });
  child.stdout.on('data', text => {
    buffer += text;
    let end;
    while ((end = buffer.indexOf('\n')) >= 0) {
      const line = buffer.slice(0, end); buffer = buffer.slice(end + 1); if (!line.trim()) continue;
      if (line.length > 1024 * 1024) { fail(new Error('RPC line bound exceeded.')); child.kill(); return; }
      let message; try { message = JSON.parse(line); } catch { fail(new Error('Invalid native JSON.')); continue; }
      const item = pending.get(message.id); if (!item) continue;
      clearTimeout(item.timer); pending.delete(message.id);
      if (message.ok === true) item.resolve(message); else item.reject(new Error(`RPC failed: ${JSON.stringify(message.error).slice(0, 500)}`));
    }
    if (buffer.length > 1024 * 1024) { fail(new Error('RPC buffer bound exceeded.')); child.kill(); }
  });
  const send = (type, extra = {}) => new Promise((resolveRequest, reject) => {
    if (failure) { reject(failure); return; }
    const id = `av-${++nextId}`, timer = setTimeout(() => { pending.delete(id); reject(new Error(`${type} timed out.`)); }, 5000);
    pending.set(id, { resolve: resolveRequest, reject, timer }); child.stdin.write(JSON.stringify({ id, type, ...extra }) + '\n');
  });
  const observe = value => {
    snapshot = value; const state = snapshot?.recording?.lifecycle;
    if (state && run.states.at(-1)?.state !== state.state) run.states.push({ at: Date.now(), ...state });
    if (state?.state === 'failed' || state?.state === 'interrupted') throw new Error(`Recording ${state.state}.`);
    if (snapshot?.programBuffer?.activeFrames !== frames || snapshot?.programBuffer?.status === 'failed') throw new Error('Requested buffer is not active.');
    if (run.generation !== undefined && snapshot.programBuffer.generation !== run.generation) throw new Error('Buffer generation changed.');
    if (!Number.isSafeInteger(snapshot.programBuffer.generation) || snapshot.programBuffer.generation <= 0) throw new Error('Missing buffer generation.');
    run.generation = snapshot.programBuffer.generation;
  };
  const sync = async commands => observe((await send('media-core-sync', { commands })).snapshot);
  const poll = async () => observe((await send('snapshot')).snapshot);
  try {
    run.profile = (await send('handshake', { protocolVersion: { major: 1, minor: 0 } })).profile;
    if (run.profile?.encoder !== 'media-foundation') throw new Error('Real Windows Media Foundation encoder required.');
    const primeDeadline = Date.now() + 15000;
    let initial;
    do {
      initial = (await send('snapshot')).snapshot?.programBuffer;
      if (initial?.status === 'failed' || initial?.status === 'unsupported') throw new Error(`Buffer is ${initial.status}.`);
      if (initial?.activeFrames === frames && initial?.generation > 0 && initial?.delivered > frames) break;
      await sleep(250);
    } while (Date.now() < primeDeadline);
    if (initial?.activeFrames !== frames || !(initial?.generation > 0) || !(initial?.delivered > frames)) throw new Error('Buffer did not prime.');
    await sync([
      { type: 'set-output-profile', width: 1920, height: 1080, fps: 60, targetBitrateMbps: 8 },
      { type: 'load-scene-graph', sceneId: 'flash-beep-proof', routes: [{ routeId: 'program', mode: 'fixed', audioRole: 'mix',
        mediaAssetId: 'flash-beep', mediaAssetName: 'Generated flash and beep', mediaAssetKind: 'video', mediaAssetPath: fixture,
        mediaAssetPlaying: true, mediaPlaybackKey: `depth-${frames}`, rect: { x: 0, y: 0, width: 1, height: 1 } }] },
      { type: 'sync-audio-routing-matrix', sends: [{ sourceId: 'media:flash-beep', busId: 'master', gainDb: 0 }] },
      { type: 'start-program-output', destinations: ['recording'], isoParticipantIds: [] },
      { type: 'set-recording-targets', targetFolder: runDirectory, filenamePrefix: 'flash-beep', format: 'mp4', quality: 'high', isoParticipantIds: [] },
      { type: 'start-recording-session', sessionId: `flash-beep-${frames}`, targetFolder: runDirectory, filenamePrefix: 'flash-beep', format: 'mp4', quality: 'high', isoParticipantIds: [] }
    ]);
    const startDeadline = Date.now() + 15000;
    while (snapshot.recording?.lifecycle?.state !== 'live' && Date.now() < startDeadline) { await sleep(200); await poll(); }
    if (snapshot.recording?.lifecycle?.state !== 'live') throw new Error('Recording did not become live.');
    const sessionId = snapshot.recording.lifecycle.sessionId;
    run.startBuffer = snapshot.programBuffer;
    const end = Date.now() + 7500;
    while (Date.now() < end) { await sleep(500); await poll(); }
    run.liveRecording = snapshot.recording;
    await sync([{ type: 'stop-recording-session', reason: 'Bounded flash/beep measurement complete' }]);
    const finalDeadline = Date.now() + 20000;
    while (snapshot.recording?.lifecycle?.state !== 'completed' && Date.now() < finalDeadline) { await sleep(200); await poll(); }
    const state = snapshot.recording?.lifecycle;
    if (state?.state !== 'completed' || !state.finalized || state.sessionId !== sessionId) throw new Error('Matching recording finalization not confirmed.');
    run.finalRecording = snapshot.recording; run.endBuffer = snapshot.programBuffer;
    const artifact = resolve(dirname(core), snapshot.recording.artifactPath ?? '');
    if (!artifact.startsWith(runDirectory + sep) || (await stat(artifact)).size < 1024) throw new Error('Missing recording artifact inside isolated run directory.');
    run.artifact = artifact;
  } catch (error) { run.errors.push(error.message); run.lastSnapshot = snapshot; }
  finally {
    child.stdin.end(); if (!closed) await Promise.race([close, sleep(3000)]);
    if (!closed) { run.errors.push('Owned child required forced shutdown.'); child.kill(); await Promise.race([close, sleep(1000)]); }
    if (!closed || run.exitCode !== 0) run.errors.push('Owned child did not exit cleanly.');
    await writeFile(join(runDirectory, 'stderr-tail.log'), stderr);
  }
  return run;
}

try {
  const fixture = join(directory, 'flash-beep.mp4');
  const videoPulseExpression = FLASH_BEEP_PULSES.map(p => `gte(n,${p.startFrame})*lt(n,${p.startFrame + p.durationFrames})`).join('+');
  const audioPulseExpression = FLASH_BEEP_PULSES.map(p => `gte(t,${p.startFrame / 60})*lt(t,${(p.startFrame + p.durationFrames) / 60})`).join('+');
  await tool(ffmpeg, ['-hide_banner', '-v', 'error', '-f', 'lavfi', '-i', 'color=c=black:s=1920x1080:r=60:d=14',
    '-f', 'lavfi', '-i', `aevalsrc='if(${audioPulseExpression},0.7*sin(2*PI*1000*t),0)':s=48000:d=14`,
    '-vf', `drawbox=color=white:t=fill:enable='${videoPulseExpression}'`, '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '18', '-pix_fmt', 'yuv420p',
    '-c:a', 'aac', '-b:a', '192k', '-movflags', '+faststart', fixture]);
  report.fixture = { path: fixture, decode: await decode(fixture) };
  if (report.fixture.decode.alignmentError) throw new Error('Fixture: ' + report.fixture.decode.alignmentError);
  for (const frames of [2, 3]) {
    const run = await record(frames, fixture); report.runs.push(run);
    if (run.artifact) {
      try {
        run.decode = await decode(run.artifact);
        run.videoArtifactAcceptance = assessRecordingVideoEvidence(run.decode, run.finalRecording?.proof);
        run.recordingDestinationDecoded = run.decode.decodeCompleted;
        run.analysisValid = run.decode.analysisValid;
        run.analysisErrors = run.decode.errors;
        run.coverageErrors = run.decode.coverageErrors;
        if (run.decode.alignment && report.fixture.decode.alignment) {
          run.sourceCorrectedAlignment = sourceCorrectedAlignment(run.decode.alignment, report.fixture.decode.alignment);
          run.sourceCorrectedAudioMinusVideoMs = run.sourceCorrectedAlignment.medianAudioMinusVideoMs;
        }
        run.alignmentWithinOneVideoFrame = run.analysisValid && run.sourceCorrectedAlignment?.alignmentWithinOneVideoFrame === true;
      } catch (error) { run.errors.push(error.message); }
    }
  }
} catch (error) { report.errors.push(error.message); }
finally {
  report.measurementCompleted = report.errors.length === 0 && report.runs.length === 2 && report.runs.every(run => !run.errors.length && run.recordingDestinationDecoded && run.analysisValid);
  report.avAlignmentWithinOneVideoFrame = report.measurementCompleted && report.runs.every(run => run.alignmentWithinOneVideoFrame === true);
  report.recordingArtifactAccepted = report.runs.length === 2 && report.runs.every(run => run.videoArtifactAcceptance?.passed === true);
  report.validationPassed = report.measurementCompleted && report.avAlignmentWithinOneVideoFrame && report.recordingArtifactAccepted;
  await writeFile(join(directory, 'report.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(JSON.stringify({ report: join(directory, 'report.json'), measurementCompleted: report.measurementCompleted,
    avAlignmentWithinOneVideoFrame: report.avAlignmentWithinOneVideoFrame, recordingArtifactAccepted: report.recordingArtifactAccepted,
    validationPassed: report.validationPassed, framePerformancePassed: false }));
  process.exitCode = report.validationPassed ? 0 : 1;
}
