import type {
  MediaCoreFrame,
  MediaCoreProgramFrame,
  MediaCoreRecordingQuality,
  MediaCoreRecordingSession,
  MediaCoreRecordingStream,
  MediaCoreRecordingTargets,
  MediaCoreRecordingWriterStatus
} from "./protocol.js";

const DEFAULT_TARGETS: MediaCoreRecordingTargets = {
  targetFolder: "Recordings/CoreVideo Pro/native-core",
  filenamePrefix: "program",
  format: "mp4",
  quality: "high",
  isoParticipantIds: []
};

const BYTES_PER_FRAME: Record<"program" | "iso", number> = {
  program: 260_000,
  iso: 140_000
};

export class RecordingSink {
  private session?: MediaCoreRecordingSession;
  private targets = DEFAULT_TARGETS;
  private lastElapsedMs = 0;

  setTargets(targets: Partial<MediaCoreRecordingTargets>) {
    this.targets = normalizeTargets({ ...this.targets, ...targets });
    return this.snapshot();
  }

  start(targets: Partial<MediaCoreRecordingTargets> = {}, elapsedMs = 0, sessionId?: string) {
    const nextTargets = normalizeTargets({ ...this.targets, ...targets });

    if (!this.session || !this.session.active || !sameTargets(this.session, nextTargets)) {
      this.session = createSession(nextTargets, elapsedMs, sessionId);
    }

    this.targets = nextTargets;
    this.lastElapsedMs = elapsedMs;
    updateSessionHealth(this.session);
    return this.snapshot();
  }

  sync(isoParticipantIds: string[], elapsedMs: number) {
    return this.start({ isoParticipantIds }, elapsedMs);
  }

  writeFrames(frames: MediaCoreFrame[], elapsedMs: number, programFrame?: MediaCoreProgramFrame) {
    if (!this.session || !this.session.active) {
      return this.snapshot();
    }

    this.lastElapsedMs = elapsedMs;
    this.session.elapsedMs = Math.max(0, elapsedMs - this.session.startedAtMs);

    const writableFrames = frames.filter((frame) => frame.health !== "dropped");
    this.session.streams.forEach((stream) => {
      const programWritableFrameCount = programFrame && programFrame.health !== "dropped" ? 1 : 0;
      const programDroppedFrameCount = programFrame?.health === "dropped" ? 1 : 0;
      const matchingFrames =
        stream.kind === "program" ? programWritableFrameCount : writableFrames.filter((frame) => frame.kind === "participant-video" && frame.participantId === stream.participantId).length;
      const matchingDropped =
        stream.kind === "program"
          ? programDroppedFrameCount
          : frames.filter((frame) => frame.kind === "participant-video" && frame.health === "dropped" && frame.participantId === stream.participantId).length;

      stream.framesWritten += matchingFrames;
      stream.droppedFrames += matchingDropped;
      stream.bytesWritten += matchingFrames * BYTES_PER_FRAME[stream.kind];
    });

    updateSessionTotals(this.session);
    updateSessionHealth(this.session);
    return this.snapshot();
  }

  stop(elapsedMs = this.lastElapsedMs) {
    if (!this.session) {
      return undefined;
    }

    this.lastElapsedMs = elapsedMs;
    this.session.active = false;
    this.session.status = "stopped";
    this.session.writerStatus = "stopped";
    this.session.stoppedAtMs = elapsedMs;
    this.session.elapsedMs = Math.max(0, elapsedMs - this.session.startedAtMs);
    this.session.streams.forEach((stream) => {
      stream.status = "stopped";
    });

    return this.snapshot();
  }

  fail(message: string, elapsedMs = this.lastElapsedMs) {
    if (!this.session) {
      this.session = createSession(this.targets, elapsedMs);
    }

    this.lastElapsedMs = elapsedMs;
    this.session.active = false;
    this.session.status = "failed";
    this.session.writerStatus = "failed";
    this.session.error = message;
    this.session.warning = undefined;
    this.session.stoppedAtMs = elapsedMs;
    this.session.elapsedMs = Math.max(0, elapsedMs - this.session.startedAtMs);
    this.session.streams.forEach((stream) => {
      stream.status = "failed";
    });

    return this.snapshot();
  }

  clear() {
    this.session = undefined;
  }

  snapshot() {
    if (!this.session) {
      return undefined;
    }

    return {
      ...this.session,
      elapsedMs: Math.max(this.session.elapsedMs, this.lastElapsedMs - this.session.startedAtMs),
      streams: this.session.streams.map((stream) => ({ ...stream })),
      encoder: { ...this.session.encoder }
    };
  }
}

function createSession(targets: MediaCoreRecordingTargets, startedAtMs: number, sessionId = `rec-${startedAtMs}`): MediaCoreRecordingSession {
  const programPath = `${targets.targetFolder}/${targets.filenamePrefix}-program-${startedAtMs}.${targets.format}`;
  const streams: MediaCoreRecordingStream[] = [
    createStream("program", programPath),
    ...targets.isoParticipantIds.map((participantId) =>
      createStream("iso", `${targets.targetFolder}/${targets.filenamePrefix}-iso-${participantId}-${startedAtMs}.${targets.format}`, participantId)
    )
  ];

  const session: MediaCoreRecordingSession = {
    sessionId,
    active: true,
    status: "recording",
    writerStatus: "writing",
    startedAtMs,
    elapsedMs: 0,
    targetFolder: targets.targetFolder,
    filenamePrefix: targets.filenamePrefix,
    format: targets.format,
    quality: targets.quality,
    encoder: encoderForQuality(targets.quality),
    estimatedDiskRateMBps: estimateDiskRate(streams.length, targets.quality),
    programPath,
    streams,
    totalFramesWritten: 0,
    totalDroppedFrames: 0,
    totalBytesWritten: 0
  };

  updateSessionHealth(session);
  return session;
}

function createStream(kind: "program" | "iso", path: string, participantId?: string): MediaCoreRecordingStream {
  return {
    kind,
    participantId,
    path,
    status: "writing",
    framesWritten: 0,
    droppedFrames: 0,
    bytesWritten: 0
  };
}

function normalizeTargets(targets: MediaCoreRecordingTargets): MediaCoreRecordingTargets {
  return {
    targetFolder: targets.targetFolder.trim() || DEFAULT_TARGETS.targetFolder,
    filenamePrefix: sanitizeFilename(targets.filenamePrefix || DEFAULT_TARGETS.filenamePrefix),
    format: targets.format,
    quality: targets.quality,
    isoParticipantIds: [...new Set(targets.isoParticipantIds)].sort()
  };
}

function sanitizeFilename(value: string) {
  return value.trim().replace(/[^a-zA-Z0-9._-]+/g, "_") || DEFAULT_TARGETS.filenamePrefix;
}

function sameTargets(session: MediaCoreRecordingSession, targets: MediaCoreRecordingTargets) {
  const currentIds = session.streams
    .filter((stream) => stream.kind === "iso")
    .map((stream) => stream.participantId as string)
    .sort();

  return (
    session.targetFolder === targets.targetFolder &&
    session.filenamePrefix === sanitizeFilename(targets.filenamePrefix) &&
    session.format === targets.format &&
    session.quality === targets.quality &&
    currentIds.length === targets.isoParticipantIds.length &&
    currentIds.every((id, index) => id === targets.isoParticipantIds[index])
  );
}

function updateSessionTotals(session: MediaCoreRecordingSession) {
  session.totalFramesWritten = session.streams.reduce((total, stream) => total + stream.framesWritten, 0);
  session.totalDroppedFrames = session.streams.reduce((total, stream) => total + stream.droppedFrames, 0);
  session.totalBytesWritten = session.streams.reduce((total, stream) => total + stream.bytesWritten, 0);
}

function updateSessionHealth(session: MediaCoreRecordingSession) {
  if (session.status === "failed" || session.status === "stopped") {
    return;
  }

  const warning = recordingWarning(session);
  const streamStatus: MediaCoreRecordingWriterStatus = warning ? "warning" : "writing";
  session.warning = warning;
  session.status = warning ? "warning" : "recording";
  session.writerStatus = streamStatus;
  session.streams.forEach((stream) => {
    stream.status = streamStatus;
  });
}

function recordingWarning(session: MediaCoreRecordingSession) {
  if (!session.targetFolder) {
    return "Recording target folder is missing.";
  }

  if (session.streams.length === 0) {
    return "Recording has no writable streams.";
  }

  if (session.streams.length > 5) {
    return "High ISO count may exceed disk bandwidth.";
  }

  if (session.totalDroppedFrames > 0) {
    return "Recording writer is skipping dropped frames.";
  }

  return undefined;
}

function encoderForQuality(quality: MediaCoreRecordingQuality) {
  if (quality === "archive") {
    return { codec: "hevc" as const, hardwareAccelerated: true, targetBitrateMbps: 32 };
  }

  if (quality === "high") {
    return { codec: "h264" as const, hardwareAccelerated: true, targetBitrateMbps: 18 };
  }

  return { codec: "h264" as const, hardwareAccelerated: true, targetBitrateMbps: 10 };
}

function estimateDiskRate(streamCount: number, quality: MediaCoreRecordingQuality) {
  const qualityMultiplier = quality === "archive" ? 2.2 : quality === "high" ? 1.35 : 1;
  return Number((Math.max(1, streamCount) * 1.85 * qualityMultiplier).toFixed(2));
}
