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
 *
 * --media leg (T1.6 / #455): media-clip audio never reached the audio engine.
 * The decoder labels each clip's PCM `media:<assetId>` while the shell's strip
 * and sends say "media"; the core's alias (core/AudioControlSourcePolicy.h)
 * joins them. This leg proves it end to end with the REAL Media Foundation
 * decoder: it generates a short H.264 + AAC 440 Hz clip with ffmpeg (temp dir,
 * deleted afterwards), routes it as a PLAYING fixed media route on Program,
 * sends the SHELL-SHAPED console (a "media" strip + a "zoom-mix" strip, and
 * "media" -> master/pgm-l/pgm-r/stream/mon at 0 dB, exactly what
 * StudioViewModel.EnsureDefaultMediaAudioRoutingSends seeds), records, and
 * FAILS unless the recording's decoded audio is non-silent AND dominated by
 * 440 Hz. It never joins Zoom (no zoom-join, no engine is spawned).
 *
 * Usage: node ./scripts/validate-record-audio.mjs [--seconds 30] [--keep-artifact] [--media]
 */
import { spawn, spawnSync } from "node:child_process";
import { existsSync, statSync, rmSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
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
const mediaLeg = args.includes("--media");
const recordSeconds = Number(argValue("seconds", mediaLeg ? 10 : 30));
const keepArtifact = args.includes("--keep-artifact");

if (!existsSync(nativeCore) || (!mediaLeg && !existsSync(fakeEngine))) {
  console.error(`Missing ${nativeCore} or ${fakeEngine}. Run scripts/build-native-dev.ps1 first.`);
  process.exit(1);
}

const findTool = (name) => [name, `C:\\ffmpeg\\bin\\${name}.exe`].find((bin) => {
  const probe = spawnSync(bin, ["-version"], { encoding: "utf8", timeout: 10000 });
  return !probe.error && probe.status === 0;
});

// --media: a real clip for the real decoder. Long enough to outlast the
// recording so the leg never depends on loop behaviour.
const MEDIA_TONE_HZ = 440;
const MEDIA_ASSET_ID = "validate-media-tone";
let mediaTempDir = null;
let mediaClipPath = null;
if (mediaLeg) {
  const ffmpegForClip = findTool("ffmpeg");
  if (!ffmpegForClip) {
    console.error("--media needs ffmpeg (PATH or C:\\ffmpeg\\bin\\ffmpeg.exe) to generate the test clip.");
    process.exit(1);
  }
  mediaTempDir = mkdtempSync(join(tmpdir(), "corevideo-validate-media-"));
  mediaClipPath = join(mediaTempDir, "tone-clip.mp4");
  const clipSeconds = recordSeconds + 15;
  const gen = spawnSync(ffmpegForClip, [
    "-v", "error", "-y",
    "-f", "lavfi", "-i", "testsrc=size=640x360:rate=30",
    "-f", "lavfi", "-i", `sine=frequency=${MEDIA_TONE_HZ}:sample_rate=48000`,
    "-t", String(clipSeconds),
    "-c:v", "libx264", "-pix_fmt", "yuv420p",
    "-c:a", "aac", "-b:a", "128k", "-ac", "2",
    "-shortest", mediaClipPath,
  ], { encoding: "utf8", timeout: 120000 });
  if (gen.status !== 0 || !existsSync(mediaClipPath)) {
    console.error(`ffmpeg could not generate the media clip: ${gen.stderr || gen.error}`);
    rmSync(mediaTempDir, { recursive: true, force: true });
    process.exit(1);
  }
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

function participantsOf(snapshot) {
  const raw = snapshot?.zoom?.participants ?? snapshot?.participants ?? [];
  return raw
    .map((p) => ({
      id: String(p.userId ?? p.id ?? ""),
      name: String(p.displayName ?? p.name ?? ""),
      videoOn: p.videoOn !== false,
    }))
    .filter((p) => p.id && p.videoOn);
}

function buildSpinePayload(participants) {
  const subscriptions = [
    {
      participantId: participants[0].id,
      kind: "meeting-audio",
      purpose: "program",
      priority: 0,
    },
    ...participants.map((p, index) => ({
      participantId: p.id,
      kind: "participant-video",
      purpose: index === 0 ? "active-speaker" : "program",
      priority: 10 + index,
    })),
  ];
  return {
    readiness: { status: "ready", platform: "windows", sdkVersion: "fake-engine", checks: [], blockers: [], warnings: [], summary: "record-audio validation" },
    participants: participants.map((p) => ({ sdkUserId: p.id, displayName: p.name, role: "guest", videoOn: true, muted: false, talking: true, audioLevel: 60, networkQuality: "good" })),
    subscriptions,
    startCapture: true,
    blocked: false,
    warnings: [],
    summary: `${participants.length} participants, ${subscriptions.length} subscriptions`,
  };
}

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

// PIXELS, not just packets. Every recording check in this repo asserted that
// streams EXIST and that their container start/duration line up — and none ever
// looked at an actual pixel. That let the program recording mux the 320x180 UI
// thumbnail into a 1920x1080 writer for months: the whole show sat in a corner of
// a black frame while every validator stayed green (a 2026-07-13 recording:
// 8995 frames, mean luma 4/255).
//
// Two cheap assertions close that hole. Decode to 8x8 gray (64 cells/frame):
//   * the frame must not be BLACK — median cell luma above a floor;
//   * content must not be CONFINED TO A CORNER — most cells must carry signal.
// A thumbnail in the corner of a 1080p frame lights ~2 of 64 cells, so the
// coverage check is what actually catches that defect; the luma floor catches a
// wholly black recording.
function programPixelVerdict(artifactPath, ffmpegBin) {
  const out = spawnSync(ffmpegBin,
    ["-v", "error", "-i", artifactPath, "-vf", "scale=8:8", "-f", "rawvideo", "-pix_fmt", "gray", "-"],
    { encoding: "buffer", maxBuffer: 1 << 28, timeout: 120000 });
  if (out.status !== 0 || !out.stdout?.length) {
    return { available: false, pass: true };  // cannot decode: skip, never fail blind
  }
  const cells = 64;
  const frames = Math.floor(out.stdout.length / cells);
  if (frames === 0) return { available: false, pass: true };
  // Sample up to 60 frames spread across the file, skipping the first second
  // (startup frames can legitimately be black before the first composite).
  const first = Math.min(frames - 1, 50);
  const step = Math.max(1, Math.floor((frames - first) / 60));
  const frameMeans = [];
  const coverages = [];
  for (let f = first; f < frames; f += step) {
    let sum = 0;
    let lit = 0;
    for (let i = 0; i < cells; i += 1) {
      const v = out.stdout[f * cells + i];
      sum += v;
      if (v > 10) lit += 1;
    }
    frameMeans.push(sum / cells);
    coverages.push(lit / cells);
  }
  const median = (xs) => [...xs].sort((a, b) => a - b)[Math.floor(xs.length / 2)];
  const medianLuma = median(frameMeans);
  const medianCoverage = median(coverages);
  const MIN_LUMA = 8;        // a black program
  const MIN_COVERAGE = 0.5;  // a program confined to part of the frame
  return {
    available: true,
    medianLuma: Number(medianLuma.toFixed(1)),
    medianCoverage: Number(medianCoverage.toFixed(2)),
    sampled: frameMeans.length,
    pass: medianLuma >= MIN_LUMA && medianCoverage >= MIN_COVERAGE,
  };
}

// SOUND, not just packets (the --media leg). An audio track full of digital
// silence carries packets, passes ffprobe and lines up with video — which is
// exactly what a recording looks like when media PCM is dropped before the bus.
// Decode the track to mono f32, skip the first second, and require:
//   * a real level (peak and RMS above silence), and
//   * that the level is the clip's tone: a Goertzel at the tone frequency must
//     hold most of the signal's energy (a click, hum or another source cannot).
function mediaAudioVerdict(artifactPath, ffmpegBin, toneHz) {
  const sampleRate = 48000;
  const out = spawnSync(ffmpegBin,
    ["-v", "error", "-i", artifactPath, "-map", "0:a:0", "-ac", "1", "-ar", String(sampleRate), "-f", "f32le", "-"],
    { encoding: "buffer", maxBuffer: 1 << 28, timeout: 120000 });
  if (out.status !== 0 || !out.stdout?.length) {
    return { available: false, pass: false, reason: "could not decode the recording's audio track" };
  }
  const buf = out.stdout;
  const samples = new Float32Array(buf.buffer, buf.byteOffset, Math.floor(buf.length / 4));
  const durationSeconds = samples.length / sampleRate;
  const body = samples.subarray(Math.min(samples.length, sampleRate));  // skip startup second
  if (body.length < sampleRate) {
    return { available: true, pass: false, durationSeconds, reason: "less than 1s of audio after the startup second" };
  }
  let peak = 0;
  let sumSquares = 0;
  for (const s of body) {
    peak = Math.max(peak, Math.abs(s));
    sumSquares += s * s;
  }
  const rms = Math.sqrt(sumSquares / body.length);
  // Goertzel over 100ms windows: energy at toneHz vs the window's total energy.
  const windowSize = sampleRate / 10;
  const coeff = 2 * Math.cos((2 * Math.PI * toneHz) / sampleRate);
  let toneEnergy = 0;
  let totalEnergy = 0;
  for (let start = 0; start + windowSize <= body.length; start += windowSize) {
    let s1 = 0;
    let s2 = 0;
    let windowEnergy = 0;
    for (let i = start; i < start + windowSize; i += 1) {
      const s0 = body[i] + coeff * s1 - s2;
      s2 = s1;
      s1 = s0;
      windowEnergy += body[i] * body[i];
    }
    const power = s1 * s1 + s2 * s2 - coeff * s1 * s2;   // |X(k)|^2
    toneEnergy += (2 * power) / windowSize;                // Parseval-scaled
    totalEnergy += windowEnergy;
  }
  const toneShare = totalEnergy > 0 ? toneEnergy / totalEnergy : 0;
  const MIN_PEAK = 0.03;       // ~-30 dBFS; the clip's sine is ~-18 dBFS
  const MIN_RMS = 0.01;
  const MIN_TONE_SHARE = 0.5;
  const peakDbfs = peak > 0 ? 20 * Math.log10(peak) : -Infinity;
  return {
    available: true,
    durationSeconds: Number(durationSeconds.toFixed(2)),
    peak: Number(peak.toFixed(4)),
    peakDbfs: Number(peakDbfs.toFixed(1)),
    rms: Number(rms.toFixed(4)),
    toneHz,
    toneShare: Number(toneShare.toFixed(3)),
    pass: peak >= MIN_PEAK && rms >= MIN_RMS && toneShare >= MIN_TONE_SHARE,
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

  let sceneAndAudioCommands;
  if (mediaLeg) {
    // No Zoom at all: the media clip is the only audio source.
    console.log(`Media clip    : ${mediaClipPath} (H.264 + AAC ${MEDIA_TONE_HZ} Hz)`);
    // Shell-shaped channel strip (MediaCoreCommandBuilder.BuildAudioMixCommand).
    const strip = (participantId) => ({
      participantId, inputLevel: 0, muted: false, noiseSuppression: false, manualGainDb: 0,
      pan: 0, solo: false, pluginInserts: [], insertSettings: {},
    });
    sceneAndAudioCommands = [
      {
        type: "load-scene-graph",
        sceneId: "record-audio-media-validation",
        routes: [{
          routeId: "media-main",
          mode: "fixed",
          mediaAssetId: MEDIA_ASSET_ID,
          mediaAssetName: "Validation tone",
          mediaAssetKind: "video",
          mediaAssetPath: mediaClipPath,
          mediaPlaybackKey: `media:${MEDIA_ASSET_ID}:live:1`,
          mediaAssetPlaying: true,
          rect: { x: 0, y: 0, width: 1, height: 1 },
        }],
      },
      // The shell ALWAYS syncs a console, so the FADER LAW is live: a media
      // source with no governing strip is dropped. This is the shape that
      // shipped silent.
      { type: "sync-participant-audio-mix", limiterEnabled: true, channels: [strip("media"), strip("zoom-mix")] },
      {
        type: "sync-audio-routing-matrix",
        sends: ["master", "pgm-l", "pgm-r", "stream", "mon"].map((busId) => ({
          sourceId: "media", busId, gainDb: 0, busPluginInserts: [],
        })),
        busSends: [],
        monitorBusId: "",
      },
    ];
    await sleep(500);
  } else {
    await send("zoom-join", {
      payload: { meetingNumber: "1234567890", displayName: "record-audio-proof" },
    });
    console.log("Joined        : fake engine (deterministic tones)");
    await sleep(1000);

    let participants = [];
    for (let attempt = 0; attempt < 15; attempt += 1) {
      const snapshot = (await send("zoom-snapshot")).snapshot;
      participants = participantsOf(snapshot);
      if (participants.length > 0) {
        await send("zoom-media-spine-sync", {
          spinePayload: buildSpinePayload(participants),
          elapsedMs: Date.now() - startedAt,
        });
        break;
      }
      await sleep(500);
    }
    if (participants.length === 0) throw new Error("fake engine did not present a video participant");
    await sleep(2000);  // audio/video subscriptions + first ring packets

    sceneAndAudioCommands = [
      {
        type: "load-scene-graph",
        sceneId: "record-audio-validation",
        routes: [{ routeId: "program", mode: "fixed", audioRole: "mix", participantId: participants[0].id }],
      },
      {
        type: "sync-audio-routing-matrix",
        sends: [
          { sourceId: "zoom-mix", busId: "master", gainDb: 0 },
          { sourceId: "zoom-mix", busId: "mon", gainDb: 0 },
          { sourceId: "zoom-mix", busId: "stream", gainDb: 0 },
        ],
      },
    ];
  }

  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [
      ...sceneAndAudioCommands,
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

    // Does the recording actually SHOW the program? (see programPixelVerdict)
    const ffmpegBin = findTool("ffmpeg");
    if (mediaLeg) {
      // Does the recording actually CARRY the clip's audio? (see mediaAudioVerdict)
      if (!ffmpegBin) {
        failures.push("--media needs ffmpeg to judge the recorded audio");
      } else {
        const sound = mediaAudioVerdict(artifactAbsolute, ffmpegBin, MEDIA_TONE_HZ);
        console.log(`media audio   : ${JSON.stringify(sound)}`);
        if (!sound.pass) {
          failures.push(
            `recorded audio does not carry the media clip ` +
            `(peak ${sound.peak ?? "n/a"}, rms ${sound.rms ?? "n/a"}, ${MEDIA_TONE_HZ} Hz share ${sound.toneShare ?? "n/a"}` +
            `${sound.reason ? `, ${sound.reason}` : ""}) — media PCM is not reaching the master bus`);
        }
      }
    }
    if (ffmpegBin) {
      const pixels = programPixelVerdict(artifactAbsolute, ffmpegBin);
      console.log(`pixels        : ${JSON.stringify(pixels)}`);
      if (!pixels.pass) {
        failures.push(
          `recorded program is black or confined to part of the frame ` +
          `(median luma ${pixels.medianLuma}, coverage ${pixels.medianCoverage}) — ` +
          `the recording is not showing the composed program`);
      }
    }
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
  if (mediaTempDir) {
    // The core may still hold the clip open for a moment after kill().
    for (let attempt = 0; attempt < 10 && existsSync(mediaTempDir); attempt += 1) {
      try { rmSync(mediaTempDir, { recursive: true, force: true }); } catch { await sleep(300); }
    }
  }
}
process.exit(failures.length === 0 ? 0 : 1);
