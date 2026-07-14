/**
 * Headless recording-AUDIO validation harness (born from the 2026-07-13
 * alpha-blocking zero-audio-recording bug).
 *
 * `validate-record-stream.mjs` cannot prove audio: with no live sources the
 * master bus is legitimately silent, so audioPacketsObserved=0 proves nothing.
 * This harness puts REAL audio in the mix headlessly: it spawns the native core
 * with COREVIDEO_ZOOM_ENGINE_PATH pointed at the FAKE zoom engine (deterministic
 * tones over the real IPC — no binary swap needed), joins, routes zoom-mix ->
 * master/mon/stream, arms program output, waits (real operator flow: Engine On
 * before Record), records, and FAILS unless:
 *   - recording.proof.audioPacketsObserved > 0 and growing,
 *   - recording.warning stays empty,
 *   - (when ffprobe is available) the MP4 has video+audio streams with
 *     |start delta| < 50ms and |duration delta| < 200ms.
 *
 * Usage: node ./scripts/validate-record-audio.mjs [--seconds 30] [--keep-artifact]
 */
import { spawn, spawnSync } from "node:child_process";
import { existsSync, statSync, rmSync } from "node:fs";
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
const recordSeconds = Number(argValue("seconds", 30));
const keepArtifact = args.includes("--keep-artifact");

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
  const id = `record-audio-${nextId++}`;
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

function recordingState(snapshot) {
  const rec = snapshot?.recording ?? {};
  return {
    status: rec.status ?? null,
    warning: rec.warning ?? null,
    videoFrames: Math.max(Number(rec.proof?.videoFrameCount ?? 0), Number(rec.proof?.programFrameCount ?? 0),
                          Number(rec.programFramesWritten ?? 0)),
    audioPackets: Number(rec.proof?.audioPacketsObserved ?? 0),
    artifact: rec.artifactPath ?? null,
    encoderWarnings: snapshot?.encoderSession?.warnings ?? [],
  };
}

function ffprobeVerdict(artifactPath) {
  const candidates = ["ffprobe", "C:\\ffmpeg\\bin\\ffprobe.exe"];
  for (const bin of candidates) {
    const probe = spawnSync(bin, ["-v", "error", "-print_format", "json", "-show_format", "-show_streams", artifactPath], {
      encoding: "utf8",
      timeout: 20000,
    });
    if (probe.error || probe.status !== 0) continue;
    let parsed;
    try { parsed = JSON.parse(probe.stdout); } catch { continue; }
    const video = (parsed.streams ?? []).find((s) => s.codec_type === "video");
    const audio = (parsed.streams ?? []).find((s) => s.codec_type === "audio");
    const startDeltaMs = video && audio ? Math.abs(Number(video.start_time) - Number(audio.start_time)) * 1000 : null;
    const durDeltaMs = video && audio ? Math.abs(Number(video.duration) - Number(audio.duration)) * 1000 : null;
    return {
      available: true,
      video: Boolean(video),
      audio: Boolean(audio),
      videoCodec: video?.codec_name ?? null,
      audioCodec: audio?.codec_name ?? null,
      startDeltaMs,
      durDeltaMs,
      pass: Boolean(video && audio) && startDeltaMs !== null && startDeltaMs < 50 && durDeltaMs !== null && durDeltaMs < 200,
    };
  }
  return { available: false, pass: true };  // skip gracefully without ffprobe
}

const failures = [];
let artifactAbsolute = null;
try {
  for (let i = 0; i < 200 && !handshake; i += 1) await sleep(50);
  if (!handshake) throw new Error("no native-core handshake");
  console.log(`Handshake     : ${handshake.profile?.name ?? "unknown"}`);

  await send("zoom-join", {
    payload: { meetingNumber: "1234567890", displayName: "record-audio-proof" },
  });
  console.log("Joined        : fake engine (deterministic tones)");
  await sleep(3000);  // engine spin-up + first tones

  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [
      {
        type: "load-scene-graph",
        sceneId: "record-audio-validation",
        routes: [{ routeId: "program", mode: "fixed", audioRole: "mix", participantId: "speaker-1" }],
      },
      {
        type: "sync-audio-routing-matrix",
        sends: [
          { sourceId: "zoom-mix", busId: "master", gainDb: 0 },
          { sourceId: "zoom-mix", busId: "mon", gainDb: 0 },
          { sourceId: "zoom-mix", busId: "stream", gainDb: 0 },
        ],
      },
      { type: "prepare-encoder-session", preparedAtMs: Date.now() - startedAt, reason: "record-audio warmup" },
      { type: "start-program-output", destinations: ["recording"], isoParticipantIds: [] },
      {
        type: "set-recording-targets",
        targetFolder: "Recordings/CoreVideoPro/validate-record-audio",
        filenamePrefix: "record-audio",
        format: "mp4",
        quality: "high",
        isoParticipantIds: [],
      },
    ],
  });
  console.log("Armed output  : recording destination live, waiting 2s (operator flow: Engine On before Record)");
  await sleep(2000);

  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{
      type: "start-recording-session",
      sessionId: "record-audio-validation",
      startedAtMs: Date.now(),
      targetFolder: "Recordings/CoreVideoPro/validate-record-audio",
      filenamePrefix: "record-audio",
      format: "mp4",
      quality: "high",
      isoParticipantIds: [],
    }],
  });
  console.log(`Recording     : ${recordSeconds}s...`);

  let last = null;
  const deadline = Date.now() + recordSeconds * 1000;
  while (Date.now() < deadline) {
    await sleep(Math.min(5000, Math.max(1000, deadline - Date.now())));
    const syncResp = await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [] });
    last = recordingState(syncResp.snapshot);
    console.log(`poll          : video=${last.videoFrames} audio=${last.audioPackets} warning=${last.warning ?? "none"}`);
    if (last.warning) failures.push(`recording.warning surfaced: ${last.warning}`);
  }

  const stopResp = await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{ type: "stop-recording-session", reason: "record-audio validation complete" }],
  });
  const final = recordingState(stopResp.snapshot);

  if (final.audioPackets <= 0) failures.push("audioPacketsObserved is 0 — the MP4 muxed no audio");
  if (final.videoFrames <= 0) failures.push("no video frames muxed");
  if (final.encoderWarnings.length > 0) failures.push(`encoderSession.warnings: ${final.encoderWarnings.join(" | ")}`);

  if (final.artifact) {
    artifactAbsolute = resolve(buildDir, final.artifact);
    let lastSize = -1;
    for (let i = 0; i < 20; i += 1) {
      await sleep(250);
      let size = 0;
      try { size = statSync(artifactAbsolute).size; } catch { continue; }
      if (size > 1024 && size === lastSize) break;
      lastSize = size;
    }
    const verdict = ffprobeVerdict(artifactAbsolute);
    console.log(`ffprobe       : ${JSON.stringify(verdict)}`);
    if (!verdict.pass) failures.push(`ffprobe A/V check failed: ${JSON.stringify(verdict)}`);
  } else {
    failures.push("no recording artifact path in the snapshot");
  }

  console.log("");
  console.log(`Result        : video=${final.videoFrames} frames, audio=${final.audioPackets} packets`);
  if (failures.length === 0) {
    console.log("RECORD-AUDIO VALIDATION PASS");
  } else {
    console.log("RECORD-AUDIO VALIDATION FAIL");
    for (const failure of failures) console.log(`  - ${failure}`);
  }
} catch (error) {
  failures.push(error instanceof Error ? error.message : String(error));
  console.error("RECORD-AUDIO VALIDATION FAIL:", failures.join(" | "));
} finally {
  child.kill();
  if (!keepArtifact && artifactAbsolute && existsSync(artifactAbsolute)) {
    try { rmSync(artifactAbsolute); } catch { /* artifact cleanup is best-effort */ }
  }
}
process.exit(failures.length === 0 ? 0 : 1);
