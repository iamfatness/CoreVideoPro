/**
 * Headless SRT / RTMP INGEST proof.
 *
 * The previous ingest adapter opened a libsrt socket and threw the packets away
 * â€” it counted bytes and emitted frames with NO PIXELS. It would have passed any
 * "is the source connected" check while showing nothing, which is exactly why
 * this harness judges on DECODED PIXELS reaching the compositor, not on status
 * strings.
 *
 * Pushes a known test pattern into the core over real SRT or RTMP (FFmpeg publisher),
 * then asserts the core's capture source reports a connected feed AND that the
 * program it composites from that source is not blank.
 *
 * Usage: node ./scripts/validate-srt-ingest.mjs [--transport srt|rtmp]
 *        [--seconds 18] [--port 9040]
 *        [--source-size 1920x1080] [--source-fps 30]
 *        [--mode listener|caller] [--restart-publisher] [--keep]
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

const args = process.argv.slice(2);
const argValue = (name, fallback) => {
  const index = args.indexOf(`--${name}`);
  return index >= 0 && args[index + 1] ? args[index + 1] : fallback;
};
const seconds = Number(argValue("seconds", 18));
const transport = argValue("transport", "srt");
const port = Number(argValue("port", transport === "rtmp" ? 19350 : 9040));
const sourceSize = argValue("source-size", "1920x1080");
const sourceFps = Number(argValue("source-fps", 30));
const mode = argValue("mode", "listener");
const preparation = argValue("cpu-source-preparation", null);
if (!/^(srt|rtmp)$/.test(transport) || !/^\d+x\d+$/.test(sourceSize) ||
    !Number.isInteger(sourceFps) || sourceFps < 1 || seconds < 1 ||
    !["listener", "caller"].includes(mode) || (transport === "rtmp" && mode !== "listener") ||
    (preparation !== null && !["0", "1"].includes(preparation))) {
  console.error("Invalid ingest test transport, source size, fps, or duration.");
  process.exit(1);
}
const keep = args.includes("--keep");
const restartPublisher = args.includes("--restart-publisher");
if (restartPublisher && (seconds < 18 || transport !== "srt")) {
  console.error("SRT publisher restart proof needs at least 18 seconds.");
  process.exit(1);
}
const deviceId = `${transport}-ingest-1`;
const label = transport.toUpperCase();
const recordDir = `Recordings/CoreVideoPro/validate-${transport}-ingest`;
const recordingName = `${transport}-ingest`;

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
  console.error(`ffmpeg/ffprobe are required for the ${label} ingest proof.`);
  process.exit(1);
}
if (!existsSync(nativeCore)) {
  console.error(`Missing ${nativeCore}.`);
  process.exit(1);
}

const child = spawn(nativeCore, [], {
  cwd: buildDir,
  env: { ...process.env, COREVIDEO_FFMPEG_DIR: "C:\\ffmpeg\\bin",
    ...(preparation === null ? {} : { COREVIDEO_CPU_SOURCE_PREPARATION: preparation,
      COREVIDEO_ISOLATE_MONITORS: "1", COREVIDEO_PROGRAM_BUFFER_FRAMES: "2", COREVIDEO_GPU_CAPTURE: "0" }) },
  stdio: ["pipe", "pipe", "pipe"],
  windowsHide: true,
});

const startedAt = Date.now();
let nextId = 1;
let stdoutBuffer = "";
let handshake;
const pending = new Map();
let coreStderrTail = "";
let diagnosticLine = "";
let sourceTextureObservations = 0;
let sourceTextureWork = 0;
child.on("exit", (code, signal) => {
  for (const [id, item] of pending) {
    clearTimeout(item.timer);
    item.reject(new Error(`native core exited (${code ?? signal}) during ${id}: ${coreStderrTail.slice(-1000)}`));
  }
  pending.clear();
});

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
child.stderr.on("data", (chunk) => {
  coreStderrTail = (coreStderrTail + chunk.toString()).slice(-4000);
  diagnosticLine = (diagnosticLine + chunk.toString()).slice(-65536);
  let lineEnd;
  while ((lineEnd = diagnosticLine.indexOf("\n")) >= 0) {
    const line = diagnosticLine.slice(0, lineEnd);
    diagnosticLine = diagnosticLine.slice(lineEnd + 1);
    const sourceWork = line.match(/source-tex uploads=(\d+) hits=\d+ creates=(\d+) scratch=(\d+)/);
    if (sourceWork) {
      sourceTextureObservations += 1;
      sourceTextureWork += Number(sourceWork[1]) + Number(sourceWork[2]) + Number(sourceWork[3]);
    }
  }
  // Surface only the ingest adapter's own lines; the core is chatty otherwise.
  for (const line of chunk.toString().split("\n")) {
    if (line.includes("[srt-ingest]") || line.includes("[recording]") || line.includes("[encoder]")) {
      console.log(`core          : ${line.trim()}`);
    }
  }
});

function send(type, payload = {}) {
  const id = `ingest-${nextId++}`;
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
let publisher = null;
let artifact = null;
try {
  for (let i = 0; i < 200 && !handshake; i += 1) await sleep(50);
  if (!handshake) throw new Error("no native-core handshake");
  const ingestCapability = handshake.profile?.capabilityStates?.[`${transport}-ingest`];
  if (ingestCapability?.state !== "available") {
    throw new Error(`${label} ingest capability is ${ingestCapability?.state ?? "missing"}; build with COREVIDEO_WITH_${label}_INGEST=ON`);
  }

  // SRT uses the opposite publisher role; RTMP publishes to a listener URL.
  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{
      type: `configure-${transport}-ingest-sources`,
      sources: [transport === "rtmp"
        ? { id: "input-1", deviceId, name: "RTMP Ingest 1", url: `rtmp://0.0.0.0:${port}/live/test` }
        : { id: "input-1", deviceId, name: "SRT Ingest 1",
            mode, host: mode === "listener" ? "0.0.0.0" : "127.0.0.1", port, latencyMs: 120 }],
    }],
  });
  // connect-capture-device is a TOP-LEVEL rpc, not a media-core-sync command.
  await send("connect-capture-device", { payload: { deviceId, outputSourceId: deviceId } });
  await sleep(2000);

  // Push a known pattern AND tone. A contribution feed carries
  // the guest's audio embedded in the same stream, so proving only video would
  // prove half a feed.
  const publisherArgs =
    ["-hide_banner", "-loglevel", "error", "-re",
     "-f", "lavfi", "-i", `testsrc=size=${sourceSize}:rate=${sourceFps}`,
     "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
     "-t", String(seconds + 6), "-c:v", "h264_nvenc", "-pix_fmt", "yuv420p",
     "-c:a", "aac", "-ac", "2",
     "-f", transport === "rtmp" ? "flv" : "mpegts",
     transport === "rtmp" ? `rtmp://127.0.0.1:${port}/live/test`
       : `srt://127.0.0.1:${port}?mode=${mode === "listener" ? "caller" : "listener"}&transtype=live`];
  let publisherErr = "";
  const startPublisher = () => {
    publisherErr = "";
    const process = spawn(ffmpeg, publisherArgs, { stdio: ["ignore", "ignore", "pipe"], windowsHide: true });
    process.stderr.on("data", (c) => { publisherErr = (publisherErr + c.toString()).slice(-4000); });
    return process;
  };
  publisher = startPublisher();
  console.log(`publisher     : pushing testsrc into ${label} ${mode === "listener" ? "listener" : "caller"} on 127.0.0.1:${port} ...`);

  // Put the ingest source on Program and record, so the proof is that decoded
  // pixels reach the compositor â€” not merely that a status flipped.
  await sleep(4000);
  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [
      { type: "set-verbose-diagnostics", enabled: true },
      ...(preparation === null ? [] : [{ type: "set-output-profile", width: 1920, height: 1080, fps: 60 }]),
      {
        type: "load-scene-graph",
        sceneId: recordingName,
        routes: [{ routeId: "program", mode: "capture-input", audioRole: "mix", captureDeviceId: deviceId }],
      },
      {
        // Route the ingested guest audio to the buses so it reaches the recording.
        type: "sync-audio-routing-matrix",
        sends: [{ sourceId: `capture:${deviceId}`, busId: "master", gainDb: 0 },
                { sourceId: `capture:${deviceId}`, busId: "stream", gainDb: 0 }],
      },
      { type: "sync-virtual-camera", on: true, mirror: false, deviceName: `${recordingName}-proof` },
      { type: "start-program-output", destinations: ["recording"], isoParticipantIds: [] },
      {
        type: "set-recording-targets",
        targetFolder: recordDir,
        filenamePrefix: recordingName, format: "mp4", quality: "high", isoParticipantIds: [],
      },
    ],
  });
  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{
      type: "start-recording-session", sessionId: recordingName, startedAtMs: Date.now(),
      targetFolder: recordDir,
      filenamePrefix: recordingName, format: "mp4", quality: "high", isoParticipantIds: [],
    }],
  });

  let device = null;
  let lastDecodedFrames = 0;
  let lastDecodedAudioSamples = 0;
  let unhealthySamples = 0;
  let healthySamples = 0;
  let renderStart = null;
  let renderEnd = null;
  let maxRenderAgeMs = 0;
  let admissionSamples = 0;
  let previousAdmission = null;
  const deadline = Date.now() + seconds * 1000;
  const restartAt = restartPublisher ? Date.now() + Math.floor(seconds * 1000 / 3) : Infinity;
  let restartCompleted = false;
  while (Date.now() < deadline) {
    await sleep(seconds >= 300 ? 10000 : 3000);
    const sync = await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [] });
    const devices = sync.snapshot?.captureDevices ?? [];
    device = devices.find((d) => d.id === deviceId) ?? null;
    if (device) {
      console.log(`source        : state=${device.connectionState} signal=${device.signalPresent} ` +
                  `decoded=${device.decodedFrames ?? "missing"} audio=${device.decodedAudioSamples ?? "missing"} ` +
                  `age=${device.lastFrameAgeMs ?? "missing"}ms failures=${device.decoderFailures ?? "missing"} ` +
                  `codecErrors=${device.codecDecodeErrors ?? "missing"} packetErrors=${device.packetDecodeErrors ?? "missing"} ` +
                  `rtt=${device.rttMs ?? device.rttStatus ?? "missing"} warning=${device.warning || "none"}`);
      if (device.signalPresent) healthySamples += 1;
      else unhealthySamples += 1;
      if (device.decodedFrames < lastDecodedFrames) failures.push("decoded frame counter regressed");
      if (device.decodedAudioSamples < lastDecodedAudioSamples) failures.push("decoded audio counter regressed");
      lastDecodedFrames = device.decodedFrames ?? 0;
      lastDecodedAudioSamples = device.decodedAudioSamples ?? 0;
    }
    if (preparation === "1" && device?.signalPresent) {
      const admission = sync.snapshot?.programSourceAdmission;
      const row = admission?.sources?.find((item) => item.sourceId === `capture:${deviceId}`);
      const fields = ["requestedEpoch", "actualEpoch", "requestedFrameId", "actualFrameId", "requestedCapture100ns", "actualCapture100ns"];
      if (admission?.version !== 1 || admission.readyOnlyRequested !== true || !row ||
          !["ready", "held"].includes(row.state) || fields.some((field) => !Number.isSafeInteger(row[field]) || row[field] < 0) ||
          row.actualEpoch !== row.requestedEpoch || row.actualFrameId > row.requestedFrameId || row.actualCapture100ns > row.requestedCapture100ns) {
        failures.push("Healthy decoded network source lacks completed identity-correct GPU admission");
      } else {
        admissionSamples += 1;
        if (previousAdmission && row.actualEpoch === previousAdmission.actualEpoch &&
            row.requestedFrameId > previousAdmission.requestedFrameId && row.actualFrameId <= previousAdmission.actualFrameId) {
          failures.push("Prepared network source image did not advance with CPU arrivals");
        }
        previousAdmission = row;
      }
    }
    if (!restartCompleted && Date.now() >= restartAt && device?.signalPresent) {
      const beforeVideo = device.decodedFrames;
      const beforeAudio = device.decodedAudioSamples;
      const beforePreparedEpoch = previousAdmission?.actualEpoch;
      const publisherExited = new Promise((resolve) => {
        if (publisher.exitCode !== null) resolve(true);
        else publisher.once("exit", () => resolve(true));
      });
      publisher.kill();
      const exited = await Promise.race([publisherExited, sleep(5000).then(() => false)]);
      if (!exited) failures.push("SRT test publisher did not exit after interruption");
      let stale = false;
      for (let attempt = 0; attempt < 12 && !stale; attempt += 1) {
        await sleep(500);
        const check = await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [] });
        const current = check.snapshot?.captureDevices?.find((d) => d.id === deviceId);
        stale = current?.signalPresent === false;
      }
      if (!stale) failures.push("SRT signal stayed live after publisher interruption");
      let recovered = false;
      let attemptsUsed = 0;
      // FFmpeg's caller is one-shot. A real contribution encoder retries when
      // it reaches the receiver before the new listener has bound its socket.
      for (let retry = 0; retry < 3 && !recovered; retry += 1) {
        attemptsUsed = retry + 1;
        await sleep(1200);
        publisher = startPublisher();
        let lastVideo = beforeVideo;
        let lastAudio = beforeAudio;
        let growingSamples = 0;
        for (let attempt = 0; attempt < 24 && !recovered; attempt += 1) {
          await sleep(500);
          if (publisher.exitCode !== null || publisher.signalCode !== null) break;
          const check = await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [] });
          const current = check.snapshot?.captureDevices?.find((d) => d.id === deviceId);
          const prepared = check.snapshot?.programSourceAdmission?.sources?.find((row) => row.sourceId === `capture:${deviceId}`);
          const preparedRecovered = preparation !== "1" || (Number.isSafeInteger(beforePreparedEpoch) &&
            ["ready", "held"].includes(prepared?.state) && prepared.actualEpoch === prepared.requestedEpoch && prepared.actualEpoch > beforePreparedEpoch);
          if (current?.signalPresent === true &&
              current.decodedFrames > lastVideo && current.decodedAudioSamples > lastAudio) {
            growingSamples += 1;
          } else {
            growingSamples = 0;
          }
          lastVideo = current?.decodedFrames ?? lastVideo;
          lastAudio = current?.decodedAudioSamples ?? lastAudio;
          if (growingSamples >= 2 && lastVideo > beforeVideo && lastAudio > beforeAudio && preparedRecovered) {
            recovered = true;
            device = current;
          }
        }
        if (!recovered && publisher.exitCode === null && publisher.signalCode === null) publisher.kill();
      }
      if (!recovered) failures.push("SRT publisher restarted but video/audio did not recover on the same source");
      console.log(`restart proof : stale=${stale} recovered=${recovered} attempts=${attemptsUsed} ` +
                  `video=${device?.decodedFrames ?? "missing"} audio=${device?.decodedAudioSamples ?? "missing"} ` +
                  `codecErrors=${device?.codecDecodeErrors ?? "missing"} ` +
                  `packetErrors=${device?.packetDecodeErrors ?? "missing"}`);
      restartCompleted = true;
    }
    // Master true-peak splits "the recording is silent" from "the bus is silent"
    // without re-deriving it from the artifact.
    const master = sync.snapshot?.audioMixSession?.masterMeter ?? null;
    if (master) console.log(`master bus    : truePeak ${master.truePeakDbfs} dBFS`);
    const render = sync.snapshot?.realtimeEvidence?.render;
    if (render?.observed) {
      renderStart ??= render;
      renderEnd = render;
      maxRenderAgeMs = Math.max(maxRenderAgeMs, render.progressAgeMs ?? 0);
    }
  }

  if (restartPublisher && !restartCompleted) failures.push("SRT publisher restart proof never ran");
  if (preparation === "1" && admissionSamples < 2) failures.push("Insufficient actual prepared network source observations");
  if (preparation === "1" && (sourceTextureObservations < 2 || sourceTextureWork !== 0)) {
    failures.push(`Program source texture work remains or is unknown: observations=${sourceTextureObservations}, uploads/creates/scratch=${sourceTextureWork}`);
  }
  if (!device) failures.push(`the ${label} ingest device never appeared in captureDevices`);
  else if (!device.signalPresent) {
    failures.push(`${label} source never reported signal (state=${device.connectionState}, ` +
                  `warning=${device.warning || "none"})` +
                  (publisherErr ? ` | publisher: ${publisherErr.trim().split("\n").pop()}` : ""));
  }
  if (!(device?.decodedFrames > 0) || !(device?.decodedAudioSamples > 0)) {
    failures.push(`${label} health did not count decoded video and audio`);
  }
  if (device?.decoderFailures !== 0) failures.push(`${label} decoder startup failed ${device?.decoderFailures ?? "unknown"} times`);
  if (transport === "srt" && !restartPublisher && device?.codecDecodeErrors !== 0)
    failures.push(`SRT codec decode errors: ${device?.codecDecodeErrors ?? "missing"}`);
  if (transport === "srt" && !restartPublisher && device?.packetDecodeErrors !== 0)
    failures.push(`SRT packet decode errors: ${device?.packetDecodeErrors ?? "missing"}`);
  if (transport === "srt" && (device?.rttMs !== null || !device?.rttStatus?.includes("FFmpeg owns SRT socket"))) {
    failures.push("SRT RTT must remain explicitly unavailable while FFmpeg owns the socket");
  }
  if (seconds >= 300 && unhealthySamples > Math.max(1, healthySamples * 0.01)) {
    failures.push(`${label} ingest was unhealthy at ${unhealthySamples}/${healthySamples + unhealthySamples} soak samples`);
  }
  if (seconds >= 300) {
    if (!renderStart || !renderEnd) failures.push("missing render-worker evidence during SRT soak");
    else {
      const completed = renderEnd.completedSlots - renderStart.completedSlots;
      const skipped = renderEnd.skippedSlots - renderStart.skippedSlots;
      const missed = renderEnd.deadlineMisses - renderStart.deadlineMisses;
      console.log(`render worker : completed=${completed} skipped=${skipped} deadlineMisses=${missed} ` +
                  `maxSampledProgressAge=${maxRenderAgeMs.toFixed(1)}ms gpuVerified=${renderEnd.gpuCompletionVerified}`);
      if (completed < (seconds - 20) * 60 * 0.9 || skipped > completed * 0.01 || maxRenderAgeMs > 500) {
        failures.push(`Program render worker starved (completed=${completed}, skipped=${skipped}, ` +
                      `max sampled progress age=${maxRenderAgeMs.toFixed(1)}ms)`);
      }
    }
  }

  const stop = await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{ type: "stop-recording-session", reason: `${recordingName} proof complete` }],
  });
  // The core's own muxer proof, asserted BEFORE the file is opened: it separates
  // "the feed never reached the encoder" from "the artifact was read too early".
  const proof = stop.snapshot?.recording?.proof ?? {};
  console.log(`muxer proof   : video ${proof.programFrameCount ?? 0} frames, ` +
              `audio ${proof.audioSampleCount ?? 0} samples (present=${proof.audioPresent ?? false})`);
  if (!(proof.programFrameCount > 0)) failures.push("the encoder muxed no program video");
  if (!(proof.audioSampleCount > 0)) failures.push("the encoder muxed no program audio");
  const programStream = stop.snapshot?.recording?.streams?.find((stream) => stream.kind === "program");
  if (programStream) {
    console.log(`recording     : missingFrames=${programStream.missingFrames ?? "missing"} ` +
                `droppedFrames=${programStream.droppedFrames ?? "missing"}`);
    if (transport === "rtmp") {
      if (typeof programStream.missingFrames !== "number" || typeof programStream.droppedFrames !== "number") {
        failures.push("RTMP Program recording frame-loss counters are missing");
      } else if (programStream.missingFrames > 0 || programStream.droppedFrames > 0) {
        failures.push(`RTMP Program recording lost frames (missing=${programStream.missingFrames}, ` +
                      `dropped=${programStream.droppedFrames})`);
      }
    }
    if (seconds >= 300 && programStream.missingFrames > proof.programFrameCount * 0.01) {
      failures.push(`Program recording lost ${programStream.missingFrames} frames`);
    }
  }
  if (seconds >= 300 && proof.programFrameCount < seconds * 60 * 0.9) {
    failures.push(`Program starved: ${proof.programFrameCount} frames over ${seconds}s, expected at least 90% of 60 fps`);
  }

  const path = stop.snapshot?.recording?.artifactPath ?? null;
  if (path) {
    artifact = resolve(buildDir, path);
    console.log(`artifact      : ${artifact}`);
    // Wait for the MP4 to FINALIZE, with the core still alive. The stop response
    // returns before the async encoder sink writes the moov atom, and an MP4 read
    // before its moov decodes as ZERO frames — indistinguishable from a dead feed
    // (this cost a full debugging round). File size stabilises well before the moov
    // lands, so size is not the signal: ask ffprobe whether the file is readable yet.
    let finalized = false;
    for (let i = 0; i < 30 && !finalized; i += 1) {
      await sleep(500);
      const probe = spawnSync(ffprobe,
        ["-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", artifact],
        { encoding: "utf8", timeout: 15000 });
      finalized = probe.status === 0 && Number.parseFloat(probe.stdout ?? "") > 0;
    }
    if (!finalized) failures.push("the recording never finalized (no moov atom) — the writer did not close");
  }
  // A retained compositor frame must not keep the capture device green after
  // the publisher has gone away. This catches the old framesReceived > 0 test.
  publisher.kill();
  let clearedSignal = false;
  for (let attempt = 0; attempt < 20 && !clearedSignal; attempt += 1) {
    await sleep(500);
    const sync = await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [] });
    const current = sync.snapshot?.captureDevices?.find((d) => d.id === deviceId);
    clearedSignal = current?.signalPresent === false && current?.decodedFrames > 0;
  }
  if (!clearedSignal) failures.push(`held ${label} frame still reports live signal after publisher exit`);
  if (transport === "rtmp" && clearedSignal) {
    // A real publisher may be restarted while the same source is on Program.
    // The channel must rebind without requiring another scene or source click.
    const framesBefore = device?.decodedFrames ?? 0;
    const audioBefore = device?.decodedAudioSamples ?? 0;
    const beforePreparedEpoch = previousAdmission?.actualEpoch;
    let recovered = false;
    for (let attempt = 0; attempt < 3 && !recovered; attempt += 1) {
      launchPublisher();
      let lastState = "missing";
      for (let sample = 0; sample < 20 && !recovered; sample += 1) {
        await sleep(500);
        const sync = await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [] });
        const current = sync.snapshot?.captureDevices?.find((d) => d.id === deviceId);
        const prepared = sync.snapshot?.programSourceAdmission?.sources?.find((row) => row.sourceId === `capture:${deviceId}`);
        const preparedRecovered = preparation !== "1" || (Number.isSafeInteger(beforePreparedEpoch) &&
          ["ready", "held"].includes(prepared?.state) && prepared.actualEpoch === prepared.requestedEpoch && prepared.actualEpoch > beforePreparedEpoch);
        lastState = `${current?.connectionState ?? "missing"}/frames=${current?.decodedFrames ?? 0}/audio=${current?.decodedAudioSamples ?? 0}`;
        recovered = current?.signalPresent === true && current?.decodedFrames > framesBefore &&
                    current?.decodedAudioSamples > audioBefore && preparedRecovered;
        if (publisher.exitCode !== null) break;
      }
      if (!recovered) {
        console.log(`publisher     : retry ${attempt + 1} state=${lastState} exit=${publisher.exitCode ?? "running"} ` +
                    `error=${publisherErr.trim().split("\n").pop() || "none"}`);
        publisher.kill();
        await sleep(700);
      }
    }
    console.log(`publisher     : reconnect ${recovered ? "restored video and audio" : "failed"}`);
    if (!recovered) failures.push("RTMP publisher reconnect did not resume decoded video and audio");
  }
} catch (error) {
  failures.push(error.message);
} finally {
  try { child.stdin.end(); } catch {}
  child.kill();
  if (publisher) { try { publisher.kill(); } catch {} }
}

await sleep(1500);

// The decisive check: did the ingested feed actually become PIXELS on program?
if (artifact && existsSync(artifact)) {
  // Long MP4s are checked near the end; decoding 30 minutes into a Node buffer
  // would make the validation itself a memory/performance test.
  const sampleWindow = seconds >= 300 ? ["-sseof", "-10", "-t", "8"] : [];
  const out = spawnSync(ffmpeg,
    ["-v", "error", ...sampleWindow, "-i", artifact, "-vf", "scale=8:8", "-f", "rawvideo", "-pix_fmt", "gray", "-"],
    { encoding: "buffer", maxBuffer: 1 << 28, timeout: 120000 });
  const cells = 64;
  const frames = Math.floor((out.stdout?.length ?? 0) / cells);
  let best = 0;
  for (let f = Math.min(frames - 1, 30); f < frames; f += 1) {
    let sum = 0;
    for (let i = 0; i < cells; i += 1) sum += out.stdout[f * cells + i];
    best = Math.max(best, sum / cells);
  }
  console.log(`program luma  : peak ${best.toFixed(1)} over ${frames} frames`);
  if (frames === 0) failures.push("program recording produced no frames");
  else if (best < 12) failures.push(`program stayed black (peak luma ${best.toFixed(1)}) — the ingested ${label} feed never became pixels`);
  // A grey placeholder can pass a luma check. Compare static top-row testsrc
  // colors with an independently generated reference, allowing codec/scale
  // error. The time-varying bottom band is deliberately excluded.
  const reference = spawnSync(ffmpeg,
    ["-v", "error", "-f", "lavfi", "-i", `testsrc=size=${sourceSize}:rate=${sourceFps}`,
     "-frames:v", "1", "-vf", "scale=16:9", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
    { encoding: "buffer", maxBuffer: 1 << 20, timeout: 15000 });
  const actual = spawnSync(ffmpeg,
    ["-v", "error", ...sampleWindow, "-i", artifact, "-vf", "scale=16:9", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
    { encoding: "buffer", maxBuffer: 1 << 28, timeout: 120000 });
  const bytesPerFrame = 16 * 9 * 3;
  let bestColorError = Infinity;
  if (reference.status === 0 && reference.stdout?.length === bytesPerFrame && actual.status === 0) {
    for (let offset = 0; offset + bytesPerFrame <= actual.stdout.length; offset += bytesPerFrame) {
      let error = 0, count = 0;
      for (const y of [1, 2]) for (const x of [2, 4, 6, 9, 11, 13]) for (let channel = 0; channel < 3; channel += 1) {
        const index = (y * 16 + x) * 3 + channel;
        error += Math.abs(actual.stdout[offset + index] - reference.stdout[index]); count += 1;
      }
      bestColorError = Math.min(bestColorError, error / count);
    }
  }
  console.log(`source pixels : independent testsrc mean color error ${bestColorError.toFixed(2)} (limit 30)`);
  if (!(bestColorError <= 30)) failures.push("Recorded Program did not match independently specified source colors; a placeholder is not input evidence");
  // AUDIO: the guest's embedded tone must reach the mixer, not just the video.
  const pcm = spawnSync(ffmpeg,
    ["-v", "error", ...sampleWindow, "-i", artifact, "-f", "s16le", "-ac", "1", "-ar", "48000", "-"],
    { encoding: "buffer", maxBuffer: 1 << 28, timeout: 120000 });
  let peakAudio = 0;
  const samples = (pcm.stdout?.length ?? 0) >> 1;
  for (let i = 0; i < samples; i += 1) {
    peakAudio = Math.max(peakAudio, Math.abs(pcm.stdout.readInt16LE(i * 2)));
  }
  console.log(`program audio : peak ${peakAudio} over ${samples} samples`);
  if (samples === 0) {
    failures.push("program recording carries no audio track");
  } else if (peakAudio < 500) {
    failures.push(`program audio is silent (peak ${peakAudio}) - the ingested feed's embedded audio never reached the mixer`);
  }

  if (!keep) { try { rmSync(artifact); } catch {} }
} else if (failures.length === 0) {
  failures.push("no program recording artifact to inspect");
}

if (failures.length) {
  console.error(`\n${label} INGEST VALIDATION FAIL`);
  for (const f of failures) console.error(`  - ${f}`);
  process.exit(1);
}
console.log(`\n${label} INGEST VALIDATION PASS`);

