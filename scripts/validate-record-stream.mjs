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
let latestProgramFramePreview;
let programPreviewSeen = false;
const warnings = [];
const stderrLines = [];
const pending = new Map();
let lastProofSummary;

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
  console.log(
    `Criteria      : compositorFrames>=${minProgramFrames}, encodedFrames>=${minProgramFrames}, recordingBytes>=${minRecordingBytes}, rtmpProof=${allowRtmpWarning ? "artifact|warning|live" : "artifact|live"}`
  );

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
    if (message.preview) {
      latestProgramFramePreview = message.preview;
    }
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
  if (snapshot.programFramePreview) {
    latestProgramFramePreview = snapshot.programFramePreview;
    programPreviewSeen = true;
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

function compositorEvidence() {
  const snapshot = latestSnapshot ?? {};
  const health = latestHealth ?? snapshot.health ?? {};
  const programFrame = snapshot.programFrame ?? {};
  const preview = latestProgramFramePreview ?? snapshot.programFramePreview ?? null;
  const frameCount = Math.max(
    numberOrZero(snapshot.programFrameCount),
    numberOrZero(health.frameCount),
    numberOrZero(programFrame.frameNumber),
    numberOrZero(preview?.frameNumber)
  );
  const previewPixelBytes = Math.max(
    numberOrZero(preview?.pixelBytes),
    numberOrZero(preview?.rgbaByteLength),
    numberOrZero(preview?.data?.length),
    numberOrZero(preview?.bgraBase64?.length)
  );

  return {
    pass: frameCount >= minProgramFrames && (programPreviewSeen || Boolean(preview) || Boolean(programFrame.frameNumber)),
    frameCount,
    previewSeen: programPreviewSeen || Boolean(preview),
    renderPlanId: programFrame.renderPlanId ?? preview?.renderPlanId ?? null,
    frameNumber: numberOrNull(programFrame.frameNumber ?? preview?.frameNumber),
    health: programFrame.health ?? preview?.health ?? null,
    renderer: health.renderer ?? snapshot.renderer ?? programFrame.renderer ?? preview?.renderer ?? null,
    width: numberOrNull(programFrame.width ?? preview?.width),
    height: numberOrNull(programFrame.height ?? preview?.height),
    previewPayloadBytes: previewPixelBytes,
    previewPixelFormat: preview?.pixelFormat ?? null,
    sharedTexture: preview?.sharedTexture ?? null,
    warning: programFrame.warning ?? null,
  };
}

function encodedFrameEvidence() {
  const snapshot = latestSnapshot ?? {};
  const health = latestHealth ?? snapshot.health ?? {};
  const encoderSession = snapshot.encoderSession ?? {};
  const targets = encoderSession.targets ?? [];
  const attachedFrameCount = Math.max(...targets.map((target) => numberOrZero(target.attachedFrameCount)), 0);
  const encodedFrameCount = Math.max(
    numberOrZero(health.encodedFrameCount),
    numberOrZero(encoderSession.encodedFrameCount),
    numberOrZero(encoderSession.programFrameCount),
    attachedFrameCount
  );

  return {
    pass: encodedFrameCount >= minProgramFrames,
    encodedFrameCount,
    attachedFrameCount,
    status: encoderSession.status ?? null,
    lifecycle: encoderSession.lifecycle?.status ?? null,
    name: health.encoder ?? latestSnapshot?.encoder ?? null,
    codec: health.codec ?? latestSnapshot?.codec ?? null,
    hardwareEncoder: Boolean(health.hardwareEncoder ?? latestSnapshot?.hardwareEncoder),
    targets: targets.map((target) => ({
      targetId: target.targetId ?? null,
      destination: target.destination ?? null,
      streamKind: target.streamKind ?? null,
      status: target.status ?? null,
      attachedFrameCount: numberOrZero(target.attachedFrameCount),
      warning: target.warning ?? null,
    })),
    warnings: encoderSession.warnings ?? [],
  };
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
    numberOrZero(recording.programFramesWritten),
    numberOrZero(health.frameCount),
    numberOrZero(health.encodedFrameCount),
    numberOrZero(encoderSession.programFrameCount),
    numberOrZero(recording.totalFramesWritten),
    numberOrZero(recording.evidence?.programFramesWritten),
    ...((recording.streams ?? []).map((stream) => stream.kind === "program" ? numberOrZero(stream.framesWritten) : 0))
  );

  const proof = recording.proof ?? {};
  const proofDurationMs = Math.max(numberOrZero(proof.durationMs), numberOrZero(health.recordingDurationMs));
  const proofFrameCount = Math.max(
    numberOrZero(proof.videoFrameCount),
    numberOrZero(proof.programFrameCount),
    numberOrZero(health.recordingFrameCount),
    framesWritten
  );
  const metadataValid = Boolean(proof.metadataValid ?? health.recordingMetadataValid);
  const audioPresent = Boolean(proof.audioPresent ?? numberOrZero(proof.audioPacketsObserved) > 0);

  return {
    pass: (artifactBytes >= minRecordingBytes || bytesWritten >= minRecordingBytes) && proofFrameCount >= minProgramFrames && metadataValid,
    artifactPath: artifactPath ?? artifactCandidates[0] ?? null,
    artifactExists: Boolean(artifactPath),
    artifactBytes,
    bytesWritten,
    framesWritten,
    proof: {
      durationMs: proofDurationMs,
      videoFrameCount: proofFrameCount,
      programFrameCount: numberOrZero(proof.programFrameCount),
      isoFrameCount: numberOrZero(proof.isoFrameCount),
      audioPacketsObserved: numberOrZero(proof.audioPacketsObserved),
      audioPresent,
      metadataValid,
      containerFormat: proof.containerFormat ?? recording.format ?? null,
      videoCodec: proof.videoCodec ?? encoderSession.codec ?? health.codec ?? null,
      audioCodec: proof.audioCodec ?? null,
      width: numberOrNull(proof.width),
      height: numberOrNull(proof.height),
      frameRate: numberOrNull(proof.frameRate),
      failureCount: numberOrZero(proof.failureCount),
      recoveryCount: numberOrZero(proof.recoveryCount),
    },
    streams: (recording.streams ?? []).map((stream) => ({
      kind: stream.kind ?? null,
      status: stream.status ?? null,
      framesWritten: numberOrZero(stream.framesWritten),
      durationMs: numberOrZero(stream.durationMs),
      bytesWritten: numberOrZero(stream.bytesWritten),
      hasAudio: Boolean(stream.hasAudio),
      metadataValid: Boolean(stream.metadataValid),
    })),
    lastFailure: recording.lastFailure ?? null,
    lastRecovery: recording.lastRecovery ?? null,
    status: recording.status ?? null,
    writerStatus: recording.writerStatus ?? null,
    active: Boolean(recording.active ?? snapshot.active),
    warning: recording.warning ?? null,
  };
}

function rtmpEvidence() {
  const snapshot = latestSnapshot ?? {};
  const senderSession = snapshot.outputSenderSession ?? {};
  const sender = (senderSession.senders ?? []).find((item) => item.destination === "rtmp");
  const packagingSignal = readFfmpegPackagingSignal();

  if (!sender) {
    return {
      pass: false,
      present: false,
      status: null,
      sendArtifactPath: null,
      sendArtifactExists: false,
      sendArtifactBytes: 0,
      sendProofSeen: false,
      sendProofRuntimeAvailable: null,
      sendProofRuntimeDetail: null,
      runtimeCandidates: [],
      runtimeAvailabilityStatuses: [],
      endpointEvidence: null,
      packagingSignal,
      framesSent: 0,
      warning: null,
    };
  }

  let sendProofSeen = false;
  let sendArtifactExists = false;
  let sendArtifactBytes = 0;
  let sendProofRuntimeAvailable = null;
  let sendProofRuntimeDetail = null;
  let sendProofAttemptCount = 0;
  let endpointEvidence = null;
  let proofPackagingSignal = null;
  let runtimeCandidates = [];
  const sendProofStatuses = new Set();
  if (sender.sendArtifactPath && existsSync(sender.sendArtifactPath)) {
    try {
      sendArtifactExists = true;
      sendArtifactBytes = statSync(sender.sendArtifactPath).size;
      const content = readFileSync(sender.sendArtifactPath, "utf8");
      sendProofSeen =
        content.includes("rtmp-send-proof-start") || content.includes("rtmp-send-attempt") || content.length > 0;
      for (const line of content.split(/\r?\n/).filter(Boolean)) {
        try {
          const event = JSON.parse(line);
          if (event.type === "rtmp-send-proof-start") {
            sendProofRuntimeAvailable = Boolean(event.runtimeAvailable);
            sendProofRuntimeDetail = event.runtimeDetail ?? null;
            runtimeCandidates = Array.isArray(event.runtimeCandidates) ? event.runtimeCandidates : [];
            endpointEvidence = {
              destination: event.destination ?? "rtmp",
              endpointConfigured: Boolean(event.endpointConfigured),
              endpointMode: event.endpointMode ?? null,
              testMode: Boolean(event.testMode),
              muxingMode: event.muxingMode ?? null,
            };
            proofPackagingSignal = event.packagingSignal ?? null;
          }
          if (event.type === "rtmp-send-attempt") {
            sendProofAttemptCount += 1;
            if (event.status) {
              sendProofStatuses.add(event.status);
            }
          }
        } catch {
          // Keep sendProofSeen true even if an older proof line is not JSON.
        }
      }
    } catch {
      sendProofSeen = false;
    }
  }

  const runtimeDetail = sender.runtimeDetail ?? sendProofRuntimeDetail ?? null;
  const runtimeAvailabilityStatuses = runtimeCandidates.map((candidate) => ({
    name: candidate.name ?? null,
    status: candidate.status ?? null,
    detail: candidate.detail ?? null,
  }));
  const runtimeMissing =
    sender.status === "warning" &&
    Boolean(
      sender.warning?.toLowerCase().includes("libavformat") ||
      String(runtimeDetail ?? "").startsWith("missing:") ||
      (runtimeAvailabilityStatuses.length > 0 && runtimeAvailabilityStatuses.every((candidate) => candidate.status !== "available"))
    );
  const pass =
    sendProofSeen ||
    sender.status === "live" ||
    numberOrZero(sender.framesSent) >= minProgramFrames ||
    (allowRtmpWarning && (sender.status === "warning" || senderSession.status === "warning"));

  return {
    pass,
    present: true,
    status: sender.status ?? null,
    sendArtifactPath: sender.sendArtifactPath ?? null,
    sendArtifactExists,
    sendArtifactBytes,
    sendProofSeen,
    sendProofRuntimeAvailable,
    sendProofRuntimeDetail,
    sendProofAttemptCount,
    sendProofStatuses: [...sendProofStatuses],
    runtimeCandidates,
    runtimeAvailabilityStatuses,
    endpointEvidence,
    packagingSignal: {
      ...packagingSignal,
      proof: proofPackagingSignal,
    },
    framesSent: numberOrZero(sender.framesSent),
    sendBytesWritten: numberOrZero(sender.sendBytesWritten),
    warning: sender.warning ?? null,
    runtimeDetail,
    runtimeMissing,
    sessionStatus: senderSession.status ?? null,
  };
}

function readFfmpegPackagingSignal() {
  const manifestPath = join(dirname(nativeCore), "corevideo-ffmpeg-runtime.json");
  if (!existsSync(manifestPath)) {
    return {
      manifestPath,
      manifestExists: false,
      status: "not-staged",
      copiedDlls: [],
    };
  }

  try {
    const manifest = JSON.parse(readFileSync(manifestPath, "utf8").replace(/^\uFEFF/, ""));
    return {
      manifestPath,
      manifestExists: true,
      status: manifest.status ?? (Array.isArray(manifest.copiedDlls) && manifest.copiedDlls.length > 0 ? "staged" : "unknown"),
      sourceDir: manifest.sourceDir ?? null,
      copiedDlls: Array.isArray(manifest.copiedDlls) ? manifest.copiedDlls : [],
      missingPatterns: Array.isArray(manifest.missingPatterns) ? manifest.missingPatterns : [],
      stagedAtUtc: manifest.stagedAtUtc ?? null,
      warning: manifest.warning ?? null,
    };
  } catch (error) {
    return {
      manifestPath,
      manifestExists: true,
      status: "unreadable",
      copiedDlls: [],
      warning: error instanceof Error ? error.message : String(error),
    };
  }
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

  const compositor = compositorEvidence();
  const encoder = encodedFrameEvidence();
  const recording = recordingEvidence();
  const rtmp = rtmpEvidence();
  const compositorOk = compositor.pass;
  const encoderOk = encoder.pass;
  const recordingOk =
    !wantsRecording ||
    recording.pass;

  const rtmpOk =
    !wantsRtmp ||
    rtmp.pass;

  lastProofSummary = { compositorOk, encoderOk, recordingOk, rtmpOk };
  return compositorOk && encoderOk && recordingOk && rtmpOk;
}

function buildReport(status, failureReason) {
  const compositor = compositorEvidence();
  const encodedFrames = encodedFrameEvidence();
  const recording = recordingEvidence();
  const rtmp = rtmpEvidence();
  const proof = {
    compositor: compositor.pass,
    encodedFrames: encodedFrames.pass,
    recordingBytes: !destinations.includes("recording") || recording.pass,
    rtmpSendProof: !destinations.includes("rtmp") || rtmp.pass,
  };

  return {
    status,
    failureReason: failureReason ?? null,
    elapsedMs: Date.now() - startedAt,
    sceneId: latestSnapshot?.sceneId ?? sceneId,
    outputs: latestSnapshot?.outputs ?? [],
    active: Boolean(latestSnapshot?.active),
    proof,
    lastProofSummary: lastProofSummary ?? proof,
    compositor,
    encodedFrames,
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
    recommendations: buildRecommendations(compositor, encodedFrames, recording, rtmp),
    warnings: collectWarnings(),
    recentStderr: stderrLines.slice(-10),
  };
}

function buildRecommendations(compositor, encodedFrames, recording, rtmp) {
  const recommendations = [];
  if (!compositor.pass) {
    recommendations.push("Compositor did not publish enough program-frame evidence; confirm the scene graph loads and the native core is emitting program-frame-preview events.");
  }
  if (!encodedFrames.pass) {
    recommendations.push("Encoder did not attach enough program frames; run build-native-dev and confirm the D3D11 compositor and Media Foundation encoder adapters are enabled.");
  }
  if (destinations.includes("recording") && !recording.artifactExists && recording.bytesWritten === 0) {
    recommendations.push("Recording did not produce artifact bytes; rerun build-native-dev and confirm Media Foundation encoder is active.");
  }
  if (destinations.includes("recording") && recording.proof.videoFrameCount < minProgramFrames) {
    recommendations.push("Recording proof did not include enough video frames; confirm the encoder is receiving compositor frames.");
  }
  if (destinations.includes("recording") && !recording.proof.metadataValid) {
    recommendations.push("Recording metadata proof is invalid; inspect encoder container/codec/duration fields before using this build for a show.");
  }
  if (destinations.includes("recording") && !recording.proof.audioPresent) {
    recommendations.push("Recording proof did not observe audio packets; confirm the native audio mixer receives Zoom raw audio or synthetic audio frames.");
  }
  if (destinations.includes("rtmp") && rtmp.runtimeMissing) {
    recommendations.push("RTMP runtime is missing. Install FFmpeg or set COREVIDEO_FFMPEG_BIN_DIR to a bin folder containing avformat*.dll before packaging or validation.");
  }
  if (destinations.includes("rtmp") && rtmp.packagingSignal?.status === "missing") {
    recommendations.push("RTMP packaging manifest reports missing FFmpeg DLLs; package output will keep recording flows usable but RTMP needs COREVIDEO_FFMPEG_BIN_DIR before release validation.");
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

function numberOrNull(value) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : null;
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
