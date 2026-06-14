import type { AutoProductionState, Participant, ProductionState } from "../domain/production";
import type { ZoomSessionSnapshot } from "./contracts";

export type AutoProductionRuleId =
  | "hold-empty"
  | "screen-share-priority"
  | "focused-interview"
  | "single-speaker"
  | "panel-discussion"
  | "producer-preview-override";

export function planAutoProduction(state: ProductionState, snapshot: ZoomSessionSnapshot): AutoProductionState {
  const liveParticipants = snapshot.participants.filter((participant) => participant.health !== "video-off");
  if (snapshot.meetingState !== "in_meeting" || liveParticipants.length === 0) {
    return {
      recommendedSceneId: state.activeSceneId,
      confidence: 99,
      reason: "No live Zoom participants are available, so hold the current program scene.",
      action: "hold",
      lastAppliedSceneId: state.activeSceneId,
      ruleId: "hold-empty",
      signals: ["meeting unavailable", "no live participants"]
    };
  }

  const rule = selectAutomationRule(liveParticipants, snapshot);
  const recommendedScene = state.scenes.find((scene) => scene.id === rule.recommendedSceneId);
  const alreadyProgram = rule.recommendedSceneId === state.activeSceneId;
  const alreadyPreview = rule.recommendedSceneId === state.previewSceneId;
  const overrideReason = producerPreviewOverride(state, rule.recommendedSceneId);
  const action = resolveAutomationAction({
    mode: state.mode,
    alreadyProgram,
    alreadyPreview,
    confidence: rule.confidence,
    overrideReason
  });

  return {
    recommendedSceneId: rule.recommendedSceneId,
    confidence: rule.confidence,
    reason: buildRecommendationReason(rule, snapshot, recommendedScene?.name ?? rule.recommendedSceneId),
    action,
    lastAppliedSceneId: alreadyProgram ? rule.recommendedSceneId : state.autoProduction.lastAppliedSceneId,
    ruleId: overrideReason ? "producer-preview-override" : rule.ruleId,
    overrideReason,
    signals: buildAutomationSignals(snapshot, liveParticipants, rule)
  };
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
