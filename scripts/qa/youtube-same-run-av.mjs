// #703 same-run external A/V evidence: one pattern send, four decoded legs.
//
//   source    the generated flash/beep fixture
//   recording the matching Program.mp4 written by the same core run
//   rtmp      the exact FLV packets the core's FFmpeg pushed (QA tee tap)
//   youtube   the destination's own PLAYBACK, captured while the send runs
//
// All four are judged by measure-av-pattern.mjs with --require-all, so a
// missing leg is a failure, never an implied end-to-end pass.
//
// Real run (owner go-ahead + a LIVE event required):
//   node scripts/qa/youtube-same-run-av.mjs --youtube-watch-url <URL> --output-dir artifacts/issue-703-same-run
//   The RTMP server and stream key come from the app's saved output settings
//   (production-output-preferences.json, StreamRtmpServerUrl/StreamRtmpStreamKey),
//   decrypted in-process with DPAPI exactly as the app does. The key is handed to
//   the sender only through COREVIDEO_QA_RTMP_KEY and is never printed or written.
//
// Dry run (no external traffic): --local-standin replaces YouTube with a local
// FFmpeg RTMP listener that re-serves the stream as live MPEG-TS over loopback UDP.
import { spawn, execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { mkdir, readFile, readdir, stat, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const exec = promisify(execFile);
const sleep = ms => new Promise(done => setTimeout(done, ms));
const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, '..', '..');

const flags = new Set(['--local-standin']);
const valued = new Set(['--youtube-watch-url', '--playback-url', '--output-dir', '--native-core', '--prefs',
  '--duration-seconds', '--capture-seconds', '--depth', '--ffmpeg', '--ffprobe', '--python']);
const options = {};
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; ++i) {
  if (flags.has(argv[i])) { options[argv[i]] = true; continue; }
  if (!valued.has(argv[i]) || argv[i + 1] === undefined) {
    throw new Error('Usage: node scripts/qa/youtube-same-run-av.mjs (--youtube-watch-url URL | --playback-url URL | --local-standin) --output-dir DIR [--native-core EXE] [--prefs FILE] [--duration-seconds 24] [--capture-seconds N] [--depth 2|3]');
  }
  options[argv[i]] = argv[++i];
}
const standin = Boolean(options['--local-standin']);
const watchUrl = options['--youtube-watch-url'];
if ([standin, Boolean(watchUrl), Boolean(options['--playback-url'])].filter(Boolean).length !== 1)
  throw new Error('Choose exactly one of --youtube-watch-url, --playback-url, --local-standin.');
if (!options['--output-dir']) throw new Error('--output-dir is required.');
// PATH FFmpeg, like the sender: the fixture generator needs libx264, which the
// C:/ffmpeg runtime build used by the core does not carry.
const ffmpeg = options['--ffmpeg'] ?? 'ffmpeg';
const ffprobe = options['--ffprobe'] ?? 'ffprobe';
const python = options['--python'] ?? 'python';
const core = resolve(options['--native-core'] ?? join(repo, 'native', 'build-dev', 'corevideo-native.exe'));
const durationSeconds = Number(options['--duration-seconds'] ?? 24);
const depth = options['--depth'] ?? '2';
// YouTube's live edge trails ingest by several seconds (more at normal latency);
// capture long enough that the whole pattern is inside the playback window.
const captureSeconds = Number(options['--capture-seconds'] ?? (standin ? durationSeconds + 30 : durationSeconds + 120));
// After the send ends, keep capturing long enough for the destination's playback
// to catch up (YouTube live trails ingest), then stop FFmpeg cleanly with 'q'.
const trailSeconds = standin ? 5 : 45;
const outputDir = resolve(options['--output-dir']);
await mkdir(outputDir, { recursive: true });
const runDir = join(outputDir, `same-run-${Date.now()}`);
await mkdir(runDir);

const secrets = [];
const redact = text => {
  let out = String(text);
  for (const secret of secrets) if (secret && secret.length >= 6) out = out.split(secret).join('[redacted]');
  // Signed googlevideo playback URLs are session credentials too.
  return out.replace(/https?:\/\/[^\s"']*googlevideo\.com[^\s"']*/g, '[googlevideo-url]');
};

// --- destination -----------------------------------------------------------
async function loadSavedRtmpDestination() {
  const prefsPath = resolve(options['--prefs'] ?? join(process.env.LOCALAPPDATA ?? '', 'CoreVideoPro', 'production-output-preferences.json'));
  const prefs = JSON.parse((await readFile(prefsPath, 'utf8')).replace(/^\uFEFF/, ''));
  const protocol = String(prefs.StreamRtmpProtocol || 'rtmp').toLowerCase();
  const serverField = String(prefs.StreamRtmpServerUrl || '').trim();
  const stored = String(prefs.StreamRtmpStreamKey || '');
  if (!serverField || !stored) throw new Error(`Saved RTMP server or stream key missing in ${prefsPath}.`);
  const server = /^rtmps?:\/\//i.test(serverField) ? serverField : `${protocol}://${serverField}`;
  let key = stored;
  if (stored.startsWith('dpapi:')) {
    // Same contract as DpapiSecretProtector: CurrentUser scope, fixed entropy.
    // The blob goes in on stdin and the plaintext comes back on stdout only.
    const script = "$ErrorActionPreference='Stop';Add-Type -AssemblyName System.Security;" +
      "$b=[Convert]::FromBase64String([Console]::In.ReadToEnd().Trim());" +
      "$e=[Text.Encoding]::UTF8.GetBytes('CoreVideoPro.secret.v1');" +
      "$p=[Security.Cryptography.ProtectedData]::Unprotect($b,$e,[Security.Cryptography.DataProtectionScope]::CurrentUser);" +
      "[Console]::Out.Write([Text.Encoding]::UTF8.GetString($p))";
    key = await new Promise((done, fail) => {
      const child = spawn('powershell', ['-NoProfile', '-NonInteractive', '-Command', script], { windowsHide: true });
      let out = '', err = '';
      child.stdout.on('data', d => { out += d; });
      child.stderr.on('data', d => { err += d; });
      child.on('error', fail);
      child.on('close', code => code === 0 && out ? done(out) : fail(new Error(`DPAPI unprotect failed (exit ${code}): ${err.slice(0, 200)}`)));
      child.stdin.end(stored.slice('dpapi:'.length));
    });
  }
  secrets.push(key);
  return { server, key, prefsPath, codec: prefs.StreamVideoCodec ?? null };
}

// --- playback capture ------------------------------------------------------
async function resolveWatchUrl(url) {
  // Best video up to 1080p plus best audio; live HLS/DASH manifests.
  const { stdout } = await exec(python, ['-m', 'yt_dlp', '-g', '--no-warnings', '-f', 'bv*[height<=1080]+ba/b', url],
    { windowsHide: true, timeout: 60000, maxBuffer: 1 << 20 });
  const urls = stdout.split(/\r?\n/).map(s => s.trim()).filter(Boolean);
  if (!urls.length) throw new Error('yt-dlp returned no playback URL (is the event live?).');
  for (const u of urls) secrets.push(u);
  return urls;
}

function startCapture(inputs, path, seconds) {
  const args = ['-hide_banner', '-loglevel', 'error', '-y'];
  for (const input of inputs) args.push('-thread_queue_size', '1024', '-i', input);
  if (inputs.length > 1) args.push('-map', '0:v:0', '-map', '1:a:0');
  args.push('-c', 'copy', '-t', String(seconds), path);
  const child = spawn(ffmpeg, args, { windowsHide: true, stdio: ['pipe', 'ignore', 'pipe'] });
  let stderr = '';
  child.stderr.on('data', d => { stderr = (stderr + d).slice(-8192); });
  // -t counts OUTPUT time, so a playback that never delivers would wait forever.
  // Bound the capture by wall time and say so, never hang the run.
  let watchdogFired = false;
  const watchdog = setTimeout(() => { watchdogFired = true; child.kill(); }, (seconds + 45) * 1000);
  const done = new Promise(r => child.once('close', code => {
    clearTimeout(watchdog);
    r({ code, watchdogFired, stderr: redact(stderr) });
  }));
  const stop = () => { try { child.stdin.write('q'); child.stdin.end(); } catch {} };
  return { child, done, stop };
}

// --- run -------------------------------------------------------------------
const summary = { issue: 703, mode: standin ? 'local-standin' : watchUrl ? 'youtube' : 'playback-url',
  core, durationSeconds, captureSeconds, depth, runDir, legs: {}, errors: [] };
let receiver = null;
try {
  let destination;
  let playbackInputs;
  if (standin) {
    const rtmpPort = 19360, udpPort = 19361;
    destination = { server: `rtmp://127.0.0.1:${rtmpPort}/live`, key: 'standin-stream-key', prefsPath: null, codec: null };
    secrets.push(destination.key);
    receiver = spawn(ffmpeg, ['-hide_banner', '-loglevel', 'error', '-listen', '1', '-i',
      `rtmp://127.0.0.1:${rtmpPort}/live/${destination.key}`, '-c', 'copy', '-f', 'mpegts',
      `udp://127.0.0.1:${udpPort}?pkt_size=1316`], { windowsHide: true, stdio: 'ignore' });
    // A read timeout lets the capture notice 'q' after the stand-in stops sending.
    playbackInputs = [`udp://127.0.0.1:${udpPort}?fifo_size=1000000&overrun_nonfatal=1&timeout=30000000`];
    await sleep(1500);
  } else {
    destination = await loadSavedRtmpDestination();
    playbackInputs = watchUrl ? await resolveWatchUrl(watchUrl) : [options['--playback-url']];
  }
  summary.destination = { server: destination.server, keySource: destination.prefsPath ? 'production-output-preferences.json (DPAPI)' : 'local stand-in',
    savedCodec: destination.codec, sentCodec: 'h264 (sender default)' };

  const capturePath = join(runDir, 'playback-capture.mkv');
  const capture = startCapture(playbackInputs, capturePath, captureSeconds);
  await sleep(1000);

  const sender = spawn(process.execPath, [join(here, 'program-buffer-recorded-av.mjs'), '--native-core', core,
    '--output-dir', runDir, '--rtmp-server', destination.server, '--rtmp-tap', '1',
    '--duration-seconds', String(durationSeconds), '--depth', depth, '--ffmpeg', ffmpeg, '--ffprobe', ffprobe],
  { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, COREVIDEO_QA_RTMP_KEY: destination.key } });
  let senderOut = '';
  sender.stdout.on('data', d => { senderOut += d; });
  sender.stderr.on('data', d => { senderOut += d; });
  const senderCode = await new Promise(r => sender.once('close', r));
  summary.senderExitCode = senderCode;
  summary.senderTail = redact(senderOut).slice(-2000);

  await Promise.race([capture.done, sleep(trailSeconds * 1000)]);
  capture.stop();
  const captureResult = await capture.done;
  summary.captureExitCode = captureResult.code;
  if (captureResult.watchdogFired) summary.errors.push('playback capture received too little media and was stopped by its wall-time watchdog');
  if (captureResult.stderr) summary.captureStderr = captureResult.stderr;

  // Locate the sender's report and legs.
  const reportDir = (await readdir(runDir)).find(name => name.startsWith('recorded-av-'));
  if (!reportDir) throw new Error('Sender produced no report directory.');
  const report = JSON.parse(await readFile(join(runDir, reportDir, 'report.json'), 'utf8'));
  const run = report.runs?.[0] ?? {};
  summary.senderReport = join(runDir, reportDir, 'report.json');
  // The tap FFmpeg is stopped without draining (#708), so its final FLV tag is
  // truncated and the strict (-xerror) analyzer rejects the whole file. Drop
  // only packets the demuxer itself flags corrupt, and say so in the summary.
  let rtmpLeg = run.rtmpTapTrimmedPath ?? run.rtmpTapPath ?? null;
  if (run.rtmpTapTrimmedPath) summary.rtmpTapStopTrimmed = { from: run.rtmpTapPath, to: rtmpLeg, reason: 'truncated final tag at FFmpeg stop (#708)' };
  else if (rtmpLeg && (await stat(rtmpLeg).catch(() => null))?.size > 1024) {
    const trimmed = rtmpLeg.replace(/\.flv$/, '.stop-trimmed.flv');
    const remux = await exec(ffmpeg, ['-hide_banner', '-loglevel', 'error', '-y', '-fflags', '+discardcorrupt',
      '-i', rtmpLeg, '-map', '0', '-c', 'copy', trimmed], { windowsHide: true, timeout: 120000 })
      .then(() => true, () => false);
    if (remux) { summary.rtmpTapStopTrimmed = { from: rtmpLeg, to: trimmed, reason: 'truncated final tag at FFmpeg stop (#708)' }; rtmpLeg = trimmed; }
  }
  summary.legs = { source: report.fixture?.path ?? null, recording: run.artifact ?? null,
    rtmp: rtmpLeg, youtube: (await stat(capturePath).catch(() => null))?.size > 1024 ? capturePath : null };

  const measureArgs = [join(here, 'measure-av-pattern.mjs'), '--source', summary.legs.source,
    '--ffmpeg', ffmpeg, '--ffprobe', ffprobe, '--require-all', '--output', join(runDir, 'same-run-measurement.json')];
  for (const role of ['recording', 'rtmp', 'youtube'])
    if (summary.legs[role]) measureArgs.push(`--${role}`, summary.legs[role]);
  const measured = await exec(process.execPath, measureArgs, { windowsHide: true, timeout: 300000, maxBuffer: 1 << 24 })
    .then(r => ({ code: 0, out: r.stdout }), e => ({ code: e.code ?? 1, out: `${e.stdout ?? ''}${e.stderr ?? ''}` }));
  summary.measurementExitCode = measured.code;
  summary.measurement = join(runDir, 'same-run-measurement.json');
  try {
    const m = JSON.parse(await readFile(summary.measurement, 'utf8'));
    summary.videoMinusAudioMs = Object.fromEntries(Object.entries(m.legs).map(([k, v]) => [k, v.videoMinusAudioMs]));
    summary.sourceCorrectedMs = Object.fromEntries(Object.entries(m.legs).map(([k, v]) => [k, v.sourceCorrectedMs]));
    summary.pairedCueIds = Object.fromEntries(Object.entries(m.legs).map(([k, v]) => [k, v.pairedCueIds]));
    summary.endToEndMeasured = m.endToEndMeasured;
    summary.errors.push(...m.errors);
  } catch (error) { summary.errors.push(`measurement unreadable: ${redact(measured.out).slice(0, 500)}`); }
  if (senderCode !== 0) summary.errors.push(`sender exited ${senderCode} (see senderReport; strict frame gates may be red)`);
} catch (error) {
  summary.errors.push(redact(error.message));
} finally {
  if (receiver && receiver.exitCode === null) receiver.kill();
}
const text = redact(JSON.stringify(summary, null, 2));
await writeFile(join(runDir, 'same-run-summary.json'), text + '\n');
console.log(text);
process.exitCode = summary.endToEndMeasured ? 0 : 1;
