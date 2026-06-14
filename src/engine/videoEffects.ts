import type { MediaFrameState, ParticipantVideoEffect } from "../domain/production";

export function getVideoEffect(effects: ParticipantVideoEffect[], participantId: string) {
  return effects.find((effect) => effect.participantId === participantId) ?? createDefaultVideoEffect(participantId);
}

export function createDefaultVideoEffect(participantId: string): ParticipantVideoEffect {
  return {
    participantId,
    cropMode: "auto",
    manualZoom: 1,
    chromaKeyEnabled: false,
    chromaKeyColor: "green",
    spillSuppression: 42
  };
}

export function toggleChromaKey(effects: ParticipantVideoEffect[], participantId: string) {
  const current = getVideoEffect(effects, participantId);
  return upsertEffect(effects, {
    ...current,
    chromaKeyEnabled: !current.chromaKeyEnabled
  });
}

export function toggleCropMode(effects: ParticipantVideoEffect[], participantId: string) {
  const current = getVideoEffect(effects, participantId);
  return upsertEffect(effects, {
    ...current,
    cropMode: current.cropMode === "auto" ? "manual" : "auto",
    manualZoom: current.cropMode === "auto" ? 1.18 : 1
  });
}

export function applyVideoEffectToFrame(frame: MediaFrameState | undefined, effect: ParticipantVideoEffect) {
  if (!frame || effect.cropMode === "auto") {
    return frame;
  }

  const width = Math.max(0.62, frame.crop.width / effect.manualZoom);
  const height = Math.max(0.62, frame.crop.height / effect.manualZoom);

  return {
    ...frame,
    crop: {
      x: clamp(frame.crop.x + (frame.crop.width - width) / 2),
      y: clamp(frame.crop.y + (frame.crop.height - height) / 2),
      width,
      height
    }
  };
}

function upsertEffect(effects: ParticipantVideoEffect[], next: ParticipantVideoEffect) {
  if (effects.some((effect) => effect.participantId === next.participantId)) {
    return effects.map((effect) => (effect.participantId === next.participantId ? next : effect));
  }

  return [...effects, next];
}

function clamp(value: number) {
  return Math.max(0, Math.min(1, value));
}
