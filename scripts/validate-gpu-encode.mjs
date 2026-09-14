/**
 * GPU-direct encode acceptance gate (#521 slice 1).
 *
 * The whole point of the slice: feed the hardware H.264 encoder from the
 * compositor's GPU texture so 1080p60 streaming runs at realtime instead of the
 * ~0.76x the GPU->CPU-readback->raw-pipe->ffmpeg path capped at. This harness
 * stands up a REAL localhost SRT sink, points the core's program output at it,
 * and fails unless the received stream actually keeps up with realtime.
 *
 * SRT, not RTMP, for the sink: the GPU-direct path is protocol-agnostic (same
 * sender, same -c:v copy bitstream muxer — only the endpoint/container differ),
 * and ffmpeg's `-listen 1` RTMP server is too flaky to gate CI on, while an SRT
 * listener is reliable (see validate-srt-output.mjs). Live RTMP to YouTube is the
 * final manual acceptance step, not this automated gate.
 *
 * Why the received stream and not the sender's own counter: on the GPU path
 * submit() is non-blocking (newest-wins), so the sender's framesSent grows at the
 * paced rate whether or not the pipeline keeps up. The received stream cannot lie
 * — with `-c:v copy` the muxer copies exactly what the encoder produced, and the
 * sink's own `-stats speed` is media-time / wall-time, i.e. the realtime ratio.
 *
 * Gate (GPU path): received fps >= 58 AND sink speed >= 0.97x over the window,
 * AND the core logged `[gpu-encode] path=gpu-direct`.
 * `--force-raw` sets COREVIDEO_GPU_ENCODE=0 and asserts the raw fallback still
 * streams (documents the A/B), without the realtime gate.
 *
 * Usage: node ./scripts/validate-gpu-encode.mjs [--seconds 30] [--port 1935]
 *                                               [--fps 60] [--bitrate 6]
 *                                               [--force-raw] [--keep]
 */
import { spawn, spawnSync } from "node:child_process";
import { existsSync, rmSync, statSync, readFileSync } from "node:fs";
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
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] ? args[i + 1] : fallback;
};
const seconds = Number(argValue("seconds", 30));
const port = Number(argValue("port", 9021));
const TARGET_FPS = Number(argValue("fps", 60));
const bitrate = Number(argValue("bitrate", 6));
const forceRaw = args.includes("--force-raw");
const keep = args.includes("--keep");

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
  console.error("ffmpeg/ffprobe are required for the GPU-direct encode gate.");
  process.exit(1);
}
if (!existsSync(nativeCore) || !existsSync(fakeEngine)) {
  console.error(`Missing ${nativeCore} or ${fakeEngine}. Build --config Release first.`);
  process.exit(1);
}

const received = join(buildDir, `gpu-encode-received-${Date.now()}.ts`);
// SRT listener sink. -stats gives us the realtime ratio (media-time/wall-time) of
// what actually arrives; -c copy means no re-encode padding to hide a lag.
// listen_timeout (microseconds) must exceed the core's join+arm time or the
// listener gives up before the caller connects.
const listenerUrl = `srt://0.0.0.0:${port}?mode=listener&transtype=live&listen_timeout=30000000`;
const listener = spawn(ffmpeg,
  ["-hide_banner", "-loglevel", "warning", "-stats", "-stats_period", "1", "-y",
   "-fflags", "+genpts", "-i", listenerUrl, "-t", String(seconds + 8), "-c", "copy", "-f", "mpegts", received],
  { stdio: ["ignore", "ignore", "pipe"] });
let listenerStderr = "";
listener.stderr.on("data", (c) => { listenerStderr += c.toString(); });
let listenerExited = false;
listener.once("exit", () => { listenerExited = true; });

const child = spawn(nativeCore, [], {
  cwd: buildDir,
  env: {
    ...process.env,
    COREVIDEO_ZOOM_ENGINE_PATH: fakeEngine,
    COREVIDEO_FAKE_NO_CHURN: "1",
    // Pin the source rate: an unpinned fake engine delivers 30fps, which would
    // starve the 60fps gate and read as a pipeline failure (CLAUDE.md rule).
    COREVIDEO_FAKE_ENGINE_FPS: String(TARGET_FPS),
    ...(forceRaw ? { COREVIDEO_GPU_ENCODE: "0" } : {}),
  },
  stdio: ["pipe", "pipe", "pipe"],
});

const startedAt = Date.now();
let nextId = 1;
let stdoutBuffer = "";
let handshake;
const pending = new Map();
let coreStderr = "";
child.stderr.on("data", (c) => { coreStderr += c.toString(); });

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

function send(type, payload = {}) {
  const id = `gpu-${nextId++}`;
  return new Promise((res, rej) => {
    const timer = setTimeout(() => { pending.delete(id); rej(new Error(`${type} timed out`)); }, 30000);
    pending.set(id, { resolve: res, reject: rej, timer });
    child.stdin.write(`${JSON.stringify({ id, type, ...payload })}\n`);
  }).then((r) => {
    if (r.ok === false) throw new Error(`${type} failed: ${r.error?.message ?? "unknown"}`);
    return r;
  });
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// The core logs the chosen path via nativeLogf; capture it from stderr and, as a
// fallback, media-core.log next to the binary.
function gpuEncodePathLine() {
  const haystacks = [coreStderr];
  const logPath = join(buildDir, "media-core.log");
  if (existsSync(logPath)) { try { haystacks.push(readFileSync(logPath, "utf8")); } catch {} }
  for (const text of haystacks) {
    const m = text.match(/\[gpu-encode\] path=(gpu-direct|cpu-fallback)[^\n]*/g);
    if (m && m.length) return m[m.length - 1];
  }
  return null;
}

const failures = [];
const senderFps = [];
let lastFrameSample = null;
let senderSnapshot = null;
try {
  for (let i = 0; i < 200 && !handshake; i += 1) await sleep(50);
  if (!handshake) throw new Error("no native-core handshake");

  await sleep(2500);  // RTMP listener head start before the caller connects
  if (listenerExited) {
    throw new Error(`RTMP listener exited before streaming started (${listenerStderr.trim().split("\n").pop() || "no output"})`);
  }
  console.log(`listener      : up on srt://127.0.0.1:${port}`);

  await send("zoom-join", { payload: { meetingNumber: "1234567890", displayName: "gpu-encode-proof" } });
  await sleep(3000);

  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [
      {
        type: "load-scene-graph",
        sceneId: "gpu-encode-proof",
        routes: [{ routeId: "program", mode: "fixed", audioRole: "mix", participantId: "101" }],
      },
      {
        type: "sync-audio-routing-matrix",
        sends: [{ sourceId: "zoom-mix", busId: "master", gainDb: 0 },
                { sourceId: "zoom-mix", busId: "stream", gainDb: 0 }],
      },
      {
        type: "start-program-output",
        destinations: ["srt"],
        destinationSettings: [{
          id: "srt",
          label: "validate-gpu-encode",
          protocol: "srt",
          host: "127.0.0.1",
          port,
          mode: "caller",
          latencyMs: 120,
          fps: TARGET_FPS,
          targetBitrateMbps: bitrate,
          encoderMode: "auto",
          ffmpegBinDirectory: "C:\\ffmpeg\\bin",
        }],
        isoParticipantIds: [],
      },
    ],
  });
  console.log(`streaming     : srt://127.0.0.1:${port} for ${seconds}s (${forceRaw ? "COREVIDEO_GPU_ENCODE=0" : "GPU-direct"})...`);

  const deadline = Date.now() + seconds * 1000;
  while (Date.now() < deadline) {
    await sleep(Math.min(5000, Math.max(1000, deadline - Date.now())));
    const sync = await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [] });
    const senders = sync.snapshot?.outputSenders?.senders ?? sync.snapshot?.outputSenderSession?.senders ?? [];
    senderSnapshot = senders.find((s) => (s.destination ?? s.senderId ?? "").includes("srt")) ?? senders[0] ?? null;
    if (senderSnapshot) {
      const frames = Number(senderSnapshot.framesSent ?? 0);
      const now = Date.now();
      if (lastFrameSample && frames > lastFrameSample.frames) {
        senderFps.push((frames - lastFrameSample.frames) / ((now - lastFrameSample.at) / 1000));
      }
      lastFrameSample = { frames, at: now };
      console.log(`sender        : status=${senderSnapshot.status} health=${senderSnapshot.destinationHealth ?? "?"} ` +
                  `frames=${frames} warning=${senderSnapshot.warning || "none"}`);
    }
  }

  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{ type: "stop-program-output", reason: "gpu-encode proof complete" }],
  });
} catch (error) {
  failures.push(error.message);
} finally {
  try { child.stdin.end(); } catch {}
  child.kill();
}

await sleep(3000);
try { listener.kill(); } catch {}
await sleep(1500);

// Dump the core's own stderr for diagnosis (kept only with --keep).
if (keep) {
  const dump = join(buildDir, `gpu-encode-core-stderr-${startedAt}.txt`);
  try { (await import("node:fs")).writeFileSync(dump, coreStderr); console.log(`core stderr   : ${dump}`); } catch {}
}

// Which path did the core take?
const pathLine = gpuEncodePathLine();
console.log(`encode path   : ${pathLine || "UNKNOWN (no [gpu-encode] path= line found)"}`);
const tookGpuDirect = !!pathLine && pathLine.includes("path=gpu-direct");
if (forceRaw) {
  if (tookGpuDirect) failures.push("COREVIDEO_GPU_ENCODE=0 but the core still took the GPU-direct path");
} else if (!tookGpuDirect) {
  failures.push(`expected GPU-direct but the core reported: ${pathLine || "no path line"}`);
}

// The received stream cannot lie.
let size = 0;
try { size = statSync(received).size; } catch {}
console.log(`received      : ${size} bytes at ${received}`);
if (size < 10000) {
  failures.push(`RTMP sink got ${size} bytes — no usable stream arrived` +
                (listenerStderr ? ` (${listenerStderr.trim().split("\n").pop()})` : ""));
} else {
  const probe = spawnSync(ffprobe, ["-v", "error", "-print_format", "json", "-show_streams", received],
    { encoding: "utf8", timeout: 20000 });
  let streams = [];
  try { streams = JSON.parse(probe.stdout).streams ?? []; } catch {}
  const video = streams.find((s) => s.codec_type === "video");
  console.log(`decoded       : ${video ? `${video.codec_name} ${video.width}x${video.height}` : "NO VIDEO STREAM"}`);
  if (!video) failures.push("received stream carries no decodable video");

  if (video) {
    const countOut = spawnSync(ffprobe,
      ["-v", "error", "-select_streams", "v:0", "-count_frames",
       "-show_entries", "stream=nb_read_frames", "-of", "default=nw=1:nk=1", received],
      { encoding: "utf8", timeout: 60000 });
    const durationOut = spawnSync(ffprobe,
      ["-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1", received],
      { encoding: "utf8", timeout: 60000 });
    const frames = Number((countOut.stdout ?? "").trim().split(/\s+/)[0]);
    const durationSec = Number((durationOut.stdout ?? "").trim().split(/\s+/)[0]);
    if (frames > 0 && durationSec > 1) {
      const fps = frames / durationSec;
      console.log(`received rate : ${fps.toFixed(1)}fps of ${TARGET_FPS} (${frames} frames / ${durationSec.toFixed(2)}s)`);
      if (!forceRaw && fps < TARGET_FPS * 0.97) {
        failures.push(`received ${fps.toFixed(1)}fps < ${(TARGET_FPS * 0.97).toFixed(1)} (0.97x of ${TARGET_FPS}) — the GPU path is not keeping up with realtime`);
      }
    } else {
      console.log("received rate : not measurable (short or unseekable capture)");
      if (!forceRaw) failures.push("could not measure received frame rate");
    }
  }
}

// The sink's own realtime ratio.
const speeds = [...listenerStderr.matchAll(/speed=\s*([0-9.]+)x/g)].map((m) => Number(m[1])).filter((n) => n > 0);
if (speeds.length) {
  const sorted = [...speeds].sort((a, b) => a - b);
  const median = sorted[Math.floor(sorted.length / 2)];
  const last = speeds[speeds.length - 1];
  console.log(`sink speed    : ${median.toFixed(2)}x median, ${last.toFixed(2)}x last (${speeds.length} samples)`);
  if (!forceRaw && median < 0.97) {
    failures.push(`sink speed ${median.toFixed(2)}x < 0.97x — the pipeline is behind realtime`);
  }
} else {
  console.log("sink speed    : not sampled");
}

if (senderFps.length) {
  const best = Math.max(...senderFps);
  console.log(`sender feed   : ${best.toFixed(1)}fps best of ${TARGET_FPS} (${senderFps.length} intervals; reported, not gated)`);
}

if (!keep) { try { rmSync(received); } catch {} }

if (failures.length) {
  console.error(`\nGPU-DIRECT ENCODE GATE FAIL${forceRaw ? " (--force-raw)" : ""}`);
  for (const f of failures) console.error(`  - ${f}`);
  process.exit(1);
}
console.log(`\nGPU-DIRECT ENCODE GATE PASS${forceRaw ? " (raw fallback)" : ""}`);
