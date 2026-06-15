import type { AutoProductionState, Participant, ProductionState } from "../domain/production";
import type { ZoomSessionSnapshot } from "./contracts";

export type AutoProductionRuleId =
  | "hold-empty"
  | "screen-share-priority"
  | "focused-interview"
  | "single-speaker"
  | "panel-discussion"
  | "producer-preview-override"
  | "director-hold";

export const SCREEN_SHARE_ENTER_HOLD_MS = 2500;
export const SCREEN_SHARE_EXIT_HOLD_MS = 3500;
export const SCENE_CHANGE_HOLD_MS = 4000;

export function planAutoProduction(
  state: ProductionState,
  snapshot: ZoomSessionSnapshot,
  elapsedMs = snapshot.elapsedSeconds * 1000
): AutoProductionState {
  const liveParticipants = snapshot.participants.filter((participant) => participant.health !== "video-off");
  if (snapshot.meetingState !== "in_meeting" || liveParticipants.length === 0) {
    return {
      recommendedSceneId: state.activeSceneId,
      confidence: 99,
      reason: "No live Zoom participants are available, so hold the current program scene.",
      action: "hold",
      lastAppliedSceneId: state.activeSceneId,
      ruleId: "hold-empty",
      heldSceneId: state.activeSceneId,
      signals: ["meeting unavailable", "no live participants"]
    };
  }

  const rule = selectAutomationRule(liveParticipants, snapshot);
  const targetSceneId = resolveTargetSceneId(state, rule, snapshot, elapsedMs);
  const recommendedScene = state.scenes.find((scene) => scene.id === targetSceneId);
  const alreadyProgram = targetSceneId === state.activeSceneId;
  const alreadyPreview = targetSceneId === state.previewSceneId;
  const overrideReason = producerPreviewOverride(state, targetSceneId);
  const stabilization = stabilizeSceneRecommendation({
    state,
    snapshot,
    elapsedMs,
    targetSceneId,
    ruleId: rule.ruleId,
    alreadyProgram
  });

  if (stabilization.action === "hold") {
    return {
      recommendedSceneId: targetSceneId,
      confidence: rule.confidence,
      reason: stabilization.reason,
      action: "hold",
      lastAppliedSceneId: alreadyProgram ? targetSceneId : state.autoProduction.lastAppliedSceneId,
      ruleId: overrideReason ? "producer-preview-override" : stabilization.holdReason ? "director-hold" : rule.ruleId,
      overrideReason,
      heldSceneId: state.activeSceneId,
      pendingSceneId: stabilization.pendingSceneId,
      holdStartedAtMs: stabilization.holdStartedAtMs,
      holdElapsedMs: stabilization.holdElapsedMs,
      holdReason: stabilization.holdReason,
      signals: buildAutomationSignals(snapshot, liveParticipants, rule)
    };
  }

  const action = resolveAutomationAction({
    mode: state.mode,
    alreadyProgram,
    alreadyPreview,
    confidence: rule.confidence,
    overrideReason
  });

  return {
    recommendedSceneId: targetSceneId,
    confidence: rule.confidence,
    reason: buildRecommendationReason(rule, snapshot, recommendedScene?.name ?? targetSceneId),
    action,
    lastAppliedSceneId: alreadyProgram ? targetSceneId : state.autoProduction.lastAppliedSceneId,
    ruleId: overrideReason ? "producer-preview-override" : rule.ruleId,
    overrideReason,
    heldSceneId: alreadyProgram ? targetSceneId : state.activeSceneId,
    pendingSceneId: alreadyProgram ? undefined : targetSceneId,
    holdStartedAtMs: alreadyProgram ? undefined : stabilization.holdStartedAtMs,
    holdElapsedMs: alreadyProgram ? 0 : stabilization.holdElapsedMs,
    signals: buildAutomationSignals(snapshot, liveParticipants, rule)
  };
}

function resolveTargetSceneId(
  state: ProductionState,
  rule: ReturnType<typeof selectAutomationRule>,
  snapshot: ZoomSessionSnapshot,
  elapsedMs: number
) {
  const previous = state.autoProduction;
  const pendingExit =
    state.activeSceneId === "speaker-slides" &&
    previous.pendingSceneId &&
    previous.pendingSceneId !== "speaker-slides" &&
    previous.holdStartedAtMs !== undefined;

  if (!pendingExit) {
    return rule.recommendedSceneId;
  }

  const holdElapsedMs = Math.max(0, elapsedMs - previous.holdStartedAtMs!);
  if (holdElapsedMs < SCREEN_SHARE_EXIT_HOLD_MS) {
    return previous.pendingSceneId!;
  }

  return previous.pendingSceneId!;
}

function selectAutomationRule(participants: Participant[], snapshot: ZoomSessionSnapshot) {
  if (snapshot.screenShareActive || participants.some((participant) => participant.isScreenSharing)) {
    return {
      ruleId: "screen-share-priority" as const,
      recommendedSceneId: "speaker-slides",
      confidence: 96
    };
  }

  if (participants.length === 1) {
    return {
      ruleId: "single-speaker" as const,
      recommendedSceneId: "intro",
      confidence: 91
    };
  }

  if (participants.length === 2) {
    return {
      ruleId: "focused-interview" as const,
      recommendedSceneId: "interview",
      confidence: 90
    };
  }

  return {
    ruleId: "panel-discussion" as const,
    recommendedSceneId: "panel",
    confidence: 92
  };
}

function stabilizeSceneRecommendation(input: {
  state: ProductionState;
  snapshot: ZoomSessionSnapshot;
  elapsedMs: number;
  targetSceneId: string;
  ruleId: AutoProductionRuleId;
  alreadyProgram: boolean;
}) {
  if (input.alreadyProgram || input.state.mode !== "set-and-forget") {
    return {
      action: "ready" as const,
      reason: "",
      pendingSceneId: undefined,
      holdStartedAtMs: undefined,
      holdElapsedMs: 0,
      holdReason: undefined
    };
  }

  const overrideReason = producerPreviewOverride(input.state, input.targetSceneId);
  if (overrideReason) {
    return {
      action: "hold" as const,
      reason: overrideReason,
      pendingSceneId: input.targetSceneId,
      holdStartedAtMs: input.state.autoProduction.holdStartedAtMs ?? input.elapsedMs,
      holdElapsedMs: input.elapsedMs - (input.state.autoProduction.holdStartedAtMs ?? input.elapsedMs),
      holdReason: "Producer preview override"
    };
  }

  const previous = input.state.autoProduction;
  const pendingSceneId = previous.pendingSceneId === input.targetSceneId ? input.targetSceneId : input.targetSceneId;
  const holdStartedAtMs =
    previous.pendingSceneId === input.targetSceneId && previous.holdStartedAtMs !== undefined
      ? previous.holdStartedAtMs
      : input.elapsedMs;
  const holdElapsedMs = Math.max(0, input.elapsedMs - holdStartedAtMs);
  const requiredHoldMs = requiredDirectorHoldMs({
    currentSceneId: input.state.activeSceneId,
    targetSceneId: input.targetSceneId,
    ruleId: input.ruleId,
    screenShareActive: input.snapshot.screenShareActive
  });

  if (holdElapsedMs < requiredHoldMs) {
    const targetScene = input.state.scenes.find((scene) => scene.id === input.targetSceneId);
    const currentScene = input.state.scenes.find((scene) => scene.id === input.state.activeSceneId);
    const remainingSeconds = Math.max(1, Math.ceil((requiredHoldMs - holdElapsedMs) / 1000));

    return {
      action: "hold" as const,
      reason: `Holding ${currentScene?.name ?? input.state.activeSceneId} for ${remainingSeconds}s before switching to ${targetScene?.name ?? input.targetSceneId}.`,
      pendingSceneId,
      holdStartedAtMs,
      holdElapsedMs,
      holdReason:
        input.ruleId === "screen-share-priority"
          ? "Screen share stabilization"
          : input.state.activeSceneId === "speaker-slides"
            ? "Screen share release stabilization"
            : "Scene change stabilization"
    };
  }

  return {
    action: "ready" as const,
    reason: "",
    pendingSceneId,
    holdStartedAtMs,
    holdElapsedMs,
    holdReason: undefined
  };
}

export function requiredDirectorHoldMs(input: {
  currentSceneId: string;
  targetSceneId: string;
  ruleId: AutoProductionRuleId;
  screenShareActive: boolean;
}) {
  if (input.currentSceneId === input.targetSceneId) {
    return 0;
  }

  if (input.targetSceneId === "speaker-slides" && input.screenShareActive) {
    return SCREEN_SHARE_ENTER_HOLD_MS;
  }

  if (input.currentSceneId === "speaker-slides" && !input.screenShareActive) {
    return SCREEN_SHARE_EXIT_HOLD_MS;
  }

  return SCENE_CHANGE_HOLD_MS;
}

function resolveAutomationAction(input: {
  mode: ProductionState["mode"];
  alreadyProgram: boolean;
  alreadyPreview: boolean;
  confidence: number;
  overrideReason?: string;
}): AutoProductionState["action"] {
  if (input.alreadyProgram || input.overrideReason) {
    return "hold";
  }

  if (input.mode !== "set-and-forget") {
    return input.alreadyPreview ? "take" : "queue";
  }

  return input.confidence >= 88 ? "take" : "queue";
}

function producerPreviewOverride(state: ProductionState, recommendedSceneId: string) {
  if (state.mode !== "set-and-forget") {
    return undefined;
  }

  if (state.previewSceneId === state.activeSceneId || state.previewSceneId === recommendedSceneId) {
    return undefined;
  }

  const previewScene = state.scenes.find((scene) => scene.id === state.previewSceneId);
  return `Producer preview override is holding ${previewScene?.name ?? state.previewSceneId}.`;
}

function buildRecommendationReason(
  rule: ReturnType<typeof selectAutomationRule>,
  snapshot: ZoomSessionSnapshot,
  sceneName: string
) {
  if (rule.ruleId === "screen-share-priority") {
    return `Screen share is active, so ${sceneName} is the safest live layout.`;
  }

  if (rule.ruleId === "single-speaker") {
    return `One live speaker is available, so ${sceneName} keeps the host framed cleanly.`;
  }

  if (rule.ruleId === "focused-interview") {
    return `Two speakers are visible, so ${sceneName} keeps the conversation focused.`;
  }

  return `${snapshot.participants.length} participants are live without slides, so ${sceneName} keeps everyone visible.`;
}

function buildAutomationSignals(snapshot: ZoomSessionSnapshot, participants: Participant[], rule: ReturnType<typeof selectAutomationRule>) {
  const activeSpeaker = participants.find((participant) => participant.isActiveSpeaker);
  return [
    `${participants.length} live participant${participants.length === 1 ? "" : "s"}`,
    snapshot.screenShareActive ? "screen share active" : "screen share inactive",
    activeSpeaker ? `active speaker ${activeSpeaker.name}` : "active speaker unavailable",
    `rule ${rule.ruleId}`
  ];
}