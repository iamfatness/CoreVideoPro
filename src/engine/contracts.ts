import type {
  AudioMixState,
  AutoProductionState,
  AiStudioState,
  CaptionOverlayState,
  CaptureDeviceState,
  MediaFrameState,
  OutputDestination,
  OutputHealth,
  OutputProfile,
  RecordingSettings,
  OutputSessionState,
  Participant,
  PresetSummary,
  ProductionState,
  SceneTemplate,
  ShowPreset,
  SupportBundle
} from "../domain/production";
import type { NativeMediaCoreOperatorAction, NativeMediaCoreStateSnapshot } from "./nativeMediaCoreProtocol";

export type MeetingState = "idle" | "joining" | "in_meeting" | "reconnecting" | "error";

export type ZoomJoinRequest = {
  meetingUrl: string;
  displayName: string;
  webinar: boolean;
  passcode?: string;
  /** Broker-minted Meeting SDK JWT from OAuth sign-in. */
  sdkJwt?: string;
  /** Zoom Access Key for attributed joins. */
  userZak?: string;
};

export type ZoomSessionSnapshot = {
  meetingState: MeetingState;
  participants: Participant[];
  mediaFrames: MediaFrameState[];
  activeSpeakerName: string;
  screenShareActive: boolean;
  caption: string;
  elapsedSeconds: number;
};

export type MagicSceneRequest = {
  participants: Participant[];
  currentScenes: SceneTemplate[];
  brandKitId?: string;
  screenShareActive: boolean;
};

export type MagicSceneResult = {
  scenes: SceneTemplate[];
  summary: string;
  warnings: string[];
};

export interface ZoomCaptureEngine {
  join(request: ZoomJoinRequest): Promise<ZoomSessionSnapshot>;
  leave(): Promise<ZoomSessionSnapshot>;
  getParticipants(): Promise<Participant[]>;
  getSnapshot(): Promise<ZoomSessionSnapshot>;
  advanceSimulation?(): Promise<ZoomSessionSnapshot>;
}

/**
 * `recommendAutoProduction` is backed by the deterministic auto-director. The
 * scene *proposal* is produced by a pluggable `DirectorStrategy`
 * (`./directorStrategy`); an implementation MAY accept an optional strategy
 * (see `RuleBasedAiProductionEngine`). The director always gates the proposal
 * with its anti-thrash holds and falls back to the always-on heuristic on any
 * strategy failure, so the contract's behavior stays deterministic and safe
 * regardless of which strategy is supplied.
 */
export interface AiProductionEngine {
  buildMagicScene(request: MagicSceneRequest): Promise<MagicSceneResult>;
  recommendAutoProduction(state: ProductionState, snapshot: ZoomSessionSnapshot): Promise<AutoProductionState>;
}

export interface OutputEngine {
  setOutputProfile(profile: OutputProfile): Promise<OutputSessionState>;
  startRecording(settings: RecordingSettings): Promise<OutputSessionState>;
  stopRecording(): Promise<OutputSessionState>;
  startStream(destinations: OutputDestination[]): Promise<OutputSessionState>;
  stopStream(): Promise<OutputSessionState>;
  getHealth(): Promise<OutputHealth>;
  getSession(): Promise<OutputSessionState>;
}

export interface AudioMixEngine {
  buildMix(participants: Participant[]): Promise<AudioMixState>;
  setParticipantMuted(participantId: string, muted: boolean, participants: Participant[]): Promise<AudioMixState>;
  setParticipantGain(participantId: string, gainDb: number, participants: Participant[]): Promise<AudioMixState>;
}

export type CaptionOverlayRequest = {
  snapshot: ZoomSessionSnapshot;
  activeScene: SceneTemplate;
};

export interface CaptionOverlayEngine {
  buildOverlay(request: CaptionOverlayRequest): Promise<CaptionOverlayState>;
}

export interface AiStudioEngine {
  generate(state: ProductionState): Promise<AiStudioState>;
}

export interface MediaCoreSyncEngine {
  syncProduction(state: ProductionState, elapsedMs: number): Promise<NativeMediaCoreStateSnapshot>;
  executeOperatorAction(state: ProductionState, action: NativeMediaCoreOperatorAction, elapsedMs: number): Promise<NativeMediaCoreStateSnapshot>;
}

export interface PresetEngine {
  savePreset(state: ProductionState): Promise<ShowPreset>;
  loadPreset(id: string): Promise<ShowPreset | undefined>;
  listPresets(): Promise<PresetSummary[]>;
}

export type SupportBundleContext = {
  platform?: string;
  version?: string;
  runtime?: import("../domain/production").SupportBundleRuntime;
  crashEvents?: import("../domain/production").SupportBundleCrashEvent[];
  freeDiskBytes?: number;
};

export interface DiagnosticsEngine {
  createSupportBundle(
    state: ProductionState,
    mediaCore?: NativeMediaCoreStateSnapshot,
    context?: SupportBundleContext
  ): Promise<SupportBundle>;
}

export interface CaptureDeviceEngine {
  listDevices(): Promise<CaptureDeviceState[]>;
  selectInput(deviceId: string, inputId: string): Promise<CaptureDeviceState[]>;
  setAudioSyncOffset(deviceId: string, offsetMs: number): Promise<CaptureDeviceState[]>;
  connectDevice(deviceId: string): Promise<CaptureDeviceState[]>;
}
