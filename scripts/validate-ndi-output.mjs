/** Independent NDI endpoint proof using NDI Tools' command-line recorder. */
import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, statSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const build = join(root, "native", "build-dev");
const core = join(build, "corevideo-native.exe");
const zoom = join(build, "corevideo-zoom-engine-fake.exe");
const toolsRoot = join(process.env.ProgramFiles ?? "C:\\Program Files", "NDI", "NDI 6 Tools");
const recorder = join(toolsRoot, "Studio Monitor", "Application.NDIRecording.x64.exe");
const runtime = join(toolsRoot, "Runtime");
const secondsIndex = process.argv.indexOf("--seconds");
const seconds = Number(secondsIndex >= 0 ? process.argv[secondsIndex + 1] : 24);
let sourceName = "COREVIDEO (Program)";
const outputDir = join(build, "Recordings", "CoreVideoPro", "validate-ndi-output");
const received = join(outputDir, `ndi-received-${Date.now()}.mov`);
const sleep = (ms) => new Promise((done) => setTimeout(done, ms));
async function discoverSources() {
  const python = spawn("python", ["-B", join(root, "scripts", "qa", "ndi-find.py"),
    "--runtime", join(runtime, "Processing.NDI.Lib.x64.dll"), "--seconds", "6"],
  { windowsHide: true, stdio: ["ignore", "pipe", "pipe"] });
  let output = "";
  let errors = "";
  python.stdout.on("data", (bytes) => { output += bytes.toString(); });
  python.stderr.on("data", (bytes) => { errors += bytes.toString(); });
  const code = await new Promise((done) => python.once("exit", done));
  if (code !== 0) throw new Error(`NDI discovery failed: ${errors}`);
  return JSON.parse(output);
}

for (const path of [core, zoom, recorder, join(runtime, "Processing.NDI.Lib.x64.dll")]) {
  if (!existsSync(path)) throw new Error(`NDI proof prerequisite missing: ${path}`);
}
if (!Number.isFinite(seconds) || seconds < 8 || seconds > 300) throw new Error("--seconds must be 8..300");
mkdirSync(outputDir, { recursive: true });

const native = spawn(core, [], {
  cwd: build, windowsHide: true,
  env: { ...process.env, COREVIDEO_ZOOM_ENGINE_PATH: zoom, COREVIDEO_FAKE_NO_CHURN: "1" },
  stdio: ["pipe", "pipe", "pipe"],
});
let nativeError = "";
native.stderr.on("data", (bytes) => { nativeError += bytes.toString(); });
let buffer = "";
let nextId = 0;
let handshake = false;
const pending = new Map();
native.stdout.on("data", (bytes) => {
  buffer += bytes.toString();
  let end;
  while ((end = buffer.indexOf("\n")) >= 0) {
    const line = buffer.slice(0, end).trim();
    buffer = buffer.slice(end + 1);
    let msg;
    try { msg = JSON.parse(line); } catch { continue; }
    if (msg.type === "handshake" && msg.ok) handshake = true;
    const waiter = pending.get(msg.id);
    if (waiter) { pending.delete(msg.id); clearTimeout(waiter.timer); waiter.resolve(msg); }
  }
});
function send(type, body = {}) {
  const id = `ndi-${++nextId}`;
  return new Promise((resolveReply, reject) => {
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`${type} timed out`)); }, 30000);
    pending.set(id, { resolve: resolveReply, timer });
    native.stdin.write(JSON.stringify({ id, type, ...body }) + "\n");
  }).then((msg) => {
    if (msg.ok === false) throw new Error(`${type}: ${msg.error?.message ?? "rejected"}`);
    return msg;
  });
}

let record;
let recordLog = "";
const started = Date.now();
let snapshot;
try {
  for (let i = 0; i < 200 && !handshake; i++) await sleep(50);
  if (!handshake) throw new Error("native handshake missing");
  const initial = await send("media-core-sync", { elapsedMs: 0, commands: [] });
  const ndiState = initial.snapshot?.profile?.capabilityStates?.["ndi-output"];
  console.log(`NDI capability: ${JSON.stringify(ndiState ?? "not in snapshot")}`);

  await send("zoom-join", { payload: { meetingNumber: "1234567890", displayName: "ndi-proof" } });
  await sleep(3000);
  await send("zoom-media-spine-sync", {
    elapsedMs: Date.now() - started,
    spinePayload: {
      readiness: { status: "ready", platform: "windows", sdkVersion: "fake-engine", checks: [], blockers: [], warnings: [] },
      participants: [{ sdkUserId: "101", displayName: "ndi-proof", role: "guest", videoOn: true,
        muted: false, talking: true, audioLevel: 60 }],
      subscriptions: [
        { participantId: "101", kind: "meeting-audio", purpose: "program", priority: 0 },
        { participantId: "101", kind: "participant-video", purpose: "program", priority: 10 },
      ],
      startCapture: true, blocked: false, warnings: [], summary: "NDI endpoint proof",
    },
  });
  await sleep(2000);
  await send("media-core-sync", {
    elapsedMs: Date.now() - started,
    commands: [
      { type: "load-scene-graph", sceneId: "ndi-proof",
        routes: [{ routeId: "program", mode: "fixed", audioRole: "mix", participantId: "101" }] },
      { type: "sync-audio-routing-matrix", sends: [
        { sourceId: "zoom-mix", busId: "master", gainDb: 0 },
        { sourceId: "zoom-mix", busId: "stream", gainDb: 0 },
      ] },
      { type: "start-program-output", destinations: ["ndi"], destinationSettings: [
        { id: "ndi", label: "NDI proof", protocol: "ndi", ndiName: sourceName,
          fps: 60, targetBitrateMbps: 125 },
      ], isoParticipantIds: [] },
    ],
  });
  await sleep(2000);
  const discovered = await discoverSources();
  console.log(`discovered: ${JSON.stringify(discovered)}`);
  sourceName = Object.keys(discovered).find((name) => /^[^()]+ \(Program\)$/.test(name));
  if (!sourceName) throw new Error("NDI Program source not discoverable under a single machine-qualified name");
  record = spawn(recorder, ["-i", sourceName, "-o", received, "-nothumbnail"], {
    cwd: dirname(recorder), windowsHide: true, stdio: ["pipe", "pipe", "pipe"],
  });
  record.stdout.on("data", (bytes) => { recordLog += bytes.toString(); });
  record.stderr.on("data", (bytes) => { recordLog += bytes.toString(); });
  console.log(`Recording ${sourceName} with independent NDI Tools receiver for ${seconds}s`);
  const deadline = Date.now() + seconds * 1000;
  while (Date.now() < deadline) {
    await sleep(Math.min(4000, Math.max(250, deadline - Date.now())));
    const reply = await send("media-core-sync", { elapsedMs: Date.now() - started, commands: [] });
    snapshot = reply.snapshot;
    const ndi = snapshot?.outputSenderSession?.senders?.find((s) => s.destination === "ndi");
    if (ndi) console.log(`sender: ${ndi.status}, frames=${ndi.framesSent}, runtime=${ndi.runtimeDetail || "?"}, warning=${ndi.warning || "none"}`);
  }
} finally {
  if (record && !record.killed) {
    record.stdin.write("<quit/>\n");
    await Promise.race([new Promise((done) => record.once("exit", done)), sleep(5000)]);
    if (record.exitCode === null) record.kill();
  }
  native.stdin.end();
  native.kill();
}

const size = existsSync(received) ? statSync(received).size : 0;
console.log(`receiver: ${received} (${size} bytes)`);
if (recordLog.trim()) console.log(`receiver log: ${recordLog.trim().slice(-1500)}`);
if (size < 10000) throw new Error(`independent NDI receiver captured no usable media; native=${nativeError.slice(-1200)}`);

const ffprobe = spawnSync("ffprobe", ["-v", "error", "-show_streams", "-of", "json", received],
  { encoding: "utf8", timeout: 30000 });
if (ffprobe.status !== 0) throw new Error(`ffprobe failed: ${ffprobe.stderr}`);
const streams = JSON.parse(ffprobe.stdout).streams ?? [];
const video = streams.find((s) => s.codec_type === "video");
const audio = streams.find((s) => s.codec_type === "audio");
console.log(`decoded: video=${video?.codec_name ?? "none"} ${video?.width ?? 0}x${video?.height ?? 0}; audio=${audio?.codec_name ?? "none"} ${audio?.sample_rate ?? 0}Hz`);
if (!video || video.width !== 1920 || video.height !== 1080 || !audio) {
  throw new Error("NDI endpoint did not receive full-resolution Program video and audio");
}
const volume = spawnSync("ffmpeg", ["-hide_banner", "-i", received, "-vn", "-af", "volumedetect", "-f", "null", "-"],
  { encoding: "utf8", timeout: 30000 });
if (volume.status !== 0) throw new Error(`NDI audio decode failed: ${volume.stderr}`);
const peakMatch = volume.stderr.match(/max_volume:\s*(-?\d+(?:\.\d+)?) dB/);
const peakDb = peakMatch ? Number(peakMatch[1]) : -Infinity;
console.log(`receiver audio peak: ${peakDb} dBFS`);
if (peakDb < -50) throw new Error(`independent NDI receiver audio is silent or too quiet (${peakDb} dBFS)`);
const ndi = snapshot?.outputSenderSession?.senders?.find((s) => s.destination === "ndi");
if (!ndi || ndi.status !== "live" || Number(ndi.framesSent) < seconds * 40) {
  throw new Error(`NDI sender did not deliver enough live frames: ${JSON.stringify(ndi)}`);
}
if (Number(ndi.audioFramesSent) < seconds * 48000 * 0.8) {
  throw new Error(`NDI sender did not deliver enough Program PCM: ${JSON.stringify(ndi)}`);
}
const receivedFrameCounts = [...recordLog.matchAll(/no_frames="(\d+)"/g)].map((match) => Number(match[1]));
const receivedFrames = Math.max(0, ...receivedFrameCounts);
console.log(`receiver video frames: ${receivedFrames}; sender audio frames: ${ndi.audioFramesSent}`);
if (receivedFrames < seconds * 50) throw new Error(`independent NDI receiver captured only ${receivedFrames} video frames`);
console.log("NDI independent endpoint PASS");
