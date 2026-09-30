// #538: real Windows Program encode feeding two independent copy muxers.
// A destination failure must not tear down the shared encoder or the survivor.
import { spawn, spawnSync } from "node:child_process";
import { createServer } from "node:http";
import { createServer as createTcpServer, connect } from "node:net";
import { existsSync, mkdirSync, statSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(fileURLToPath(new URL("..", import.meta.url)));
const build = join(root, "native", "build-dev");
const ffmpeg = existsSync("C:/ffmpeg/bin/ffmpeg.exe") ? "C:/ffmpeg/bin/ffmpeg.exe" : "ffmpeg";
const ffprobe = existsSync("C:/ffmpeg/bin/ffprobe.exe") ? "C:/ffmpeg/bin/ffprobe.exe" : "ffprobe";
const port = Number(process.env.COREVIDEO_SHARED_STREAM_PORT || 19091);
const seconds = Number(process.argv.find((x) => x.startsWith("--seconds="))?.split("=")[1] || 30);
const blockRtmp = process.argv.includes("--block-rtmp");
const reconnectRtmp = process.argv.includes("--reconnect-rtmp");
if (blockRtmp && reconnectRtmp) throw new Error("choose one RTMP fault mode");
const output = join(build, `shared-stream-${Date.now()}`);
mkdirSync(output, { recursive: true });
const hlsReceived = new Map();
const hlsOrigin = createServer(async (request, response) => {
  const filename = new URL(request.url, "http://localhost").pathname.split("/").pop();
  if (request.method !== "PUT" || !filename || !/^[a-zA-Z0-9_.-]+$/.test(filename)) {
    response.writeHead(404).end();
    return;
  }
  const parts = [];
  for await (const part of request) parts.push(part);
  const body = Buffer.concat(parts);
  writeFileSync(join(output, filename), body);
  hlsReceived.set(filename, body.length);
  response.writeHead(201).end();
});
await new Promise((resolve) => hlsOrigin.listen(port + 2, "127.0.0.1", resolve));
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const receiver = (kind, args, file) => {
  const p = spawn(ffmpeg, ["-hide_banner", "-loglevel", "warning", "-y", "-fflags", "+genpts",
    ...args, "-t", String(seconds + 10), "-c", "copy", "-f", kind, file],
  { stdio: ["ignore", "ignore", "pipe"] });
  let stderr = "";
  p.stderr.on("data", (data) => { stderr += data.toString(); });
  return { p, get stderr() { return stderr; } };
};
const rtmpFile = join(output, "rtmp.flv");
const reconnectedRtmpFile = join(output, "rtmp-reconnected.flv");
const srtFile = join(output, "srt.ts");
let rtmp = receiver("flv", ["-listen", "1", "-i",
  `rtmp://127.0.0.1:${blockRtmp ? port + 3 : port}/live/gate`], rtmpFile);
const srt = receiver("mpegts", ["-i", `srt://0.0.0.0:${port + 1}?mode=listener&transtype=live&listen_timeout=45000000`], srtFile);
const proxyClients = new Set();
let rtmpProxy;
if (blockRtmp) {
  rtmpProxy = createTcpServer((client) => {
    const upstream = connect(port + 3, "127.0.0.1");
    proxyClients.add(client);
    client.pipe(upstream);
    upstream.pipe(client);
    const close = () => { proxyClients.delete(client); client.destroy(); upstream.destroy(); };
    client.on("error", close); upstream.on("error", close);
    client.on("close", close); upstream.on("close", close);
  });
  await new Promise((resolve) => rtmpProxy.listen(port, "127.0.0.1", resolve));
}
const core = spawn(join(build, "corevideo-native.exe"), [], {
  cwd: build, stdio: ["pipe", "pipe", "pipe"], env: {
    ...process.env,
    COREVIDEO_ZOOM_ENGINE_PATH: join(build, "corevideo-zoom-engine-fake.exe"),
    COREVIDEO_FAKE_NO_CHURN: "1", COREVIDEO_FAKE_ENGINE_FPS: "60",
  },
});
let stderr = "", stdout = "", handshake = false, nextId = 1;
const pending = new Map();
core.stderr.on("data", (data) => { stderr += data.toString(); });
core.stdout.on("data", (data) => {
  stdout += data.toString();
  let cut;
  while ((cut = stdout.indexOf("\n")) >= 0) {
    const line = stdout.slice(0, cut).trim();
    stdout = stdout.slice(cut + 1);
    let value;
    try { value = JSON.parse(line); } catch { continue; }
    if (value.type === "handshake" && value.ok) handshake = true;
    const wait = pending.get(value.id);
    if (wait) { pending.delete(value.id); clearTimeout(wait.timer); wait.resolve(value); }
  }
});
function send(type, body = {}) {
  const id = `shared-${nextId++}`;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`${type} timed out`)); }, 30000);
    pending.set(id, { resolve, reject, timer });
    core.stdin.write(`${JSON.stringify({ id, type, ...body })}\n`);
  }).then((value) => {
    if (value.ok === false) throw new Error(`${type}: ${value.error?.message || "failed"}`);
    return value;
  });
}
const sender = (snapshot, id) => (snapshot?.outputSenderSession?.senders || [])
  .find((item) => (item.destination || item.senderId || "").includes(id));
const failures = [];
let firstSrtFrames = 0, lastSrtFrames = 0, firstRtmpFrames = 0;
let firstHlsFrames = 0, lastHlsFrames = 0;
let firstSrtBytes = 0, firstHlsBytes = 0, firstSenderStates = {}, lastSenderStates = {};
let firstRenderSlots = 0, lastRenderSlots = 0, firstAt = 0, lastAt = 0;
let firstExportDivisor = -1, lastExportDivisor = -1, proxyConnections = 0;
const bytes = (path) => existsSync(path) ? statSync(path).size : 0;
const hlsBytes = () => [...hlsReceived.values()].reduce((sum, size) => sum + size, 0);
const senderState = (snapshot, id) => {
  const item = sender(snapshot, id);
  return { status: item?.status || "absent", health: item?.destinationHealth || "unknown",
    result: item?.lastResultCode || "", warning: item?.warning || "",
    muxInputVideo: item?.muxInputVideo ?? null };
};
try {
  for (let i = 0; i < 200 && !handshake; i++) await sleep(50);
  if (!handshake) throw new Error("core handshake missing");
  await sleep(2000);
  await send("zoom-join", { payload: { meetingNumber: "1234567890", displayName: "shared-encode-proof" } });
  await sleep(3000);
  const settings = [
    { id: "rtmp", label: "shared-encode-rtmp", protocol: "rtmp",
      url: `rtmp://127.0.0.1:${port}/live`, streamKey: "gate" },
    { id: "srt", label: "shared-encode-srt", protocol: "srt",
      host: "127.0.0.1", port: port + 1, mode: "caller", latencyMs: 400 },
    { id: "hls", label: "shared-encode-hls", protocol: "hls",
      url: `http://127.0.0.1:${port + 2}/live/program.m3u8` },
  ].map((item) => ({ ...item, fps: 60, targetBitrateMbps: 6, encoderMode: "auto",
    videoCodec: "h264", ffmpegBinDirectory: "C:\\ffmpeg\\bin" }));
  const start = Date.now();
  await send("media-core-sync", { elapsedMs: 0, commands: [
    { type: "load-scene-graph", sceneId: "shared-encode-proof", routes: [
      { routeId: "program", mode: "fixed", audioRole: "mix", participantId: "101" }] },
    { type: "sync-audio-routing-matrix", sends: [
      { sourceId: "zoom-mix", busId: "master", gainDb: 0 },
      { sourceId: "zoom-mix", busId: "stream", gainDb: 0 }] },
    { type: "start-program-output", destinations: ["rtmp", "srt", "hls"], destinationSettings: settings,
      streamOutputProfile: { codec: "h264", fps: 60, targetBitrateMbps: 6 }, isoParticipantIds: [] },
  ] });
  await sleep(seconds * 500);
  const midpoint = (await send("media-core-sync", { elapsedMs: Date.now() - start, commands: [] })).snapshot;
  firstSrtFrames = Number(sender(midpoint, "srt")?.framesSent || 0);
  firstRtmpFrames = Number(sender(midpoint, "rtmp")?.framesSent || 0);
  firstHlsFrames = Number(sender(midpoint, "hls")?.framesSent || 0);
  firstSenderStates = Object.fromEntries(["rtmp", "srt", "hls"].map((id) => [id, senderState(midpoint, id)]));
  firstSrtBytes = bytes(srtFile);
  firstHlsBytes = hlsBytes();
  firstRenderSlots = Number(midpoint?.realtimeEvidence?.render?.completedSlots || 0);
  firstExportDivisor = Number(midpoint?.realtimeEvidence?.encoderExport?.divisor ?? -1);
  firstAt = Date.now();
  if (blockRtmp) {
    proxyConnections = proxyClients.size;
    for (const client of proxyClients) client.pause();
  } else if (reconnectRtmp) {
    const oldReceiver = rtmp.p;
    oldReceiver.kill();
    if (oldReceiver.exitCode === null) {
      await Promise.race([
        new Promise((resolve) => oldReceiver.once("exit", resolve)),
        sleep(5000).then(() => { throw new Error("first RTMP receiver did not release its port"); }),
      ]);
    }
    rtmp = receiver("flv", ["-listen", "1", "-i", `rtmp://127.0.0.1:${port}/live/gate`],
      reconnectedRtmpFile);
    await sleep(500);
    if (rtmp.p.exitCode !== null) throw new Error(`replacement RTMP receiver exited: ${rtmp.stderr}`);
  } else {
    rtmp.p.kill();
  }
  await sleep(seconds * 500);
  const end = (await send("media-core-sync", { elapsedMs: Date.now() - start, commands: [] })).snapshot;
  lastAt = Date.now();
  lastSrtFrames = Number(sender(end, "srt")?.framesSent || 0);
  lastHlsFrames = Number(sender(end, "hls")?.framesSent || 0);
  lastRenderSlots = Number(end?.realtimeEvidence?.render?.completedSlots || 0);
  lastExportDivisor = Number(end?.realtimeEvidence?.encoderExport?.divisor ?? -1);
  lastSenderStates = Object.fromEntries(["rtmp", "srt", "hls"].map((id) => [id, senderState(end, id)]));
  await send("media-core-sync", { elapsedMs: Date.now() - start,
    commands: [{ type: "start-program-output", destinations: [], destinationSettings: [],
      isoParticipantIds: [] }] });
} catch (error) { failures.push(error.message); }
finally {
  core.stdin.end(); core.kill(); rtmp.p.kill(); srt.p.kill(); hlsOrigin.close();
  for (const client of proxyClients) client.destroy();
  rtmpProxy?.close();
}
await sleep(2000);
const starts = stderr.match(/\[gpu-encode\] started[^\n]*/g) || [];
const paths = stderr.match(/\[gpu-encode\] path=[^\n]*/g) || [];
const pressureLines = stderr.match(/\[stream-backpressure\] (?:enter|overflow-discard)[^\n]*/g) || [];
const probe = (path) => {
  if (!bytes(path)) return "empty";
  const result = spawnSync(ffprobe, ["-v", "error", "-select_streams", "v:0",
    "-show_entries", "stream=codec_name,width,height", "-of", "csv=p=0", path], { encoding: "utf8" });
  return result.status === 0 ? result.stdout.trim() : result.stderr.trim();
};
const probeCodecs = (path) => {
  if (!bytes(path)) return "empty";
  const result = spawnSync(ffprobe, ["-v", "error", "-show_entries", "stream=codec_name",
    "-of", "csv=p=0", path], { encoding: "utf8" });
  return result.status === 0 ? result.stdout.trim() : result.stderr.trim();
};
if (starts.length !== 1) failures.push(`expected one hardware encoder start, got ${starts.length}`);
if (!paths.length || paths.some((line) => !line.includes("gpu-direct")))
  failures.push(`expected GPU-direct paths, got ${paths.join(" | ")}`);
if (firstRtmpFrames < 30 || firstSrtFrames < 30 || firstHlsFrames < 30)
  failures.push("all destinations did not send video before RTMP failure");
if (lastSrtFrames - firstSrtFrames < seconds * 15)
  failures.push("SRT did not continue at >=30 fps after RTMP was killed");
if (lastHlsFrames - firstHlsFrames < seconds * 15)
  failures.push("HLS did not continue at >=30 fps after RTMP was killed");
for (const id of ["rtmp", "srt", "hls"]) {
  const measured = firstSenderStates[id]?.muxInputVideo;
  if (!measured || measured.payloadBytes <= 0 || measured.packets <= 0 || measured.mbps <= 0)
    failures.push(`${id} has no measured compressed video at its mux input`);
}
for (const id of ["srt", "hls"]) {
  const before = firstSenderStates[id]?.muxInputVideo;
  const after = lastSenderStates[id]?.muxInputVideo;
  if (!after || after.payloadBytes <= before?.payloadBytes || after.fps < 30)
    failures.push(`${id} mux input video did not continue at >=30 fps after RTMP fault`);
}
if (bytes(srtFile) - firstSrtBytes < 500_000)
  failures.push("SRT receiver bytes did not advance after RTMP fault");
if (hlsBytes() - firstHlsBytes < 500_000)
  failures.push("HLS origin bytes did not advance after RTMP fault");
if (blockRtmp) {
  if (!proxyConnections) failures.push("RTMP blocker had no live connection; no stall was tested");
  if (!pressureLines.length) failures.push("RTMP queue never entered pressure; no blocked-socket path was tested");
  if (!pressureLines.some((line) => line.includes("overflow-discard resync") && line.includes("idrRequested=1")))
    failures.push("blocked RTMP never requested a fresh IDR through the shared encoder");
  if (firstExportDivisor !== 1 || lastExportDivisor !== 1)
    failures.push("RTMP pressure changed the global compositor export divisor");
  if ((lastRenderSlots - firstRenderSlots) / ((lastAt - firstAt) / 1000) < 58)
    failures.push("Program render cadence fell below 58 fps during RTMP block");
  if (lastSenderStates.rtmp?.muxInputVideo?.lastWriteAgeMs > 2000 &&
      lastSenderStates.rtmp?.status === "live")
    failures.push("RTMP still reports live after compressed video stopped reaching its mux input");
}
if (reconnectRtmp) {
  const joins = stderr.match(/\[stream-join\] destination=rtmp fresh-idr requested=1/g) || [];
  if (joins.length < 2) failures.push("RTMP reconnect did not request a fresh IDR from the shared encoder");
  if (bytes(reconnectedRtmpFile) < 500_000)
    failures.push("replacement RTMP receiver captured less than 500 KB");
  if (!probe(reconnectedRtmpFile).startsWith("h264"))
    failures.push("replacement RTMP receiver did not decode H.264 after reconnect");
  if (!probeCodecs(reconnectedRtmpFile).includes("aac"))
    failures.push("replacement RTMP receiver did not decode AAC after reconnect");
  if (lastSenderStates.rtmp?.muxInputVideo?.fps < 30)
    failures.push("RTMP mux input did not return to at least 30 fps after reconnect");
}
if (![...hlsReceived.keys()].some((item) => item.endsWith(".m3u8")) ||
    ![...hlsReceived.keys()].some((item) => item.endsWith(".ts")))
  failures.push("HLS origin did not receive playlist and segments");
const hlsSegment = [...hlsReceived.keys()].find((item) => item.endsWith(".ts"));
if (hlsSegment && !probe(join(output, hlsSegment)).startsWith("h264"))
  failures.push("received HLS segment was not decodable H.264");
if (!probe(srtFile).startsWith("h264") || !probe(rtmpFile).startsWith("h264"))
  failures.push("received output was not decodable H.264");
const report = { output, blockRtmp, reconnectRtmp, proxyConnections, pressureLines, starts: starts.length,
  paths, firstRtmpFrames,
  firstSrtFrames, lastSrtFrames, firstHlsFrames, lastHlsFrames,
  firstSenderStates, lastSenderStates, firstSrtBytes, firstHlsBytes,
  firstRenderSlots, lastRenderSlots, firstAt, lastAt, firstExportDivisor, lastExportDivisor,
  hlsFiles: [...hlsReceived.entries()], rtmpBytes: bytes(rtmpFile),
  reconnectedRtmpBytes: bytes(reconnectedRtmpFile), srtBytes: bytes(srtFile),
  rtmpProbe: probe(rtmpFile), reconnectedRtmpProbe: probe(reconnectedRtmpFile),
  reconnectedRtmpCodecs: probeCodecs(reconnectedRtmpFile), srtProbe: probe(srtFile),
  hlsProbe: hlsSegment ? probe(join(output, hlsSegment)) : "missing", failures };
writeFileSync(join(output, "report.json"), JSON.stringify(report, null, 2));
writeFileSync(join(output, "core-stderr.log"), stderr);
console.log(JSON.stringify(report, null, 2));
if (failures.length) { console.error(stderr.slice(-2500)); process.exitCode = 1; }
