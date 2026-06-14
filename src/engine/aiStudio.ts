import type { AiStudioState, ProductionState } from "../domain/production";

export class RuleBasedAiStudioEngine {
  async generate(state: ProductionState): Promise<AiStudioState> {
    return buildAiStudioState(state);
  }
}

export function buildAiStudioState(state: ProductionState): AiStudioState {
  const activeSpeaker = state.participants.find((participant) => participant.isActiveSpeaker);
  const speakerName = state.captionOverlay.speakerName || activeSpeaker?.name || "Program audio";
  const elapsedSeconds = Math.max(state.outputSession.elapsedSeconds, 1);
  const captionText = state.captionOverlay.text || state.captions || "Transcript is standing by for the next speaker.";
  const transcript = [
    {
      id: `segment-${elapsedSeconds}-${slugify(speakerName)}`,
      startSeconds: Math.max(0, elapsedSeconds - 12),
      endSeconds: elapsedSeconds,
      speakerName,
      text: captionText,
      confidence: state.captionOverlay.confidence
    }
  ];
  const activeScene = state.scenes.find((scene) => scene.id === state.activeSceneId);
  const chapters = [
    {
      id: `chapter-${state.activeSceneId}`,
      title: activeScene ? activeScene.name : "Live discussion",
      startSeconds: Math.max(0, elapsedSeconds - 60),
      summary: buildChapterSummary(state)
    }
  ];
  const highlights = buildHighlights(state, speakerName, elapsedSeconds);

  return {
    transcript,
    chapters,
    highlights,
    showNotes: {
      title: `${state.meetingTitle} show notes`,
      summary: buildShowSummary(state, speakerName),
      bullets: buildShowBullets(state),
      nextActions: buildNextActions(state)
    },
    rundown: buildRundown(state)
  };
}

function buildHighlights(state: ProductionState, speakerName: string, elapsedSeconds: number) {
  const highlights = [
    {
      id: `highlight-${slugify(state.activeSceneId)}`,
      title: state.participants.some((participant) => participant.isScreenSharing) ? "Presenter plus slides moment" : "Key speaker moment",
      summary: state.captionOverlay.text,
      startSeconds: Math.max(0, elapsedSeconds - 20),
      endSeconds: elapsedSeconds + 5,
      speakerName,
      confidence: Math.min(98, state.captionOverlay.confidence + 2),
      suggestedClipName: `${slugify(state.meetingTitle)}-${slugify(speakerName)}-${state.activeSceneId}`
    }
  ];

  if (state.autoProduction.confidence >= 88) {
    highlights.push({
      id: "highlight-auto-director",
      title: "Auto-directed production beat",
      summary: state.autoProduction.reason,
      startSeconds: Math.max(0, elapsedSeconds - 35),
      endSeconds: elapsedSeconds + 8,
      speakerName,
      confidence: state.autoProduction.confidence,
      suggestedClipName: `${slugify(state.meetingTitle)}-auto-director`
    });
  }

  return highlights;
}

function buildShowSummary(state: ProductionState, speakerName: string) {
  const shareText = state.participants.some((participant) => participant.isScreenSharing)
    ? "screen share is part of the current story"
    : "the discussion is participant-led";
  return `${state.meetingTitle} is currently focused on ${speakerName}; ${shareText}, with ${state.participants.length} Zoom participants available for AI-generated clips and chapters.`;
}

function buildChapterSummary(state: ProductionState) {
  if (state.activeSceneId === "speaker-slides") {
    return "Presenter-led section with slides, adaptive captions, and lower-third protection.";
  }

  if (state.activeSceneId === "panel") {
    return "Panel discussion section with active speaker guidance and gallery context.";
  }

  return `${state.activeSceneId} section with current Zoom participant routing and captions.`;
}

function buildShowBullets(state: ProductionState) {
  const activeSpeaker = state.participants.find((participant) => participant.isActiveSpeaker);
  return [
    `Featured speaker: ${activeSpeaker?.name ?? state.captionOverlay.speakerName}`,
    `Current scene: ${state.scenes.find((scene) => scene.id === state.activeSceneId)?.name ?? state.activeSceneId}`,
    `Caption confidence: ${state.captionOverlay.confidence}%`,
    `Output posture: ${state.outputSession.statusText}`
  ];
}

function buildNextActions(state: ProductionState) {
  const actions = ["Review suggested clip names before export."];

  if (!state.recording) {
    actions.push("Start recording to preserve highlight source media.");
  }

  if (state.captionOverlay.confidence < 90) {
    actions.push("Check caption quality before publishing show notes.");
  }

  if (state.participants.some((participant) => participant.health !== "live")) {
    actions.push("Resolve low-quality participant feeds before clipping.");
  }

  return actions;
}

function buildRundown(state: ProductionState) {
  return state.scenes.map((scene, index) => {
    const status = scene.id === state.activeSceneId ? "live" : scene.id === state.previewSceneId ? "queued" : "ready";
    return `${index + 1}. ${scene.name} - ${status} - ${scene.automation}`;
  });
}

function slugify(value: string) {
  return value.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/(^-|-$)/g, "") || "corevideo";
}
