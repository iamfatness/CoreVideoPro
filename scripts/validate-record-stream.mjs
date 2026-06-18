/**
 * Record/stream validation harness for Phase C.
 *
 * Launches the native media core over stdio, arms recording + RTMP program
 * output via media-core-sync, polls snapshot/output health for recording
 * artifact bytes and RTMP send-proof evidence, and prints a pass/fail report.
 *
 * Usage:
 *   node ./scripts/validate-record-stream.mjs
 *
 * Optional:
 *   --timeout-ms 30000 --poll-ms 250 --min-recording-bytes 1 --min-program-frames 3
 *   --destinations recording,rtmp --session-id record-stream-proof
 *   --allow-rtmp-warning (default true)
 */
import { spawn } from "node:child_process";
import { existsSync, readFileSync, statSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, "..");
const buildDir = join(repoRoot, "native", "build-dev");
const exeSuffix = process.platform === "win32" ? ".exe" : "";

const args = parseArgs(process.argv.slice(2));
const timeoutMs = numberArg(args["timeout-ms"], 30000);
const pollMs = numberArg(args["poll-ms"], 250);
const minRecordingBytes = numberArg(args["min-recording-bytes"], 1);
const minProgramFrames = numberArg(args["min-program-frames"], 3);
const sessionId = args["session-id"] ?? "record-stream-proof";
const sceneId = args["scene-id"] ?? "record-stream-validation";
const allowRtmpWarning = !booleanArg(args["disallow-rtmp-warning"]);
const destinations = parseDestinations(args.destinations ?? "recording,rtmp");
const nativeCore = resolve(args["native-core"] ?? join(buildDir, `corevideo-native${exeSuffix}`));

if (!existsSync(nativeCore)) {
  failBeforeSpawn(`Missing native core at ${nativeCore}. Run ./scripts/build-native-dev.ps1 first.`);
}

const child = spawn(nativeCore, [], {
  cwd: dirname(nativeCore),
  env: { ...process.env },
  stdio: ["pipe", "pipe", "pipe"],
});

const startedAt = Date.now();
let nextId = 1;
let stdoutBuffer = "";
let handshake;
let latestSnapshot;
let latestHealth;
let programPreviewSeen = false;
const warnings = [];
const stderrLines = [];
const pending = new Map();

child.stdout.on("data", (chunk) => {
  stdoutBuffer += chunk.toString();
  let newlineIndex;
  while ((newlineIndex = stdoutBuffer.indexOf("\n")) >= 0) {
    const line = stdoutBuffer.slice(0, newlineIndex).trim();
    stdoutBuffer = stdoutBuffer.slice(newlineIndex + 1);
    if (line) {
      onLine(line);
    }
  }
});

child.stderr.on("data", (chunk) => {
  const text = chunk.toString();
  process.stderr.write(text);
  stderrLines.push(...text.split(/\r?\n/).filter(Boolean).slice(-20));
});

child.once("exit", (code) => {
  if (pending.size === 0) {
    return;
  }
  for (const { reject, timer } of pending.values()) {
    clearTimeout(timer);
    reject(new Error(`native core exited with code ${code ?? "unknown"}`));
  }
  pending.clear();
});

try {
  console.log(`Native core   : ${nativeCore}`);
  console.log(`Destinations  : ${destinations.join(", ")}`);
  console.log(`Criteria      : recordingBytes>=${minRecordingBytes}, programFrames>=${minProgramFrames}, rtmpProof=${allowRtmpWarning ? "artifact|warning|live" : "artifact|live"}`);

  handshake = await waitForHandshake();
  console.log(`Handshake     : ${handshake.profile?.name ?? "unknown"} (${(handshake.profile?.capabilities ?? []).join(", ")})`);

  const armResponse = await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: buildArmCommands(),
  });
  latestSnapshot = armResponse.snapshot;
  recordSnapshot(latestSnapshot);
  console.log(
    `Armed output  : active=${Boolean(latestSnapshot?.active)} outputs=${(latestSnapshot?.outputs ?? []).join(",")} recording=${latestSnapshot?.recording?.status ?? "none"}`
  );

  await validationLoop();
  const report = buildReport("passed");
  printReport(report);
  cleanup(0);
} catch (error) {
  const report = buildReport("failed", error instanceof Error ? error.message : String(error));
  printReport(report);
  cleanup(1);
}

function buildArmCommands() {
  const commands = [
    {
      type: "load-scene-graph",
      sceneId,
      routes: [
        {
          routeId: "program",
          mode: "fixed",
          audioRole: "mix",
          participantId: "speaker-1",
        },
      ],
    },
    {
      type: "start-program-output",
      destinations,
      isoParticipantIds: [],
    },
    {
      type: "set-recording-targets",
      targetFolder: "Recordings/CoreVideo Pro/validate-record-stream",
      filenamePrefix: sessionId,
      format: "mp4",
      quality: "high",
      isoParticipantIds: [],
    },
    {
      type: "start-recording-session",
      sessionId,
      startedAtMs: Date.now(),
      targetFolder: "Recordings/CoreVideo Pro/validate-record-stream",
      filenamePrefix: sessionId,
      format: "mp4",
      quality: "high",
      isoParticipantIds: [],
    },
  ];

  if (destinations.includes("recording") || destinations.includes("rtmp")) {
    commands.unshift({
      type: "prepare-encoder-session",
      preparedAtMs: Date.now() - startedAt,
      reason: "Record/stream validation harness warmup.",
    });
  }

  return commands;
}

async function validationLoop() {
  const deadline = startedAt + timeoutMs;

  while (Date.now() < deadline) {
    const elapsedMs = Date.now() - startedAt;
    const syncResponse = await send("media-core-sync", { elapsedMs, commands: [] });
    latestSnapshot = syncResponse.snapshot;
    recordSnapshot(latestSnapshot);

    const healthResponse = await send("get-output-health").catch(() => undefined);
    if (healthResponse?.health) {
      latestHealth = healthResponse.health;
    }

    await send("ping").catch(() => undefined);
    if (criteriaMet()) {
      return;
    }
    await sleep(pollMs);
  }

  throw new Error("Timed out before record/stream criteria were met.");
}

function onLine(line) {
  let message;
  try {
    message = JSON.parse(line);
  } catch {
    return;
  }

  if (message.type === "program-frame-preview") {
    programPreviewSeen = true;
    return;
  }

  if (message.type === "handshake" && message.ok === true) {
    handshake = message;
  }

  if (typeof message.id === "string") {
    const item = pending.get(message.id);
    if (item) {
      pending.delete(message.id);
      clearTimeout(item.timer);
      item.resolve(message);
    }
  }
}

function send(type, payload = {}) {
  const id = `record-stream-${nextId++}`;
  const request = { id, type, ...payload };
  return new Promise((resolvePromise, reject) => {
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`request ${type} timed out`));
    }, Math.max(5000, timeoutMs));
    pending.set(id, { resolve: resolvePromise, reject, timer });
    child.stdin.write(`${JSON.stringify(request)}\n`);
  }).then((response) => {
    if (response.ok === false) {
      throw new Error(`${type} failed: ${response.error?.message ?? "unknown error"}`);
    }
    return response;
  });
}

function waitForHandshake() {
  if (handshake) {
    return Promise.resolve(handshake);
  }
  return new Promise((resolvePromise, reject) => {
    const timer = setTimeout(() => reject(new Error("Timed out waiting for native core handshake.")), 10000);
    const interval = setInterval(() => {
      if (handshake) {
        clearTimeout(timer);
        clearInterval(interval);
        resolvePromise(handshake);
      }
    }, 25);
  });
}

function recordSnapshot(snapshot) {
  if (!snapshot) {
    return;
  }
  for (const warning of [
    ...(snapshot.warnings ?? []),
    ...(snapshot.encoderSession?.warnings ?? []),
    ...(snapshot.outputSenderSession?.warnings ?? []),
    ...(snapshot.recording?.warning ? [snapshot.recording.warning] : []),
    ...(snapshot.health?.messages ?? []),
  ]) {
    warnings.push(warning);
  }
}

function recordingEvidence() {
  const snapshot = latestSnapshot ?? {};
  const health = latestHealth ?? snapshot.health ?? {};
  const recording = snapshot.recording ?? {};
  const encoderSession = snapshot.encoderSession ?? {};

  const artifactCandidates = [
    recording.artifactPath,
    encoderSession.recordingArtifactPath,
    health.recordingArtifactPath,
  ].filter(Boolean);

  let artifactPath;
  let artifactBytes = 0;
  for (const candidate of artifactCandidates) {
    if (existsSync(candidate)) {
      artifactPath = candidate;
      artifactBytes = statSync(candidate).size;
      break;
    }
  }

  const bytesWritten = Math.max(
    numberOrZero(health.recordingBytesWritten),
    numberOrZero(encoderSession.recordingBytesWritten),
    numberOrZero(recording.totalBytesWritten),
    artifactBytes
  );

  const framesWritten = Math.max(
    numberOrZero(snapshot.programFrameCount),
    numberOrZero(health.frameCount),
    numberOrZero(health.encodedFrameCount),
    numberOrZero(encoderSession.programFrameCount),
    numberOrZero(recording.totalFramesWritten)
  );

  return {
    artifactPath: artifactPath ?? artifactCandidates[0] ?? null,
    artifactExists: Boolean(artifactPath),
    bytesWritten,
    framesWritten,
    status: recording.status ?? null,
    writerStatus: recording.writerStatus ?? null,
    active: Boolean(recording.active ?? snapshot.active),
  };
}

function rtmpEvidence() {
  const snapshot = latestSnapshot ?? {};
  const senderSession = snapshot.outputSenderSession ?? {};
  const sender = (senderSession.senders ?? []).find((item) => item.destination === "rtmp");

  if (!sender) {
    return {
      present: false,
      status: null,
      sendArtifactPath: null,
      sendProofSeen: false,
      framesSent: 0,
      warning: null,
    };
  }

  let sendProofSeen = false;
  let sendProofRuntimeAvailable = null;
  let sendProofRuntimeDetail = null;
  let sendProofAttemptCount = 0;
  if (sender.sendArtifactPath && existsSync(sender.sendArtifactPath)) {
    try {
      const content = readFileSync(sender.sendArtifactPath, "utf8");
      sendProofSeen =
        content.includes("rtmp-send-proof-start") || content.includes("rtmp-send-attempt") || content.length > 0;
      for (const line of content.split(/\r?\n/).filter(Boolean)) {
        try {
          const event = JSON.parse(line);
          if (event.type === "rtmp-send-proof-start") {
            sendProofRuntimeAvailable = Boolean(event.runtimeAvailable);
            sendProofRuntimeDetail = event.runtimeDetail ?? null;
          }
          if (event.type === "rtmp-send-attempt") {
            sendProofAttemptCount += 1;
          }
        } catch {
          // Keep sendProofSeen true even if an older proof line is not JSON.
        }
      }
    } catch {
      sendProofSeen = false;
    }
  }

  return {
    present: true,
    status: sender.status ?? null,
    sendArtifactPath: sender.sendArtifactPath ?? null,
    sendProofSeen,
    sendProofRuntimeAvailable,
    sendProofRuntimeDetail,
    sendProofAttemptCount,
    framesSent: numberOrZero(sender.framesSent),
    sendBytesWritten: numberOrZero(sender.sendBytesWritten),
    warning: sender.warning ?? null,
    runtimeDetail: sender.runtimeDetail ?? sendProofRuntimeDetail,
    runtimeMissing:
      sender.status === "warning" &&
      Boolean(sender.warning?.toLowerCase().includes("libavformat") || String(sender.runtimeDetail ?? sendProofRuntimeDetail ?? "").startsWith("missing:")),
    sessionStatus: senderSession.status ?? null,
  };
}

function criteriaMet() {
  const outputs = latestSnapshot?.outputs ?? [];
  const wantsRecording = destinations.includes("recording");
  const wantsRtmp = destinations.includes("rtmp");

  if (!latestSnapshot?.active) {
    return false;
  }

  if (wantsRecording && !outputs.includes("recording")) {
    return false;
  }
  if (wantsRtmp && !outputs.includes("rtmp")) {
    return false;
  }

  const recording = recordingEvidence();
  const recordingOk =
    !wantsRecording ||
    recording.artifactExists ||
    recording.bytesWritten >= minRecordingBytes ||
    (recording.status === "recording" && recording.framesWritten >= minProgramFrames);

  const rtmp = rtmpEvidence();
  const rtmpOk =
    !wantsRtmp ||
    rtmp.sendProofSeen ||
    (allowRtmpWarning && (rtmp.status === "warning" || rtmp.sessionStatus === "warning")) ||
    rtmp.status === "live" ||
    rtmp.framesSent >= minProgramFrames;

  return recordingOk && rtmpOk;
}

function buildReport(status, failureReason) {
  const recording = recordingEvidence();
  const rtmp = rtmpEvidence();

  return {
    status,
    failureReason,
    elapsedMs: Date.now() - startedAt,
    sceneId: latestSnapshot?.sceneId ?? sceneId,
    outputs: latestSnapshot?.outputs ?? [],
    active: Boolean(latestSnapshot?.active),
    programPreviewSeen,
    recording,
    rtmp,
    encoder: {
      name: latestSnapshot?.encoder ?? latestHealth?.encoder ?? null,
      codec: latestSnapshot?.codec ?? latestHealth?.codec ?? null,
      hardwareEncoder: latestSnapshot?.hardwareEncoder ?? latestHealth?.hardwareEncoder ?? null,
      lifecycle: latestSnapshot?.encoderSession?.lifecycle?.status ?? null,
    },
    health: latestHealth ?? latestSnapshot?.health ?? null,
    criteria: {
      destinations,
      minRecordingBytes,
      minProgramFrames,
      allowRtmpWarning,
    },
    recommendations: buildRecommendations(recording, rtmp),
    warnings: collectWarnings(),
    recentStderr: stderrLines.slice(-10),
  };
}

function buildRecommendations(recording, rtmp) {
  const recommendations = [];
  if (destinations.includes("recording") && !recording.artifactExists && recording.bytesWritten === 0) {
    recommendations.push("Recording did not produce artifact bytes; rerun build-native-dev and confirm Media Foundation encoder is active.");
  }
  if (destinations.includes("rtmp") && rtmp.runtimeMissing) {
    recommendations.push("RTMP runtime is missing. Install FFmpeg or set COREVIDEO_FFMPEG_BIN_DIR to a bin folder containing avformat*.dll before packaging or validation.");
  }
  if (destinations.includes("rtmp") && rtmp.present && !rtmp.sendProofSeen) {
    recommendations.push("RTMP sender was present but no send-proof artifact was readable; check temp directory permissions.");
  }
  return recommendations;
}

function printReport(report) {
  console.log("");
  console.log("Record/stream validation report");
  console.log(JSON.stringify(report, null, 2));
}

function collectWarnings() {
  return [...new Set(warnings.filter(Boolean))];
}

function parseDestinations(value) {
  return value
    .split(",")
    .map((entry) => entry.trim())
    .filter(Boolean);
}

function parseArgs(values) {
  const result = {};
  for (let index = 0; index < values.length; index += 1) {
    const value = values[index];
    if (!value.startsWith("--")) {
      continue;
    }
    const key = value.slice(2);
    const next = values[index + 1];
    if (!next || next.startsWith("--")) {
      result[key] = "true";
      continue;
    }
    result[key] = next;
    index += 1;
  }
  return result;
}

function numberArg(value, fallback) {
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : fallback;
}

function numberOrZero(value) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : 0;
}

function booleanArg(value) {
  return value === true || value === "true" || value === "1" || value === "yes";
}

function sleep(ms) {
  return new Promise((resolvePromise) => setTimeout(resolvePromise, ms));
}

function cleanup(code) {
  for (const { reject, timer } of pending.values()) {
    clearTimeout(timer);
    reject(new Error("validation harness exiting"));
  }
  pending.clear();
  child.kill();
  process.exit(code);
}

function failBeforeSpawn(message) {
  console.error(message);
  process.exit(1);
}
