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
 *                                             --monitor-id <WASAPI endpoint id>]
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
const monitorDevice = argValue("monitor-device", "Game (TC-HELICON GoXLR)");
const monitorId = argValue("monitor-id", "");
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
if (livePaths) env.COREVIDEO_AV_SYNC_TRACE = "1";
if (frameSyncOff) env.COREVIDEO_FRAME_SYNC = "0";

const child = spawn(nativeCore, [], { cwd: buildDir, env, stdio: ["pipe", "pipe", "pipe"] });

const startedAt = Date.now();
let nextId = 1;
let stdoutBuffer = "";
let handshake;
const pending = new Map();
const displayClaps = [];
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
    ["-v", "error", "-i", artifact, "-vf", "scale=8:8", "-f", "rawvideo", "-pix_fmt", "gray", "-"],
    { encoding: "buffer", maxBuffer: 1 << 28, timeout: 120000 });
  if (out.status !== 0 || !out.stdout?.length) return [];
  const px = 64;
  const frames = Math.floor(out.stdout.length / px);
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
    if (hot && !prevHot) times.push(f / fps);
    prevHot = hot;
  }
  return times;
}

/** Burst onsets in the decoded audio, in seconds. */
function audioBurstTimes(artifact) {
  const out = spawnSync(ffmpeg,
    ["-v", "error", "-i", artifact, "-f", "s16le", "-ac", "1", "-ar", "48000", "-"],
    { encoding: "buffer", maxBuffer: 1 << 28, timeout: 120000 });
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
  const pairs = [];
  for (const v of videoTimes) {
    let best = null;
    for (const a of audioTimes) {
      const d = v - a;
      if (best === null || Math.abs(d) < Math.abs(best)) best = d;
    }
    // Anything beyond half a clap interval is a mis-pair, not a measurement.
    if (best !== null && Math.abs(best) < clapIntervalMs / 2000) pairs.push(best * 1000);
  }
  return pairs;
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

const failures = [];
let artifactAbsolute = null;
let loopback = null;
let loopbackDone = null;
let loopbackStderr = "";
const liveCaptureDir = join(buildDir, "Recordings", "CoreVideoPro", "validate-av-clap", `live-paths-${Date.now()}`);
const loopbackRaw = join(liveCaptureDir, "monitor.f32");
const loopbackPackets = join(liveCaptureDir, "monitor-packets.csv");
let startCounters = null;
let endCounters = null;
let recordSummary = null;
try {
  for (let i = 0; i < 200 && !handshake; i += 1) await sleep(50);
  if (!handshake) throw new Error("no native-core handshake");
  console.log(`Frame sync    : ${frameSyncOff ? "OFF (control)" : "ON (default)"}`);

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
      { type: "start-program-output", destinations: ["recording"], isoParticipantIds: [] },
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
  const deadline = Date.now() + recordSeconds * 1000;
  while (Date.now() < deadline) {
    await sleep(Math.min(5000, Math.max(1000, deadline - Date.now())));
    const syncResp = await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [] });
    last = syncResp.snapshot?.recording ?? {};
    endCounters = syncResp.snapshot;
  }

  const stopResp = await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{ type: "stop-recording-session", reason: "av-clap complete" }],
  });
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
  const [num, den] = String(vStream.r_frame_rate ?? "60/1").split("/").map(Number);
  const fps = den ? num / den : 60;

  const videoTimes = videoFlashTimes(artifactAbsolute, fps);
  const audioTimes = audioBurstTimes(artifactAbsolute);
  console.log(`Events        : ${videoTimes.length} video flashes, ${audioTimes.length} audio bursts @ ${fps.toFixed(2)}fps`);

  const pairs = pairEvents(videoTimes, audioTimes);
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
    if (livePaths) recordSummary = describePairs("Record v-a", pairs);
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
    const monitorTimes = loopbackBurstTimes(loopbackRaw, loopbackPackets, Number(format[1]), Number(format[2]));
    const displayTimes = displayClaps.map((clap) => clap.qpc100ns / 1e7);
    const monitorSummary = describePairs("Display-MON", pairEvents(displayTimes, monitorTimes));
    if (!recordSummary) throw new Error("recording clap result missing while live paths were requested");
    const monitorBefore = startCounters?.audioMixSession?.monitorUnderruns;
    const monitorAfter = endCounters?.audioMixSession?.monitorUnderruns;
    const lostBefore = startCounters?.realtimeEvidence?.audio?.audioLostSamples;
    const lostAfter = endCounters?.realtimeEvidence?.audio?.audioLostSamples;
    if (![monitorBefore, monitorAfter, lostBefore, lostAfter].every(Number.isFinite)) {
      throw new Error("monitor underrun or lost-sample counters missing from the same run");
    }
    const monitorUnderruns = monitorAfter - monitorBefore;
    const lostSamples = lostAfter - lostBefore;
    console.log(`Continuity    : monitor underruns +${monitorUnderruns}, audio lost samples +${lostSamples}`);
    if (monitorUnderruns !== 0 || lostSamples !== 0) {
      failures.push(`continuity failed: monitor underruns +${monitorUnderruns}, audio lost samples +${lostSamples}`);
    }
    const evidencePath = join(liveCaptureDir, "evidence.json");
    writeFileSync(evidencePath, JSON.stringify({
      source: "fake-engine timed clap; Program texture publish, endpoint WASAPI loopback, and recorded Program from one run",
      buildDir, monitorDevice, monitorId, seconds: recordSeconds,
      displayMarker: "delivered Program source frame at DXGI shared-texture publish; actual monitor vsync is not measured",
      displayClaps, monitorTimes, recordSummary, monitorSummary,
      monitorUnderruns, lostSamples, recordingArtifact: artifactAbsolute,
      loopbackRaw, loopbackPackets, loopbackCaptureLog: loopbackStderr,
    }, null, 2));
    console.log(`Live evidence : ${evidencePath}`);
    if (Math.abs(monitorSummary.medianMs) > budgetMs) {
      failures.push(`live display/monitor skew ${monitorSummary.medianMs.toFixed(1)}ms exceeds the ${budgetMs}ms budget`);
    }
  }
} catch (error) {
  failures.push(error.message);
} finally {
  if (loopback && loopback.exitCode === null) loopback.kill();
  try { child.stdin.end(); } catch {}
  child.kill();
  if (!keepArtifact && !livePaths && artifactAbsolute && existsSync(artifactAbsolute)) {
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
