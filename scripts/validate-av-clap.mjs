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
 */
import { spawn, spawnSync } from "node:child_process";
import { existsSync, rmSync, statSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, "..");
const buildDir = join(repoRoot, "native", "build-dev");
const exeSuffix = process.platform === "win32" ? ".exe" : "";
const nativeCore = join(buildDir, `corevideo-native${exeSuffix}`);
const fakeEngine = join(buildDir, `corevideo-zoom-engine-fake${exeSuffix}`);

const args = process.argv.slice(2);
const argValue = (name, fallback) => {
  const index = args.indexOf(`--${name}`);
  return index >= 0 && args[index + 1] ? args[index + 1] : fallback;
};
const recordSeconds = Number(argValue("seconds", 24));
// ITU-R BT.1359 / ATSC IS-191 put the audible thresholds near +45ms (audio ahead)
// and -125ms (audio behind); the repo's own G2 gate is 50ms, so use that.
const budgetMs = Number(argValue("budget-ms", 50));
const keepArtifact = args.includes("--keep-artifact");
const frameSyncOff = args.includes("--no-frame-sync");
const verbose = args.includes("--verbose");
const clapIntervalMs = 3000;

if (!existsSync(nativeCore) || !existsSync(fakeEngine)) {
  console.error(`Missing ${nativeCore} or ${fakeEngine}. Build them first.`);
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
if (frameSyncOff) env.COREVIDEO_FRAME_SYNC = "0";

const child = spawn(nativeCore, [], { cwd: buildDir, env, stdio: ["pipe", "pipe", "pipe"] });

const startedAt = Date.now();
let nextId = 1;
let stdoutBuffer = "";
let handshake;
const pending = new Map();

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
child.stderr.on("data", (c) => { if (verbose) process.stderr.write(c.toString()); });
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

const failures = [];
let artifactAbsolute = null;
try {
  for (let i = 0; i < 200 && !handshake; i += 1) await sleep(50);
  if (!handshake) throw new Error("no native-core handshake");
  console.log(`Frame sync    : ${frameSyncOff ? "OFF (control)" : "ON (default)"}`);

  await send("zoom-join", { payload: { meetingNumber: "1234567890", displayName: "av-clap" } });
  await sleep(3000);

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
        ],
      },
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

  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{
      type: "start-recording-session", sessionId: "av-clap", startedAtMs: Date.now(),
      targetFolder: "Recordings/CoreVideoPro/validate-av-clap",
      filenamePrefix: "av-clap", format: "mp4", quality: "high", isoParticipantIds: [],
    }],
  });
  console.log(`Recording     : ${recordSeconds}s (clap every ${clapIntervalMs}ms)...`);

  let last = null;
  const deadline = Date.now() + recordSeconds * 1000;
  while (Date.now() < deadline) {
    await sleep(Math.min(5000, Math.max(1000, deadline - Date.now())));
    const syncResp = await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [] });
    last = syncResp.snapshot?.recording ?? {};
  }

  const stopResp = await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{ type: "stop-recording-session", reason: "av-clap complete" }],
  });
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
    console.log(`Skew (v-a)    : ${pairs.map((p) => p.toFixed(1)).join(", ")} ms`);
    console.log(`              : median ${median.toFixed(1)}ms, mean ${mean.toFixed(1)}ms, spread ${spread.toFixed(1)}ms`);
    console.log(median > 0
      ? `              : AUDIO LEADS video by ${Math.abs(median).toFixed(1)}ms`
      : `              : audio lags video by ${Math.abs(median).toFixed(1)}ms`);
    if (Math.abs(median) > budgetMs) {
      failures.push(`A/V skew ${median.toFixed(1)}ms exceeds the ${budgetMs}ms budget`);
    }
  }
} catch (error) {
  failures.push(error.message);
} finally {
  try { child.stdin.end(); } catch {}
  child.kill();
  if (!keepArtifact && artifactAbsolute && existsSync(artifactAbsolute)) {
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
