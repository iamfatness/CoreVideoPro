/**
 * Headless ISO-record validation harness (ISO-1 video + ISO-2 audio = Demo E).
 *
 * Proves the ISO pipeline end-to-end WITHOUT a real meeting or an operator:
 * spawns the native core with COREVIDEO_ZOOM_ENGINE_PATH pointed at the FAKE
 * zoom engine (multi-participant animated I420 + deterministic per-participant
 * tones over the real IPC), joins, subscribes participant video + isolate audio
 * through `zoom-media-spine-sync`, enables ISO on TWO participants
 * (`isoSourceIds: ["zoom:<id>", ...]`), records, and FAILS unless:
 *   - the recording snapshot reports one ISO stream per selected source,
 *   - each ISO stream muxed video frames (framesWritten > 0) with no per-stream
 *     warning, frameId-DEDUPED (well below the raw ~50 submit/s tick rate),
 *   - each ISO stream reports audioSamples > 0 and hasAudio (ISO-2 stems),
 *   - recording.warning stays empty (program never regressed — the #286 class),
 *   - each ISO-NN-*.mp4 exists on disk, non-zero, and (with ffprobe) carries
 *     BOTH a video stream AND an aac audio stream,
 *   - **Demo E head-clap alignment**: each ISO audio start vs the PROGRAM audio
 *     start is within 50 ms (inherited from the ONE shared recording epoch).
 *
 * Usage: node ./scripts/validate-iso-record.mjs [--seconds 20] [--source-fps 60] [--keep-artifacts]
 */
import { spawn, spawnSync } from "node:child_process";
import { existsSync, statSync, rmSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname, join, resolve, isAbsolute } from "node:path";
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
const recordSeconds = Number(argValue("seconds", 20));
const keepArtifacts = args.includes("--keep-artifacts");
const sourceCount = Number(argValue("sources", 2));
const stallSource = argValue("stall-source", "");
const stallMs = Number(argValue("stall-ms", 100));
const writeQueueDepth = Number(argValue("write-queue-depth", 10));
const evidencePath = argValue("evidence", "");
const allowCapacityWarning = args.includes("--allow-capacity-warning");
if (!Number.isInteger(sourceCount) || sourceCount < 2 || sourceCount > 8 ||
    !Number.isInteger(writeQueueDepth) || writeQueueDepth < 4 || writeQueueDepth > 30 ||
    (stallSource && (stallMs < 1 || stallMs > 2000))) throw new Error("Invalid recording queue QA options");
const snapshots = [], probes = [];
let stallBegin = false, stallEnd = false;
let lossBaseline = null;

// PIN the fake engine's source frame rate. Left unset it silently ran at the
// engine's default 30, so an ISO fps number from this rig could not be compared
// against a source rate — the denominator was never stated. mac-show-drill.py and
// qa/collect-runtime-snapshots.mjs both set it explicitly; so does this now.
// Zoom delivers up to 1080p60 and the product targets it, so drive 60.
const sourceFps = String(argValue("source-fps", process.env.COREVIDEO_FAKE_ENGINE_FPS ?? "60"));
const targetFolder = "Recordings/CoreVideoPro/validate-iso-record";

if (!existsSync(nativeCore) || !existsSync(fakeEngine)) {
  console.error(`Missing ${nativeCore} or ${fakeEngine}. Run scripts/build-native-dev.ps1 first.`);
  process.exit(1);
}

// A trial must not inherit unrelated CoreVideo experiment flags from the shell.
const trialEnv = Object.fromEntries(Object.entries(process.env).filter(([key]) => !key.startsWith("COREVIDEO_")));
const child = spawn(nativeCore, [], {
  cwd: buildDir,
  env: {
    ...trialEnv,
    COREVIDEO_ZOOM_ENGINE_PATH: fakeEngine,
    COREVIDEO_FAKE_NO_CHURN: "1",
    COREVIDEO_FAKE_ENGINE_FPS: sourceFps,
    COREVIDEO_FAKE_ENGINE_PARTICIPANTS: String(sourceCount),
    COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE: "0",
    COREVIDEO_FAKE_ENGINE_RES: "2",
    COREVIDEO_PROGRAM_BUFFER_FRAMES: "2",
    COREVIDEO_CPU_SOURCE_PREPARATION: "1",
    COREVIDEO_ISOLATE_MONITORS: "1",
    COREVIDEO_GPU_CAPTURE: "0",
    COREVIDEO_QA_RECORDING_STALL_SOURCE: stallSource,
    COREVIDEO_QA_RECORDING_STALL_MS: stallSource ? String(stallMs) : "0",
    COREVIDEO_QA_RECORDING_STALL_AFTER_FRAMES: stallSource ? "180" : "0",
  },
  stdio: ["pipe", "pipe", "pipe"],
});

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
child.stderr.on("data", (chunk) => {
  const text = chunk.toString();
  if (text.includes("[recording-write-stall-qa]") && text.includes(" begin")) stallBegin = true;
  if (text.includes("[recording-write-stall-qa]") && text.includes(" end")) stallEnd = true;
  process.stderr.write(text);
});
child.once("exit", (code) => {
  for (const { reject, timer } of pending.values()) { clearTimeout(timer); reject(new Error(`native core exited ${code}`)); }
  pending.clear();
});

function send(type, payload = {}) {
  const id = `iso-record-${nextId++}`;
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

function participantsOf(snapshot) {
  const raw = snapshot?.zoom?.participants ?? snapshot?.participants ?? [];
  return raw
    .map((p) => ({ id: String(p.userId ?? p.id ?? ""), name: String(p.displayName ?? p.name ?? ""), videoOn: p.videoOn !== false }))
    .filter((p) => p.id && p.videoOn);
}

function buildSpinePayload(participants) {
  const subscriptions = [
    { participantId: participants[0].id, kind: "meeting-audio", purpose: "program", priority: 0 },
    ...participants.map((p, i) => ({ participantId: p.id, kind: "participant-video", purpose: i === 0 ? "active-speaker" : "program", priority: 10 + i })),
    ...participants.map((p, i) => ({ participantId: p.id, kind: "participant-audio", purpose: "mix", priority: 40 + i })),
  ];
  return {
    readiness: { status: "ready", platform: process.platform === "darwin" ? "macos" : "windows", sdkVersion: "zoom-engine", checks: [], blockers: [], warnings: [], summary: "ISO record validation" },
    participants: participants.map((p) => ({ sdkUserId: p.id, displayName: p.name, role: "guest", videoOn: true, muted: false, talking: true, audioLevel: 60, networkQuality: "good" })),
    subscriptions,
    startCapture: true,
    blocked: false,
    warnings: [],
    summary: `${participants.length} participants, ${subscriptions.length} subscriptions`,
  };
}

function isoStreamsOf(snapshot) {
  const streams = snapshot?.recording?.streams ?? [];
  return streams.filter((s) => s.kind === "iso");
}

function ffprobeStreams(artifactPath) {
  for (const bin of ["ffprobe", "C:\\ffmpeg\\bin\\ffprobe.exe"]) {
    const probe = spawnSync(bin, ["-v", "error", "-print_format", "json", "-count_frames", "-show_streams", artifactPath], { encoding: "utf8", timeout: 60000 });
    if (probe.error || probe.status !== 0) continue;
    let parsed;
    try { parsed = JSON.parse(probe.stdout); } catch { continue; }
    const streams = parsed.streams ?? [];
    const video = streams.find((s) => s.codec_type === "video");
    const audio = streams.find((s) => s.codec_type === "audio");
    const audioStart = audio && audio.start_time != null ? Number(audio.start_time) : null;
    return {
      available: true,
      video: Boolean(video),
      videoCodec: video?.codec_name ?? null,
      videoFrames: video?.nb_read_frames == null ? null : Number(video.nb_read_frames),
      width: video?.width ?? null,
      height: video?.height ?? null,
      audio: Boolean(audio),
      audioCodec: audio?.codec_name ?? null,
      audioStartSec: Number.isFinite(audioStart) ? audioStart : null,
    };
  }
  return { available: false, video: true, audio: true, audioCodec: "aac", audioStartSec: null };  // skip gracefully
}

const failures = [];
const artifacts = [];
try {
  for (let i = 0; i < 200 && !handshake; i += 1) await sleep(50);
  if (!handshake) throw new Error("no native-core handshake");
  console.log(`Handshake     : ${handshake.profile?.name ?? "unknown"}`);

  await send("zoom-join", { payload: { meetingNumber: "1234567890", displayName: "iso-record-proof" } });
  console.log(`Source rate   : COREVIDEO_FAKE_ENGINE_FPS=${sourceFps} (one video stream per participant)`);
  console.log("Joined        : fake engine (multi-participant animated I420)");
  await sleep(3000);

  // Subscribe participant video via the spine until frames flow.
  let selected = [];
  for (let attempt = 0; attempt < 15; attempt += 1) {
    const snap = (await send("zoom-snapshot")).snapshot;
    const participants = participantsOf(snap);
    if (participants.length >= sourceCount) {
      await send("zoom-media-spine-sync", { spinePayload: buildSpinePayload(participants), elapsedMs: Date.now() - startedAt });
      selected = participants.slice(0, sourceCount);
    }
    await sleep(1000);
    if (selected.length >= sourceCount) break;
  }
  if (selected.length < sourceCount) throw new Error(`fake engine did not present 2 video participants (got ${selected.length})`);
  const isoSourceIds = selected.map((p) => `zoom:${p.id}`);
  console.log(`ISO sources   : ${isoSourceIds.join(", ")} (${selected.map((p) => p.name).join(", ")})`);

  // Arm output + recording with ISO on the two sources.
  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [
      { type: "set-verbose-diagnostics", enabled: false },
      { type: "set-output-profile", width:1920, height:1080, fps:60 },
      { type: "load-scene-graph", sceneId: "iso-record", routes: [{ routeId: "program", mode: "fixed", audioRole: "mix", participantId: selected[0].id }] },
      { type: "sync-audio-routing-matrix", sends: [{ sourceId: "zoom-mix", busId: "master", gainDb: 0 }, { sourceId: "zoom-mix", busId: "stream", gainDb: 0 }] },
      { type: "prepare-encoder-session", preparedAtMs: Date.now() - startedAt, reason: "iso-record warmup" },
      { type: "start-program-output", destinations: ["recording"], isoSourceIds },
      { type: "set-recording-targets", writeQueueDepth, renderProfile: {width:1920, height:1080, fps:60, codec:"h264"}, targetFolder, filenamePrefix: "iso", format: "mp4", quality: "high", isoSourceIds },
    ],
  });
  await sleep(2000);

  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{ type: "start-recording-session", writeQueueDepth, renderProfile: {width:1920, height:1080, fps:60, codec:"h264"}, sessionId: "iso-record-validation", startedAtMs: Date.now(), targetFolder, filenamePrefix: "iso", format: "mp4", quality: "high", isoSourceIds }],
  });
  console.log(`Recording     : ${recordSeconds}s with ISO on ${isoSourceIds.length} sources...`);

  let lastStreams = [];
  let lastWarning = null;
  const deadline = Date.now() + recordSeconds * 1000;
  while (Date.now() < deadline) {
    await sleep(Math.min(100, Math.max(1, deadline - Date.now())));
    const snap = (await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [] })).snapshot;
    snapshots.push(snap);
    if (!lossBaseline && (snap.recording?.proof?.programFrameCount ?? 0) > 120)
      lossBaseline = snap;
    lastStreams = isoStreamsOf(snap);
    lastWarning = snap?.recording?.warning ?? null;
    console.log(`poll          : iso=[${lastStreams.map((s) => `${s.displayName ?? s.sourceId}:${s.framesWritten}f/${s.audioSamples ?? 0}a`).join(", ")}] warning=${lastWarning ?? "none"}`);
    if (lastWarning && !(allowCapacityWarning && /^\d+ ISO sources? will record on the CPU software encoder, not the GPU/.test(lastWarning)))
      failures.push(`recording.warning surfaced: ${lastWarning}`);
  }

  let stopSnap = (await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [{ type: "stop-recording-session", reason: "iso-record validation complete" }] })).snapshot;
  for (let attempt = 0; attempt < 200 && !["completed", "failed"].includes(stopSnap.recording?.lifecycle?.state); ++attempt) {
    await sleep(100);
    stopSnap = (await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [] })).snapshot;
  }
  snapshots.push(stopSnap);
  if (stopSnap.recording?.lifecycle?.state !== "completed") failures.push("Recording did not finalize as completed");
  if (stallSource && (!stallBegin || !stallEnd)) failures.push("Requested stalled write was not observed begin/end");
  if (stallSource) {
    const target = (stopSnap.recording?.streams ?? []).find(s => stallSource === "program" ? s.kind === "program" : s.sourceId === stallSource);
    if (!target?.writeQueue || target.writeQueue.highWater < 2) failures.push("Stalled file queue did not show measured backlog");
    if ((target?.writeQueue?.dropped ?? -1) !== 0 || (target?.writeQueue?.droppedAudio ?? -1) !== 0) failures.push("Stalled file shed steady-state media");
  }
  if (lossBaseline) {
    const before = lossBaseline.recording?.proof ?? {}, after = stopSnap.recording?.proof ?? {};
    for (const key of ["programMissingFrames", "encoderQueueDroppedVideoFrames", "encoderQueueDroppedAudioPackets"]) {
      if (typeof before[key] !== "number" || typeof after[key] !== "number") failures.push(`Recording loss evidence missing: ${key}`);
      else if (after[key] > before[key]) failures.push(`Steady-state recording loss: ${key}`);
    }
    const b = lossBaseline.programBuffer, a = stopSnap.programBuffer;
    if (!b || !a) failures.push("Program presentation loss evidence missing");
    else {
      if (a.activeFrames !== 2 || b.activeFrames !== 2) failures.push("Live Program buffer changed from two frames");
      if (typeof a.underruns !== "number" || typeof b.underruns !== "number") failures.push("Program underrun counter missing");
      else if (a.underruns > b.underruns) failures.push("Program presentation underruns increased");
    }
  } else failures.push("Steady-state baseline missing");
  const finalStreams = isoStreamsOf(stopSnap);
  const programPath = stopSnap?.recording?.programPath ??
    (stopSnap?.recording?.streams ?? []).find((s) => s.kind === "program")?.path ?? null;

  // Wait for the async finalize to flush a writer's moov before probing.
  async function settle(abs) {
    let lastSize = -1;
    for (let i = 0; i < 20; i += 1) {
      await sleep(250);
      let size = 0;
      try { size = statSync(abs).size; } catch { continue; }
      if (size > 1024 && size === lastSize) break;
      lastSize = size;
    }
    return existsSync(abs) && statSync(abs).size > 0;
  }

  // Demo E baseline: the PROGRAM audio start on the shared epoch (every ISO
  // audio start is measured against this — same recordingClock_ epoch).
  let programAudioStartSec = null;
  if (programPath) {
    const programAbs = isAbsolute(programPath) ? programPath : resolve(buildDir, programPath);
    artifacts.push(programAbs);
    if (await settle(programAbs)) {
      const pp = ffprobeStreams(programAbs);
      probes.push({path:programAbs,kind:"program",...pp});
      programAudioStartSec = pp.audioStartSec;
      console.log(`ffprobe pgm   : ${JSON.stringify(pp)}`);
      if (pp.available && !pp.audio) failures.push("PROGRAM has no audio stream (program regressed)");
      if (!pp.available || !pp.video) failures.push("PROGRAM playable video evidence missing");
      if (pp.width !== 1920 || pp.height !== 1080) failures.push("Program recording geometry is not 1080p");
      if (pp.videoFrames !== stopSnap.recording?.proof?.recordingMuxVideoFrameCount) failures.push("Program decoded frame count differs from native mux count");
    }
  }

  if (finalStreams.length !== sourceCount) failures.push(`expected ${sourceCount} ISO streams, got ${finalStreams.length}`);
  // Upper bound on RAW submits, which is what the frameId dedup has to sit below.
  // ISO video is submitted by the 60Hz video tick (renderVideoOutputTick), beside
  // Program — NOT by the ~50Hz audio worker it used to ride, whose 20ms period is
  // an audio constant (960 samples at 48k) and structurally capped every ISO stem
  // at ~50 distinct frames/s. Leaving 50 here after that move turns the fix itself
  // into a failure: a healthy 60fps ISO writes ~52-60 frames/s, which is above the
  // old bound and below this one.
  const rawTickRate = recordSeconds * 60;
  const clapAlignmentsMs = [];
  for (const s of finalStreams) {
    if (Number(s.framesWritten) <= 0) failures.push(`ISO ${s.sourceId} muxed 0 frames`);
    if (s.warning) failures.push(`ISO ${s.sourceId} warning: ${s.warning}`);
    if (Number(s.framesWritten) >= rawTickRate) failures.push(`ISO ${s.sourceId} framesWritten=${s.framesWritten} not deduped (>= ${rawTickRate} raw ticks)`);
    // ISO-2: each ISO must carry its own audio stem (self-contained A+V).
    if (Number(s.audioSamples ?? 0) <= 0) failures.push(`ISO ${s.sourceId} muxed 0 audio samples (no stem)`);
    if (s.hasAudio === false) failures.push(`ISO ${s.sourceId} snapshot hasAudio=false`);
    if (s.path) {
      const abs = isAbsolute(s.path) ? s.path : resolve(buildDir, s.path);
      artifacts.push(abs);
      if (!(await settle(abs))) {
        failures.push(`ISO artifact missing/empty: ${abs}`);
      } else {
        const probe = ffprobeStreams(abs);
        probes.push({path:abs,kind:"iso",sourceId:s.sourceId,...probe});
        if (!probe.available) failures.push(`ISO ${s.sourceId} decode evidence missing`);
        if (probe.videoFrames !== s.framesWritten) failures.push(`ISO ${s.sourceId} decoded frame count differs from native writer count`);
        if (probe.width !== 1920 || probe.height !== 1080) failures.push(`ISO ${s.sourceId} geometry is not 1080p`);
        console.log(`ffprobe       : ${s.displayName ?? s.sourceId} -> ${JSON.stringify(probe)}`);
        if (!probe.video) failures.push(`ISO ${s.sourceId} has no video stream`);
        if (probe.available && !probe.audio) failures.push(`ISO ${s.sourceId} has no audio stream (not self-contained A+V)`);
        if (probe.available && probe.audio && probe.audioCodec !== "aac") failures.push(`ISO ${s.sourceId} audio codec ${probe.audioCodec} != aac`);
        // Demo E head-clap alignment: ISO audio start vs program audio start on
        // the ONE shared epoch, budget < 50 ms.
        if (probe.audioStartSec != null && programAudioStartSec != null) {
          const skewMs = Math.abs(probe.audioStartSec - programAudioStartSec) * 1000;
          clapAlignmentsMs.push(skewMs);
          console.log(`clap-align    : ${s.displayName ?? s.sourceId} audioStart=${probe.audioStartSec}s vs program=${programAudioStartSec}s -> ${skewMs.toFixed(1)}ms`);
          if (skewMs >= 50) failures.push(`ISO ${s.sourceId} head-clap skew ${skewMs.toFixed(1)}ms >= 50ms (Demo E)`);
        }
      }
    } else {
      failures.push(`ISO ${s.sourceId} has no path in the snapshot`);
    }
  }

  console.log("");
  if (clapAlignmentsMs.length > 0) {
    const worst = Math.max(...clapAlignmentsMs);
    console.log(`Demo E clap   : worst ISO-vs-program audio-start skew ${worst.toFixed(1)}ms (budget 50ms)`);
  }
  console.log(`Measured at   : source ${sourceFps} fps/participant, ${recordSeconds}s -> ${finalStreams.map((s) => `${s.displayName ?? s.sourceId}:${(Number(s.framesWritten) / recordSeconds).toFixed(1)}fps`).join(", ")}`);
  console.log(`Result        : ${finalStreams.length} ISO streams -> [${finalStreams.map((s) => `${s.displayName ?? s.sourceId}:${s.framesWritten}f/${s.audioSamples ?? 0}a`).join(", ")}]`);
  if (failures.length === 0) {
    console.log("ISO-RECORD VALIDATION PASS");
  } else {
    console.log("ISO-RECORD VALIDATION FAIL");
    for (const failure of failures) console.log(`  - ${failure}`);
  }
} catch (error) {
  failures.push(error instanceof Error ? error.message : String(error));
  console.error("ISO-RECORD VALIDATION FAIL:", failures.join(" | "));
} finally {
  if (evidencePath) {
    mkdirSync(dirname(resolve(evidencePath)), {recursive:true});
    writeFileSync(evidencePath, JSON.stringify({sourceCount,sourceFps,recordSeconds,stallSource,stallMs,writeQueueDepth,allowCapacityWarning,programBufferFrames:2,monitorIsolation:true,cpuSourcePreparation:true,verboseDiagnostics:false,stallBegin,stallEnd,failures,probes,snapshots}, null, 2));
  }
  child.kill();
  if (!keepArtifacts) {
    for (const abs of artifacts) {
      try { if (existsSync(abs)) rmSync(abs); } catch { /* best-effort */ }
    }
  }
}
process.exit(failures.length === 0 ? 0 : 1);
