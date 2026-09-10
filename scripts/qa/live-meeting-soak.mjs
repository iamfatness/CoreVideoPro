/**
 * live-meeting-soak — the A/V load soak (beta plan B6 / alpha G3) against a REAL
 * Zoom meeting, driven headlessly over the core's stdio wire. No WinUI, no
 * Computer Use, no desktop takeover.
 *
 * What it proves (and what it refuses to assume):
 *   - N real Zoom participants become live GPU tiles on the wall (not just roster rows).
 *   - Recording AND the virtual camera run SIMULTANEOUSLY for the whole soak.
 *   - Sustained render fps, dropped/skipped slots, ingest->render latency percentiles,
 *     operator command round-trip percentiles and coreMutex over-budget ratio, all
 *     sampled from the core's own evidence at both ends of the window (deltas, not
 *     lifetime totals).
 *   - The core's working set is sampled throughout so a SLOPE after warm-up is visible.
 *     (CLAUDE.md: engine memory plateaus ~3.3GB and that plateau is not a leak.)
 *   - The ARTIFACT is validated, not the counters: ffprobe -count_frames for the real
 *     muxed rate, a full decode to null, and a mean-luma pixel check. CLAUDE.md records
 *     a recording that shipped 8995 frames of flat luma while every validator passed,
 *     so a frame count alone is not evidence of pictures.
 *   - With --takes N, N Takes are driven by alternating `load-scene-graph` between two
 *     SYNTHESIZED scenes (`--scene-a`/`--scene-b`, default `take-a`/`take-b` — these are
 *     harness-invented scene ids on the core's stdio wire, not the shell's real scene
 *     names such as "speaker-slides"/"panel", which do not exist on this wire), and the
 *     core's own `sessionState().takeRecords` is judged by `judgeTakeRecords` (see
 *     take-verdict-judge.mjs) instead of eyeballing the picture across a Take.
 *
 * Usage:
 *   node scripts/qa/live-meeting-soak.mjs --meeting-url "<url>" [--minutes 30]
 *        [--sources 8] [--passcode X] [--churn] [--out DIR] [--keep-artifact]
 *        [--takes N] [--scene-a take-a] [--scene-b take-b]
 *   COREVIDEO_TEST_MEETING_URL / COREVIDEO_ZOOM_MEETING_PASSCODE are honoured so the
 *   URL never has to appear on a command line or in a log. The URL is NEVER printed:
 *   only a redacted form reaches stdout or the evidence file.
 */
import { spawn, execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { existsSync, readFileSync } from 'node:fs';
import { mkdir, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { judgeTakeRecords } from './take-verdict-judge.mjs';

const exec = promisify(execFile);
const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, '..', '..');
const EXE = process.platform === 'win32' ? '.exe' : '';
const buildDir = process.env.COREVIDEO_BUILD_DIR
  || join(repoRoot, 'native', process.platform === 'win32' ? 'build-dev' : 'build-metal');

const argv = process.argv.slice(2);
const flag = (name) => argv.includes(`--${name}`);
const arg = (name, fallback) => {
  const i = argv.indexOf(`--${name}`);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : fallback;
};

const meetingUrl = arg('meeting-url', process.env.COREVIDEO_TEST_MEETING_URL);
const passcode = arg('passcode', process.env.COREVIDEO_ZOOM_MEETING_PASSCODE);
const minutes = Number(arg('minutes', '30'));
const wantSources = Number(arg('sources', '8'));
const doChurn = flag('churn');
const outDir = resolve(arg('out', join(repoRoot, 'artifacts', 'live-meeting-soak')));
const nativeCore = resolve(arg('native-core', join(buildDir, `corevideo-native${EXE}`)));
const zoomEngine = resolve(arg('zoom-engine',
  process.env.COREVIDEO_ZOOM_ENGINE_PATH || join(buildDir, `corevideo-zoom-engine${EXE}`)));

// Takes: N alternations of load-scene-graph between two SYNTHESIZED scene ids, judged
// by the core's own takeRecords ring (see take-verdict-judge.mjs / CLAUDE.md "A Take is
// traceable"). Defaults are harness-invented ids — "speaker-slides"/"panel" are shell
// scene names that do not exist on this stdio wire.
const takes = Number(arg('takes', '0'));
const sceneA = arg('scene-a', 'take-a');
const sceneB = arg('scene-b', 'take-b');

function usage() {
  console.log(`Usage: node scripts/qa/live-meeting-soak.mjs --meeting-url "<url>" [--minutes 30]
       [--sources 8] [--passcode X] [--churn] [--out DIR] [--keep-artifact]
       [--takes N] [--scene-a take-a] [--scene-b take-b]
COREVIDEO_TEST_MEETING_URL / COREVIDEO_ZOOM_MEETING_PASSCODE are honoured in place of
--meeting-url / --passcode so the URL never has to appear on a command line.`);
}

// A soak must be reproducible from evidence, not from what the operator remembers.
const redactUrl = (u) => {
  if (!u) return 'none';
  try {
    const parsed = new URL(u);
    const id = (parsed.pathname.match(/\/j\/(\d+)/) || [])[1];
    return `${parsed.host}/j/${id ? `***${id.slice(-3)}` : '***'}${parsed.search ? '?<redacted>' : ''}`;
  } catch { return '<unparseable url, redacted>'; }
};

if (!meetingUrl) { usage(); die('Missing --meeting-url (or COREVIDEO_TEST_MEETING_URL).'); }
if (!Number.isFinite(takes) || takes < 0) die(`--takes must be a non-negative number, got ${arg('takes', '0')}.`);
if (!existsSync(nativeCore)) die(`Missing native core at ${nativeCore}.`);
if (!existsSync(zoomEngine)) die(`Missing Zoom engine at ${zoomEngine}.`);
function die(msg) { console.error(`live-meeting-soak: ${msg}`); process.exit(2); }

const embeddedKey = (() => {
  try {
    return JSON.parse(readFileSync(join(repoRoot, 'src', 'config', 'zoomMeetingSdk.json'), 'utf8')).publicAppKey;
  } catch { return undefined; }
})();

await mkdir(outDir, { recursive: true });

const child = spawn(nativeCore, [], {
  cwd: dirname(nativeCore),
  windowsHide: true,
  stdio: ['pipe', 'pipe', 'pipe'],
  env: {
    ...process.env,
    COREVIDEO_ZOOM_ENGINE_PATH: zoomEngine,
    COREVIDEO_ZOOM_PUBLIC_APP_KEY: process.env.COREVIDEO_ZOOM_PUBLIC_APP_KEY ?? embeddedKey ?? '',
    COREVIDEO_ZOOM_JOIN_WAIT_MS: process.env.COREVIDEO_ZOOM_JOIN_WAIT_MS ?? '90000',
  },
});

const stderrLines = [];
child.stderr.on('data', (c) => {
  for (const line of c.toString().split(/\r?\n/)) if (line) stderrLines.push(line);
});

let nextId = 1, buf = '', handshake;
const pending = new Map();
child.stdout.on('data', (c) => {
  buf += c.toString();
  let i;
  while ((i = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, i).trim(); buf = buf.slice(i + 1);
    if (!line) continue;
    let msg; try { msg = JSON.parse(line); } catch { continue; }
    if (msg.type === 'handshake' || msg.profile) { handshake ??= msg; continue; }
    const entry = pending.get(msg.id);
    if (entry) { pending.delete(msg.id); clearTimeout(entry.timer); entry.resolve(msg); }
  }
});

const send = (type, extra = {}, timeoutMs = 30000) => new Promise((resolveP, rejectP) => {
  const id = nextId++;
  const timer = setTimeout(() => { pending.delete(id); rejectP(new Error(`${type} timed out after ${timeoutMs}ms`)); }, timeoutMs);
  pending.set(id, { resolve: resolveP, timer });
  child.stdin.write(JSON.stringify({ id, type, ...extra }) + '\n');
});
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** One 2Hz operator poll, timed. This is the number the ISO worker thread could hurt. */
async function sync(commands, elapsedMs) {
  const t0 = process.hrtime.bigint();
  const reply = await send('media-core-sync', { elapsedMs, commands }, 15000);
  return { ms: Number(process.hrtime.bigint() - t0) / 1e6, snapshot: reply?.snapshot ?? {} };
}

const pct = (sorted, p) => sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * p))] : null;
const counter = (node, ...path) => {
  for (const k of path) { if (!node || typeof node !== 'object') return null; node = node[k]; }
  return typeof node === 'number' && Number.isFinite(node) ? node : null;
};

/**
 * The ring in sessionState().takeRecords keeps only the last 8 records — a --takes N
 * with N > 8 would lose records if read once at the end. So we read after EVERY take
 * and accumulate the ones we have not seen yet, deduped by the record's own identity
 * (fromSceneId|toSceneId|armedAtMs). See take-7 controller ruling: this is the "read
 * after every take" option, not the "cap at 8" option.
 */
function collectTakeRecords(snapshot, seen) {
  const records = snapshot?.takeRecords?.records ?? [];
  for (const r of records) {
    const key = `${r.fromSceneId}|${r.toSceneId}|${r.armedAtMs}`;
    if (!seen.has(key)) seen.set(key, r);
  }
}

const evidence = {
  scope: 'Real Zoom meeting A/V load soak: N live 1080p participants on the wall, recording and virtual camera simultaneous.',
  status: 'failed', startedAt: new Date().toISOString(),
  core: nativeCore, engine: zoomEngine, meeting: redactUrl(meetingUrl),
  requestedSources: wantSources, minutes, churn: doChurn, takes, sceneA, sceneB,
};
const failures = [];

try {
  // Wait for the handshake before anything else — a command sent into a core that
  // has not announced its profile is a race, not a test.
  for (let i = 0; i < 100 && !handshake; i++) await sleep(100);
  if (!handshake) throw new Error('core never sent a handshake');
  evidence.profile = handshake.profile?.name ?? 'unknown';
  console.log(`Core        : ${nativeCore}`);
  console.log(`Engine      : ${zoomEngine}`);
  console.log(`Meeting     : ${evidence.meeting}`);
  console.log(`Handshake   : ${evidence.profile}`);

  // The perf lines this harness reads are suppressed in production mode.
  await send('media-core-sync', { elapsedMs: 0, commands: [{ type: 'set-verbose-diagnostics', enabled: true }] });

  const joined = await send('zoom-join', {
    payload: { meetingUrl, displayName: 'CoreVideo Live Soak', ...(passcode ? { passcode } : {}) },
  }, 120000);
  evidence.joinState = joined?.snapshot?.meetingState ?? null;
  console.log(`Join        : ${JSON.stringify(evidence.joinState)}`);

  // ── roster -> live tiles ────────────────────────────────────────────────────
  // Participants populate the roster but are NOT GPU tiles until they are assigned
  // to inputs, so poll the roster first and then assign what is actually there.
  let roster = [];
  const rosterDeadline = Date.now() + 120000;
  while (Date.now() < rosterDeadline) {
    const { snapshot } = await sync([], 500);
    roster = (snapshot?.zoom?.participants ?? snapshot?.participants ?? [])
      .filter((p) => p && (p.hasVideo ?? p.videoOn ?? true));
    if (roster.length >= wantSources) break;
    await sleep(1000);
  }
  evidence.rosterVideoParticipants = roster.length;
  console.log(`Roster      : ${roster.length} participant(s) with video (wanted ${wantSources})`);
  if (roster.length === 0) throw new Error('no participants with video in the meeting — nothing to soak');
  if (roster.length < wantSources) failures.push(`only ${roster.length} of ${wantSources} video participants were present — this soak is at reduced load and does not prove the 8-source claim`);

  const chosen = roster.slice(0, wantSources);
  const pid = (p) => String(p.participantId ?? p.id ?? p.userId);
  const sources = chosen.map((p, i) => ({
    sourceId: `zoom:${pid(p)}`, kind: 'zoom', participantId: pid(p), slot: i,
    label: p.displayName ?? p.name ?? `CAM ${i + 1}`,
  }));

  await sync([
    { type: 'configure-multiviewer', layoutMode: 'pgmPvwTop', tileCount: Math.max(10, sources.length) },
    { type: 'set-multiview-layout', canvasWidth: 1920, canvasHeight: 1080, sources },
    { type: 'load-scene-graph', sceneId: 'pgm', routes: [
      { routeId: 'pgm-0', slot: 0, mode: 'fixed', participantId: sources[0].participantId }] },
    { type: 'set-preview-scene', sceneId: 'pvw', routes: [
      { routeId: 'pvw-0', slot: 0, mode: 'fixed', participantId: (sources[1] ?? sources[0]).participantId }] },
  ], 500);
  await sleep(5000);   // engine subscribe + first frame on every source

  // ── Takes: judge Takes by the take record, not by eye ───────────────────────
  const takeRecordsSeen = new Map();
  if (takes > 0) {
    console.log(`Takes       : driving ${takes} take(s), alternating "${sceneA}" <-> "${sceneB}" (synthesized scenes, not shell scene ids)`);
    for (let i = 0; i < takes; i++) {
      const targetScene = i % 2 === 0 ? sceneA : sceneB;
      const src = sources[i % sources.length];
      const { snapshot } = await sync([
        { type: 'load-scene-graph', sceneId: targetScene, routes: [
          { routeId: `${targetScene}-0`, slot: 0, mode: 'fixed', participantId: src.participantId }] },
      ], 500);
      collectTakeRecords(snapshot, takeRecordsSeen);
      await sleep(3000);
      // One more poll per take: the record completes on the render tick AFTER the
      // scene swap, so the reply to the swap itself can still show it pending.
      const { snapshot: after } = await sync([], 500);
      collectTakeRecords(after, takeRecordsSeen);
    }
    const takeRecords = [...takeRecordsSeen.values()];
    const takeJudge = judgeTakeRecords(takeRecords, { expectedTakes: takes });
    evidence.takeJudge = takeJudge;
    console.log(`Takes       : ${takeJudge.cuts} cut, ${takeJudge.rebuilt} rebuilt (of ${takeJudge.total}/${takeJudge.expectedTakes} expected)`);
    for (const r of takeJudge.reasons) {
      console.log(`  take ${r.fromSceneId} -> ${r.toSceneId}: ${r.verdict} restartedSources=${JSON.stringify(r.restartedSources)} missingSources=${JSON.stringify(r.missingSources)} backgroundDropped=${r.backgroundDropped} subscriptionsChurned=${r.subscriptionsChurned}`);
    }
    if (!takeJudge.ok) failures.push(`takes: ${takeJudge.cuts} cut, ${takeJudge.rebuilt} rebuilt of ${takeJudge.total}/${takes} expected — not every take cut cleanly (see takeJudge.reasons in the evidence file)`);
  }

  // ── record AND virtual camera, together ─────────────────────────────────────
  await sync([{ type: 'sync-virtual-camera', on: true, mirror: false, deviceName: 'live-meeting-soak' }], 500);
  const recordStartedAt = Date.now();
  const start = await sync([
    { type: 'start-program-output', destinations: ['recording'], isoSourceIds: [], isoParticipantIds: [] },
    { type: 'set-recording-targets', targetFolder: outDir, filenamePrefix: 'live-soak',
      format: 'mp4', quality: 'high', isoSourceIds: [], isoParticipantIds: [] },
    { type: 'start-recording-session', sessionId: 'live-soak' },
  ], 1000);
  const before = start.snapshot;
  evidence.virtualCameraAtStart = before?.virtualCamera ?? null;

  // ── the soak ────────────────────────────────────────────────────────────────
  const commandMs = [];
  const memorySamples = [];
  const churnAt = doChurn ? recordStartedAt + (minutes * 60000) / 2 : Infinity;
  let churned = false, tick = 0, last = before;
  const endAt = recordStartedAt + minutes * 60000;
  while (Date.now() < endAt) {
    tick++;
    const elapsed = Date.now() - recordStartedAt + 1000;
    let commands = [];
    if (tick % 10 === 0) {
      // Re-cue preview — the solo-scene path, the traffic that historically flooded.
      commands = [{ type: 'set-preview-scene', sceneId: tick % 20 === 0 ? 'solo-a' : 'solo-b',
        routes: [{ routeId: 'solo-0', slot: 0, mode: 'fixed',
          participantId: sources[tick % sources.length].participantId }] }];
    }
    let r;
    try { r = await sync(commands, elapsed); }
    catch (e) { failures.push(`operator sync timed out mid-soak: ${e.message}`); break; }
    commandMs.push(r.ms);
    last = r.snapshot;
    if (tick % 4 === 0) {
      memorySamples.push({ t: (Date.now() - recordStartedAt) / 1000,
        rssMb: counter(last, 'process', 'workingSetBytes') !== null
          ? counter(last, 'process', 'workingSetBytes') / 1048576 : null });
    }
    if (!churned && Date.now() >= churnAt) {
      churned = true;
      // CLAUDE.md: leaving a meeting once killed the entire studio. Prove a clean
      // leave and rejoin on the SAME core, mid-soak, with recording still armed.
      console.log('Churn       : leaving and rejoining on the same core…');
      try {
        await send('zoom-leave', { payload: { reason: 'soak churn' } }, 60000);
        await sleep(3000);
        const rejoined = await send('zoom-join', {
          payload: { meetingUrl, displayName: 'CoreVideo Live Soak', ...(passcode ? { passcode } : {}) },
        }, 120000);
        evidence.rejoinState = rejoined?.snapshot?.meetingState ?? null;
        await sleep(8000);
        await sync([{ type: 'set-multiview-layout', canvasWidth: 1920, canvasHeight: 1080, sources }], 500);
        console.log(`Churn       : rejoined (${JSON.stringify(evidence.rejoinState)})`);
      } catch (e) {
        failures.push(`leave/rejoin failed mid-soak: ${e.message}`);
      }
    }
    await sleep(500);
  }

  const stop = await sync([{ type: 'stop-recording-session', reason: 'soak complete' }], Date.now() - recordStartedAt + 1000);
  const recordSeconds = (Date.now() - recordStartedAt) / 1000;
  await sync([{ type: 'stop-encoder-session', reason: 'soak complete' }], Date.now() - recordStartedAt + 2000);
  await sync([{ type: 'sync-virtual-camera', on: false, mirror: false, deviceName: 'live-meeting-soak' }], 0);
  await sleep(3000);
  const after = stop.snapshot;

  // ── counters as DELTAS over the record window ───────────────────────────────
  const stages = [
    ['compositor render', ['realtimeEvidence', 'render', 'completedSlots'], 'rate'],
    ['video-out tick', ['realtimeEvidence', 'videoOutput', 'completedTicks'], 'rate'],
    ['audio worker tick', ['realtimeEvidence', 'audio', 'completedTicks'], 'rate'],
    ['encoder program-video written', ['encoderEvidence', 'programVideoWritten'], 'rate'],
    ['render skipped slots', ['realtimeEvidence', 'render', 'skippedSlots'], 'count'],
    ['render deadline misses', ['realtimeEvidence', 'render', 'deadlineMisses'], 'count'],
    ['encoder dropped video', ['encoderEvidence', 'droppedVideo'], 'count'],
    ['program-buffer underruns', ['programBuffer', 'underruns'], 'count'],
    ['program-buffer gpuNotReady', ['programBuffer', 'gpuNotReady'], 'count'],
  ];
  evidence.stageRates = {};
  for (const [label, path, kind] of stages) {
    const a = counter(before, ...path), b = counter(after, ...path);
    evidence.stageRates[label] = (a === null || b === null) ? 'unavailable'
      : b < a ? `counter reset (${a} -> ${b})`
      : kind === 'rate' ? `${((b - a) / recordSeconds).toFixed(1)}/s (${b - a} over ${recordSeconds.toFixed(1)}s)`
      : String(b - a);
  }

  const sortedCmd = [...commandMs].sort((x, y) => x - y);
  evidence.operatorCommand = {
    samples: sortedCmd.length, p50: pct(sortedCmd, 0.5), p95: pct(sortedCmd, 0.95),
    p99: pct(sortedCmd, 0.99), worst: sortedCmd.at(-1),
  };
  evidence.recordSeconds = recordSeconds;
  evidence.encoderProbeLines = stderrLines.filter((l) => l.includes('[encoder-probe]')).slice(-8);
  evidence.isoAdmissionLines = stderrLines.filter((l) => l.includes('iso-admission')).slice(-8);
  evidence.renderWindows = stderrLines.filter((l) => l.startsWith('[render] ') && l.includes('dropped=')).slice(-5);
  evidence.latencyWindows = stderrLines.filter((l) => l.includes('[zoom-latency]')).slice(-5);
  evidence.ingestWindows = stderrLines.filter((l) => l.includes('[zoom-ingest]')).slice(-5);
  evidence.slotWindows = stderrLines.filter((l) => l.includes('[zoom-slot]')).slice(-5);
  evidence.lockGuardrail = stderrLines.filter((l) => l.toLowerCase().includes('lockhold')).slice(-8);
  evidence.memorySamples = memorySamples;
  evidence.recordingLifecycle = after?.recording?.lifecycle ?? null;
  evidence.recordingProof = after?.recording?.proof ?? null;
  evidence.virtualCameraAtEnd = after?.virtualCamera ?? null;

  // ── validate the ARTIFACT, not the counters ─────────────────────────────────
  let artifact = after?.recording?.artifactPath || '';
  if (artifact && !/^([a-zA-Z]:|\/)/.test(artifact)) artifact = join(repoRoot, artifact);
  evidence.artifact = artifact;
  if (!artifact || !existsSync(artifact)) {
    failures.push(`recording produced no artifact on disk (path=${artifact || 'none'})`);
  } else {
    const probe = JSON.parse((await exec('ffprobe', ['-v', 'error', '-count_frames',
      '-show_streams', '-show_format', '-of', 'json', artifact],
      { windowsHide: true, maxBuffer: 64 * 1024 * 1024 })).stdout);
    const video = probe.streams.find((s) => s.codec_type === 'video');
    const audio = probe.streams.find((s) => s.codec_type === 'audio');
    if (!video) failures.push('recording has no video stream');
    if (!audio) failures.push('recording has no audio stream');
    const frames = Number(video?.nb_read_frames), dur = Number(video?.duration ?? probe.format?.duration);
    evidence.muxed = { countedFrames: frames, durationSeconds: dur, fps: frames / dur,
      width: video?.width, height: video?.height, audioDurationSeconds: Number(audio?.duration ?? NaN) };
    if (!(frames > 0) || !(dur > 0)) failures.push('ffprobe could not count frames in the artifact');
    else if (frames / dur < 57) failures.push(`muxed ${(frames / dur).toFixed(1)}fps of 60 counted frame-by-frame`);
    if (Number.isFinite(evidence.muxed.audioDurationSeconds)
        && Math.abs(evidence.muxed.audioDurationSeconds - dur) > 0.5)
      failures.push(`A/V duration skew ${(evidence.muxed.audioDurationSeconds - dur).toFixed(2)}s exceeds 0.5s`);

    // A FULL decode. A file that ffprobe describes can still fail to decode.
    try {
      await exec('ffmpeg', ['-v', 'error', '-xerror', '-i', artifact, '-f', 'null', '-'],
        { windowsHide: true, maxBuffer: 64 * 1024 * 1024 });
      evidence.fullDecode = 'ok';
    } catch (e) {
      evidence.fullDecode = String(e.stderr || e.message).slice(0, 2000);
      failures.push('full decode of the artifact reported errors');
    }

    // PIXELS. 8995 frames of flat luma once passed every validator that counted
    // frames, so measure the picture: mean luma must be in a sane range AND the
    // frame-to-frame luma must actually MOVE.
    const stats = await exec('ffmpeg', ['-v', 'error', '-i', artifact,
      '-vf', 'signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=-',
      '-f', 'null', '-'], { windowsHide: true, maxBuffer: 128 * 1024 * 1024 });
    const yavg = [...String(stats.stdout).matchAll(/YAVG=([\d.]+)/g)].map((m) => Number(m[1]));
    if (yavg.length === 0) failures.push('could not read per-frame luma from the artifact');
    else {
      const mean = yavg.reduce((a, b) => a + b, 0) / yavg.length;
      const min = Math.min(...yavg), max = Math.max(...yavg);
      const deltas = yavg.slice(1).map((v, i) => Math.abs(v - yavg[i]));
      const movingFrames = deltas.filter((d) => d > 0.05).length;
      evidence.pixels = { sampledFrames: yavg.length, meanLuma: mean, minLuma: min, maxLuma: max,
        movingFrameRatio: deltas.length ? movingFrames / deltas.length : 0 };
      if (mean < 8 || mean > 247) failures.push(`mean luma ${mean.toFixed(1)} — the recording is effectively blank`);
      if (evidence.pixels.movingFrameRatio < 0.2)
        failures.push(`only ${(evidence.pixels.movingFrameRatio * 100).toFixed(0)}% of frames changed luma — the picture is not moving`);
    }
  }

  evidence.failures = failures;
  evidence.status = failures.length === 0 ? 'passed' : 'failed';
} catch (error) {
  failures.push(error instanceof Error ? error.message : String(error));
  evidence.failures = failures;
  evidence.status = 'failed';
} finally {
  try { child.stdin.end(); } catch {}
  setTimeout(() => { try { child.kill(); } catch {} }, 4000).unref?.();
  evidence.finishedAt = new Date().toISOString();
  await writeFile(join(outDir, 'core-stderr.log'), stderrLines.join('\n') + '\n');
  await writeFile(join(outDir, 'live-meeting-soak-evidence.json'), JSON.stringify(evidence, null, 2) + '\n');
}

console.log('');
console.log(`Record window : ${evidence.recordSeconds?.toFixed?.(1) ?? '?'}s`);
for (const [k, v] of Object.entries(evidence.stageRates ?? {})) console.log(`  ${k}: ${v}`);
console.log(`Operator cmd  : ${JSON.stringify(evidence.operatorCommand)}`);
console.log(`Muxed         : ${JSON.stringify(evidence.muxed)}`);
console.log(`Pixels        : ${JSON.stringify(evidence.pixels)}`);
if (evidence.takeJudge) console.log(`Takes         : ${evidence.takeJudge.cuts} cut, ${evidence.takeJudge.rebuilt} rebuilt`);
console.log(`Evidence      : ${join(outDir, 'live-meeting-soak-evidence.json')}`);
console.log(evidence.status === 'passed' ? 'LIVE-MEETING SOAK PASS' : `LIVE-MEETING SOAK FAIL\n  - ${failures.join('\n  - ')}`);
process.exit(evidence.status === 'passed' ? 0 : 1);
