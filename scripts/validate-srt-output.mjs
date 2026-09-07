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
// The rate the sender below is configured to declare, and therefore the rate the
// received stream must actually carry.
const TARGET_FPS = 60;
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
// listen_timeout is LOAD-BEARING (microseconds). The core needs several seconds
// to hand shake, join and arm the sender before it calls; without an explicit
// window the listener gives up first and the run reports "0 bytes — no usable
// stream arrived", which reads as a broken sender rather than a harness race.
const listenerUrl =
  `srt://0.0.0.0:${port}?mode=listener&transtype=live&listen_timeout=30000000` +
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

// Give the SRT listener a head start before the core calls it. Without one the
// caller can attempt first, fail, and sit in backoff longer than the whole run —
// which surfaces as "0 bytes — no usable stream arrived" and reads as a broken
// sender rather than a startup race (FFmpeg's own log said
// "Connection to srt://... failed: I/O error").
//
// DO NOT "improve" this by probing the port with a UDP bind. SRT is UDP, so a
// probe that binds to test availability STEALS the port from the very listener
// it is waiting for, and the listener then exits — turning an intermittent race
// into a reliable failure. Tried it; it made things worse.
async function awaitListenerHeadStart(listenerProcess, ms = 2500) {
  let exitedEarly = false;
  listenerProcess.once("exit", () => { exitedEarly = true; });
  await sleep(ms);
  return !exitedEarly;
}

const failures = [];
let senderSnapshot = null;
// Sender feed cadence, sampled from its accepted-frame counter (see below).
const senderFps = [];
let lastFrameSample = null;
try {
  for (let i = 0; i < 200 && !handshake; i += 1) await sleep(50);
  if (!handshake) throw new Error("no native-core handshake");

  if (!(await awaitListenerHeadStart(listener))) {
    throw new Error(
      `SRT listener exited before the stream started — nothing could have connected` +
      (listenerStderr ? ` (${listenerStderr.trim().split("\n").pop()})` : ""));
  }
  console.log(`listener      : up on ${port}`);

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
      // Sample the sender's OWN accepted-frame counter. This — not the received
      // container's frame rate — is what reveals the feed cadence: FFmpeg pads
      // duplicates up to its declared -r, so a sender fed at 50fps still emits a
      // stream that reads 60fps. Measured on a 50Hz feed: ~250 frames per 5s
      // interval; on a 60Hz feed: ~301.
      const now = Date.now();
      const frames = Number(senderSnapshot.framesSent ?? 0);
      if (lastFrameSample && frames > lastFrameSample.frames) {
        senderFps.push((frames - lastFrameSample.frames) / ((now - lastFrameSample.at) / 1000));
      }
      lastFrameSample = { frames, at: now };
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

  // Container rate, reported but NOT gated — see the sender-cadence gate below
  // for why this number cannot catch the defect it looks like it catches.
  if (video) {
    // TWO probes, deliberately: combining -show_entries for a stream and for the
    // format emits bare values with no keys, and reading them positionally
    // silently produced "1046 frames / 1046.00s" (the same number twice).
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
    } else {
      console.log("received rate : not measurable (short or unseekable capture)");
    }
  }

  // AUDIO SURVIVED THE SPLIT. Video and audio reach FFmpeg through two separate
  // inputs and are now pushed on two different cadences (video on the 60Hz video
  // tick, PCM on the ~50Hz audio worker). A stream that carries perfect video and
  // silence is the exact failure that decoupling risks, so judge on SAMPLES, not
  // on the presence of an audio track.
  const audio = streams.find((s) => s.codec_type === "audio");
  console.log(`audio track   : ${audio ? `${audio.codec_name} ${audio.sample_rate}Hz ${audio.channels}ch` : "NONE"}`);
  if (!audio) {
    failures.push("received SRT stream carries no audio track — the program audio never reached the sender");
  } else {
    const pcm = spawnSync(ffmpeg,
      ["-v", "error", "-i", received, "-f", "s16le", "-ac", "1", "-ar", "48000", "-"],
      { encoding: "buffer", maxBuffer: 1 << 28, timeout: 120000 });
    let peak = 0;
    const samples = (pcm.stdout?.length ?? 0) >> 1;
    for (let i = 0; i < samples; i += 1) {
      peak = Math.max(peak, Math.abs(pcm.stdout.readInt16LE(i * 2)));
    }
    console.log(`audio signal  : peak ${peak} over ${samples} samples`);
    if (samples === 0) failures.push("received audio track decodes to nothing");
    else if (peak < 500) failures.push(`received audio is silent (peak ${peak}) — the tone never made it out`);
  }
}

// THE CADENCE GATE. The senders were fed by the ~50Hz audio worker, so the
// program left this app at 50fps while the compositor produced 60. That is
// invisible downstream: FFmpeg pads duplicates up to its declared -r, so the
// received stream reads ~60fps either way (measured 59.8fps BEFORE the fix).
// The sender's own accepted-frame counter is what exposes it — ~250 frames per
// 5s interval on the old path, ~301 on the video cadence.
if (senderFps.length) {
  const sorted = [...senderFps].sort((a, b) => a - b);
  const median = sorted[Math.floor(sorted.length / 2)];
  const best = sorted[sorted.length - 1];
  console.log(`sender feed   : ${median.toFixed(1)}fps median, ${best.toFixed(1)}fps best ` +
              `of ${TARGET_FPS} (${senderFps.length} intervals)`);
  // Judge on the BEST interval, not the median. The defect under test is a
  // STRUCTURAL cap — video fed from the ~50Hz audio worker can never exceed ~50
  // on any interval. A busy machine makes the async sender coalesce frames and
  // dip (46-53fps observed on a dev box mid-build), which a median-based gate
  // reports as the same failure. The peak separates "capped" from "loaded".
  if (best < TARGET_FPS * 0.92) {
    failures.push(
      `the sender never exceeded ${best.toFixed(1)}fps while the program is composited ` +
      `at ${TARGET_FPS} — that is a cadence cap, not load: the stream carries ` +
      `duplicated frames rather than ${TARGET_FPS}fps of motion`);
  }
} else {
  console.log("sender feed   : not measurable (no frame-counter samples)");
}

if (!keep) { try { rmSync(received); } catch {} }

if (failures.length) {
  console.error("\nSRT DELIVERY VALIDATION FAIL");
  for (const f of failures) console.error(`  - ${f}`);
  process.exit(1);
}
console.log("\nSRT DELIVERY VALIDATION PASS");
