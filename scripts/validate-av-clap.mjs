/**
 * Headless A/V CLAP harness — content-level lip-sync measurement (G2).
 *
 * `validate-record-audio.mjs` proves a recording HAS both tracks and that their
 * container start/duration line up. It cannot see a CONTENT shift: delay every
 * video frame by 100ms and its numbers do not move, because both streams still
 * start together and run the same length. That is exactly the class of error the
 * ingest frame synchronizer can introduce (one frame of video delay against an
 * unchanged audio path), so it needs its own measurement.
 *
 * The fake zoom engine can now emit a synthetic CLAP: on a frame boundary the
 * video goes full white for exactly one frame and the audio carries a full-scale
 * burst placed sample-accurately on that same instant (COREVIDEO_FAKE_CLAP_MS).
 * Because both events are armed from ONE instant, any separation between them in
 * the recording is skew our pipeline added.
 *
 * Reports skew as VIDEO minus AUDIO:
 *   positive => video is late   => AUDIO LEADS video (the perceptually worse way)
 *   negative => video is early  => audio lags
 *
 * Usage: node ./scripts/validate-av-clap.mjs [--seconds 24] [--budget-ms 50]
 *                                            [--no-frame-sync] [--keep-artifact]
 *                                            [--build-dir path/to/binaries]
 *                                            [--live-paths --monitor-device "Game"
 *                                             --monitor-id <WASAPI endpoint id>
 *                                             --program-buffer 2]
 *                                            [--rtmp-local] (decoded local RTMP receiver)
 *                                            [--drift-gate] (requires 15 minutes + RTMP)
 *                                            [--gap-gate] (5 s routed Program audio mute)
 *                                            [--video-gap-gate] (5 s fake Zoom source dropout)
 */
import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, "..");
const exeSuffix = process.platform === "win32" ? ".exe" : "";

const args = process.argv.slice(2);
const argValue = (name, fallback) => {
  const index = args.indexOf(`--${name}`);
  return index >= 0 && args[index + 1] ? args[index + 1] : fallback;
};
const buildDir = resolve(argValue("build-dir", join(repoRoot, "native", "build-dev")));
const binarySourceRoot = resolve(buildDir, "..", "..");
const nativeCore = join(buildDir, `corevideo-native${exeSuffix}`);
const fakeEngine = join(buildDir, `corevideo-zoom-engine-fake${exeSuffix}`);
const recordSeconds = Number(argValue("seconds", 24));
// ITU-R BT.1359 / ATSC IS-191 put the audible thresholds near +45ms (audio ahead)
// and -125ms (audio behind); the repo's own G2 gate is 50ms, so use that.
const budgetMs = Number(argValue("budget-ms", 50));
const keepArtifact = args.includes("--keep-artifact");
const frameSyncOff = args.includes("--no-frame-sync");
const verbose = args.includes("--verbose");
const livePaths = args.includes("--live-paths");
const rtmpLocal = args.includes("--rtmp-local");
const driftGate = args.includes("--drift-gate");
const gapGate = args.includes("--gap-gate");
const videoGapGate = args.includes("--video-gap-gate");
if (driftGate && (!rtmpLocal || recordSeconds < 900))
  throw new Error("--drift-gate requires --rtmp-local and --seconds >= 900");
if (gapGate && (!rtmpLocal || recordSeconds < 24))
  throw new Error("--gap-gate requires --rtmp-local and --seconds >= 24");
if (videoGapGate && (!rtmpLocal || recordSeconds < 30))
  throw new Error("--video-gap-gate requires --rtmp-local and --seconds >= 30");
if (gapGate && videoGapGate) throw new Error("choose one gap gate per run");
const decodeTimeoutMs = driftGate ? 900000 : 120000;
const rtmpTapPath = process.env.COREVIDEO_QA_RTMP_TAP_PATH;
if (rtmpTapPath && !rtmpLocal) throw new Error('COREVIDEO_QA_RTMP_TAP_PATH requires --rtmp-local');
const monitorDevice = argValue("monitor-device", "Game (TC-HELICON GoXLR)");
const monitorId = argValue("monitor-id", "");
const programBufferFrames = Number(argValue("program-buffer", "2"));
const loopbackRecorder = join(buildDir, `corevideo-loopback-rec${exeSuffix}`);
const clapIntervalMs = 3000;

if (!existsSync(nativeCore) || !existsSync(fakeEngine)) {
  console.error(`Missing ${nativeCore} or ${fakeEngine}. Build them first.`);
  process.exit(1);
}
if (livePaths && (process.platform !== "win32" || !existsSync(loopbackRecorder))) {
  console.error(`--live-paths needs Windows and ${loopbackRecorder}`);
  process.exit(1);
}
if (livePaths && ![2, 3].includes(programBufferFrames)) {
  console.error("--program-buffer must be 2 or 3 frames");
  process.exit(1);
}

const ffBin = (name) => {
  for (const bin of [name, `C:\\ffmpeg\\bin\\${name}.exe`]) {
    const probe = spawnSync(bin, ["-version"], { encoding: "utf8", timeout: 10000 });
    if (!probe.error && probe.status === 0) return bin;
  }
  return null;
};
const ffmpeg = ffBin("ffmpeg");
const ffprobe = ffBin("ffprobe");
if (!ffmpeg || !ffprobe) {
  console.error("ffmpeg/ffprobe required for the clap measurement.");
  process.exit(1);
}

const env = {
  ...process.env,
  COREVIDEO_ZOOM_ENGINE_PATH: fakeEngine,
  COREVIDEO_FAKE_NO_CHURN: "1",
  COREVIDEO_FAKE_CLAP_MS: String(clapIntervalMs),
};
if (videoGapGate) {
  env.COREVIDEO_FAKE_VIDEO_GAP_START_MS = "18000";
  env.COREVIDEO_FAKE_VIDEO_GAP_DURATION_MS = "5000";
}
if (livePaths || rtmpLocal) {
  env.COREVIDEO_AV_SYNC_TRACE = "1";
  // This is a process-start setting. Both sides of the A/B run use the same
  // depth; the live operator reported active depth 2, so that is our default.
  env.COREVIDEO_PROGRAM_BUFFER_FRAMES = String(programBufferFrames);
}
if (frameSyncOff) env.COREVIDEO_FRAME_SYNC = "0";

const child = spawn(nativeCore, [], { cwd: buildDir, env, stdio: ["pipe", "pipe", "pipe"] });

const startedAt = Date.now();
let nextId = 1;
let stdoutBuffer = "";
let handshake;
let rtmpProcessStarts = null;
const pending = new Map();
const displayClaps = [];
const sourceVideoClaps = [];
const sourceAudioClaps = [];
let stderrBuffer = "";

child.stdout.on("data", (chunk) => {
  stdoutBuffer += chunk.toString();
  let idx;
  while ((idx = stdoutBuffer.indexOf("\n")) >= 0) {
    const line = stdoutBuffer.slice(0, idx).trim();
    stdoutBuffer = stdoutBuffer.slice(idx + 1);
    if (!line) continue;
    let msg;
    try { msg = JSON.parse(line); } catch { continue; }
    if (msg.type === "handshake" && msg.ok === true) handshake = msg;
    if (typeof msg.id === "string" && pending.has(msg.id)) {
      const item = pending.get(msg.id);
      pending.delete(msg.id);
      clearTimeout(item.timer);
      item.resolve(msg);
    }
  }
});
child.stderr.on("data", (c) => {
  const chunk = c.toString();
  if (verbose) process.stderr.write(chunk);
  if (!livePaths) return;
  stderrBuffer += chunk;
  let idx;
  while ((idx = stderrBuffer.indexOf("\n")) >= 0) {
    const line = stderrBuffer.slice(0, idx);
    stderrBuffer = stderrBuffer.slice(idx + 1);
    const match = line.match(/\[av-sync\] program-publish qpc100ns=(\d+) frame=(\d+)/);
    if (match) displayClaps.push({ qpc100ns: Number(match[1]), frame: Number(match[2]) });
    const sourceVideo = line.match(/\[av-sync\] source-video qpc100ns=(\d+) frame=(\d+)/);
    if (sourceVideo) sourceVideoClaps.push({ qpc100ns: Number(sourceVideo[1]), frame: Number(sourceVideo[2]) });
    const sourceAudio = line.match(/\[av-sync\] source-audio qpc100ns=(\d+)/);
    if (sourceAudio) sourceAudioClaps.push({ qpc100ns: Number(sourceAudio[1]) });
  }
  if (stderrBuffer.length > 8192) stderrBuffer = stderrBuffer.slice(-8192);
});
child.once("exit", (code) => {
  for (const { reject, timer } of pending.values()) {
    clearTimeout(timer);
    reject(new Error(`native core exited ${code}`));
  }
  pending.clear();
});

function send(type, payload = {}) {
  const id = `clap-${nextId++}`;
  return new Promise((resolvePromise, reject) => {
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`${type} timed out`)); }, 30000);
    pending.set(id, { resolve: resolvePromise, reject, timer });
    child.stdin.write(`${JSON.stringify({ id, type, ...payload })}\n`);
  }).then((response) => {
    if (response.ok === false) throw new Error(`${type} failed: ${response.error?.message ?? "unknown"}`);
    return response;
  });
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** Per-frame mean luma, downscaled to 8x8 so this is cheap and robust. */
function videoFlashTimes(artifact, fps) {
  const out = spawnSync(ffmpeg,
    ["-v", "error", "-i", artifact, "-vf", "scale=8:8", "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", "gray", "-"],
    { encoding: "buffer", maxBuffer: 1 << 28, timeout: decodeTimeoutMs });
  if (out.status !== 0 || !out.stdout?.length) return [];
  const px = 64;
  const frames = Math.floor(out.stdout.length / px);
  const ptsProbe = spawnSync(ffprobe,
    ["-v", "error", "-select_streams", "v:0", "-show_frames",
      "-show_entries", "frame=best_effort_timestamp_time", "-of", "csv=p=0", artifact],
    { encoding: "utf8", maxBuffer: 1 << 24, timeout: decodeTimeoutMs });
  const framePts = ptsProbe.status === 0
    ? ptsProbe.stdout.trim().split(/\r?\n/).map(Number) : [];
  if (framePts.length !== frames || !framePts.every(Number.isFinite)) {
    throw new Error(`video frame timestamps missing: decoded=${frames}, timestamps=${framePts.length}`);
  }
  const means = new Array(frames);
  for (let f = 0; f < frames; f += 1) {
    let sum = 0;
    for (let i = 0; i < px; i += 1) sum += out.stdout[f * px + i];
    means[f] = sum / px;
  }
  // The flash is a full-white frame against animated content, so it is a clear
  // outlier. Threshold off the MEDIAN (not the mean) so the flashes themselves
  // cannot drag the baseline up.
  const sorted = [...means].sort((a, b) => a - b);
  const median = sorted[Math.floor(sorted.length / 2)];
  const threshold = Math.max(median + 25, 200);
  const times = [];
  let prevHot = false;
  for (let f = 0; f < frames; f += 1) {
    const hot = means[f] >= threshold;
    if (hot && !prevHot) times.push(framePts[f]);
    prevHot = hot;
  }
  return times;
}

/** Burst onsets in the decoded audio, in seconds. */
function audioBurstTimes(artifact) {
  const out = spawnSync(ffmpeg,
    ["-v", "error", "-i", artifact, "-f", "s16le", "-ac", "1", "-ar", "48000", "-"],
    { encoding: "buffer", maxBuffer: 1 << 28, timeout: decodeTimeoutMs });
  if (out.status !== 0 || !out.stdout?.length) return [];
  const samples = out.stdout.length >> 1;
  // The clap is the loudest thing in the file, but NOT at full scale by the time
  // it lands: the master bus limiter pulls a full-scale burst down (measured
  // 18449 of 32767). So key off the file's OWN peak instead of an absolute
  // number, which also keeps this honest if the master gain ever changes.
  let peak = 0;
  for (let s2 = 0; s2 < samples; s2 += 1) {
    const v = Math.abs(out.stdout.readInt16LE(s2 * 2));
    if (v > peak) peak = v;
  }
  const threshold = peak * 0.6;
  if (peak < 4000) return [];  // nothing loud enough to be a clap
  const times = [];
  let lastHit = -Infinity;
  for (let s = 0; s < samples; s += 1) {
    const v = out.stdout.readInt16LE(s * 2);
    if (Math.abs(v) < threshold) continue;
    const t = s / 48000;
    if (t - lastHit > 0.5) times.push(t);   // one onset per burst
    lastHit = t;
  }
  return times;
}

/** Pair each video flash with its nearest audio burst; skew = video - audio. */
function pairEvents(videoTimes, audioTimes) {
  return pairTimedEvents(videoTimes, audioTimes)
    .filter((pair) => pair.skewMs !== null).map((pair) => pair.skewMs);
}

function pairTimedEvents(videoTimes, audioTimes) {
  const pairs = [];
  for (const v of videoTimes) {
    let best = null;
    for (const a of audioTimes) {
      const d = v - a;
      if (best === null || Math.abs(d) < Math.abs(best)) best = d;
    }
    // Anything beyond half a clap interval is a mis-pair, not a measurement.
    pairs.push({ videoTime: v,
      skewMs: best !== null && Math.abs(best) < clapIntervalMs / 2000 ? best * 1000 : null });
  }
  return pairs;
}

function assessGapSync(label, videoTimes, audioTimes) {
  const cues = pairTimedEvents(videoTimes, audioTimes);
  const gapIndex = cues.findIndex((cue, index) => index >= 2 && index < cues.length - 2 && cue.skewMs === null);
  if (cues.length < 6 || gapIndex < 0) {
    failures.push(`${label}: five-second mute did not remove a middle audio cue while video continued`);
    return null;
  }
  // RTMP can miss its first clap while FFmpeg connects. Compare two actual
  // pairs immediately around the mute, not the first two video flashes.
  const before = cues.slice(0, gapIndex).filter((cue) => cue.skewMs !== null)
    .slice(-2).map((cue) => cue.skewMs);
  const after = cues.slice(gapIndex + 1).filter((cue) => cue.skewMs !== null)
    .slice(0, 2).map((cue) => cue.skewMs);
  if (before.length < 2 || after.length < 2) {
    failures.push(`${label}: missing A/V cue before or after audio resumed`);
    return null;
  }
  const beforeMs = median(before);
  const afterMs = median(after);
  const changeMs = afterMs - beforeMs;
  console.log(`${label.padEnd(14)}: before ${beforeMs.toFixed(1)} ms, after ${afterMs.toFixed(1)} ms, change ${changeMs.toFixed(1)} ms`);
  if (Math.abs(changeMs) > 1000 / 60)
    failures.push(`${label}: sync changed ${changeMs.toFixed(1)} ms across audio gap (>1 frame)`);
  return { beforeMs, afterMs, changeMs, unpairedMiddleCues: cues.slice(2, -2).filter((cue) => cue.skewMs === null).length };
}

function assessVideoGapSync(label, videoTimes, audioTimes) {
  const gapIndex = videoTimes.findIndex((time, index) =>
    index >= 2 && time - videoTimes[index - 1] > clapIntervalMs / 1000 * 1.5);
  const audioContinued = gapIndex >= 0 && audioTimes.some((time) =>
    time > videoTimes[gapIndex - 1] + clapIntervalMs / 2000 &&
    time < videoTimes[gapIndex] - clapIntervalMs / 2000);
  if (gapIndex < 0 || !audioContinued) {
    failures.push(`${label}: source-video dropout did not remove flashes while audio kept arriving`);
    return null;
  }
  const cues = pairTimedEvents(videoTimes, audioTimes);
  const before = cues.slice(0, gapIndex).filter((cue) => cue.skewMs !== null)
    .slice(-2).map((cue) => cue.skewMs);
  const after = cues.slice(gapIndex).filter((cue) => cue.skewMs !== null)
    .slice(0, 2).map((cue) => cue.skewMs);
  if (before.length < 2 || after.length < 2) {
    failures.push(`${label}: missing paired A/V cues around video recovery`);
    return null;
  }
  const beforeMs = median(before);
  const afterMs = median(after);
  const changeMs = afterMs - beforeMs;
  const sourceGapMs = (videoTimes[gapIndex] - videoTimes[gapIndex - 1]) * 1000;
  console.log(`${label.padEnd(14)}: flash gap ${sourceGapMs.toFixed(0)} ms, before ${beforeMs.toFixed(1)} ms, after ${afterMs.toFixed(1)} ms`);
  if (Math.abs(changeMs) > 1000 / 60)
    failures.push(`${label}: sync changed ${changeMs.toFixed(1)} ms after video source recovery (>1 frame)`);
  return { sourceGapMs, beforeMs, afterMs, changeMs };
}

function decodedVideoPts(path) {
  const probe = spawnSync(ffprobe, ["-v", "error", "-select_streams", "v:0",
    "-show_entries", "frame=best_effort_timestamp_time", "-of", "csv=p=0", path],
  { encoding: "utf8", timeout: decodeTimeoutMs, maxBuffer: 1 << 22 });
  if (probe.status !== 0) throw new Error(`video PTS probe failed: ${probe.stderr}`);
  return probe.stdout.trim().split(/\r?\n/).map(Number).filter(Number.isFinite);
}

/** Map each endpoint loopback burst to the packet's WASAPI QPC clock. */
function loopbackBurstTimes(rawPath, packetPath, sampleRate, channels) {
  const pcm = readFileSync(rawPath);
  const packetRows = readFileSync(packetPath, "utf8").trim().split(/\r?\n/).slice(1)
    .map((line) => line.split(",").map(Number))
    .filter((row) => row.length === 4 && row.every(Number.isFinite));
  if (!packetRows.length) return [];
  const frameBytes = channels * 4;
  const frameCount = Math.floor(pcm.length / frameBytes);
  let peak = 0;
  for (let frame = 0; frame < frameCount; frame += 1) {
    for (let channel = 0; channel < channels; channel += 1) {
      peak = Math.max(peak, Math.abs(pcm.readFloatLE(frame * frameBytes + channel * 4)));
    }
  }
  if (peak < 0.08) return [];
  const threshold = peak * 0.6;
  const times = [];
  let lastHit = -Infinity;
  let packet = 0;
  for (let frame = 0; frame < frameCount; frame += 1) {
    let loud = false;
    for (let channel = 0; channel < channels; channel += 1) {
      if (Math.abs(pcm.readFloatLE(frame * frameBytes + channel * 4)) >= threshold) loud = true;
    }
    if (!loud || frame - lastHit <= sampleRate * 0.5) continue;
    while (packet + 1 < packetRows.length && packetRows[packet + 1][0] <= frame) packet += 1;
    const [start, count, qpc100ns] = packetRows[packet];
    if (qpc100ns > 0 && frame < start + count) {
      times.push((qpc100ns + (frame - start) * 1e7 / sampleRate) / 1e7);
      lastHit = frame;
    }
  }
  return times;
}

function describePairs(label, pairs) {
  if (pairs.length < 2) throw new Error(`${label}: only ${pairs.length} paired claps; all live paths are required`);
  const sorted = [...pairs].sort((a, b) => a - b);
  const median = sorted[Math.floor(sorted.length / 2)];
  const spread = sorted.at(-1) - sorted[0];
  console.log(`${label.padEnd(14)}: median ${median.toFixed(1)} ms (${(median / (1000 / 60)).toFixed(2)} frames), spread ${spread.toFixed(1)} ms; video-audio ${median < 0 ? "audio late" : "audio early"}`);
  return { pairsMs: pairs, medianMs: median, spreadMs: spread, framesAt60: median / (1000 / 60) };
}

function median(values) {
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}

function assessLongDrift(recordPairs, streamPairs, seconds) {
  const expected = Math.floor(seconds * 1000 / clapIntervalMs) - 1;
  const minimum = Math.ceil(expected * 0.9);
  if (recordPairs.length < minimum || streamPairs.length < minimum) {
    throw new Error(`long drift evidence missing cues: record=${recordPairs.length}, RTMP=${streamPairs.length}, expected >=${minimum}`);
  }
  const window = 10;
  const earlyRecordMs = median(recordPairs.slice(0, window));
  const lateRecordMs = median(recordPairs.slice(-window));
  const earlyStreamMs = median(streamPairs.slice(0, window));
  const lateStreamMs = median(streamPairs.slice(-window));
  const earlyRelativeMs = earlyStreamMs - earlyRecordMs;
  const lateRelativeMs = lateStreamMs - lateRecordMs;
  const driftMs = lateRelativeMs - earlyRelativeMs;
  return { expected, recordCues: recordPairs.length, streamCues: streamPairs.length,
    window, earlyRecordMs, lateRecordMs, earlyStreamMs, lateStreamMs,
    earlyRelativeMs, lateRelativeMs, driftMs };
}

const failures = [];
let artifactAbsolute = null;
let loopback = null;
let loopbackDone = null;
let loopbackStderr = "";
let rtmpReceiver = null;
let rtmpReceiverDone = null;
let rtmpReceiverStderr = "";
const liveCaptureDir = join(buildDir, "Recordings", "CoreVideoPro", "validate-av-clap", `live-paths-${Date.now()}`);
const rtmpReceived = join(liveCaptureDir, "received-rtmp.flv");
const rtmpUrl = "rtmp://127.0.0.1:19357/live/clap";
const loopbackRaw = join(liveCaptureDir, "monitor.f32");
const loopbackPackets = join(liveCaptureDir, "monitor-packets.csv");
let startCounters = null;
let endCounters = null;
let recordSummary = null;
let receivedSummary = null;
let monitorSummary = null;
let monitorTimes = null;
let monitorUnderruns = null;
let lostSamples = null;
let sourceSummary = null;
let videoToPublish = null;
let audioToMonitor = null;
try {
  for (let i = 0; i < 200 && !handshake; i += 1) await sleep(50);
  if (!handshake) throw new Error("no native-core handshake");
  console.log(`Frame sync    : ${frameSyncOff ? "OFF (control)" : "ON (default)"}`);
  if (livePaths || rtmpLocal) console.log(`Program depth : ${programBufferFrames} frames (set before core launch)`);

  await send("zoom-join", { payload: { meetingNumber: "1234567890", displayName: "av-clap" } });
  await sleep(3000);

  // The fake only auto-subscribes VIDEO. Audio now follows the same explicit
  // spine contract as the real engine; without this the old harness recorded
  // flashes against silence on both the released build and the candidate.
  await send("zoom-media-spine-sync", {
    elapsedMs: Date.now() - startedAt,
    spinePayload: {
      readiness: { status: "ready", platform: "windows", sdkVersion: "fake-engine", checks: [], blockers: [], warnings: [] },
      participants: [{ sdkUserId: "101", displayName: "clap", role: "guest", videoOn: true, muted: false, talking: true, audioLevel: 60 }],
      subscriptions: [
        { participantId: "101", kind: "meeting-audio", purpose: "program", priority: 0 },
        { participantId: "101", kind: "participant-video", purpose: "program", priority: 10 },
      ],
      startCapture: true, blocked: false, warnings: [], summary: "A/V clap subscription",
    },
  });
  await sleep(2000);

  if (rtmpLocal) {
    mkdirSync(liveCaptureDir, { recursive: true });
    rtmpReceiver = spawn(ffmpeg,
      ["-hide_banner", "-loglevel", "warning", "-y", "-listen", "1", "-i", rtmpUrl,
        "-map", "0:v:0", "-map", "0:a:0", "-c", "copy", rtmpReceived],
      { cwd: buildDir, stdio: ["ignore", "ignore", "pipe"] });
    rtmpReceiver.stderr.on("data", (chunk) => { rtmpReceiverStderr += chunk.toString(); });
    rtmpReceiverDone = new Promise((resolvePromise, reject) => {
      rtmpReceiver.once("error", reject);
      rtmpReceiver.once("exit", (code) => code === 0 ? resolvePromise() : reject(new Error(`RTMP receiver exited ${code}: ${rtmpReceiverStderr}`)));
    });
    rtmpReceiverDone.catch(() => {});
    await sleep(500);
    if (rtmpReceiver.exitCode !== null) throw new Error(`RTMP receiver exited before stream start: ${rtmpReceiverStderr}`);
  }

  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [
      {
        type: "load-scene-graph",
        sceneId: "av-clap",
        // ONE participant, full frame: the flash must fill the program raster so
        // it is unmistakable, and its audio must be the mix we are measuring.
        // NO explicit rect — a single route with no rect lays out as gridCell(1,0),
        // i.e. the whole canvas. (An explicit {0,0,1,1} rect rendered the source
        // as a small corner tile instead; not chased here, but worth knowing.)
        routes: [{
          routeId: "program", mode: "fixed", audioRole: "mix", participantId: "101",
          fitMode: "fill", zIndex: 0,
        }],
      },
      {
        type: "sync-audio-routing-matrix",
        sends: [
          { sourceId: "zoom-mix", busId: "master", gainDb: 0 },
          { sourceId: "zoom-mix", busId: "stream", gainDb: 0 },
          ...(livePaths ? [{ sourceId: "zoom-mix", busId: "mon", gainDb: 0 }] : []),
        ],
      },
      ...(livePaths ? [{
        type: "sync-audio-monitor", enabled: true,
        deviceId: monitorId, deviceName: monitorDevice, volume: 1.0,
      }] : []),
      { type: "prepare-encoder-session", preparedAtMs: Date.now() - startedAt, reason: "av-clap warmup" },
      {
        type: "start-program-output", destinations: rtmpLocal ? ["recording", "rtmp"] : ["recording"],
        isoParticipantIds: [],
        ...(rtmpLocal ? { destinationSettings: [{
          id: "rtmp", label: "av-clap-local", protocol: "rtmp", url: "rtmp://127.0.0.1:19357/live",
          streamKey: "clap", fps: 60, targetBitrateMbps: 6,
        }] } : {}),
      },
      {
        type: "set-recording-targets",
        targetFolder: "Recordings/CoreVideoPro/validate-av-clap",
        filenamePrefix: "av-clap", format: "mp4", quality: "high", isoParticipantIds: [],
      },
    ],
  });
  await sleep(2000);

  if (livePaths) {
    mkdirSync(liveCaptureDir, { recursive: true });
    loopback = spawn(loopbackRecorder,
      [monitorDevice, String(recordSeconds + 5), loopbackRaw, loopbackPackets],
      { cwd: buildDir, stdio: ["ignore", "ignore", "pipe"] });
    loopback.stderr.on("data", (chunk) => { loopbackStderr += chunk.toString(); });
    loopbackDone = new Promise((resolvePromise, reject) => {
      loopback.once("error", reject);
      loopback.once("exit", (code) => code === 0 ? resolvePromise() : reject(new Error(`loopback recorder exited ${code}: ${loopbackStderr}`)));
    });
    loopbackDone.catch(() => {}); // handled after the recording has finalized
    await sleep(500);
  }

  const startResp = await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{
      type: "start-recording-session", sessionId: "av-clap", startedAtMs: Date.now(),
      targetFolder: "Recordings/CoreVideoPro/validate-av-clap",
      filenamePrefix: "av-clap", format: "mp4", quality: "high", isoParticipantIds: [],
    }],
  });
  startCounters = startResp.snapshot;
  console.log(`Recording     : ${recordSeconds}s (clap every ${clapIntervalMs}ms)...`);

  let last = null;
  const recordingStartedAt = Date.now();
  const deadline = recordingStartedAt + recordSeconds * 1000;
  let muted = false;
  let unmuted = false;
  let audioMutedAt = null;
  let audioResumedAt = null;
  const routeControl = startResp.snapshot?.audioRoutingMatrix?.control;
  let routeRevision = routeControl?.revision;
  const routeEpoch = routeControl?.authorityEpoch;
  if (gapGate && (!routeEpoch || !Number.isInteger(routeRevision)))
    throw new Error("audio gap gate requires revisioned route control evidence");
  const setProgramAudioEnabled = async (enabled) => {
    for (const busId of ["master", "stream"]) {
      const response = await send("media-core-sync", {
        elapsedMs: Date.now() - startedAt,
        commands: [{ type: "set-audio-route-control",
          operationId: `gap-${enabled ? "resume" : "mute"}-${busId}`,
          authorityEpoch: routeEpoch, expectedRevision: routeRevision,
          sourceId: "zoom-mix", busId, enabled, gainDb: 0 }],
      });
      const control = response.snapshot?.audioRoutingMatrix?.control;
      if (control?.lastResult?.status !== "applied" || control.revision !== routeRevision + 1)
        throw new Error(`audio gap ${enabled ? "resume" : "mute"} ${busId} was not applied`);
      routeRevision = control.revision;
    }
  };
  while (Date.now() < deadline) {
    await sleep(Math.min(gapGate ? 500 : 5000, Math.max(100, deadline - Date.now())));
    if (gapGate && !muted && Date.now() - recordingStartedAt >= 8000) {
      await setProgramAudioEnabled(false);
      audioMutedAt = Date.now();
      muted = true;
    }
    if (gapGate && muted && !unmuted && Date.now() - recordingStartedAt >= 13000) {
      await setProgramAudioEnabled(true);
      audioResumedAt = Date.now();
      unmuted = true;
    }
    const syncResp = await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [] });
    last = syncResp.snapshot?.recording ?? {};
    endCounters = syncResp.snapshot;
  }

  const stopResp = await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{ type: "stop-recording-session", reason: "av-clap complete" }],
  });
  const audioGapDurationMs = gapGate ? audioResumedAt - audioMutedAt : null;
  if (gapGate && (!unmuted || audioGapDurationMs < 4500 || audioGapDurationMs > 6000))
    failures.push(`audio route was muted for ${audioGapDurationMs} ms, expected about 5000 ms`);
  if (gapGate || videoGapGate) {
    const proofPath = stopResp.snapshot?.outputSenderSession?.senders
      ?.find((sender) => sender.destination === "rtmp")?.sendArtifactPath;
    if (!proofPath || !existsSync(proofPath)) {
      failures.push("audio gap has no RTMP process proof artifact");
    } else {
      rtmpProcessStarts = readFileSync(proofPath, "utf8").split(/\r?\n/)
        .filter((line) => line.includes('"type":"ffmpeg-process-start"')).length;
    }
  }
  if (rtmpLocal) {
    await send("media-core-sync", {
      elapsedMs: Date.now() - startedAt,
      commands: [{ type: "start-program-output", destinations: ["recording"], isoParticipantIds: [] }],
    });
    await Promise.race([rtmpReceiverDone, sleep(15000).then(() => { throw new Error(`RTMP receiver did not finalize: ${rtmpReceiverStderr}`); })]);
    if (!existsSync(rtmpReceived) || statSync(rtmpReceived).size < 1024) {
      throw new Error(`RTMP receiver captured no stream: ${rtmpReceiverStderr}`);
    }
  }
  endCounters = stopResp.snapshot ?? endCounters;
  const artifact = stopResp.snapshot?.recording?.artifactPath ?? last?.artifactPath ?? null;
  if (!artifact) throw new Error("no recording artifact");
  artifactAbsolute = resolve(buildDir, artifact);
  // The muxer finalizes (moov atom) asynchronously — probing before the size
  // settles reads a file with no readable streams yet.
  let lastSize = -1;
  for (let i = 0; i < 40; i += 1) {
    await sleep(250);
    let size = 0;
    try { size = statSync(artifactAbsolute).size; } catch { continue; }
    if (size > 1024 && size === lastSize) break;
    lastSize = size;
  }
  if (!existsSync(artifactAbsolute)) throw new Error(`artifact missing: ${artifactAbsolute}`);

  const probe = spawnSync(ffprobe,
    ["-v", "error", "-print_format", "json", "-show_streams", artifactAbsolute],
    { encoding: "utf8", timeout: 20000 });
  const streams = JSON.parse(probe.stdout).streams ?? [];
  const vStream = streams.find((s) => s.codec_type === "video");
  if (!vStream) throw new Error("recording has no video stream");
  const [num, den] = String(vStream.avg_frame_rate ?? "60/1").split("/").map(Number);
  const fps = den ? num / den : 60;

  const videoTimes = videoFlashTimes(artifactAbsolute, fps);
  const audioTimes = audioBurstTimes(artifactAbsolute);
  console.log(`Events        : ${videoTimes.length} video flashes, ${audioTimes.length} audio bursts @ ${fps.toFixed(2)}fps`);

  const pairs = pairEvents(videoTimes, audioTimes);
  if (pairs.length >= 2) recordSummary = describePairs("Record v-a", pairs);
  const recordGap = gapGate ? assessGapSync("Record gap", videoTimes, audioTimes) : null;
  const recordVideoGap = videoGapGate ? assessVideoGapSync("Record video", videoTimes, audioTimes) : null;
  if (rtmpLocal) {
    const receivedProbe = spawnSync(ffprobe,
      ["-v", "error", "-print_format", "json", "-show_streams", rtmpReceived],
      { encoding: "utf8", timeout: 20000 });
    if (receivedProbe.status !== 0) throw new Error(`decoded RTMP probe failed: ${receivedProbe.stderr}`);
    const receivedStreams = JSON.parse(receivedProbe.stdout).streams ?? [];
    const receivedVideo = receivedStreams.find((stream) => stream.codec_type === "video");
    const receivedAudio = receivedStreams.find((stream) => stream.codec_type === "audio");
    if (!receivedVideo || !receivedAudio) throw new Error("decoded RTMP receiver is missing video or audio");
    const [receivedNum, receivedDen] = String(receivedVideo.avg_frame_rate ?? "60/1").split("/").map(Number);
    const receivedFps = receivedDen ? receivedNum / receivedDen : 60;
    const receivedVideoTimes = videoFlashTimes(rtmpReceived, receivedFps);
    const receivedAudioTimes = audioBurstTimes(rtmpReceived);
    const receivedPairs = pairEvents(receivedVideoTimes, receivedAudioTimes);
    if (receivedPairs.length < 2) throw new Error(`decoded RTMP has only ${receivedPairs.length} paired claps (video=${receivedVideoTimes.length}, audio=${receivedAudioTimes.length})`);
    receivedSummary = describePairs("RTMP v-a", receivedPairs);
    const streamGap = gapGate ? assessGapSync("RTMP gap", receivedVideoTimes, receivedAudioTimes) : null;
    const streamVideoGap = videoGapGate
      ? assessVideoGapSync("RTMP video", receivedVideoTimes, receivedAudioTimes) : null;
    if ((gapGate || videoGapGate) && rtmpProcessStarts !== 1)
      failures.push(`gap restarted RTMP FFmpeg ${rtmpProcessStarts} times (expected one launch)`);
    if (gapGate && recordGap && streamGap &&
        Math.abs(streamGap.changeMs - recordGap.changeMs) > 1000 / 60)
      failures.push("RTMP gained more than one frame of drift relative to recording across audio gap");
    if (videoGapGate && recordVideoGap && streamVideoGap &&
        Math.abs(streamVideoGap.changeMs - recordVideoGap.changeMs) > 1000 / 60)
      failures.push("RTMP gained more than one frame of drift relative to recording after video recovery");
    let receivedVideoPts = null;
    let worstVideoPtsGapMs = null;
    if (videoGapGate) {
      receivedVideoPts = decodedVideoPts(rtmpReceived);
      if (receivedVideoPts.length < recordSeconds * 50)
        failures.push(`RTMP carried only ${receivedVideoPts.length} video frames through source dropout`);
      worstVideoPtsGapMs = 0;
      for (let index = 1; index < receivedVideoPts.length; index += 1)
        worstVideoPtsGapMs = Math.max(worstVideoPtsGapMs,
          (receivedVideoPts[index] - receivedVideoPts[index - 1]) * 1000);
      if (worstVideoPtsGapMs > 1000 / 30)
        failures.push(`RTMP video PTS has a ${worstVideoPtsGapMs.toFixed(1)} ms gap during source dropout`);
    }
    writeFileSync(join(liveCaptureDir, "rtmp-evidence.json"), JSON.stringify({
      source: "fake-engine timed clap; local decoded RTMP receiver and recorded Program from one run",
      buildSha: spawnSync("git", ["rev-parse", "HEAD"], { cwd: binarySourceRoot, encoding: "utf8" }).stdout.trim(),
      buildDir, seconds: recordSeconds, programBufferFrames,
      receivedArtifact: rtmpReceived, recordingArtifact: artifactAbsolute,
      receivedVideoTimes, receivedAudioTimes, receivedSummary, streamGap, streamVideoGap,
      receivedVideoFrameCount: receivedVideoPts?.length ?? null, worstVideoPtsGapMs,
      recordVideoTimes: videoTimes, recordAudioTimes: audioTimes, recordSummary, recordGap, recordVideoGap,
      rtmpProcessStarts, audioGapDurationMs,
      receiverLog: rtmpReceiverStderr,
    }, null, 2));
    if (driftGate) {
      if (!recordSummary) throw new Error("long drift gate is missing paired Program claps");
      const drift = assessLongDrift(recordSummary.pairsMs, receivedSummary.pairsMs, recordSeconds);
      console.log(`Long drift    : first RTMP−record ${drift.earlyRelativeMs.toFixed(1)} ms, ` +
                  `last ${drift.lateRelativeMs.toFixed(1)} ms, change ${drift.driftMs.toFixed(1)} ms ` +
                  `(${drift.recordCues}/${drift.streamCues} paired cues)`);
      writeFileSync(join(liveCaptureDir, "long-drift-evidence.json"), JSON.stringify({
        source: "same-run recorded Program and decoded local RTMP timed claps",
        buildSha: spawnSync("git", ["rev-parse", "HEAD"], { cwd: binarySourceRoot, encoding: "utf8" }).stdout.trim(),
        recordingArtifact: artifactAbsolute, receivedArtifact: rtmpReceived, drift,
      }, null, 2));
      if (Math.abs(drift.driftMs) > 1000 / 60)
        failures.push(`RTMP−record drift ${drift.driftMs.toFixed(1)}ms exceeds one 60fps frame`);
      if (Math.abs(drift.earlyRelativeMs) > budgetMs || Math.abs(drift.lateRelativeMs) > budgetMs)
        failures.push("RTMP−record skew exceeded the configured budget in an early or late window");
    }
    if (Math.abs(receivedSummary.medianMs) > budgetMs) {
      failures.push(`decoded RTMP skew ${receivedSummary.medianMs.toFixed(1)}ms exceeds the ${budgetMs}ms budget`);
    }
    if (rtmpTapPath) {
      if (!existsSync(rtmpTapPath) || statSync(rtmpTapPath).size < 1024) {
        failures.push('same-run muxed RTMP tap is missing');
      } else {
        const tapProbe = spawnSync(ffprobe,
          ["-v", "error", "-print_format", "json", "-show_streams", rtmpTapPath],
          { encoding: "utf8", timeout: 20000 });
        if (tapProbe.status !== 0) throw new Error(`muxed tap probe failed: ${tapProbe.stderr}`);
        const tapStreams = JSON.parse(tapProbe.stdout).streams ?? [];
        const tapVideo = tapStreams.find(stream => stream.codec_type === 'video');
        const tapAudio = tapStreams.find(stream => stream.codec_type === 'audio');
        if (!tapVideo || !tapAudio) throw new Error('muxed tap is missing video or audio');
        const [tapNum, tapDen] = String(tapVideo.avg_frame_rate ?? '60/1').split('/').map(Number);
        const tapFps = tapDen ? tapNum / tapDen : 60;
        const tapVideoTimes = videoFlashTimes(rtmpTapPath, tapFps);
        const tapAudioTimes = audioBurstTimes(rtmpTapPath);
        const tapPairs = pairEvents(tapVideoTimes, tapAudioTimes);
        if (tapPairs.length < 2) throw new Error(`muxed tap has only ${tapPairs.length} paired claps`);
        const tapSummary = describePairs('Muxed v-a', tapPairs);
        writeFileSync(join(liveCaptureDir, 'rtmp-tap-evidence.json'), JSON.stringify({
          muxedArtifact: rtmpTapPath, receivedArtifact: rtmpReceived,
          tapVideoTimes, tapAudioTimes, tapSummary, receivedSummary,
          receiverMinusMuxerMs: receivedSummary.medianMs - tapSummary.medianMs,
        }, null, 2));
        if (Math.abs(receivedSummary.medianMs - tapSummary.medianMs) > 1000 / 60)
          failures.push('local RTMP receiver differs from the same-run muxed tap by over one frame');
      }
    }
  }
  if (audioTimes.length >= 2 && videoTimes.length === 0) {
    // KNOWN LIMITATION of the headless rig, not a bug in the clap. With no shell
    // attached there is no GPU readback, so `lastProgramFrame_.preview.bgra` is
    // empty and MediaCore fills it with fillSyntheticProgramFramePreview() — the
    // encoder therefore muxes a SYNTHETIC preview raster, not the real composited
    // program, and a one-frame flash in the source video never reaches the file.
    // Worth knowing more broadly: it means validate-record-audio.mjs's A/V numbers
    // are also measured on the preview path, not the real program path.
    failures.push(
      "audio bursts present but NO video flashes: headless recordings mux the " +
      "synthetic program preview (fillSyntheticProgramFramePreview), not the real " +
      "composited program — run this against the full app to measure content A/V skew");
  } else if (pairs.length < 2) {
    failures.push(`only ${pairs.length} clap(s) paired — cannot measure (video=${videoTimes.length} audio=${audioTimes.length})`);
  } else {
    pairs.sort((a, b) => a - b);
    const mean = pairs.reduce((a, b) => a + b, 0) / pairs.length;
    const median = pairs[Math.floor(pairs.length / 2)];
    const spread = pairs[pairs.length - 1] - pairs[0];
    console.log(`Skew (v-a)    : ${pairs.map((p) => p.toFixed(1)).join(", ")} ms`);
    console.log(`              : median ${median.toFixed(1)}ms, mean ${mean.toFixed(1)}ms, spread ${spread.toFixed(1)}ms`);
    console.log(median > 0
      ? `              : AUDIO LEADS video by ${Math.abs(median).toFixed(1)}ms`
      : `              : audio lags video by ${Math.abs(median).toFixed(1)}ms`);
    if (Math.abs(median) > budgetMs) {
      failures.push(`A/V skew ${median.toFixed(1)}ms exceeds the ${budgetMs}ms budget`);
    }
  }

  if (livePaths) {
    await loopbackDone;
    const format = loopbackStderr.match(/format: (\d+)Hz (\d+)ch 32-bit/);
    if (!format) throw new Error(`loopback format missing: ${loopbackStderr}`);
    monitorTimes = loopbackBurstTimes(loopbackRaw, loopbackPackets, Number(format[1]), Number(format[2]));
    const displayTimes = displayClaps.map((clap) => clap.qpc100ns / 1e7);
    monitorSummary = describePairs("Display-MON", pairEvents(displayTimes, monitorTimes));
    const sourceVideoTimes = sourceVideoClaps.map((clap) => clap.qpc100ns / 1e7);
    const sourceAudioTimes = sourceAudioClaps.map((clap) => clap.qpc100ns / 1e7);
    sourceSummary = describePairs("Source v-a", pairEvents(sourceVideoTimes, sourceAudioTimes));
    videoToPublish = describePairs("Ingress→DXGI", pairEvents(displayTimes, sourceVideoTimes));
    audioToMonitor = describePairs("Ingress→MON", pairEvents(monitorTimes, sourceAudioTimes));
    if (!recordSummary) throw new Error("recording clap result missing while live paths were requested");
    if (endCounters?.audioMixSession?.monitorStatus !== "playing") {
      throw new Error(`monitor did not stay playing: ${endCounters?.audioMixSession?.monitorStatus ?? "missing"}`);
    }
    const monitorBefore = startCounters?.audioMixSession?.monitorUnderruns;
    const monitorAfter = endCounters?.audioMixSession?.monitorUnderruns;
    const lostBefore = startCounters?.realtimeEvidence?.audio?.audioLostSamples;
    const lostAfter = endCounters?.realtimeEvidence?.audio?.audioLostSamples;
    if (![monitorBefore, monitorAfter, lostBefore, lostAfter].every(Number.isFinite)) {
      throw new Error("monitor underrun or lost-sample counters missing from the same run");
    }
    monitorUnderruns = monitorAfter - monitorBefore;
    lostSamples = lostAfter - lostBefore;
    console.log(`Continuity    : monitor underruns +${monitorUnderruns}, audio lost samples +${lostSamples}`);
    if (monitorUnderruns !== 0 || lostSamples !== 0) {
      failures.push(`continuity failed: monitor underruns +${monitorUnderruns}, audio lost samples +${lostSamples}`);
    }
    const evidencePath = join(liveCaptureDir, "evidence.json");
    writeFileSync(evidencePath, JSON.stringify({
      source: "fake-engine timed clap; Program texture publish, endpoint WASAPI loopback, and recorded Program from one run",
      buildDir, monitorDevice, monitorId, programBufferFrames, seconds: recordSeconds,
      displayMarker: "delivered Program source frame at DXGI shared-texture publish; actual monitor vsync is not measured",
      sourceVideoClaps, sourceAudioClaps, sourceSummary, videoToPublish, audioToMonitor,
      displayClaps, monitorTimes, recordSummary, receivedSummary, monitorSummary,
      monitorUnderruns, lostSamples, recordingArtifact: artifactAbsolute,
      loopbackRaw, loopbackPackets, loopbackCaptureLog: loopbackStderr,
    }, null, 2));
    console.log(`Live evidence : ${evidencePath}`);
    if (Math.abs(monitorSummary.medianMs) > budgetMs) {
      failures.push(`live display/monitor skew ${monitorSummary.medianMs.toFixed(1)}ms exceeds the ${budgetMs}ms budget`);
    }
  }
  if (livePaths && rtmpLocal) {
    const reportPath = join(liveCaptureDir, "boundary-report.json");
    writeFileSync(reportPath, JSON.stringify({
      source: "one fake-engine flash/click run; all requested local outputs captured concurrently",
      buildSha: spawnSync("git", ["rev-parse", "HEAD"], { cwd: binarySourceRoot, encoding: "utf8" }).stdout.trim(),
      buildDir, seconds: recordSeconds, programBufferFrames,
      boundaries: {
        sourceIngress: "first decoded Zoom frame and PCM returned to core pollers; QPC, not Zoom sender time",
        program: "DXGI Program shared-texture publish; physical display vsync unmeasured",
        monitor: `WASAPI loopback from ${monitorDevice}; QPC`,
        recording: "finalized Program MP4 decoded content PTS",
        rtmp: "local receiver FLV decoded content PTS",
        youtube: "MISSING_EVIDENCE: no same-run YouTube ingest/playback capture",
      },
      sourceVideoClaps, sourceAudioClaps, displayClaps, monitorTimes,
      sourceIngressVideoMinusAudio: sourceSummary, sourceVideoIngressToProgramPublish: videoToPublish,
      sourceAudioIngressToMonitorLoopback: audioToMonitor,
      programMinusMonitor: monitorSummary, recordingVideoMinusAudio: recordSummary,
      rtmpVideoMinusAudio: receivedSummary, monitorUnderruns, lostSamples,
      artifacts: { recording: artifactAbsolute, receivedRtmp: rtmpReceived, loopbackRaw, loopbackPackets },
    }, null, 2));
    console.log(`Boundary report: ${reportPath}`);
  }
} catch (error) {
  failures.push(error.message);
} finally {
  if (loopback && loopback.exitCode === null) loopback.kill();
  if (rtmpReceiver && rtmpReceiver.exitCode === null) rtmpReceiver.kill();
  try { child.stdin.end(); } catch {}
  child.kill();
  if (!keepArtifact && !livePaths && !rtmpLocal && artifactAbsolute && existsSync(artifactAbsolute)) {
    try { rmSync(artifactAbsolute); } catch {}
  } else if (artifactAbsolute) {
    console.log(`Artifact      : ${artifactAbsolute}`);
  }
}

if (failures.length) {
  console.error("\nA/V CLAP VALIDATION FAIL");
  for (const f of failures) console.error(`  - ${f}`);
  process.exit(1);
}
console.log("\nA/V CLAP VALIDATION PASS");
