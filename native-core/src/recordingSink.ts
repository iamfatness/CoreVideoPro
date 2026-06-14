import type { MediaCoreFrame, MediaCoreRecordingSession, MediaCoreRecordingStream } from "./protocol.js";

const BASE_RECORDING_FOLDER = "Recordings/CoreVideo Pro/native-core";

export class RecordingSink {
  private session?: MediaCoreRecordingSession;
  private lastElapsedMs = 0;

  sync(isoParticipantIds: string[], elapsedMs: number) {
    const normalizedIsoIds = [...new Set(isoParticipantIds)].sort();

    if (!this.session || !sameIsoStreams(this.session, normalizedIsoIds)) {
      this.session = createSession(normalizedIsoIds, elapsedMs);
    }

    this.lastElapsedMs = elapsedMs;
    this.session.elapsedMs = Math.max(0, elapsedMs - this.session.startedAtMs);
    this.session.estimatedDiskRateMBps = estimateDiskRate(this.session.streams.length);
    this.session.status = this.session.streams.length > 5 ? "warning" : "recording";
    this.session.warning = this.session.status === "warning" ? "High ISO count may exceed disk bandwidth." : undefined;

    return this.snapshot();
  }

  writeFrames(frames: MediaCoreFrame[], elapsedMs: number) {
    if (!this.session) {
      return undefined;
    }

    this.lastElapsedMs = elapsedMs;
    this.session.elapsedMs = Math.max(0, elapsedMs - this.session.startedAtMs);

    const writableFrames = frames.filter((frame) => frame.health !== "dropped");
    const program = this.session.streams.find((stream) => stream.kind === "program");
    if (program) {
      program.framesWritten += writableFrames.length;
    }

    this.session.streams
      .filter((stream) => stream.kind === "iso")
      .forEach((stream) => {
        stream.framesWritten += writableFrames.filter((frame) => frame.participantId === stream.participantId).length;
      });

    this.session.totalFramesWritten = this.session.streams.reduce((total, stream) => total + stream.framesWritten, 0);
    return this.snapshot();
  }

  stop() {
    this.session = undefined;
  }

  snapshot() {
    if (!this.session) {
      return undefined;
    }

    return {
      ...this.session,
      elapsedMs: Math.max(this.session.elapsedMs, this.lastElapsedMs - this.session.startedAtMs),
      streams: this.session.streams.map((stream) => ({ ...stream }))
    };
  }
}

function createSession(isoParticipantIds: string[], startedAtMs: number): MediaCoreRecordingSession {
  const programPath = `${BASE_RECORDING_FOLDER}/program-${startedAtMs}.mp4`;
  const streams: MediaCoreRecordingStream[] = [
    {
      kind: "program",
      path: programPath,
      framesWritten: 0
    },
    ...isoParticipantIds.map((participantId) => ({
      kind: "iso" as const,
      participantId,
      path: `${BASE_RECORDING_FOLDER}/iso-${participantId}-${startedAtMs}.mp4`,
      framesWritten: 0
    }))
  ];

  return {
    active: true,
    status: streams.length > 5 ? "warning" : "recording",
    startedAtMs,
    elapsedMs: 0,
    estimatedDiskRateMBps: estimateDiskRate(streams.length),
    programPath,
    streams,
    totalFramesWritten: 0,
    warning: streams.length > 5 ? "High ISO count may exceed disk bandwidth." : undefined
  };
}

function sameIsoStreams(session: MediaCoreRecordingSession, isoParticipantIds: string[]) {
  const currentIds = session.streams
    .filter((stream) => stream.kind === "iso")
    .map((stream) => stream.participantId as string)
    .sort();
  return currentIds.length === isoParticipantIds.length && currentIds.every((id, index) => id === isoParticipantIds[index]);
}

function estimateDiskRate(streamCount: number) {
  return Number((Math.max(1, streamCount) * 1.85).toFixed(2));
}
