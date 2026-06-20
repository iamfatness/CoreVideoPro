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

/**
 * A single speaker-turn observation. The intelligence service consumes a short
 * recent window of these to reason about conversational dynamics (who is
 * driving, hand-offs, cross-talk) beyond the instantaneous active-speaker flag.
 */
export type SpeakerTurn = {
  participantId: string;
  /** Monotonic ms when this turn began (relative to session start). */
  startedAtMs: number;
  /** Turn duration in ms; omit while the turn is still open. */
  durationMs?: number;
};

/** Coarse sentiment read of the room, derived upstream from audio/transcript. */
export type RoomSentiment = "positive" | "neutral" | "tense" | "mixed";

/** What the screen share currently appears to contain, when classifiable. */
export type ScreenShareContentKind =
  | "slides"
  | "demo"
  | "document"
  | "video"
  | "code"
  | "spreadsheet"
  | "unknown";

/**
 * The richer signal bundle (Item 10) the optional intelligence service consumes.
 * Every field is optional and best-effort: the always-on heuristic ignores all
 * of it, so the director degrades cleanly when these are absent. These signals
 * MAY include meeting content (transcript-derived sentiment, screen-share
 * classification); whether they leave the machine is a config/privacy decision —
 * see `AutoProductionDirectorServiceConfig`.
 */
export type AutoProductionSignals = {
  /** Recent speaker turns, most-recent-last. */
  speakerTurns?: SpeakerTurn[];
  /** Coarse room sentiment. */
  sentiment?: RoomSentiment;
  /** Classified screen-share content kind. */
  screenShareContent?: ScreenShareContentKind;
  /** 0-100 engagement score (reactions, chat rate, attentiveness). */
  engagement?: number;
};

export type MagicSceneRequest = {
  participants: Participant[];
  currentScenes: SceneTemplate[];
  brandKitId?: string;
  screenShareActive: boolean;
  /** Optional richer signals for a model-backed scene proposal. */
  signals?: AutoProductionSignals;
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
  /**
   * Recommend the next auto-production action. `signals` are optional richer
   * inputs (speaker turns, sentiment, screen-share content, engagement) that a
   * model-backed implementation may consume; the heuristic implementation
   * ignores them, so omitting them is always safe.
   */
  recommendAutoProduction(
    state: ProductionState,
    snapshot: ZoomSessionSnapshot,
    signals?: AutoProductionSignals
  ): Promise<AutoProductionState>;
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
