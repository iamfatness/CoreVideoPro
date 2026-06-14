import {
  initialProduction,
  type OutputDestination,
  type OutputDestinationState,
  type OutputHealth,
  type OutputProfile,
  type OutputSessionState,
  type RecordingSettings
} from "../domain/production";

export class SimulatedOutputSession {
  private recording = false;
  private streaming = false;
  private activeDestinationIds: string[] = [];
  private recordingFile: string | undefined;
  private recordingSettings: RecordingSettings | undefined;
  private tick = 0;
  private destinations: OutputDestination[];
  private profile: OutputProfile;

  constructor(
    destinations: OutputDestination[] = initialProduction.outputDestinations,
    profile: OutputProfile = initialProduction.outputProfiles.find((item) => item.id === initialProduction.selectedOutputProfileId) ?? initialProduction.outputProfiles[1]
  ) {
    this.destinations = destinations;
    this.profile = profile;
  }

  setOutputProfile(profile: OutputProfile): OutputSessionState {
    this.profile = profile;
    return this.getSession();
  }

  startRecording(settings: RecordingSettings): OutputSessionState {
    this.recording = true;
    this.recordingSettings = settings;
    this.recordingFile = buildRecordingFile(settings);
    return this.getSession();
  }

  stopRecording(): OutputSessionState {
    this.recording = false;
    return this.getSession();
  }

  startStream(destinations: OutputDestination[]): OutputSessionState {
    const selectedDestinations = destinations.length > 0 ? destinations : this.destinations.filter((destination) => destination.enabled);
    this.destinations = this.destinations.map((destination) => selectedDestinations.find((item) => item.id === destination.id) ?? destination);
    this.streaming = true;
    this.activeDestinationIds = selectedDestinations.map((destination) => destination.id);
    return this.getSession();
  }

  stopStream(): OutputSessionState {
    this.streaming = false;
    return this.getSession();
  }

  getHealth(): OutputHealth {
    return this.buildHealth();
  }

  getSession(): OutputSessionState {
    this.tick += this.recording || this.streaming ? 1 : 0;

    const health = this.buildHealth();
    return Object.freeze({
      recording: this.recording,
      streaming: this.streaming,
      recordingSettings: this.recordingSettings,
      recordingFile: this.recordingFile,
      streamDestinationId: this.activeDestinationIds[0],
      activeDestinationIds: this.activeDestinationIds,
      destinations: this.buildDestinationStates(health),
      elapsedSeconds: this.tick * 5,
      health,
      statusText: buildStatusText(this.recording, this.streaming, health, this.activeDestinationIds.length, this.recordingSettings)
    });
  }

  private buildHealth(): OutputHealth {
    const active = this.recording || this.streaming;
    return {
      ...initialProduction.output,
      resolution: this.profile.resolution,
      fps: this.profile.fps,
      bitrateMbps: this.streaming ? roundToTenth(this.profile.targetBitrateMbps * Math.max(1, this.activeDestinationIds.length)) : 0,
      droppedFrames: active && this.tick > 3 ? 1 : 0,
      encoderLoad: active ? getEncoderLoad(this.profile, this.tick, this.activeDestinationIds.length, this.recordingSettings) : 18,
      network: this.streaming && this.activeDestinationIds.length > 2 ? "warning" : this.streaming ? "excellent" : "good"
    };
  }

  private buildDestinationStates(health: OutputHealth): OutputDestinationState[] {
    return this.destinations.map((destination) => {
      const active = this.streaming && this.activeDestinationIds.includes(destination.id);
      return {
        ...destination,
        active,
        health: active ? (health.network === "warning" && destination.protocol !== "NDI" ? "warning" : "live") : "idle",
        bitrateMbps: active ? getDestinationBitrate(destination) : 0
      };
    });
  }
}

function buildStatusText(
  recording: boolean,
  streaming: boolean,
  health: OutputHealth,
  destinationCount: number,
  recordingSettings: RecordingSettings | undefined
) {
  const isoCount = recordingSettings?.isoParticipantIds.length ?? 0;
  const isoText = isoCount > 0 ? ` + ${isoCount} ISO${isoCount === 1 ? "" : "s"}` : "";

  if (recording && streaming) {
    return `Recording program${isoText} and streaming to ${destinationCount} destination${destinationCount === 1 ? "" : "s"} at ${health.resolution} ${health.fps}fps`;
  }

  if (recording) {
    return `Recording program${isoText} locally at ${health.resolution} ${health.fps}fps`;
  }

  if (streaming) {
    return `Streaming to ${destinationCount} destination${destinationCount === 1 ? "" : "s"} at ${health.bitrateMbps} Mbps`;
  }

  return "Outputs idle";
}

function getDestinationBitrate(destination: OutputDestination) {
  if (destination.protocol === "NDI") {
    return 12;
  }

  if (destination.protocol === "SRT") {
    return 7.5;
  }

  if (destination.protocol === "WebRTC") {
    return 5;
  }

  return 8.2;
}

function buildRecordingFile(settings: RecordingSettings) {
  const safePrefix = settings.filenamePrefix.trim().replace(/[^a-z0-9_-]+/gi, "_") || "CoreVideo_Recording";
  const folder = settings.folder.trim().replace(/[\\/]+$/g, "") || "Recordings";
  return `${folder}/${safePrefix}.${settings.format}`;
}

function getEncoderLoad(profile: OutputProfile, tick: number, destinationCount: number, recordingSettings: RecordingSettings | undefined) {
  const profileCost = profile.resolution === "3840x2160" ? 24 : 0;
  const frameRateCost = profile.fps === 60 ? 8 : 0;
  const qualityCost = recordingSettings?.quality === "archive" ? 10 : recordingSettings?.quality === "high" ? 5 : 0;
  const isoCost = (recordingSettings?.isoParticipantIds.length ?? 0) * 4;
  return Math.min(96, 40 + profileCost + frameRateCost + qualityCost + isoCost + Math.min(tick * 2, 16) + Math.max(0, destinationCount - 1) * 5);
}

function roundToTenth(value: number) {
  return Math.round(value * 10) / 10;
}
