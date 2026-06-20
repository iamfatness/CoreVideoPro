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
import { planAutoProduction } from "./autoProductionDirector";
import { heuristicDirectorStrategy, type DirectorStrategy } from "./directorStrategy";

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
  /**
   * Optional pluggable director strategy. Defaults to the always-on heuristic.
   * A future model-backed strategy plugs in here; `planAutoProduction` always
   * gates its proposal and silently falls back to the heuristic on any
   * failure/invalid proposal, so passing a strategy can never reduce safety.
   */
  private readonly directorStrategy: DirectorStrategy;

  constructor(directorStrategy: DirectorStrategy = heuristicDirectorStrategy) {
    this.directorStrategy = directorStrategy;
  }

  async buildMagicScene(request: MagicSceneRequest): Promise<MagicSceneResult> {
    return buildMagicScene(request);
  }

  async recommendAutoProduction(state: ProductionState, snapshot: ZoomSessionSnapshot): Promise<AutoProductionState> {
    return recommendAutoProduction(state, snapshot, this.directorStrategy);
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
  snapshot: ZoomSessionSnapshot,
  directorStrategy: DirectorStrategy = heuristicDirectorStrategy
): AutoProductionState {
  return planAutoProduction(state, snapshot, snapshot.elapsedSeconds * 1000, directorStrategy);
}
