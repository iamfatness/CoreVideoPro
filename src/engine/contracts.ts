import type {
  AudioMixState,
  AutoProductionState,
  CaptionOverlayState,
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

export type MeetingState = "idle" | "joining" | "in_meeting" | "reconnecting" | "error";

export type ZoomJoinRequest = {
  meetingUrl: string;
  displayName: string;
  webinar: boolean;
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

export interface PresetEngine {
  savePreset(state: ProductionState): Promise<ShowPreset>;
  loadPreset(id: string): Promise<ShowPreset | undefined>;
  listPresets(): Promise<PresetSummary[]>;
}

export interface DiagnosticsEngine {
  createSupportBundle(state: ProductionState): Promise<SupportBundle>;
}
