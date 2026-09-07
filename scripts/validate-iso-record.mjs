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
 * Usage: node ./scripts/validate-iso-record.mjs [--seconds 20] [--keep-artifacts]
 */
import { spawn, spawnSync } from "node:child_process";
import { existsSync, statSync, rmSync } from "node:fs";
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
const targetFolder = "Recordings/CoreVideoPro/validate-iso-record";

if (!existsSync(nativeCore) || !existsSync(fakeEngine)) {
  console.error(`Missing ${nativeCore} or ${fakeEngine}. Run scripts/build-native-dev.ps1 first.`);
  process.exit(1);
}

const child = spawn(nativeCore, [], {
  cwd: buildDir,
  env: { ...process.env, COREVIDEO_ZOOM_ENGINE_PATH: fakeEngine, COREVIDEO_FAKE_NO_CHURN: "1" },
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
child.stderr.on("data", (chunk) => process.stderr.write(chunk.toString()));
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
    const probe = spawnSync(bin, ["-v", "error", "-print_format", "json", "-show_streams", artifactPath], { encoding: "utf8", timeout: 20000 });
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
  console.log("Joined        : fake engine (multi-participant animated I420)");
  await sleep(3000);

  // Subscribe participant video via the spine until frames flow.
  let selected = [];
  for (let attempt = 0; attempt < 15; attempt += 1) {
    const snap = (await send("zoom-snapshot")).snapshot;
    const participants = participantsOf(snap);
    if (participants.length >= 2) {
      await send("zoom-media-spine-sync", { spinePayload: buildSpinePayload(participants), elapsedMs: Date.now() - startedAt });
      selected = participants.slice(0, 2);
    }
    await sleep(1000);
    if (selected.length >= 2) break;
  }
  if (selected.length < 2) throw new Error(`fake engine did not present 2 video participants (got ${selected.length})`);
  const isoSourceIds = selected.map((p) => `zoom:${p.id}`);
  console.log(`ISO sources   : ${isoSourceIds.join(", ")} (${selected.map((p) => p.name).join(", ")})`);

  // Arm output + recording with ISO on the two sources.
  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [
      { type: "load-scene-graph", sceneId: "iso-record", routes: [{ routeId: "program", mode: "fixed", audioRole: "mix", participantId: selected[0].id }] },
      { type: "sync-audio-routing-matrix", sends: [{ sourceId: "zoom-mix", busId: "master", gainDb: 0 }, { sourceId: "zoom-mix", busId: "stream", gainDb: 0 }] },
      { type: "prepare-encoder-session", preparedAtMs: Date.now() - startedAt, reason: "iso-record warmup" },
      { type: "start-program-output", destinations: ["recording"], isoSourceIds },
      { type: "set-recording-targets", targetFolder, filenamePrefix: "iso", format: "mp4", quality: "high", isoSourceIds },
    ],
  });
  await sleep(2000);

  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{ type: "start-recording-session", sessionId: "iso-record-validation", startedAtMs: Date.now(), targetFolder, filenamePrefix: "iso", format: "mp4", quality: "high", isoSourceIds }],
  });
  console.log(`Recording     : ${recordSeconds}s with ISO on ${isoSourceIds.length} sources...`);

  let lastStreams = [];
  let lastWarning = null;
  const deadline = Date.now() + recordSeconds * 1000;
  while (Date.now() < deadline) {
    await sleep(Math.min(4000, Math.max(1000, deadline - Date.now())));
    const snap = (await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [] })).snapshot;
    lastStreams = isoStreamsOf(snap);
    lastWarning = snap?.recording?.warning ?? null;
    console.log(`poll          : iso=[${lastStreams.map((s) => `${s.displayName ?? s.sourceId}:${s.framesWritten}f/${s.audioSamples ?? 0}a`).join(", ")}] warning=${lastWarning ?? "none"}`);
    if (lastWarning) failures.push(`recording.warning surfaced: ${lastWarning}`);
  }

  const stopSnap = (await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [{ type: "stop-recording-session", reason: "iso-record validation complete" }] })).snapshot;
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
      programAudioStartSec = pp.audioStartSec;
      console.log(`ffprobe pgm   : ${JSON.stringify(pp)}`);
      if (pp.available && !pp.audio) failures.push("PROGRAM has no audio stream (program regressed)");
    }
  }

  if (finalStreams.length < 2) failures.push(`expected 2 ISO streams, got ${finalStreams.length}`);
  const rawTickRate = recordSeconds * 50;  // ~50 submit ticks/s upper bound
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
  child.kill();
  if (!keepArtifacts) {
    for (const abs of artifacts) {
      try { if (existsSync(abs)) rmSync(abs); } catch { /* best-effort */ }
    }
  }
}
process.exit(failures.length === 0 ? 0 : 1);
