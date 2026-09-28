/**
 * #538 HLS Program push proof. Receives HTTP PUT playlists and segments from
 * the product sender, then independently decodes the received files.
 *
 * node scripts/validate-hls-output.mjs --seconds 24
 */
import { spawn, spawnSync } from "node:child_process";
import { createServer } from "node:http";
import { existsSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const buildDir = resolve(process.env.COREVIDEO_NATIVE_BUILD_DIR || join(root, "native", "build-dev"));
const suffix = process.platform === "win32" ? ".exe" : "";
const coreExe = join(buildDir, `corevideo-native${suffix}`);
const fakeEngine = join(buildDir, `corevideo-zoom-engine-fake${suffix}`);
const secondsArg = process.argv.indexOf("--seconds");
const seconds = secondsArg >= 0 ? Number(process.argv[secondsArg + 1]) : 24;
if (!Number.isFinite(seconds) || seconds < 8 || seconds > 600) throw new Error("--seconds must be 8–600");
if (!existsSync(coreExe) || !existsSync(fakeEngine)) throw new Error("Build the Release native core and fake engine first");

const ffprobe = process.platform === "win32" ? "ffprobe.exe" : "ffprobe";
const ffmpeg = process.platform === "win32" ? "ffmpeg.exe" : "ffmpeg";
for (const exe of [ffprobe, ffmpeg]) {
  if (spawnSync(exe, ["-version"], { encoding: "utf8" }).status !== 0) throw new Error(`${exe} is required`);
}

const artifactDir = mkdtempSync(join(tmpdir(), "corevideo-hls-output-"));
const received = new Map();
const origin = createServer(async (request, response) => {
  const path = new URL(request.url || "/", "http://localhost").pathname;
  if (!path.startsWith("/live/") || path.split("/").length !== 3) {
    response.writeHead(404).end();
    return;
  }
  if (request.method === "PUT") {
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    const body = Buffer.concat(chunks);
    writeFileSync(join(artifactDir, basename(path)), body);
    received.set(path, body.length);
    response.writeHead(201).end();
  } else if (request.method === "GET" && received.has(path)) {
    response.writeHead(200, { "Content-Type": path.endsWith(".m3u8") ? "application/vnd.apple.mpegurl" : "video/mp2t" });
    response.end(readFileSync(join(artifactDir, basename(path))));
  } else {
    response.writeHead(404).end();
  }
});
await new Promise((resolveListen) => origin.listen(0, "127.0.0.1", resolveListen));
const port = origin.address().port;
const playlistUrl = `http://127.0.0.1:${port}/live/program.m3u8`;

const child = spawn(coreExe, [], {
  cwd: buildDir,
  env: { ...process.env, COREVIDEO_ZOOM_ENGINE_PATH: fakeEngine, COREVIDEO_FAKE_NO_CHURN: "1" },
  stdio: ["pipe", "pipe", "pipe"],
});
const startedAt = Date.now();
let handshake = false;
let nextId = 1;
let stdoutBuffer = "";
let stderrTail = "";
const pending = new Map();
child.stdout.on("data", (chunk) => {
  stdoutBuffer += chunk.toString();
  let end;
  while ((end = stdoutBuffer.indexOf("\n")) >= 0) {
    const line = stdoutBuffer.slice(0, end).trim();
    stdoutBuffer = stdoutBuffer.slice(end + 1);
    let value;
    try { value = JSON.parse(line); } catch { continue; }
    if (value.type === "handshake" && value.ok === true) handshake = true;
    if (typeof value.id === "string" && pending.has(value.id)) {
      const request = pending.get(value.id);
      clearTimeout(request.timer);
      pending.delete(value.id);
      request.resolve(value);
    }
  }
});
child.stderr.on("data", (chunk) => { stderrTail = (stderrTail + chunk.toString()).slice(-4096); });
const sleep = (ms) => new Promise((resolveSleep) => setTimeout(resolveSleep, ms));
function send(type, payload = {}) {
  const id = `hls-${nextId++}`;
  return new Promise((resolveResponse, reject) => {
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`${type} timed out`)); }, 30000);
    pending.set(id, { resolve: resolveResponse, timer });
    child.stdin.write(`${JSON.stringify({ id, type, ...payload })}\n`);
  }).then((response) => {
    if (response.ok === false) throw new Error(`${type} failed: ${response.error?.message || "unknown"}`);
    return response;
  });
}

const failures = [];
let sender;
let peakFrames = 0;
try {
  for (let i = 0; i < 200 && !handshake; i++) await sleep(50);
  if (!handshake) throw new Error("Native core did not handshake");
  await send("zoom-join", { payload: { meetingNumber: "1234567890", displayName: "hls-proof" } });
  await sleep(3000);
  await send("zoom-media-spine-sync", {
    elapsedMs: Date.now() - startedAt,
    spinePayload: {
      readiness: { status: "ready", platform: "windows", sdkVersion: "fake-engine", checks: [], blockers: [], warnings: [] },
      participants: [{ sdkUserId: "101", displayName: "hls-proof", role: "guest", videoOn: true, muted: false, talking: true, audioLevel: 60 }],
      subscriptions: [
        { participantId: "101", kind: "meeting-audio", purpose: "program", priority: 0 },
        { participantId: "101", kind: "participant-video", purpose: "program", priority: 10 },
      ],
      startCapture: true, blocked: false, warnings: [], summary: "HLS delivery proof subscription",
    },
  });
  await sleep(2000);
  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [
      { type: "load-scene-graph", sceneId: "hls-proof", routes: [{ routeId: "program", mode: "fixed", audioRole: "mix", participantId: "101" }] },
      { type: "sync-audio-routing-matrix", sends: [
        { sourceId: "zoom-mix", busId: "master", gainDb: 0 },
        { sourceId: "zoom-mix", busId: "stream", gainDb: 0 },
      ] },
      { type: "start-program-output", destinations: ["hls"], destinationSettings: [{
        id: "hls", label: "validate-hls-output", protocol: "hls", url: playlistUrl,
        fps: 60, targetBitrateMbps: 4, audioBitrateKbps: 160,
        videoCodec: "h264", encoderMode: "auto", keyframeIntervalSeconds: 2,
      }], isoParticipantIds: [] },
    ],
  });

  const deadline = Date.now() + seconds * 1000;
  while (Date.now() < deadline) {
    await sleep(Math.min(4000, Math.max(250, deadline - Date.now())));
    const snapshot = (await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [] })).snapshot;
    sender = (snapshot?.outputSenders?.senders || snapshot?.outputSenderSession?.senders || [])
      .find((item) => item.destination === "hls");
    peakFrames = Math.max(peakFrames, Number(sender?.framesSent || 0));
    console.log(`hls sender: ${sender?.status || "absent"}, frames=${sender?.framesSent || 0}, ` +
                `playlist PUT=${received.has("/live/program.m3u8")}, segments=${[...received.keys()].filter((key) => key.endsWith(".ts")).length}`);
  }
  await send("media-core-sync", { elapsedMs: Date.now() - startedAt,
    commands: [{ type: "stop-program-output", reason: "HLS proof complete" }] });
} catch (error) {
  failures.push(error.message);
} finally {
  child.stdin.end();
  child.kill();
}

await sleep(1500);
const playlistPath = join(artifactDir, "program.m3u8");
const segments = [...received.keys()].filter((key) => key.endsWith(".ts"));
if (!existsSync(playlistPath)) failures.push("The HTTP origin received no playlist PUT");
if (segments.length < 2) failures.push(`The HTTP origin received only ${segments.length} segment PUTs`);
if (peakFrames < seconds * 15) failures.push(`Sender accepted only ${peakFrames} Program frames`);
if (sender?.status === "failed") failures.push(`HLS sender failed: ${sender.warning || sender.lastError || "unknown"}`);

if (existsSync(playlistPath) && segments.length >= 2) {
  // The HTTP receiver wrote exactly the bytes PUT by FFmpeg. Rebase manifest
  // entries onto those files for an independent local ffprobe/decoder check.
  const playlist = readFileSync(playlistPath, "utf8");
  const listedSegments = playlist.split(/\r?\n/).filter((line) => line.endsWith(".ts"));
  if (!playlist.startsWith("#EXTM3U")) failures.push("Received playlist is not M3U8");
  if (listedSegments.length < 2 || listedSegments.length > 6) {
    failures.push(`Received live playlist lists ${listedSegments.length} segments; expected 2–6`);
  }
  if (playlist.includes("#EXT-X-ENDLIST")) failures.push("HLS playlist ended while the live output was armed");
  for (const line of listedSegments) {
    if (!received.has(`/live/${basename(line)}`)) failures.push(`Playlist references missing segment ${line}`);
  }
  const localPlaylist = join(artifactDir, "received-local.m3u8");
  writeFileSync(localPlaylist, playlist.replaceAll(`http://127.0.0.1:${port}/live/`, ""));
  const probe = spawnSync(ffprobe, ["-v", "error", "-print_format", "json", "-show_streams", localPlaylist],
    { encoding: "utf8", timeout: 30000 });
  let streams = [];
  try { streams = JSON.parse(probe.stdout).streams || []; } catch {}
  const video = streams.find((stream) => stream.codec_type === "video");
  const audio = streams.find((stream) => stream.codec_type === "audio");
  console.log(`received: ${video?.codec_name || "NO VIDEO"} ${video?.width || 0}x${video?.height || 0}, ` +
              `${audio?.codec_name || "NO AUDIO"}, ${segments.length} segments`);
  if (!video || video.codec_name !== "h264") failures.push("Received HLS has no decodable H.264 video");
  if (!audio || audio.codec_name !== "aac") failures.push("Received HLS has no decodable AAC audio");
  if (audio) {
    const decode = spawnSync(ffmpeg, ["-v", "error", "-i", localPlaylist, "-t", "5", "-f", "s16le", "-ac", "1", "-ar", "48000", "-"],
      { encoding: "buffer", timeout: 30000, maxBuffer: 1 << 25 });
    let peak = 0;
    for (let i = 0; i + 1 < (decode.stdout?.length || 0); i += 2) {
      peak = Math.max(peak, Math.abs(decode.stdout.readInt16LE(i)));
    }
    console.log(`audio peak: ${peak}`);
    if (peak < 500) failures.push("Received HLS audio is silent");
  }
}
origin.close();
console.log(`artifacts: ${artifactDir}`);
if (failures.length) {
  console.error("HLS DELIVERY FAIL");
  failures.forEach((failure) => console.error(`  - ${failure}`));
  if (stderrTail) console.error(`core stderr tail: ${stderrTail.slice(-500)}`);
  process.exitCode = 1;
} else {
  console.log("HLS DELIVERY PASS");
}
