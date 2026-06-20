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
import {
  heuristicDirectorStrategy,
  proposeWithProviderAsync,
  strategyFromResolution,
  type AiDirectorProvider,
  type DirectorSignals,
  type DirectorStrategy
} from "./directorStrategy";

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

/**
 * Optional injection points for {@link RuleBasedAiProductionEngine}.
 *
 * - `directorStrategy`: synchronous, always-on proposal seam (defaults to the
 *   heuristic).
 * - `directorProvider`: the optional ASYNC intelligence-provider seam (Item 10).
 *   A future model-backed / `services/`-hosted director is injected here; the
 *   engine runs it under a timeout and falls back to the heuristic on absence /
 *   timeout / error / invalid proposal. `planAutoProduction` still gates whatever
 *   it returns, so a provider can never reduce safety. NONE ships in `src/engine`.
 * - `providerTimeoutMs`: per-call budget before the heuristic takes over.
 */
export type RuleBasedAiProductionEngineOptions = {
  directorStrategy?: DirectorStrategy;
  directorProvider?: AiDirectorProvider;
  providerTimeoutMs?: number;
};

export class RuleBasedAiProductionEngine implements AiProductionEngine {
  private readonly directorStrategy: DirectorStrategy;
  private readonly directorProvider?: AiDirectorProvider;
  private readonly providerTimeoutMs?: number;

  constructor(options: RuleBasedAiProductionEngineOptions | DirectorStrategy = {}) {
    // Back-compat: a bare DirectorStrategy may still be passed directly.
    const normalized: RuleBasedAiProductionEngineOptions =
      typeof (options as DirectorStrategy).propose === "function"
        ? { directorStrategy: options as DirectorStrategy }
        : (options as RuleBasedAiProductionEngineOptions);
    this.directorStrategy = normalized.directorStrategy ?? heuristicDirectorStrategy;
    this.directorProvider = normalized.directorProvider;
    this.providerTimeoutMs = normalized.providerTimeoutMs;
  }

  async buildMagicScene(request: MagicSceneRequest): Promise<MagicSceneResult> {
    return buildMagicScene(request);
  }

  async recommendAutoProduction(state: ProductionState, snapshot: ZoomSessionSnapshot): Promise<AutoProductionState> {
    // With no async provider configured, take the deterministic sync path so the
    // engine stays synchronous-equivalent and offline.
    if (!this.directorProvider) {
      return recommendAutoProduction(state, snapshot, this.directorStrategy);
    }

    // Resolve the async provider proposal (timeout + heuristic fallback), then
    // gate it through the SAME stabilizer the heuristic uses. The provider can
    // only ever influence the *proposal*; the holds remain the safety arbiter.
    const elapsedMs = snapshot.elapsedSeconds * 1000;
    const liveParticipants = snapshot.participants.filter((participant) => participant.health !== "video-off");
    const signals: DirectorSignals = { state, snapshot, liveParticipants, elapsedMs };
    const resolution = await proposeWithProviderAsync(signals, this.directorProvider, {
      timeoutMs: this.providerTimeoutMs,
      fallback: this.directorStrategy
    });
    return recommendAutoProduction(state, snapshot, strategyFromResolution(resolution));
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
