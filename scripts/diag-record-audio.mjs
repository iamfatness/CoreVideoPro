// Headless diagnostic: does the MF recorder mux an AAC track when a real
// WASAPI loopback source feeds the mixer? Spawns the native core over stdio,
// arms a loopback capture source + recording, records for --seconds (default
// 20), stops, and ffprobes the artifact. Run a tone (e.g.
//   ffplay -f lavfi -i "sine=frequency=440" -nodisp -autoexit -t 30
// ) alongside so the loopback carries signal.
//
// Usage: node scripts/diag-record-audio.mjs [--native-core <exe>] [--seconds 20]
import { spawn, spawnSync } from "node:child_process";
import { existsSync, statSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, "..");
const args = process.argv.slice(2);
const argValue = (name, fallback) => {
  const index = args.indexOf(`--${name}`);
  return index >= 0 && args[index + 1] ? args[index + 1] : fallback;
};
const nativeCore = resolve(argValue("native-core", join(repoRoot, "native", "build-dev", "corevideo-native.exe")));
const seconds = Number(argValue("seconds", "20"));

if (!existsSync(nativeCore)) {
  console.error(`missing native core: ${nativeCore}`);
  process.exit(1);
}

const child = spawn(nativeCore, [], { cwd: dirname(nativeCore), stdio: ["pipe", "pipe", "pipe"] });
let buffer = "";
let nextId = 1;
const pending = new Map();
const stderrTail = [];

child.stderr.on("data", (chunk) => {
  for (const line of chunk.toString().split("\n")) {
    const trimmed = line.trim();
    if (trimmed) {
      stderrTail.push(trimmed);
      if (stderrTail.length > 40) stderrTail.shift();
    }
  }
});

child.stdout.on("data", (chunk) => {
  buffer += chunk.toString();
  let idx;
  while ((idx = buffer.indexOf("\n")) >= 0) {
    const line = buffer.slice(0, idx).trim();
    buffer = buffer.slice(idx + 1);
    if (!line) continue;
    let message;
    try { message = JSON.parse(line); } catch { continue; }
    if (typeof message.id === "string" && pending.has(message.id)) {
      const { resolve: res, timer } = pending.get(message.id);
      pending.delete(message.id);
      clearTimeout(timer);
      res(message);
    }
  }
});

function send(type, payload = {}) {
  const id = `diag-${nextId++}`;
  return new Promise((res, rej) => {
    const timer = setTimeout(() => { pending.delete(id); rej(new Error(`${type} timed out`)); }, 30000);
    pending.set(id, { resolve: res, timer });
    child.stdin.write(`${JSON.stringify({ id, type, ...payload })}\n`);
  });
}

const sleep = (ms) => new Promise((res) => setTimeout(res, ms));

try {
  await sleep(1500); // handshake settle

  const arm = await send("media-core-sync", {
    elapsedMs: 0,
    commands: [
      {
        type: "sync-capture-audio-sources",
        sources: [
          {
            captureDeviceId: "local-machine-audio",
            audioDeviceId: "default-loopback",
            audioDeviceName: "Default system output",
            audioSourceKind: "wasapi-loopback",
            nativeAudioDeviceId: "",
            audioDriverName: "WASAPI",
            embedded: false,
            audioSyncOffsetMs: 0,
          },
        ],
      },
      {
        type: "set-recording-targets",
        targetFolder: "Recordings/CoreVideo Pro/diag-record-audio",
        filenamePrefix: "diag-audio",
        format: "mp4",
        quality: "high",
        isoParticipantIds: [],
      },
      {
        type: "start-recording-session",
        sessionId: "diag-audio",
        startedAtMs: Date.now(),
        targetFolder: "Recordings/CoreVideo Pro/diag-record-audio",
        filenamePrefix: "diag-audio",
        format: "mp4",
        quality: "high",
        isoParticipantIds: [],
      },
    ],
  });
  console.log(`armed: recording=${arm.snapshot?.recording?.status} writer=${arm.snapshot?.recording?.writerStatus}`);

  for (let i = 0; i < seconds; i += 5) {
    await sleep(5000);
    const sync = await send("media-core-sync", { elapsedMs: (i + 5) * 1000, commands: [] });
    const es = sync.snapshot?.encoderSession ?? {};
    console.log(
      `t+${i + 5}s audioPackets=${es.recordingAudioPacketCount ?? "?"} audioSamples=${es.recordingAudioSampleCount ?? "?"} ` +
      `frames=${es.recordingFrameCount ?? "?"} warn=${es.recordingWarning ?? ""}`
    );
  }

  const stop = await send("media-core-sync", {
    elapsedMs: seconds * 1000,
    commands: [{ type: "stop-recording-session", reason: "diag done" }],
  });
  const artifact = stop.snapshot?.encoderSession?.recordingArtifactPath ?? null;
  console.log(`stopped; artifact=${artifact}`);
  await sleep(1000);

  if (artifact) {
    const full = resolve(dirname(nativeCore), artifact);
    if (existsSync(full)) {
      console.log(`on disk: ${full} (${statSync(full).size} bytes)`);
      const probe = spawnSync("ffprobe", ["-v", "error", "-print_format", "json", "-show_format", "-show_streams", full], { encoding: "utf8" });
      if (probe.status === 0) {
        const info = JSON.parse(probe.stdout);
        const kinds = (info.streams ?? []).map((s) => `${s.codec_type}:${s.codec_name}:${s.duration ?? "?"}s`);
        console.log(`ffprobe: nb_streams=${info.format?.nb_streams} duration=${info.format?.duration}s streams=[${kinds.join(", ")}]`);
      } else {
        console.log(`ffprobe failed: ${probe.stderr}`);
      }
    } else {
      console.log(`artifact missing on disk at ${full}`);
    }
  }
  console.log("--- core stderr tail ---");
  for (const line of stderrTail.slice(-15)) console.log(line);
} catch (error) {
  console.error(`diag failed: ${error instanceof Error ? error.message : error}`);
  console.log("--- core stderr tail ---");
  for (const line of stderrTail.slice(-20)) console.log(line);
} finally {
  child.kill();
}
