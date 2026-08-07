/**
 * Headless SRT DELIVERY proof.
 *
 * Owner requirement 2026-08-06: SRT ingest and delivery are both mandatory for a
 * pro AV application. Delivery rides the shared FFmpeg sender (same process
 * pipeline as RTMP, MPEG-TS container, srt:// endpoint) because the staged
 * FFmpeg is built with libsrt.
 *
 * "The sender exists" proves nothing — the SRT output adapter previously
 * returned nullptr on BOTH sides of its #if and nobody noticed. This harness
 * therefore stands up a REAL SRT listener (ffmpeg, mode=listener) on loopback,
 * points the core's program output at it, and fails unless decodable video
 * actually lands in the receiver.
 *
 * Usage: node ./scripts/validate-srt-output.mjs [--seconds 15] [--port 9020]
 *                                               [--passphrase <10+ chars>] [--keep]
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
const seconds = Number(argValue("seconds", 15));
const port = Number(argValue("port", 9020));
const passphrase = argValue("passphrase", "");
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
  console.error("ffmpeg/ffprobe are required for the SRT delivery proof.");
  process.exit(1);
}
// A build without libsrt cannot carry SRT at all — say so rather than failing
// later with a confusing protocol error.
const protocols = spawnSync(ffmpeg, ["-hide_banner", "-protocols"], { encoding: "utf8", timeout: 15000 });
if (!/^\s*srt\s*$/m.test(protocols.stdout ?? "")) {
  console.error("This FFmpeg has no srt protocol (needs --enable-libsrt).");
  process.exit(1);
}
if (!existsSync(nativeCore) || !existsSync(fakeEngine)) {
  console.error(`Missing ${nativeCore} or ${fakeEngine}.`);
  process.exit(1);
}

const received = join(buildDir, `srt-received-${Date.now()}.ts`);
const listenerUrl =
  `srt://0.0.0.0:${port}?mode=listener&transtype=live` +
  (passphrase ? `&passphrase=${encodeURIComponent(passphrase)}&pbkeylen=16` : "");
const listener = spawn(ffmpeg,
  ["-hide_banner", "-loglevel", "error", "-y", "-i", listenerUrl,
   "-t", String(seconds + 8), "-c", "copy", "-f", "mpegts", received],
  { stdio: ["ignore", "ignore", "pipe"] });
let listenerStderr = "";
listener.stderr.on("data", (chunk) => { listenerStderr += chunk.toString(); });

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
child.stderr.on("data", () => {});

function send(type, payload = {}) {
  const id = `srt-${nextId++}`;
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

const failures = [];
let senderSnapshot = null;
try {
  for (let i = 0; i < 200 && !handshake; i += 1) await sleep(50);
  if (!handshake) throw new Error("no native-core handshake");

  await send("zoom-join", { payload: { meetingNumber: "1234567890", displayName: "srt-proof" } });
  await sleep(3000);

  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [
      {
        type: "load-scene-graph",
        sceneId: "srt-proof",
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
          label: "validate-srt-output",
          protocol: "srt",
          host: "127.0.0.1",
          port,
          mode: "caller",
          latencyMs: 120,
          passphrase,
          keyLength: passphrase ? 16 : 0,
          ffmpegBinDirectory: "C:\\ffmpeg\\bin",
          fps: 60,
          targetBitrateMbps: 4,
          encoderMode: "auto",
        }],
        isoParticipantIds: [],
      },
    ],
  });
  console.log(`Streaming     : srt://127.0.0.1:${port} (${passphrase ? "encrypted" : "clear"}) for ${seconds}s...`);

  const deadline = Date.now() + seconds * 1000;
  while (Date.now() < deadline) {
    await sleep(Math.min(5000, Math.max(1000, deadline - Date.now())));
    const sync = await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [] });
    const senders = sync.snapshot?.outputSenders?.senders ?? sync.snapshot?.outputSenderSession?.senders ?? [];
    senderSnapshot = senders.find((s) => (s.destination ?? s.senderId ?? "").includes("srt")) ?? senders[0] ?? null;
    if (senderSnapshot) {
      console.log(`sender        : status=${senderSnapshot.status} health=${senderSnapshot.destinationHealth ?? "?"} ` +
                  `frames=${senderSnapshot.framesSent ?? 0} warning=${senderSnapshot.warning || "none"}`);
    }
  }

  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{ type: "stop-program-output", reason: "srt proof complete" }],
  });
} catch (error) {
  failures.push(error.message);
} finally {
  try { child.stdin.end(); } catch {}
  child.kill();
}

// Give the listener a moment to flush, then judge on RECEIVED BYTES.
await sleep(3000);
try { listener.kill(); } catch {}
await sleep(1500);

let size = 0;
try { size = statSync(received).size; } catch {}
console.log(`received      : ${size} bytes at ${received}`);
if (size < 10000) {
  failures.push(`SRT receiver got ${size} bytes — no usable stream arrived` +
                (listenerStderr ? ` (listener: ${listenerStderr.trim().split("\n").pop()})` : ""));
} else {
  const probe = spawnSync(ffprobe,
    ["-v", "error", "-print_format", "json", "-show_streams", received],
    { encoding: "utf8", timeout: 20000 });
  let streams = [];
  try { streams = JSON.parse(probe.stdout).streams ?? []; } catch {}
  const video = streams.find((s) => s.codec_type === "video");
  console.log(`decoded       : ${video ? `${video.codec_name} ${video.width}x${video.height}` : "NO VIDEO STREAM"}`);
  if (!video) failures.push("received SRT stream carries no decodable video");
}

if (!keep) { try { rmSync(received); } catch {} }

if (failures.length) {
  console.error("\nSRT DELIVERY VALIDATION FAIL");
  for (const f of failures) console.error(`  - ${f}`);
  process.exit(1);
}
console.log("\nSRT DELIVERY VALIDATION PASS");
