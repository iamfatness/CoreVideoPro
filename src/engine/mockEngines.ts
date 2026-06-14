import type {
  AiProductionEngine,
  MagicSceneRequest,
  MagicSceneResult,
  OutputEngine,
  ZoomCaptureEngine,
  ZoomJoinRequest,
  ZoomSessionSnapshot
} from "./contracts";
import {
  getActiveSpeaker,
  hasActiveScreenShare,
  sortParticipantsForProduction,
  type AutoProductionState,
  type OutputDestination,
  type OutputHealth,
  type OutputProfile,
  type OutputSessionState,
  type Participant,
  type ProductionState,
  type RecordingSettings,
  type SceneTemplate
} from "../domain/production";
import { SimulatedZoomSession } from "./simulatedZoomSession";
import { SimulatedOutputSession } from "./outputSession";

export class MockZoomCaptureEngine implements ZoomCaptureEngine {
  private session = new SimulatedZoomSession();

  async join(_request: ZoomJoinRequest): Promise<ZoomSessionSnapshot> {
    return this.session.join(_request);
  }

  async leave(): Promise<ZoomSessionSnapshot> {
    return this.session.leave();
  }

  async getParticipants(): Promise<Participant[]> {
    return this.session.getParticipants();
  }

  async getSnapshot(): Promise<ZoomSessionSnapshot> {
    return this.session.getSnapshot();
  }

  async advanceSimulation(): Promise<ZoomSessionSnapshot> {
    return this.session.advance();
  }
}

export class RuleBasedAiProductionEngine implements AiProductionEngine {
  async buildMagicScene(request: MagicSceneRequest): Promise<MagicSceneResult> {
    return buildMagicScene(request);
  }

  async recommendAutoProduction(state: ProductionState, snapshot: ZoomSessionSnapshot): Promise<AutoProductionState> {
    return recommendAutoProduction(state, snapshot);
  }
}

export class MockOutputEngine implements OutputEngine {
  private session = new SimulatedOutputSession();

  async setOutputProfile(profile: OutputProfile): Promise<OutputSessionState> {
    return this.session.setOutputProfile(profile);
  }

  async startRecording(settings: RecordingSettings): Promise<OutputSessionState> {
    return this.session.startRecording(settings);
  }

  async stopRecording(): Promise<OutputSessionState> {
    return this.session.stopRecording();
  }

  async startStream(destinations: OutputDestination[]): Promise<OutputSessionState> {
    return this.session.startStream(destinations);
  }

  async stopStream(): Promise<OutputSessionState> {
    return this.session.stopStream();
  }

  async getHealth(): Promise<OutputHealth> {
    return this.session.getHealth();
  }

  async getSession(): Promise<OutputSessionState> {
    return this.session.getSession();
  }
}

export function buildMagicScene(request: MagicSceneRequest): MagicSceneResult {
  const participants = sortParticipantsForProduction(request.participants);
  const screenShareActive = request.screenShareActive || hasActiveScreenShare(participants);
  const activeSpeaker = getActiveSpeaker(participants);
  const host = participants.find((participant) => participant.role === "Host");
  const presenter = participants.find((participant) => participant.role === "Presenter") ?? activeSpeaker;
  const panelists = participants.filter((participant) => participant.id !== host?.id && participant.id !== presenter?.id);

  const scenes: SceneTemplate[] = [
    {
      id: "intro",
      name: "Intro",
      type: "intro",
      layout: "host-focus",
      automation: host ? `Host open with ${host.name}` : "Auto host open",
      slots: host ? [host.id] : [],
      routes: host ? [{ id: "intro-1", mode: "fixed", participantId: host.id, audioRole: "isolated" }] : []
    },
    {
      id: "interview",
      name: "Interview",
      type: "interview",
      layout: "two-up",
      automation: "Two-up speaker hold + dynamic lower-thirds",
      slots: [host?.id, activeSpeaker?.id ?? presenter?.id].filter(Boolean) as string[],
      routes: [
        host ? { id: "interview-1", mode: "fixed", participantId: host.id, audioRole: "isolated" } : undefined,
        { id: "interview-2", mode: "active-speaker", participantId: activeSpeaker?.id ?? presenter?.id, audioRole: "mix" }
      ].filter(Boolean) as SceneTemplate["routes"]
    },
    {
      id: "speaker-slides",
      name: "Speaker + Slides",
      type: "slides",
      layout: "speaker-slides",
      automation: screenShareActive ? "Presenter priority + active screen share" : "Presenter priority + ready for screen share",
      selected: screenShareActive,
      slots: [presenter?.id, "screen-share"].filter(Boolean) as string[],
      routes: [
        presenter ? { id: "speaker-slides-1", mode: "fixed", participantId: presenter.id, audioRole: "isolated" } : undefined,
        { id: "speaker-slides-2", mode: "screen-share", audioRole: "audience" }
      ].filter(Boolean) as SceneTemplate["routes"]
    },
    {
      id: "panel",
      name: "Panel",
      type: "panel",
      layout: "smart-grid",
      automation: `${Math.min(participants.length, 6)}-up smart grid + active speaker highlight`,
      selected: !screenShareActive && participants.length > 2,
      slots: participants.slice(0, 6).map((participant) => participant.id),
      routes: participants.slice(0, 6).map((participant, index) => ({
        id: `panel-${index + 1}`,
        mode: index === 0 ? "active-speaker" : "fixed",
        participantId: participant.id,
        audioRole: index === 0 ? "mix" : "isolated"
      }))
    },
    {
      id: "closing",
      name: "Closing",
      type: "closing",
      layout: "outro",
      automation: host ? `Host outro with ${host.name}` : "Auto outro",
      slots: host ? [host.id] : [],
      routes: host ? [{ id: "closing-1", mode: "fixed", participantId: host.id, audioRole: "isolated" }] : []
    }
  ];

  if (!scenes.some((scene) => scene.selected)) {
    const preferredId = participants.length <= 2 ? "interview" : "panel";
    scenes.forEach((scene) => {
      scene.selected = scene.id === preferredId;
    });
  }

  const selected = scenes.find((scene) => scene.selected);
  const warnings = participants
    .filter((participant) => participant.health === "low-resolution" || participant.health === "recovering")
    .map((participant) => `${participant.name} is ${participant.health.replace("-", " ")}.`);

  return {
    scenes,
    summary: `Magic Scene built ${scenes.length} scenes; selected ${selected?.name ?? "best fit"} for ${participants.length} participants${screenShareActive ? " with screen share" : ""}.`,
    warnings
  };
}

export function recommendAutoProduction(
  state: ProductionState,
  snapshot: ZoomSessionSnapshot
): AutoProductionState {
  if (snapshot.meetingState !== "in_meeting" || snapshot.participants.length === 0) {
    return {
      recommendedSceneId: state.activeSceneId,
      confidence: 99,
      reason: "No live Zoom participants are available, so hold the current program scene.",
      action: "hold"
    };
  }

  const recommendedSceneId = getRecommendedSceneId(snapshot);
  const recommendedScene = state.scenes.find((scene) => scene.id === recommendedSceneId);
  const alreadyProgram = recommendedSceneId === state.activeSceneId;
  const alreadyPreview = recommendedSceneId === state.previewSceneId;
  const action = alreadyProgram ? "hold" : state.mode === "set-and-forget" ? "take" : alreadyPreview ? "take" : "queue";

  return {
    recommendedSceneId,
    confidence: getRecommendationConfidence(snapshot),
    reason: buildRecommendationReason(snapshot, recommendedScene?.name ?? recommendedSceneId),
    action,
    lastAppliedSceneId: alreadyProgram ? recommendedSceneId : state.autoProduction.lastAppliedSceneId
  };
}

function getRecommendedSceneId(snapshot: ZoomSessionSnapshot) {
  if (snapshot.screenShareActive) {
    return "speaker-slides";
  }

  if (snapshot.participants.length <= 2) {
    return "interview";
  }

  return "panel";
}

function getRecommendationConfidence(snapshot: ZoomSessionSnapshot) {
  if (snapshot.screenShareActive) {
    return 94;
  }

  if (snapshot.participants.length <= 2) {
    return 88;
  }

  return 90;
}

function buildRecommendationReason(snapshot: ZoomSessionSnapshot, sceneName: string) {
  if (snapshot.screenShareActive) {
    return `Screen share is active, so ${sceneName} is the safest live layout.`;
  }

  if (snapshot.participants.length <= 2) {
    return `Only ${snapshot.participants.length} speakers are visible, so ${sceneName} keeps the conversation focused.`;
  }

  return `${snapshot.participants.length} participants are live without slides, so ${sceneName} keeps everyone visible.`;
}
